// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeAgentCatalogService.h"
#include "RuntimeProjectCatalogService.h"
#include "RuntimeTaskBoardService.h"
#include "RuntimeProductHostInternal.h"
#include "RuntimeProductExecutableDiscovery.h"

#include "RuntimeCrypto.h"
#include "RuntimeFoundationRouter.h"
#include "RuntimeCodexDiscovery.h"
#include "RuntimeRtaskCapabilityRegistry.h"
#include "RuntimeRtaskCapabilityToken.h"
#include "RuntimeTaskProviderService.h"
#include "RuntimeTaskProviderSink.h"
#include "RuntimeTaskProjectionService.h"
#include "RuntimeTaskTerminalCallbackBridge.h"
#include "RuntimeTaskTerminalService.h"
#include "RuntimeTaskOrchestrator.h"

#include "CodexAppServerAdapter.h"

#include "SqliteTaskStore.h"

#include "ProtocolCapabilities.h"

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
                static_cast<uint64_t>(ProtocolCapability::TaskQueries) |
                static_cast<uint64_t>(ProtocolCapability::TaskCommands) |
                static_cast<uint64_t>(ProtocolCapability::TerminalStream) |
                static_cast<uint64_t>(ProtocolCapability::InputControllerLease) |
                static_cast<uint64_t>(ProtocolCapability::TaskTerminalSurfaces) |
                static_cast<uint64_t>(ProtocolCapability::TaskOrchestration);
            handshake.Capabilities |= static_cast<uint64_t>(ProtocolCapability::RuntimeDrain) |
                                      static_cast<uint64_t>(ProtocolCapability::AgentProfiles);
            wchar_t projectMode[2]{};
            if (GetEnvironmentVariableW(L"RICHER_WORKSPACE_PROJECTS", projectMode, 2) != 1 || projectMode[0] != L'0')
                handshake.Capabilities |= static_cast<uint64_t>(ProtocolCapability::ProjectCatalog) | static_cast<uint64_t>(ProtocolCapability::TaskBoard);
            return handshake;
        }
    }

    RuntimeProductHost::Impl::Impl(
        RuntimeBootstrapResult bootstrap,
        std::string instanceId,
        std::shared_ptr<RuntimeProductRouter> router,
        std::shared_ptr<Microsoft::Terminal::TaskCore::ITaskStore> store,
        std::shared_ptr<RuntimeTaskProviderService> provider,
        std::shared_ptr<RuntimeTaskTerminalService> terminals,
        std::shared_ptr<RuntimeTaskOrchestrator> orchestrator,
        std::shared_ptr<std::atomic_bool> drainRequested,
        RuntimeProductHostIdleConfiguration idleConfiguration) :
        Bootstrap{ std::move(bootstrap) },
        InstanceId{ std::move(instanceId) },
        Router{ std::move(router) },
        Store{ std::move(store) },
        Provider{ std::move(provider) },
        Terminals{ std::move(terminals) },
        Orchestrator{ std::move(orchestrator) },
        DrainRequested{ std::move(drainRequested) },
        IdleConfiguration{ std::move(idleConfiguration) }
    {
    }

    void RuntimeProductHost::Impl::Finalize() noexcept
    {
        StopIdleMonitor();
        {
            std::scoped_lock lock{ LifecycleMutex };
            Server.Stop();
            if (Orchestrator)
            {
                Orchestrator->Stop();
            }
            if (Provider)
            {
                Provider->Stop();
            }
            if (Terminals)
            {
                Terminals->Stop();
            }
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
            auto drainRequested = std::make_shared<std::atomic_bool>(false);
            auto foundation = std::make_shared<RuntimeFoundationRouter>(
                bootstrap.Identity.ProtocolIdentity,
                *instanceId,
                GetCurrentProcessId(),
                [drainRequested] {
                    drainRequested->store(true, std::memory_order_release);
                    return true;
                });
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
            auto terminalCallbacks = std::make_shared<RuntimeTaskTerminalCallbackBridge>(store);
            auto terminals = std::make_shared<RuntimeTaskTerminalService>(
                bootstrap.Identity,
                *instanceId,
                [terminalCallbacks](const std::string_view taskId, const std::span<const uint8_t> input) {
                    return terminalCallbacks->Input(taskId, input);
                },
                [terminalCallbacks](const std::string_view taskId) {
                    return terminalCallbacks->Interrupt(taskId);
                });
            if (!terminals->Valid())
            {
                result.Status = RuntimeProductHostStartStatus::TaskStoreFailure;
                result.Error = "Runtime terminal service could not initialize";
                return result;
            }
            auto providerSink = std::make_shared<RuntimeTaskProviderSink>(store, terminals);
            auto provider = std::make_shared<RuntimeTaskProviderService>(
                [] {
                    const auto executable = DiscoverCodexExecutable();
                    return Microsoft::Terminal::TaskProvider::MakeCodexAppServerAdapter(
                        executable.value_or(std::filesystem::path{}));
                },
                std::move(providerSink));
            if (!provider->Valid())
            {
                result.Status = RuntimeProductHostStartStatus::TaskStoreFailure;
                result.Error = "Runtime provider service could not initialize";
                return result;
            }
            terminalCallbacks->SetProvider(provider);
            auto orchestrator = std::make_shared<RuntimeTaskOrchestrator>(
                store,
                provider,
                rtask,
                terminals,
                bootstrap.Identity.SettingsRoot,
                FindGitExecutable(),
                FindRuntimeProductSibling(L"rtask.exe"),
                NarrowRuntimeAscii(bootstrap.Identity.ControlPipeName));
            if (!orchestrator->Valid())
            {
                result.Status = RuntimeProductHostStartStatus::TaskStoreFailure;
                result.Error = "Runtime Task orchestrator could not initialize";
                return result;
            }
            terminalCallbacks->SetOrchestrator(orchestrator);
            auto router = std::make_shared<RuntimeProductRouter>(
                std::move(foundation),
                rtask,
                provider,
                std::make_shared<RuntimeTaskProjectionService>(
                    store,
                    provider,
                    bootstrap.Identity.SettingsRoot,
                    terminals,
                    orchestrator),
                std::make_shared<RuntimeAgentCatalogService>(store),
                std::make_shared<RuntimeProjectCatalogService>(store, bootstrap.Identity.SettingsRoot),
                std::make_shared<RuntimeTaskBoardService>(store, orchestrator));
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
                std::move(provider),
                std::move(terminals),
                std::move(orchestrator),
                std::move(drainRequested),
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
            if (!impl->Orchestrator->Start())
            {
                result.Status = RuntimeProductHostStartStatus::IdleMonitorFailure;
                result.Error = "Runtime Task orchestrator failed to start";
                return result;
            }
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
