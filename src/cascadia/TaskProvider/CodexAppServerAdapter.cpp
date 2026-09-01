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
            if (_initialized) return { TaskProviderStatus::AlreadyApplied, {} };
            if (!_timeouts.Valid()) return _invalid("Codex App Server timeouts are invalid");
        }
        const auto started = _rpc.Start();
        if (!started.Succeeded()) return started;

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
        if (!request.Model.empty()) params["model"] = request.Model;
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
        const auto response = _rpc.Request(
            "thread/resume", params, _timeouts.SessionMilliseconds, cancellationEvent);
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
        resume["cwd"] = *cwd;
        resume["runtimeWorkspaceRoots"] = Json::Value{ Json::arrayValue };
        resume["runtimeWorkspaceRoots"].append(*cwd);
        resume["developerInstructions"] = prepared.Instructions;
        resume["config"] = std::move(config);
        if (!request.Context.Model.empty()) resume["model"] = request.Context.Model;
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
        return { response.Status, response.Error };
    }

    TaskProviderResult CodexAppServerAdapter::SuspendSession(const std::string_view sessionId, const HANDLE cancellationEvent)
    {
        return _unsubscribe(sessionId, cancellationEvent);
    }

    TaskProviderResult CodexAppServerAdapter::CloseSessionProjection(const std::string_view sessionId, const HANDLE cancellationEvent)
    {
        return _unsubscribe(sessionId, cancellationEvent);
    }

    TaskProviderFailureDisposition CodexAppServerAdapter::ClassifyFailureAndResumeSafety(
        const TaskProviderStatus status,
        const std::string_view providerCode) const noexcept
    {
        return ClassifyTaskProviderFailure(status, providerCode);
    }

    void CodexAppServerAdapter::Stop() noexcept
    {
        _rpc.Stop();
        const std::scoped_lock lock{ _stateMutex };
        _initialized = false;
        _capabilities.reset();
        _approvals.clear();
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
