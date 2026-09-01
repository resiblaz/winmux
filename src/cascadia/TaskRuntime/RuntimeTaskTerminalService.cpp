// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskTerminalService.h"

#include "RuntimeTaskTerminalPipeServer.h"

#include "ProtocolValidation.h"
#include "TaskProviderVtRenderer.h"
#include "TerminalFrame.h"

#include <mutex>
#include <unordered_map>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskProvider;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        constexpr size_t MaximumLiveTaskTerminalSessions{ 256 };
        RuntimeTaskTerminalServiceResult _failure(
            const RuntimeTaskTerminalServiceStatus status,
            std::string error)
        {
            RuntimeTaskTerminalServiceResult result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }

        std::vector<uint8_t> _initialSnapshot(const std::string_view title)
        {
            std::string value{ "\x1b[2J\x1b[H\x1b[1mTask terminal\x1b[0m" };
            if (!title.empty())
            {
                value.append(": ");
                value.append(title);
            }
            value.append("\r\nConnected to the background Task Runtime.\r\n\r\n");
            return { value.begin(), value.end() };
        }
    }

    struct RuntimeTaskTerminalService::Impl
    {
        RuntimeIdentity Identity;
        std::string RuntimeInstanceId;
        RuntimeTaskTerminalInputHandler Input;
        RuntimeTaskTerminalInterruptHandler Interrupt;
        mutable std::mutex Mutex;
        std::unordered_map<std::string, std::shared_ptr<RuntimeTaskTerminalPipeServer>> Entries;
        bool Stopping{};

        [[nodiscard]] std::shared_ptr<RuntimeTaskTerminalPipeServer> Find(
            const std::string_view taskId) const
        {
            const std::scoped_lock lock{ Mutex };
            const auto found = Entries.find(std::string{ taskId });
            return found == Entries.end() ? nullptr : found->second;
        }
    };

    RuntimeTaskTerminalService::RuntimeTaskTerminalService(
        RuntimeIdentity identity,
        std::string runtimeInstanceId,
        RuntimeTaskTerminalInputHandler input,
        RuntimeTaskTerminalInterruptHandler interrupt) noexcept
    {
        try
        {
            _impl = std::make_unique<Impl>();
            _impl->Identity = std::move(identity);
            _impl->RuntimeInstanceId = std::move(runtimeInstanceId);
            _impl->Input = std::move(input);
            _impl->Interrupt = std::move(interrupt);
        }
        catch (...)
        {
        }
    }

    RuntimeTaskTerminalService::~RuntimeTaskTerminalService()
    {
        Stop();
    }

    bool RuntimeTaskTerminalService::Valid() const noexcept
    {
        return _impl && !_impl->Identity.ProtocolIdentity.empty() &&
               IsCanonicalProtocolUuid(_impl->RuntimeInstanceId) &&
               _impl->Input && _impl->Interrupt;
    }

    RuntimeTerminalEndpointResult RuntimeTaskTerminalService::Endpoint(
        const std::string_view terminalSessionId,
        const uint64_t generation) const
    {
        return _impl ? BuildRuntimeTerminalEndpoint(_impl->Identity, terminalSessionId, generation) :
                       RuntimeTerminalEndpointResult{};
    }

    RuntimeTaskTerminalServiceResult RuntimeTaskTerminalService::Open(
        const RuntimeTaskTerminalOpenRequest& request)
    {
        if (!Valid() || !IsCanonicalProtocolUuid(request.TaskId) ||
            !IsCanonicalProtocolUuid(request.TerminalSessionId) ||
            (!request.OwnerClientInstanceId.empty() &&
             !IsCanonicalProtocolUuid(request.OwnerClientInstanceId)) ||
            request.Generation == 0 || request.Title.size() > 1024)
        {
            return _failure(RuntimeTaskTerminalServiceStatus::InvalidRequest, "Task terminal open request is invalid");
        }

        std::shared_ptr<RuntimeTaskTerminalPipeServer> replaced;
        {
            const std::scoped_lock lock{ _impl->Mutex };
            if (_impl->Stopping)
            {
                return _failure(RuntimeTaskTerminalServiceStatus::NotFound, "Task terminal service is stopping");
            }
            if (!_impl->Entries.contains(request.TaskId) &&
                _impl->Entries.size() >= MaximumLiveTaskTerminalSessions)
            {
                return _failure(RuntimeTaskTerminalServiceStatus::ResourceFailure, "Task terminal session capacity was reached");
            }
            if (const auto found = _impl->Entries.find(request.TaskId); found != _impl->Entries.end())
            {
                const auto currentGeneration = found->second->Endpoint().Generation;
                if (currentGeneration > request.Generation)
                {
                    return _failure(RuntimeTaskTerminalServiceStatus::StaleGeneration, "Task terminal generation is stale");
                }
                if (currentGeneration == request.Generation)
                {
                    RuntimeTaskTerminalServiceResult result;
                    result.Endpoint = found->second->Endpoint();
                    result.ClientCount = found->second->ClientCount();
                    result.Status = request.OwnerClientInstanceId.empty() ||
                                            found->second->ClaimSurface(request.OwnerClientInstanceId) ?
                                        RuntimeTaskTerminalServiceStatus::AlreadyOpen :
                                        RuntimeTaskTerminalServiceStatus::OwnedElsewhere;
                    return result;
                }
                replaced = std::move(found->second);
                _impl->Entries.erase(found);
            }
        }
        if (replaced)
            replaced->Stop(false);

        const auto endpoint = Endpoint(request.TerminalSessionId, request.Generation);
        if (!endpoint.Succeeded())
        {
            return _failure(RuntimeTaskTerminalServiceStatus::InvalidRequest, endpoint.Error);
        }
        try
        {
            auto backend = std::make_shared<RuntimeTaskTerminalBackend>(
                request.TaskId, _initialSnapshot(request.Title), _impl->Input, _impl->Interrupt);
            auto server = std::make_shared<RuntimeTaskTerminalPipeServer>(
                _impl->Identity, _impl->RuntimeInstanceId, request.TaskId, endpoint.Endpoint, std::move(backend));
            if (!server->Valid() ||
                (!request.OwnerClientInstanceId.empty() &&
                 !server->ClaimSurface(request.OwnerClientInstanceId)) ||
                !server->Start())
            {
                server->Stop(false);
                return _failure(RuntimeTaskTerminalServiceStatus::ResourceFailure, "Task terminal listener could not start");
            }
            {
                const std::scoped_lock lock{ _impl->Mutex };
                if (_impl->Stopping || !_impl->Entries.emplace(request.TaskId, server).second)
                {
                    server->Stop(false);
                    return _failure(RuntimeTaskTerminalServiceStatus::ResourceFailure, "Task terminal changed while opening");
                }
            }
            RuntimeTaskTerminalServiceResult result;
            result.Status = RuntimeTaskTerminalServiceStatus::Succeeded;
            result.Endpoint = server->Endpoint();
            return result;
        }
        catch (...)
        {
            return _failure(RuntimeTaskTerminalServiceStatus::ResourceFailure, "Task terminal allocation failed");
        }
    }

    RuntimeTaskTerminalServiceResult RuntimeTaskTerminalService::PublishEvent(
        const std::string_view taskId,
        const TaskProviderEvent& event)
    {
        const auto rendered = RenderTaskProviderEventAsVt(event);
        if (!rendered.Succeeded)
        {
            return _failure(RuntimeTaskTerminalServiceStatus::InvalidRequest, rendered.Error);
        }
        if (rendered.Bytes.empty())
        {
            RuntimeTaskTerminalServiceResult result;
            result.Status = RuntimeTaskTerminalServiceStatus::Succeeded;
            result.ClientCount = ClientCount(taskId);
            return result;
        }
        return PublishText(
            taskId,
            std::span<const uint8_t>{
                reinterpret_cast<const uint8_t*>(rendered.Bytes.data()), rendered.Bytes.size() });
    }

    RuntimeTaskTerminalServiceResult RuntimeTaskTerminalService::PublishText(
        const std::string_view taskId,
        const std::span<const uint8_t> output,
        const uint32_t flags)
    {
        const auto server = _impl ? _impl->Find(taskId) : nullptr;
        if (!server)
        {
            return _failure(RuntimeTaskTerminalServiceStatus::NotFound, "Task terminal is not open");
        }
        const auto published = server->Publish(output, flags);
        if (!published.Succeeded)
        {
            return _failure(RuntimeTaskTerminalServiceStatus::ResourceFailure, published.Error);
        }
        RuntimeTaskTerminalServiceResult result;
        result.Status = RuntimeTaskTerminalServiceStatus::Succeeded;
        result.Endpoint = server->Endpoint();
        result.ClientCount = published.ClientCount;
        return result;
    }

    RuntimeTaskTerminalServiceResult RuntimeTaskTerminalService::Close(
        const std::string_view taskId,
        const uint64_t generation,
        const bool graceful)
    {
        if (!_impl || !IsCanonicalProtocolUuid(taskId) || generation == 0)
        {
            return _failure(RuntimeTaskTerminalServiceStatus::InvalidRequest, "Task terminal close request is invalid");
        }
        std::shared_ptr<RuntimeTaskTerminalPipeServer> server;
        {
            const std::scoped_lock lock{ _impl->Mutex };
            const auto found = _impl->Entries.find(std::string{ taskId });
            if (found == _impl->Entries.end())
            {
                return _failure(RuntimeTaskTerminalServiceStatus::NotFound, "Task terminal is not open");
            }
            if (found->second->Endpoint().Generation != generation)
            {
                return _failure(RuntimeTaskTerminalServiceStatus::StaleGeneration, "Task terminal close generation is stale");
            }
            server = std::move(found->second);
            _impl->Entries.erase(found);
        }
        RuntimeTaskTerminalServiceResult result;
        result.Status = RuntimeTaskTerminalServiceStatus::Succeeded;
        result.Endpoint = server->Endpoint();
        result.ClientCount = server->ClientCount();
        server->Stop(graceful);
        return result;
    }

    size_t RuntimeTaskTerminalService::ClientCount(const std::string_view taskId) const noexcept
    {
        const auto server = _impl ? _impl->Find(taskId) : nullptr;
        return server ? server->ClientCount() : 0;
    }

    size_t RuntimeTaskTerminalService::SessionCount() const noexcept
    {
        if (!_impl)
            return 0;
        const std::scoped_lock lock{ _impl->Mutex };
        return _impl->Entries.size();
    }

    void RuntimeTaskTerminalService::Stop() noexcept
    {
        if (!_impl)
            return;
        std::vector<std::shared_ptr<RuntimeTaskTerminalPipeServer>> servers;
        try
        {
            {
                const std::scoped_lock lock{ _impl->Mutex };
                if (_impl->Stopping)
                    return;
                _impl->Stopping = true;
                servers.reserve(_impl->Entries.size());
                for (auto& [_, server] : _impl->Entries)
                    servers.emplace_back(std::move(server));
                _impl->Entries.clear();
            }
            for (const auto& server : servers)
                server->Stop(false);
        }
        catch (...)
        {
        }
    }
}
