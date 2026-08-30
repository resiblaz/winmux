// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProductHostInternal.h"

#include "RuntimeCrypto.h"
#include "RuntimeFoundationRouter.h"
#include "RuntimeRtaskCapabilityRegistry.h"
#include "RuntimeRtaskCapabilityToken.h"

#include "SqliteTaskStore.h"

#include "ProtocolCapabilities.h"
#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        RuntimeHandshakeConfiguration _foundationHandshake(
            const RuntimeIdentity& identity,
            const std::string& runtimeInstanceId)
        {
            RuntimeHandshakeConfiguration handshake;
            handshake.RuntimeIdentity = identity.ProtocolIdentity;
            handshake.RuntimeInstanceId = runtimeInstanceId;
            handshake.Capabilities =
                static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle) |
                static_cast<uint64_t>(ProtocolCapability::TaskCommands);
            return handshake;
        }
    }

    RuntimeProductHost::Impl::Impl(
        RuntimeBootstrapResult bootstrap,
        std::string instanceId,
        std::shared_ptr<RuntimeProductRouter> router,
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
        RuntimeProductHostIdleConfiguration idleConfiguration) :
        Bootstrap{ std::move(bootstrap) },
        InstanceId{ std::move(instanceId) },
        Router{ std::move(router) },
        Store{ std::move(store) },
        IdleConfiguration{ std::move(idleConfiguration) }
    {
    }

    void RuntimeProductHost::Impl::Finalize() noexcept
    {
        StopIdleMonitor();
        {
            std::scoped_lock lock{ LifecycleMutex };
            Server.Stop();
            // A callback-thread Stop is intentionally non-blocking. Retain
            // single-instance ownership until an external owner observes that
            // the acceptor and every client session have actually stopped.
            if (const auto stopped = Server.StoppedHandle();
                stopped && WaitForSingleObject(stopped, 0) == WAIT_OBJECT_0)
            {
                Bootstrap.Instance = {};
            }
        }
    }

    RuntimeProductHost::RuntimeProductHost() noexcept = default;
    RuntimeProductHost::~RuntimeProductHost()
    {
        Stop();
    }

    RuntimeProductHost::RuntimeProductHost(RuntimeProductHost&&) noexcept = default;
    RuntimeProductHost& RuntimeProductHost::operator=(RuntimeProductHost&& other) noexcept
    {
        if (this != &other)
        {
            Stop();
            _impl = std::move(other._impl);
        }
        return *this;
    }

    RuntimeProductHost::RuntimeProductHost(std::unique_ptr<Impl> impl) noexcept :
        _impl{ std::move(impl) }
    {
    }

    RuntimeProductHost::operator bool() const noexcept
    {
        return static_cast<bool>(_impl);
    }

    bool RuntimeProductHost::Running() const noexcept
    {
        return _impl && _impl->Server.Running();
    }

    RuntimeProductHostTerminalStatus RuntimeProductHost::TerminalStatus() const noexcept
    {
        if (!_impl)
        {
            return RuntimeProductHostTerminalStatus::NotStarted;
        }
        switch (_impl->Server.TerminalStatus())
        {
        case RuntimeControlServerTerminalStatus::Running:
            return RuntimeProductHostTerminalStatus::Running;
        case RuntimeControlServerTerminalStatus::Stopped:
            return RuntimeProductHostTerminalStatus::Stopped;
        case RuntimeControlServerTerminalStatus::TransportFailure:
            return RuntimeProductHostTerminalStatus::TransportFailure;
        case RuntimeControlServerTerminalStatus::InternalFailure:
            return RuntimeProductHostTerminalStatus::InternalFailure;
        default:
            return RuntimeProductHostTerminalStatus::NotStarted;
        }
    }

    std::string RuntimeProductHost::TerminalError() const
    {
        switch (TerminalStatus())
        {
        case RuntimeProductHostTerminalStatus::TransportFailure:
            return "Runtime control server stopped after a transport failure";
        case RuntimeProductHostTerminalStatus::InternalFailure:
            return "Runtime control server stopped after an internal failure";
        default:
            return {};
        }
    }

    const RuntimeIdentity* RuntimeProductHost::Identity() const noexcept
    {
        return _impl ? &_impl->Bootstrap.Identity : nullptr;
    }

    std::string_view RuntimeProductHost::RuntimeInstanceId() const noexcept
    {
        return _impl ? std::string_view{ _impl->InstanceId } : std::string_view{};
    }

    HANDLE RuntimeProductHost::StoppedHandle() const noexcept
    {
        return _impl ? _impl->Server.StoppedHandle() : nullptr;
    }

    RuntimeProductHostWaitResult RuntimeProductHost::Wait(
        const uint32_t timeoutMilliseconds) noexcept
    {
        const auto stopped = StoppedHandle();
        if (!stopped)
        {
            return {
                RuntimeProductHostWaitStatus::Failed,
                ERROR_INVALID_HANDLE,
            };
        }
        const auto waited = WaitForSingleObject(stopped, timeoutMilliseconds);
        if (waited == WAIT_OBJECT_0)
        {
            _impl->Finalize();
            return { RuntimeProductHostWaitStatus::Stopped, ERROR_SUCCESS };
        }
        if (waited == WAIT_TIMEOUT)
        {
            return { RuntimeProductHostWaitStatus::Timeout, ERROR_SUCCESS };
        }
        return {
            RuntimeProductHostWaitStatus::Failed,
            waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_FUNCTION,
        };
    }

    void RuntimeProductHost::Stop() noexcept
    {
        if (_impl)
        {
            _impl->Finalize();
        }
    }

    RuntimeProductHostStartResult StartRuntimeProductHost(
        const std::wstring_view channel,
        const std::filesystem::path& settingsRoot,
        RuntimeControlServerConfiguration configuration,
        RuntimeProductHostIdleConfiguration idleConfiguration)
    {
        RuntimeProductHostStartResult result;
        try
        {
            if (!idleConfiguration.Valid())
            {
                result.Status = RuntimeProductHostStartStatus::IdleMonitorFailure;
                result.Error = "Runtime helper idle configuration is invalid";
                return result;
            }
            auto bootstrap = BootstrapCurrentUserRuntime(channel, settingsRoot);
            result.BootstrapStatus = bootstrap.Status;
            if (!bootstrap.Ready())
            {
                result.Status = bootstrap.Status == RuntimeBootstrapStatus::AlreadyRunning ?
                                    RuntimeProductHostStartStatus::AlreadyRunning :
                                    RuntimeProductHostStartStatus::BootstrapFailure;
                result.Error = std::move(bootstrap.Error);
                return result;
            }

            auto instanceId = NewRuntimeUuid();
            if (!instanceId)
            {
                result.Status = RuntimeProductHostStartStatus::RuntimeInstanceIdFailure;
                result.Error = "Runtime instance ID generation failed";
                return result;
            }
            auto foundation = std::make_shared<RuntimeFoundationRouter>(
                bootstrap.Identity.ProtocolIdentity,
                *instanceId,
                GetCurrentProcessId());
            if (!foundation->Valid())
            {
                result.Status = RuntimeProductHostStartStatus::RuntimeInstanceIdFailure;
                result.Error = "Runtime foundation identity is invalid";
                return result;
            }
            auto store = std::make_shared<Microsoft::Terminal::TaskCore::SqliteTaskStore>(
                bootstrap.Identity.SettingsRoot / L"workspace-tasks.db");
            if (!store->IsReady())
            {
                result.Status = RuntimeProductHostStartStatus::TaskStoreFailure;
                result.Error = store->LastError();
                return result;
            }
            auto tokenSource =
                std::make_shared<SystemRuntimeRtaskCapabilityTokenSource>();
            auto capabilities = std::make_shared<RuntimeRtaskCapabilityRegistry>(
                RuntimeRtaskCapabilityRegistryConfiguration{},
                std::move(tokenSource));
            auto rtask = std::make_shared<RuntimeRtaskCommandService>(
                store,
                std::move(capabilities));
            auto router = std::make_shared<RuntimeProductRouter>(
                std::move(foundation),
                std::move(rtask));
            if (!router->Valid())
            {
                result.Status = RuntimeProductHostStartStatus::TaskStoreFailure;
                result.Error = "Runtime rtask command service is invalid";
                return result;
            }

            auto impl = std::make_unique<RuntimeProductHost::Impl>(
                std::move(bootstrap),
                std::move(*instanceId),
                std::move(router),
                std::move(store),
                std::move(idleConfiguration));
            configuration.Handshake = _foundationHandshake(
                impl->Bootstrap.Identity,
                impl->InstanceId);
            auto server = StartRuntimeControlServer(
                impl->Bootstrap.Identity,
                std::move(impl->Bootstrap.Security),
                std::move(impl->Bootstrap.ControlPipe),
                std::move(configuration),
                impl->Router);
            result.ServerStatus = server.Status;
            if (!server.Started())
            {
                result.Status = RuntimeProductHostStartStatus::ServerStartFailure;
                result.Error = std::move(server.Error);
                return result;
            }
            impl->Server = std::move(server.Server);
            if (!impl->StartIdleMonitor())
            {
                result.Status = RuntimeProductHostStartStatus::IdleMonitorFailure;
                result.Error = "Runtime helper idle monitor failed to start";
                return result;
            }
            result.Host = RuntimeProductHost{ std::move(impl) };
            result.Status = RuntimeProductHostStartStatus::Started;
            result.Error.clear();
            return result;
        }
        catch (const std::exception& exception)
        {
            result.Status = RuntimeProductHostStartStatus::ResourceFailure;
            result.Error = exception.what();
            return result;
        }
        catch (...)
        {
            result.Status = RuntimeProductHostStartStatus::ResourceFailure;
            result.Error = "Unknown Runtime product host failure";
            return result;
        }
    }
}
