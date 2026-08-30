// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeControlChannel.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        struct HandleOwner
        {
            HANDLE Value{ INVALID_HANDLE_VALUE };

            ~HandleOwner()
            {
                if (Value != INVALID_HANDLE_VALUE && Value != nullptr)
                {
                    CloseHandle(Value);
                }
            }

            [[nodiscard]] HANDLE Release() noexcept
            {
                return std::exchange(Value, INVALID_HANDLE_VALUE);
            }
        };

        RuntimeTransportResult _failure(
            const RuntimeTransportStatus status,
            std::string error,
            const DWORD systemError = ERROR_SUCCESS)
        {
            RuntimeTransportResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        struct RuntimeEndpointWaitResult : RuntimeTransportResult
        {
            RuntimeControlEndpointObservation Observation{
                RuntimeControlEndpointObservation::Unknown
            };
        };

        RuntimeEndpointWaitResult _endpointFailure(
            const RuntimeTransportStatus status,
            const RuntimeControlEndpointObservation observation,
            std::string error,
            const DWORD systemError = ERROR_SUCCESS)
        {
            RuntimeEndpointWaitResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            result.Observation = observation;
            return result;
        }

        RuntimeTransportStatus _classifyIoError(const DWORD error) noexcept
        {
            if (error == ERROR_OPERATION_ABORTED)
            {
                return RuntimeTransportStatus::Cancelled;
            }
            return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                           error == ERROR_NO_DATA ?
                       RuntimeTransportStatus::Disconnected :
                       RuntimeTransportStatus::SystemError;
        }

        std::optional<RuntimeTransportResult> _cancellationFailure(
            const HANDLE cancellationEvent)
        {
            if (!cancellationEvent)
            {
                return std::nullopt;
            }
            const auto wait = WaitForSingleObject(cancellationEvent, 0);
            if (wait == WAIT_TIMEOUT)
            {
                return std::nullopt;
            }
            if (wait == WAIT_OBJECT_0)
            {
                return _failure(
                    RuntimeTransportStatus::Cancelled,
                    "Runtime control pipe wait was cancelled",
                    ERROR_OPERATION_ABORTED);
            }
            return _failure(
                RuntimeTransportStatus::SystemError,
                "Runtime control cancellation wait failed",
                GetLastError());
        }

        bool _validTimeout(const uint32_t timeoutMilliseconds) noexcept
        {
            return timeoutMilliseconds != 0 &&
                   timeoutMilliseconds <= MaximumRuntimeTransportTimeoutMilliseconds;
        }

        bool _validDiscoveryPolicy(
            const RuntimeControlEndpointDiscoveryPolicy policy) noexcept
        {
            return policy == RuntimeControlEndpointDiscoveryPolicy::WaitUntilDeadline ||
                   policy == RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent;
        }

        uint32_t _remaining(const uint64_t deadline) noexcept
        {
            const auto now = GetTickCount64();
            return now >= deadline ? 0 : static_cast<uint32_t>(deadline - now);
        }

        RuntimeTransportResult _waitForRetry(
            const uint64_t deadline,
            const HANDLE cancellationEvent)
        {
            constexpr uint32_t waitSliceMilliseconds{ 50 };
            const auto remaining = _remaining(deadline);
            if (remaining == 0)
            {
                return _failure(RuntimeTransportStatus::Timeout, "Runtime control pipe wait timed out", WAIT_TIMEOUT);
            }
            const auto slice = std::min(remaining, waitSliceMilliseconds);
            if (!cancellationEvent)
            {
                Sleep(slice);
            }
            else
            {
                const auto wait = WaitForSingleObject(cancellationEvent, slice);
                if (wait == WAIT_OBJECT_0)
                {
                    return _failure(
                        RuntimeTransportStatus::Cancelled,
                        "Runtime control pipe wait was cancelled",
                        ERROR_OPERATION_ABORTED);
                }
                if (wait != WAIT_TIMEOUT)
                {
                    return _failure(
                        RuntimeTransportStatus::SystemError,
                        "Runtime control cancellation wait failed",
                        GetLastError());
                }
            }
            RuntimeTransportResult result;
            result.Status = RuntimeTransportStatus::Succeeded;
            return result;
        }

        RuntimeEndpointWaitResult _waitForPipe(
            const std::wstring& pipeName,
            const uint64_t deadline,
            const HANDLE cancellationEvent,
            const RuntimeControlEndpointDiscoveryPolicy discoveryPolicy)
        {
            constexpr uint32_t waitSliceMilliseconds{ 50 };
            auto observation = RuntimeControlEndpointObservation::Unknown;
            while (true)
            {
                if (const auto cancelled = _cancellationFailure(cancellationEvent))
                {
                    return _endpointFailure(
                        cancelled->Status,
                        observation,
                        cancelled->Error,
                        cancelled->SystemError);
                }
                const auto remaining = _remaining(deadline);
                if (remaining == 0)
                {
                    return _endpointFailure(
                        RuntimeTransportStatus::Timeout,
                        observation,
                        "Runtime control pipe wait timed out",
                        WAIT_TIMEOUT);
                }
                if (WaitNamedPipeW(pipeName.c_str(), std::min(remaining, waitSliceMilliseconds)))
                {
                    RuntimeEndpointWaitResult result;
                    result.Status = RuntimeTransportStatus::Succeeded;
                    result.Observation = RuntimeControlEndpointObservation::Available;
                    return result;
                }

                const auto error = GetLastError();
                if (error == ERROR_SEM_TIMEOUT)
                {
                    observation = RuntimeControlEndpointObservation::Busy;
                    continue;
                }
                if (error == ERROR_FILE_NOT_FOUND)
                {
                    const auto neverObserved =
                        observation == RuntimeControlEndpointObservation::Unknown;
                    if (neverObserved)
                    {
                        observation = RuntimeControlEndpointObservation::Absent;
                    }
                    if (neverObserved && discoveryPolicy ==
                        RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent)
                    {
                        return _endpointFailure(
                            RuntimeTransportStatus::Disconnected,
                            observation,
                            "Runtime control endpoint is absent",
                            error);
                    }
                    const auto waited = _waitForRetry(deadline, cancellationEvent);
                    if (waited.Succeeded())
                    {
                        continue;
                    }
                    return _endpointFailure(
                        waited.Status,
                        observation,
                        waited.Error,
                        waited.SystemError);
                }
                return _endpointFailure(
                    _classifyIoError(error),
                    observation,
                    "WaitNamedPipe failed",
                    error);
            }
        }
    }

    RuntimeControlChannelResult ConnectRuntimeControlChannel(
        const RuntimeIdentity& identity,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        return ConnectRuntimeControlChannel(
            identity,
            timeoutMilliseconds,
            cancellationEvent,
            RuntimeControlEndpointDiscoveryPolicy::WaitUntilDeadline);
    }

    RuntimeControlChannelResult ConnectRuntimeControlChannel(
        const RuntimeIdentity& identity,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent,
        const RuntimeControlEndpointDiscoveryPolicy discoveryPolicy)
    {
        RuntimeControlChannelResult result;
        if (identity.ControlPipeName.empty() || !_validTimeout(timeoutMilliseconds) ||
            !_validDiscoveryPolicy(discoveryPolicy))
        {
            result.Status = RuntimeTransportStatus::InvalidConfiguration;
            result.Error = "Runtime identity or timeout is invalid";
            return result;
        }
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        while (true)
        {
            const auto waited = _waitForPipe(
                identity.ControlPipeName,
                deadline,
                cancellationEvent,
                discoveryPolicy);
            result.EndpointObservation = waited.Observation;
            if (!waited.Succeeded())
            {
                static_cast<RuntimeTransportResult&>(result) = waited;
                return result;
            }

            HandleOwner pipe{ CreateFileW(
                identity.ControlPipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                nullptr) };
            if (pipe.Value == INVALID_HANDLE_VALUE)
            {
                const auto error = GetLastError();
                if (error == ERROR_PIPE_BUSY)
                {
                    result.EndpointObservation = RuntimeControlEndpointObservation::Busy;
                    continue;
                }
                if (error == ERROR_FILE_NOT_FOUND)
                {
                    if (discoveryPolicy ==
                        RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent)
                    {
                        result.Status = RuntimeTransportStatus::Disconnected;
                        result.SystemError = error;
                        result.Error = "Observed Runtime control endpoint disappeared before connect";
                        return result;
                    }
                    continue;
                }
                result.Status = _classifyIoError(error);
                result.SystemError = error;
                result.Error = "CreateFile for control pipe failed";
                return result;
            }
            DWORD mode{ PIPE_READMODE_BYTE };
            if (!SetNamedPipeHandleState(pipe.Value, &mode, nullptr, nullptr))
            {
                result.SystemError = GetLastError();
                result.Error = "SetNamedPipeHandleState failed";
                return result;
            }

            try
            {
                RuntimeControlChannel channel{ pipe.Value };
                (void)pipe.Release();
                result.Channel = std::move(channel);
                result.Status = RuntimeTransportStatus::Succeeded;
                result.EndpointObservation = RuntimeControlEndpointObservation::Available;
                return result;
            }
            catch (const std::exception& exception)
            {
                result.Error = exception.what();
                return result;
            }
        }
    }
}
