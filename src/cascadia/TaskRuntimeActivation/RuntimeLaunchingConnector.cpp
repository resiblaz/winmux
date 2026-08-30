// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeLaunchingConnector.h"

#include "RuntimeActivationDeadline.h"
#include "RuntimeDirectConnectDisposition.h"
#include "RuntimeLaunchingConnectorResults.h"
#include "RuntimeProcessObservation.h"
#include "RuntimeProcessExitCode.h"

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeLaunchingControlConnector::RuntimeLaunchingControlConnector(
        RuntimeLaunchingConnectorConfiguration configuration,
        RuntimeLaunchingConnectorDependencies dependencies) noexcept :
        _configuration{ std::move(configuration) },
        _dependencies{ std::move(dependencies) }
    {
    }

    bool RuntimeLaunchingControlConnector::Valid() const noexcept
    {
        return !_configuration.RuntimeExecutable.empty() &&
               !_configuration.TrustedRoot.empty() &&
               _configuration.RetryPollMilliseconds != 0 &&
               _configuration.RetryPollMilliseconds <= 1000 &&
               _dependencies.DirectConnector && _dependencies.LaunchLocks &&
               _dependencies.ProcessLauncher && _dependencies.Services;
    }

    RuntimeControlConnectionResult RuntimeLaunchingControlConnector::Connect(
        const RuntimeIdentity& identity,
        const Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest& request,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            if (!Valid() || timeoutMilliseconds == 0 ||
                timeoutMilliseconds > MaximumRuntimeTransportTimeoutMilliseconds)
            {
                return details::ActivationFailure(
                    RuntimeTransportStatus::InvalidConfiguration,
                    RuntimeControlConnectFailureReason::InvalidConfiguration,
                    ERROR_INVALID_PARAMETER,
                    "Runtime launching connector configuration is invalid");
            }
            const auto deadline = MakeRuntimeActivationDeadline(
                _dependencies.Services->Now(),
                timeoutMilliseconds);
            if (!deadline)
            {
                return details::ActivationDeadlineFailure();
            }

            auto remaining = details::ActivationRemaining(*_dependencies.Services, *deadline);
            if (remaining == 0)
            {
                return details::ActivationDeadlineFailure();
            }
            auto direct = _dependencies.DirectConnector->Connect(
                identity,
                request,
                remaining,
                cancellationEvent);
            auto disposition = EvaluateRuntimeDirectConnectDisposition(direct);
            if (disposition != RuntimeDirectConnectDisposition::MayLaunch)
            {
                return direct;
            }

            auto lock = _dependencies.LaunchLocks->Acquire(
                identity,
                *deadline,
                cancellationEvent,
                *_dependencies.Services);
            if (!lock.Acquired())
            {
                return details::ActivationLockFailure(lock);
            }

            remaining = details::ActivationRemaining(*_dependencies.Services, *deadline);
            if (remaining == 0)
            {
                return details::ActivationDeadlineFailure();
            }
            direct = _dependencies.DirectConnector->Connect(
                identity,
                request,
                remaining,
                cancellationEvent);
            disposition = EvaluateRuntimeDirectConnectDisposition(direct);
            if (disposition != RuntimeDirectConnectDisposition::MayLaunch)
            {
                return direct;
            }

            RuntimeProcessLaunchRequest launchRequest;
            launchRequest.ExecutablePath = _configuration.RuntimeExecutable;
            launchRequest.TrustedRoot = _configuration.TrustedRoot;
            launchRequest.Identity = identity;
            launchRequest.CancellationEvent = cancellationEvent;
            auto launched = _dependencies.ProcessLauncher->Launch(launchRequest);
            if (!launched.Started())
            {
                return details::ActivationLaunchFailure(launched);
            }
            auto process = std::move(launched.Process);

            for (;;)
            {
                remaining = details::ActivationRemaining(*_dependencies.Services, *deadline);
                if (remaining == 0)
                {
                    return details::ActivationDeadlineFailure();
                }
                direct = _dependencies.DirectConnector->Connect(
                    identity,
                    request,
                    remaining,
                    cancellationEvent);
                disposition = EvaluateRuntimeDirectConnectDisposition(direct);
                if (disposition != RuntimeDirectConnectDisposition::MayLaunch)
                {
                    return direct;
                }

                if (process)
                {
                    if (auto exited = details::ObserveRuntimeProcessExit(*process))
                    {
                        if (exited->LaunchedProcessExitCode == static_cast<DWORD>(
                                RuntimeProcessExitCode::AlreadyRunning))
                        {
                            process->Close();
                            process.reset();
                        }
                        else
                        {
                            return std::move(*exited);
                        }
                    }
                }

                const auto now = _dependencies.Services->Now();
                if (RuntimeActivationDeadlineReached(*deadline, now))
                {
                    return details::ActivationDeadlineFailure();
                }
                const auto sliceValue = _configuration.RetryPollMilliseconds >
                                                deadline->Value - now ?
                                            deadline->Value :
                                            now + _configuration.RetryPollMilliseconds;
                const RuntimeActivationDeadline slice{ sliceValue };
                std::array<HANDLE, 2> handles{};
                size_t count{};
                std::optional<size_t> cancellationIndex;
                std::optional<size_t> processIndex;
                if (cancellationEvent)
                {
                    cancellationIndex = count;
                    handles[count++] = cancellationEvent;
                }
                if (process && process->WaitHandle())
                {
                    processIndex = count;
                    handles[count++] = process->WaitHandle();
                }
                const auto waited = _dependencies.Services->WaitUntil(
                    slice,
                    std::span<const HANDLE>{ handles.data(), count });
                if (waited.Status == RuntimeActivationWaitStatus::Failed ||
                    waited.Status == RuntimeActivationWaitStatus::WakeAbandoned)
                {
                    return details::ActivationFailure(
                        RuntimeTransportStatus::SystemError,
                        RuntimeControlConnectFailureReason::ActivationWaitFailed,
                        waited.SystemError,
                        "Runtime activation progress wait failed");
                }
                if (cancellationIndex && waited.WakeIndex == *cancellationIndex)
                {
                    return details::ActivationFailure(
                        RuntimeTransportStatus::Cancelled,
                        RuntimeControlConnectFailureReason::Cancelled,
                        ERROR_OPERATION_ABORTED,
                        "Runtime activation was cancelled after launch");
                }
                if (processIndex && waited.WakeIndex == *processIndex && process)
                {
                    if (auto exited = details::ObserveRuntimeProcessExit(*process))
                    {
                        if (exited->LaunchedProcessExitCode == static_cast<DWORD>(
                                RuntimeProcessExitCode::AlreadyRunning))
                        {
                            process->Close();
                            process.reset();
                        }
                        else
                        {
                            return std::move(*exited);
                        }
                    }
                }
                if (waited.Status == RuntimeActivationWaitStatus::DeadlineReached &&
                    slice.Value == deadline->Value)
                {
                    return details::ActivationDeadlineFailure();
                }
            }
        }
        catch (...)
        {
            return details::ActivationFailure(
                RuntimeTransportStatus::SystemError,
                RuntimeControlConnectFailureReason::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Runtime launching connector exhausted local resources");
        }
    }

    std::shared_ptr<IRuntimeControlConnector> MakeRuntimeLaunchingControlConnector(
        std::filesystem::path runtimeExecutable,
        std::filesystem::path trustedRoot)
    {
        try
        {
            RuntimeLaunchingConnectorDependencies dependencies;
            dependencies.DirectConnector = std::make_shared<RuntimeNamedPipeControlConnector>(
                RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent);
            dependencies.LaunchLocks = MakeRuntimeLaunchLockProvider();
            dependencies.ProcessLauncher = MakeRuntimeProcessLauncher();
            dependencies.Services = MakeRuntimeActivationWindowsServices();
            auto connector = std::make_shared<RuntimeLaunchingControlConnector>(
                RuntimeLaunchingConnectorConfiguration{
                    std::move(runtimeExecutable),
                    std::move(trustedRoot),
                },
                std::move(dependencies));
            return connector->Valid() ? connector : std::shared_ptr<IRuntimeControlConnector>{};
        }
        catch (...)
        {
            return {};
        }
    }
}
