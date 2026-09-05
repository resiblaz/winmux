// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CodexAppServerAdapter.h"

#include "CodexAppServerContract.h"
#include "TaskProviderFailurePolicy.h"
#include "TaskProviderRunContext.h"

namespace Microsoft::Terminal::TaskProvider
{
    namespace
    {
        bool _validIdentifier(const std::string_view value) noexcept
        {
            return !value.empty() && value.size() <= MaximumProviderIdentifierBytes &&
                   value.find('\0') == std::string_view::npos;
        }

        std::optional<std::string> _utf8(const std::wstring_view value)
        {
            if (value.empty()) return std::string{};
            if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return std::nullopt;
            const auto required = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                nullptr, 0, nullptr, nullptr);
            if (required <= 0) return std::nullopt;
            std::string result(static_cast<size_t>(required), '\0');
            if (WideCharToMultiByte(
                    CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                    result.data(), required, nullptr, nullptr) != required)
            {
                return std::nullopt;
            }
            return result;
        }

        TaskProviderResult _invalid(std::string error)
        {
            return { TaskProviderStatus::InvalidRequest, std::move(error) };
        }

        std::optional<std::string> _threadId(const Json::Value& result)
        {
            if (!result.isObject() || !result.isMember("thread") ||
                !result["thread"].isObject() || !result["thread"].isMember("id") ||
                !result["thread"]["id"].isString())
            {
                return std::nullopt;
            }
            auto value = result["thread"]["id"].asString();
            return _validIdentifier(value) ? std::optional<std::string>{ std::move(value) } : std::nullopt;
        }

        std::optional<std::string> _turnId(const Json::Value& result)
        {
            if (!result.isObject() || !result.isMember("turn") ||
                !result["turn"].isObject() || !result["turn"].isMember("id") ||
                !result["turn"]["id"].isString())
            {
                return std::nullopt;
            }
            auto value = result["turn"]["id"].asString();
            return _validIdentifier(value) ? std::optional<std::string>{ std::move(value) } : std::nullopt;
        }

