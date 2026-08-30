// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "RuntimeActivationServices.h"
#include "RuntimeLaunchLockProvider.h"
#include "RuntimeLaunchingConnector.h"
#include "RuntimeProcessLauncher.h"
#include "TaskRuntimeClient.h"

#include "ProtocolCapabilities.h"
#include "ProtocolRpc.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace
{
    constexpr DWORD ParentWaitMilliseconds{ 20000 };

    struct Options
    {
        std::wstring Channel;
        std::filesystem::path SettingsRoot;
        std::filesystem::path RuntimeExecutable;
        std::filesystem::path TrustedRoot;
        std::string ClientInstanceId;
        std::optional<std::wstring> StartEventName;
        std::optional<std::filesystem::path> LaunchRecord;
    };

    struct LaunchRecordState
    {
        std::atomic_size_t Calls{};
        std::mutex Mutex;
        std::optional<RuntimeProcessLaunchStatus> Status;
        std::optional<RuntimeProcessLaunchPhase> Phase;
        std::optional<RuntimeParentJobStrategy> Strategy;
        DWORD SystemError{};
    };

    class RecordingProcessLauncher final : public IRuntimeProcessLauncher
    {
    public:
        RecordingProcessLauncher(
            std::shared_ptr<IRuntimeProcessLauncher> inner,
            std::shared_ptr<LaunchRecordState> state) noexcept :
            _inner{ std::move(inner) },
            _state{ std::move(state) }
        {
        }

        RuntimeProcessLaunchResult Launch(
            const RuntimeProcessLaunchRequest& request) noexcept override
        {
            _state->Calls.fetch_add(1, std::memory_order_relaxed);
            auto result = _inner->Launch(request);
            {
                std::scoped_lock lock{ _state->Mutex };
                _state->Status = result.Status;
                _state->Phase = result.Phase;
                _state->Strategy = result.JobStrategy;
                _state->SystemError = result.SystemError;
            }
            return result;
        }

    private:
        std::shared_ptr<IRuntimeProcessLauncher> _inner;
        std::shared_ptr<LaunchRecordState> _state;
    };

    std::optional<Options> _parse(const int argc, wchar_t* const* const argv)
    {
        if (argc < 11 || argc % 2 == 0)
        {
            return std::nullopt;
        }
        Options options;
        for (int index = 1; index < argc; index += 2)
        {
            const std::wstring_view name{ argv[index] };
            const std::wstring value{ argv[index + 1] };
            if (name == L"--channel")
            {
                options.Channel = value;
            }
            else if (name == L"--settings-root")
            {
                options.SettingsRoot = value;
            }
            else if (name == L"--runtime-executable")
            {
                options.RuntimeExecutable = value;
            }
            else if (name == L"--trusted-root")
            {
                options.TrustedRoot = value;
            }
            else if (name == L"--client-id")
            {
                options.ClientInstanceId.clear();
                options.ClientInstanceId.reserve(value.size());
                for (const auto character : value)
                {
                    if (character > 0x7f)
                    {
                        return std::nullopt;
                    }
                    options.ClientInstanceId.push_back(
                        static_cast<char>(character));
                }
            }
            else if (name == L"--start-event")
            {
                options.StartEventName = value;
            }
            else if (name == L"--launch-record")
            {
                options.LaunchRecord = value;
            }
            else
            {
                return std::nullopt;
            }
        }
        if (options.Channel.empty() || options.SettingsRoot.empty() ||
            options.RuntimeExecutable.empty() || options.TrustedRoot.empty() ||
            options.ClientInstanceId.empty())
        {
            return std::nullopt;
        }
        return options;
    }

    bool _waitForStart(const Options& options) noexcept
    {
        if (!options.StartEventName)
        {
            return true;
        }
        const auto event = OpenEventW(
            SYNCHRONIZE,
            FALSE,
            options.StartEventName->c_str());
        if (!event)
        {
            return false;
        }
        const auto waited = WaitForSingleObject(event, ParentWaitMilliseconds);
        CloseHandle(event);
        return waited == WAIT_OBJECT_0;
    }

    bool _writeLaunchRecord(
        const Options& options,
        const std::shared_ptr<LaunchRecordState>& state) noexcept
    {
        if (!options.LaunchRecord)
        {
            return true;
        }
        try
        {
            std::ofstream output{ *options.LaunchRecord, std::ios::binary | std::ios::trunc };
            if (!output)
            {
                return false;
            }
            std::scoped_lock lock{ state->Mutex };
            output << "calls=" << state->Calls.load(std::memory_order_relaxed) << '\n';
            output << "status=" << (state->Status ? static_cast<int>(*state->Status) : -1) << '\n';
            output << "phase=" << (state->Phase ? static_cast<int>(*state->Phase) : -1) << '\n';
            output << "strategy=" << (state->Strategy ? static_cast<int>(*state->Strategy) : -1) << '\n';
            output << "systemError=" << state->SystemError << '\n';
            return static_cast<bool>(output);
        }
        catch (...)
        {
            return false;
        }
    }

    TaskRuntimeClientConfiguration _configuration(
        RuntimeIdentity identity,
        const Options& options)
    {
        TaskRuntimeClientConfiguration configuration;
        configuration.Identity = std::move(identity);
        configuration.Hello.ClientInstanceId = options.ClientInstanceId;
        configuration.Hello.ClientKind = ProtocolClientKind::TestPeer;
        configuration.Hello.ExpectedRuntimeIdentity =
            configuration.Identity.ProtocolIdentity;
        configuration.Hello.OptionalCapabilities =
            static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle);
        configuration.Activation = TaskRuntimeClientActivationConfiguration{
            options.RuntimeExecutable,
            options.TrustedRoot,
        };
        configuration.ConnectTimeoutMilliseconds = 15000;
        return configuration;
    }

    TaskRuntimeClientDependencies _recordingDependencies(
        const Options& options,
        const std::shared_ptr<LaunchRecordState>& state)
    {
        TaskRuntimeClientDependencies dependencies;
        auto launcher = MakeRuntimeProcessLauncher();
        if (!launcher)
        {
            return dependencies;
        }
        RuntimeLaunchingConnectorDependencies connectorDependencies;
        connectorDependencies.DirectConnector =
            std::make_shared<RuntimeNamedPipeControlConnector>(
                RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent);
        connectorDependencies.LaunchLocks = MakeRuntimeLaunchLockProvider();
        connectorDependencies.ProcessLauncher =
            std::make_shared<RecordingProcessLauncher>(
                std::move(launcher),
                state);
        connectorDependencies.Services = MakeRuntimeActivationWindowsServices();
        dependencies.Connector =
            std::make_shared<RuntimeLaunchingControlConnector>(
                RuntimeLaunchingConnectorConfiguration{
                    options.RuntimeExecutable,
                    options.TrustedRoot,
                },
                std::move(connectorDependencies));
        return dependencies;
    }

    int _run(const Options& options, const std::shared_ptr<LaunchRecordState>& record)
    {
        if (!_waitForStart(options))
        {
            return 3;
        }
        auto identity = BuildCurrentUserRuntimeIdentity(
            options.Channel,
            options.SettingsRoot);
        if (!identity.Succeeded())
        {
            return 4;
        }

        auto configuration = _configuration(std::move(identity.Identity), options);
        auto dependencies = options.LaunchRecord ?
                                _recordingDependencies(options, record) :
                                TaskRuntimeClientDependencies{};
        TaskRuntimeClient client{ std::move(configuration), std::move(dependencies) };
        if (!client.Valid() || !client.Start())
        {
            return 5;
        }

        const auto deadline = GetTickCount64() + ParentWaitMilliseconds;
        while (client.State() != TaskRuntimeClientState::Ready &&
               client.State() != TaskRuntimeClientState::Faulted &&
               GetTickCount64() < deadline)
        {
            Sleep(5);
        }
        int result{};
        if (client.State() != TaskRuntimeClientState::Ready)
        {
            result = 6;
        }
        else
        {
            const auto ping = client.Submit(ControlMethod::Ping);
            if (!ping.Accepted ||
                ping.Completion.wait_for(std::chrono::milliseconds{ 10000 }) !=
                    std::future_status::ready ||
                ping.Completion.get().Outcome != RuntimeClientRequestOutcome::Succeeded)
            {
                result = 7;
            }
        }
        client.RequestStop();
        if (!client.WaitForStopped() && result == 0)
        {
            result = 8;
        }
        return result;
    }
}

int wmain(const int argc, wchar_t* const* const argv)
{
    const auto options = _parse(argc, argv);
    if (!options)
    {
        return 2;
    }
    auto record = std::make_shared<LaunchRecordState>();
    int result{};
    try
    {
        result = _run(*options, record);
    }
    catch (...)
    {
        result = 9;
    }
    if (!_writeLaunchRecord(*options, record) && result == 0)
    {
        result = 10;
    }
    return result;
}