        std::string _approvalKey(
            const std::string_view sessionId,
            const std::string_view runId,
            const std::string_view approvalId)
        {
            std::string key;
            key.reserve(sessionId.size() + runId.size() + approvalId.size() + 2);
            key.append(sessionId).push_back('\n');
            key.append(runId).push_back('\n');
            key.append(approvalId);
            return key;
        }
    }

    bool CodexAppServerTimeouts::Valid() const noexcept
    {
        constexpr uint32_t maximum{ 300'000 };
        return InitializeMilliseconds > 0 && InitializeMilliseconds <= maximum &&
               SessionMilliseconds > 0 && SessionMilliseconds <= maximum &&
               RunMilliseconds > 0 && RunMilliseconds <= maximum &&
               CommandMilliseconds > 0 && CommandMilliseconds <= maximum;
    }

    CodexAppServerAdapter::CodexAppServerAdapter(
        CodexAppServerTransportFactory transportFactory,
        CodexAppServerTimeouts timeouts) :
        _rpc{ std::move(transportFactory) },
        _timeouts{ std::move(timeouts) }
    {
    }

    CodexAppServerAdapter::~CodexAppServerAdapter()
    {
        Stop();
    }

    TaskProviderResult CodexAppServerAdapter::_ensureInitialized(const HANDLE cancellationEvent)
    {
        {
            const std::scoped_lock lock{ _stateMutex };
            if (_stopped) return { TaskProviderStatus::TransportClosed, "Codex adapter was stopped" };
            if (_initialized) return { TaskProviderStatus::AlreadyApplied, {} };
        }
        const std::scoped_lock initialization{ _initializeMutex };
        {
            const std::scoped_lock lock{ _stateMutex };
            if (_stopped) return { TaskProviderStatus::TransportClosed, "Codex adapter was stopped" };
            if (_initialized) return { TaskProviderStatus::AlreadyApplied, {} };
            if (!_timeouts.Valid()) return _invalid("Codex App Server timeouts are invalid");
        }
        const auto started = _rpc.Start();
        if (!started.Succeeded()) return started;
        bool stopped;
        {
            const std::scoped_lock lock{ _stateMutex };
            stopped = _stopped;
        }
        if (stopped)
        {
            _rpc.Stop();
            return { TaskProviderStatus::TransportClosed, "Codex adapter was stopped during launch" };
        }

        Json::Value clientInfo{ Json::objectValue };
        clientInfo["name"] = "richer-terminal-task-runtime";
        clientInfo["title"] = "Richer Terminal Task Runtime";
        clientInfo["version"] = "1.0.0";
        Json::Value capabilities{ Json::objectValue };
        capabilities["experimentalApi"] = true;
        Json::Value params{ Json::objectValue };
        params["clientInfo"] = std::move(clientInfo);
        params["capabilities"] = std::move(capabilities);
        const auto initialized = _rpc.Request(
            "initialize", params, _timeouts.InitializeMilliseconds, cancellationEvent);
        if (!initialized.Succeeded())
        {
            _rpc.Stop();
            return { initialized.Status, initialized.Error };
        }
        auto parsed = ParseCodexAppServerCapabilities(initialized.Value);
        if (!parsed.Succeeded() || !parsed.Value)
        {
            _rpc.Stop();
            return { parsed.Status, parsed.Error };
        }
        const auto notified = _rpc.Notify(
            "initialized", Json::Value{ Json::objectValue },
            _timeouts.InitializeMilliseconds, cancellationEvent);
        if (!notified.Succeeded())
        {
            _rpc.Stop();
            return notified;
        }
        {
            const std::scoped_lock lock{ _stateMutex };
            if (_stopped || !_rpc.Running()) return { TaskProviderStatus::TransportClosed, "Codex initialization was stopped" };
            _capabilities = std::move(parsed.Value.value());
            _initialized = true;
            _nextEventSequence = 1;
            _approvals.clear();
        }
        return { TaskProviderStatus::Succeeded, {} };
    }

    TaskProviderCapabilitiesResult CodexAppServerAdapter::ProbeCapabilities(const HANDLE cancellationEvent)
    {
        TaskProviderCapabilitiesResult result;
        const auto initialized = _ensureInitialized(cancellationEvent);
        if (!initialized.Succeeded())
        {
            result.Status = initialized.Status;
            result.Error = initialized.Error;
            return result;
        }
        const std::scoped_lock lock{ _stateMutex };
        if (!_capabilities)
        {
            result.Status = TaskProviderStatus::ResourceFailure;
            result.Error = "Codex capabilities were not retained";
            return result;
        }
        result.Status = TaskProviderStatus::Succeeded;
        result.Value = _capabilities;
        return result;
    }

    TaskProviderSessionResult CodexAppServerAdapter::StartSession(
        const TaskProviderSessionRequest& request,
        const HANDLE cancellationEvent)
    {
        TaskProviderSessionResult result;
        if (!_validIdentifier(request.TaskId) || request.WorkingDirectory.empty() ||
            !request.WorkingDirectory.is_absolute() || request.Model.size() > MaximumProviderIdentifierBytes)
        {
            result.Status = TaskProviderStatus::InvalidRequest;
            result.Error = "Codex session request is invalid";
            return result;
        }
        const auto cwd = _utf8(request.WorkingDirectory.native());
        if (!cwd)
        {
            result.Status = TaskProviderStatus::InvalidRequest;
            result.Error = "Codex working directory is not valid UTF-8";
            return result;
        }
        const auto initialized = _ensureInitialized(cancellationEvent);
        if (!initialized.Succeeded())
        {
            result.Status = initialized.Status;
            result.Error = initialized.Error;
            return result;
        }
        Json::Value params{ Json::objectValue };
        params["cwd"] = *cwd;
        params["runtimeWorkspaceRoots"] = Json::Value{ Json::arrayValue };
        params["runtimeWorkspaceRoots"].append(*cwd);
        params["threadSource"] = "richer-terminal-task-runtime";
        params["ephemeral"] = false;
        // App Server does not inherit the launcher's --yolo permissions when
        // creating threads. Preserve the Task provider's existing execution mode.
        params["approvalPolicy"] = "never";
        params["sandbox"] = "danger-full-access";
        if (!request.Model.empty()) params["model"] = request.Model;
        if (request.InitialRunContext)
        {
            const auto& context = *request.InitialRunContext;
            const auto prepared = PrepareTaskProviderRunContext(context);
            if (!prepared.Succeeded || context.TaskId != request.TaskId ||
                context.WorkingDirectory != request.WorkingDirectory || context.Model != request.Model)
            {
                result.Status = TaskProviderStatus::InvalidRequest;
                result.Error = "Codex initial Run context is invalid";
                return result;
            }
            Json::Value environment{ Json::objectValue };
            for (const auto& entry : prepared.Environment)
            {
                const auto name = _utf8(entry.Name);
                const auto value = _utf8(entry.Value);
                if (!name || !value)
                {
                    result.Status = TaskProviderStatus::InvalidRequest;
                    result.Error = "Codex initial Run environment is not valid UTF-8";
                    return result;
                }
                environment[*name] = *value;
            }
            params["developerInstructions"] = prepared.Instructions;
            params["config"]["shell_environment_policy"]["inherit"] = "all";
            params["config"]["shell_environment_policy"]["set"] = std::move(environment);
        }
        const auto response = _rpc.Request(
            "thread/start", params, _timeouts.SessionMilliseconds, cancellationEvent);
        if (!response.Succeeded())
        {
            result.Status = response.Status;
            result.Error = response.Error;
            return result;
        }
        const auto id = _threadId(response.Value);
        if (!id)
        {
            result.Status = TaskProviderStatus::MalformedResponse;
            result.Error = "Codex thread/start result is invalid";
            return result;
        }
        result.Status = TaskProviderStatus::Succeeded;
        result.Value = TaskProviderSession{ *id, TaskProviderSessionState::Ready };
        if (request.InitialRunContext)
        {
            const auto lock = std::scoped_lock{ _stateMutex };
            _initialRunBySession.insert_or_assign(*id, request.InitialRunContext->RunId);
        }
        return result;
    }

    TaskProviderSessionResult CodexAppServerAdapter::ResumeSession(
        const std::string_view sessionId,
        const HANDLE cancellationEvent)
    {
        TaskProviderSessionResult result;
        if (!_validIdentifier(sessionId))
        {
            result.Status = TaskProviderStatus::InvalidRequest;
            result.Error = "Codex session identity is invalid";
            return result;
        }
        const auto initialized = _ensureInitialized(cancellationEvent);
        if (!initialized.Succeeded())
        {
            result.Status = initialized.Status;
            result.Error = initialized.Error;
            return result;
        }
        Json::Value params{ Json::objectValue };
        params["threadId"] = std::string{ sessionId };
        params["excludeTurns"] = true;
        auto response = _rpc.Request(
            "thread/resume", params, _timeouts.SessionMilliseconds, cancellationEvent);
        // Recover a process interruption between unloading and restoring the
        // stored thread. The rollout is preserved by the archive operation.
        if (!response.Succeeded() && response.Error.find(" is archived.") != std::string::npos)
        {
            const auto restored = _rpc.Request("thread/unarchive", params, _timeouts.SessionMilliseconds, cancellationEvent);
            if (!restored.Succeeded())
            {
                result.Status = restored.Status;
                result.Error = restored.Error;
                return result;
            }
            response = _rpc.Request("thread/resume", params, _timeouts.SessionMilliseconds, cancellationEvent);
        }
        if (!response.Succeeded())
        {
            result.Status = response.Status;
            result.Error = response.Error;
            return result;
        }
        const auto id = _threadId(response.Value);
        if (!id || *id != sessionId)
        {
            result.Status = TaskProviderStatus::MalformedResponse;
            result.Error = "Codex thread/resume returned a different thread";
            return result;
        }
        result.Status = TaskProviderStatus::Succeeded;
        result.Value = TaskProviderSession{ *id, TaskProviderSessionState::Ready };
        {
            const std::scoped_lock lock{ _stateMutex };
            _pendingUnarchive.erase(*id);
        }
        return result;
    }

    TaskProviderRunResult CodexAppServerAdapter::StartRun(
        const TaskProviderRunRequest& request,
        const HANDLE cancellationEvent)
    {
        TaskProviderRunResult result;
        if (!_validIdentifier(request.SessionId) || request.Prompt.empty() ||
            request.Prompt.size() > MaximumProviderTextBytes || request.Prompt.find('\0') != std::string::npos)
        {
            result.Status = TaskProviderStatus::InvalidRequest;
            result.Error = "Codex Run request is invalid";
            return result;
        }
        auto prepared = PrepareTaskProviderRunContext(request.Context);
        if (!prepared.Succeeded)
        {
            result.Status = TaskProviderStatus::InvalidRequest;
            result.Error = std::move(prepared.Error);
            return result;
        }
        const auto initialized = _ensureInitialized(cancellationEvent);
        if (!initialized.Succeeded())
        {
            result.Status = initialized.Status;
            result.Error = initialized.Error;
            return result;
        }
        const auto cwd = _utf8(request.Context.WorkingDirectory.native());
        if (!cwd)
        {
            result.Status = TaskProviderStatus::InvalidRequest;
            result.Error = "Codex Run directory is not valid UTF-8";
            return result;
        }
        Json::Value environment{ Json::objectValue };
        for (const auto& entry : prepared.Environment)
        {
            const auto name = _utf8(entry.Name);
            const auto value = _utf8(entry.Value);
            if (!name || !value)
            {
                result.Status = TaskProviderStatus::InvalidRequest;
                result.Error = "Codex Run environment is not valid UTF-8";
                return result;
            }
            environment[*name] = *value;
        }
        Json::Value shellEnvironment{ Json::objectValue };
        shellEnvironment["inherit"] = "all";
        shellEnvironment["set"] = std::move(environment);
        Json::Value config{ Json::objectValue };
        config["shell_environment_policy"] = std::move(shellEnvironment);
        Json::Value resume{ Json::objectValue };
        resume["threadId"] = request.SessionId;
        resume["excludeTurns"] = true;
        resume["approvalPolicy"] = "never";
        resume["sandbox"] = "danger-full-access";
        resume["cwd"] = *cwd;
        resume["runtimeWorkspaceRoots"] = Json::Value{ Json::arrayValue };
        resume["runtimeWorkspaceRoots"].append(*cwd);
        resume["developerInstructions"] = prepared.Instructions;
        resume["config"] = std::move(config);
        if (!request.Context.Model.empty()) resume["model"] = request.Context.Model;
        bool initialRun{};
        {
            const auto lock = std::scoped_lock{ _stateMutex };
            const auto initial = _initialRunBySession.find(request.SessionId);
            initialRun = initial != _initialRunBySession.end() && initial->second == request.Context.RunId;
        }
        if (!initialRun)
        {
            // Unsubscribe retains loaded threads for an idle grace period.
            // Unload this thread before applying the next Run's capability.
            const auto unloaded = CloseSessionProjection(request.SessionId, cancellationEvent);
            if (!unloaded.Succeeded())
            {
                result.Status = unloaded.Status;
                result.Error = unloaded.Error;
                return result;
            }
            const auto resumed = _rpc.Request(
                "thread/resume", resume, _timeouts.SessionMilliseconds, cancellationEvent);
            if (!resumed.Succeeded())
            {
                result.Status = resumed.Status;
                result.Error = resumed.Error;
                return result;
            }
            const auto resumedId = _threadId(resumed.Value);
            if (!resumedId || *resumedId != request.SessionId)
            {
                result.Status = TaskProviderStatus::MalformedResponse;
                result.Error = "Codex Run preparation resumed a different thread";
                return result;
            }
        }

        Json::Value input{ Json::objectValue };
        input["type"] = "text";
        input["text"] = request.Prompt;
        Json::Value additional{ Json::objectValue };
        additional["kind"] = "application";
        additional["value"] = prepared.Instructions;
        Json::Value params{ Json::objectValue };
        params["threadId"] = request.SessionId;
        params["input"] = Json::Value{ Json::arrayValue };
        params["input"].append(std::move(input));
        params["additionalContext"] = Json::Value{ Json::objectValue };
        params["additionalContext"]["richer-task-runtime"] = std::move(additional);
        params["cwd"] = *cwd;
        const auto response = _rpc.Request(
            "turn/start", params, _timeouts.RunMilliseconds, cancellationEvent);
        if (!response.Succeeded())
        {
            result.Status = response.Status;
            result.Error = response.Error;
            return result;
        }
        const auto id = _turnId(response.Value);
        if (!id)
        {
            result.Status = TaskProviderStatus::MalformedResponse;
            result.Error = "Codex turn/start result is invalid";
            return result;
        }
        result.Status = TaskProviderStatus::Succeeded;
        result.Value = TaskProviderRun{ request.SessionId, *id, TaskProviderRunState::Running };
        {
            const auto lock = std::scoped_lock{ _stateMutex };
            _initialRunBySession.erase(request.SessionId);
        }
        return result;
    }

    TaskProviderResult CodexAppServerAdapter::SteerRun(
        const std::string_view sessionId,
        const std::string_view runId,
        const std::string_view message,
        const HANDLE cancellationEvent)
    {
        if (!_validIdentifier(sessionId) || !_validIdentifier(runId) || message.empty() ||
            message.size() > MaximumProviderTextBytes || message.find('\0') != std::string_view::npos)
        {
            return _invalid("Codex steering request is invalid");
        }
        Json::Value input{ Json::objectValue };
        input["type"] = "text";
        input["text"] = std::string{ message };
        Json::Value params{ Json::objectValue };
        params["threadId"] = std::string{ sessionId };
        params["expectedTurnId"] = std::string{ runId };
        params["input"] = Json::Value{ Json::arrayValue };
        params["input"].append(std::move(input));
        const auto response = _rpc.Request(
            "turn/steer", params, _timeouts.CommandMilliseconds, cancellationEvent);
        return { response.Status, response.Error };
    }

    TaskProviderResult CodexAppServerAdapter::InterruptRun(
        const std::string_view sessionId,
        const std::string_view runId,
        const HANDLE cancellationEvent)
    {
        if (!_validIdentifier(sessionId) || !_validIdentifier(runId)) return _invalid("Codex interrupt request is invalid");
        Json::Value params{ Json::objectValue };
        params["threadId"] = std::string{ sessionId };
        params["turnId"] = std::string{ runId };
        const auto response = _rpc.Request(
            "turn/interrupt", params, _timeouts.CommandMilliseconds, cancellationEvent);
        return { response.Status, response.Error };
    }

    TaskProviderEventResult CodexAppServerAdapter::NextEvent(
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        TaskProviderEventResult result;
        if (timeoutMilliseconds == 0)
        {
            result.Status = TaskProviderStatus::InvalidRequest;
            result.Error = "Codex event timeout is invalid";
            return result;
        }
        const auto notification = _rpc.NextNotification(timeoutMilliseconds, cancellationEvent);
        if (notification.Status == TaskProviderStatus::Timeout)
        {
            result.Status = TaskProviderStatus::Succeeded;
            result.NoEvent = true;
            return result;
        }
        if (!notification.Succeeded())
        {
            result.Status = notification.Status;
            result.Error = notification.Error;
            return result;
        }
        uint64_t sequence{};
        {
            const std::scoped_lock lock{ _stateMutex };
            if (_nextEventSequence == 0)
            {
                result.Status = TaskProviderStatus::ResourceFailure;
                result.Error = "Codex event sequence is exhausted";
                return result;
            }
            sequence = _nextEventSequence++;
        }
        auto parsed = ParseCodexAppServerEvent(notification.Value, sequence);
        if (!parsed.Succeeded() || !parsed.Event)
        {
            result.Status = parsed.Status;
            result.Error = parsed.Error;
            return result;
        }
        if (parsed.Approval)
        {
            const auto key = _approvalKey(
                parsed.Approval->SessionId,
                parsed.Approval->RunId,
                parsed.Approval->ApprovalId);
            const std::scoped_lock lock{ _stateMutex };
            if (_approvals.size() >= MaximumProviderQueuedEvents)
            {
                result.Status = TaskProviderStatus::OutputLimitExceeded;
                result.Error = "Codex pending approval limit was reached";
                return result;
            }
            _approvals.insert_or_assign(key, std::move(parsed.Approval.value()));
        }
        result.Status = TaskProviderStatus::Succeeded;
        result.Value = std::move(parsed.Event.value());
        return result;
    }

    TaskProviderResult CodexAppServerAdapter::RespondToApproval(
        const std::string_view sessionId,
        const std::string_view runId,
        const std::string_view approvalId,
        const TaskProviderApprovalDecision decision,
        const HANDLE cancellationEvent)
    {
        if (!_validIdentifier(sessionId) || !_validIdentifier(runId) || !_validIdentifier(approvalId))
        {
            return _invalid("Codex approval response identity is invalid");
        }
        const auto key = _approvalKey(sessionId, runId, approvalId);
        CodexPendingApproval approval;
        {
            const std::scoped_lock lock{ _stateMutex };
            const auto found = _approvals.find(key);
            if (found == _approvals.end()) return { TaskProviderStatus::NotFound, "Codex approval request is no longer pending" };
            approval = found->second;
        }
        const auto response = CodexApprovalResponse(approval, decision);
        if (!response.isObject()) return { TaskProviderStatus::ResourceFailure, "Codex approval response could not be encoded" };
        const auto sent = _rpc.Respond(
            approval.RequestId, response, _timeouts.CommandMilliseconds, cancellationEvent);
        if (sent.Succeeded())
        {
            const std::scoped_lock lock{ _stateMutex };
            _approvals.erase(key);
        }
        return sent;
    }

    TaskProviderResult CodexAppServerAdapter::_unsubscribe(
        const std::string_view sessionId,
        const HANDLE cancellationEvent)
    {
        if (!_validIdentifier(sessionId)) return _invalid("Codex unsubscribe identity is invalid");
        Json::Value params{ Json::objectValue };
        params["threadId"] = std::string{ sessionId };
        const auto response = _rpc.Request(
            "thread/unsubscribe", params, _timeouts.CommandMilliseconds, cancellationEvent);
        if (response.Succeeded())
        {
            const auto lock = std::scoped_lock{ _stateMutex };
            _initialRunBySession.erase(std::string{ sessionId });
        }
        return { response.Status, response.Error };
    }

    TaskProviderResult CodexAppServerAdapter::SuspendSession(const std::string_view sessionId, const HANDLE cancellationEvent)
    {
        return _unsubscribe(sessionId, cancellationEvent);
    }

    TaskProviderResult CodexAppServerAdapter::CloseSessionProjection(const std::string_view sessionId, const HANDLE cancellationEvent)
    {
        if (!_validIdentifier(sessionId)) return _invalid("Codex close identity is invalid");
        const std::string id{ sessionId };
        Json::Value params{ Json::objectValue };
        params["threadId"] = id;
        bool restorePending{};
        {
            const std::scoped_lock lock{ _stateMutex };
            restorePending = _pendingUnarchive.contains(id);
        }
        if (!restorePending)
        {
            // Archive shuts down only this thread and its children. Unarchive
            // restores the history without reloading its processes. In contrast,
            // unsubscribe alone can leave Node/MCP cwd handles alive for 30 min.
            auto archived = _rpc.Request("thread/archive", params, _timeouts.SessionMilliseconds, cancellationEvent);
            if (!archived.Succeeded() && archived.Error.find("no rollout found for thread id " + id) != std::string::npos)
            {
                // A fresh thread may not have a rollout yet. A no-section move
                // materializes it without running an Agent turn or a shell command.
                auto materialize = params;
                materialize["sectionId"] = Json::Value{};
                const auto persisted = _rpc.Request("thread/section/move", materialize, _timeouts.SessionMilliseconds, cancellationEvent);
                if (!persisted.Succeeded()) return { persisted.Status, persisted.Error };
                archived = _rpc.Request("thread/archive", params, _timeouts.SessionMilliseconds, cancellationEvent);
            }
            if (!archived.Succeeded()) return { archived.Status, archived.Error };
            const std::scoped_lock lock{ _stateMutex };
            _pendingUnarchive.insert(id);
            _initialRunBySession.erase(id);
        }
        const auto restored = _rpc.Request("thread/unarchive", params, _timeouts.SessionMilliseconds, cancellationEvent);
        if (!restored.Succeeded()) return { restored.Status, restored.Error };
        const auto restoredId = _threadId(restored.Value);
        if (!restoredId || *restoredId != id)
            return { TaskProviderStatus::MalformedResponse, "Codex close restored a different thread" };
        const std::scoped_lock lock{ _stateMutex };
        _pendingUnarchive.erase(id);
        return { TaskProviderStatus::Succeeded, {} };
    }

    TaskProviderFailureDisposition CodexAppServerAdapter::ClassifyFailureAndResumeSafety(
        const TaskProviderStatus status,
        const std::string_view providerCode) const noexcept
    {
        return ClassifyTaskProviderFailure(status, providerCode);
    }

    void CodexAppServerAdapter::Stop() noexcept
    {
        {
            const std::scoped_lock lock{ _stateMutex };
            // Coordinators replace stopped adapters. Late calls on the old
            // instance must never start a second provider after replacement.
            _stopped = true;
        }
        _rpc.Stop();
        const std::scoped_lock lock{ _stateMutex };
        _initialized = false;
        _capabilities.reset();
        _approvals.clear();
        _initialRunBySession.clear();
        _pendingUnarchive.clear();
        _nextEventSequence = 1;
    }

    std::unique_ptr<ITaskProviderAdapter> MakeCodexAppServerAdapter(
        const std::filesystem::path& executable,
        CodexAppServerTimeouts timeouts)
    {
        return std::make_unique<CodexAppServerAdapter>(
            MakeCodexAppServerProcessTransportFactory(executable),
            std::move(timeouts));
    }
}
