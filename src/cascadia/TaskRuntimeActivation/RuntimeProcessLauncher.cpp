// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProcessLauncher.h"

#include "RuntimeExecutablePathValidation.h"
#include "RuntimeParentJob.h"
#include "RuntimeProcessContainmentInternal.h"
#include "RuntimeProcessObservation.h"
#include "RuntimeSuspendedProcessInternal.h"
#include "RuntimeWindowsCommandLine.h"
#include "RuntimeWindowsProcessApiInternal.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        RuntimeProcessLaunchResult _failure(
            const RuntimeProcessLaunchStatus status,
            const RuntimeProcessLaunchPhase phase,
            const RuntimeParentJobStrategy strategy,
            const DWORD systemError,
            std::string error)
        {
            RuntimeProcessLaunchResult result;
            result.Status = status;
            result.Phase = phase;
            result.JobStrategy = strategy;
            result.SystemError = systemError == ERROR_SUCCESS ? ERROR_GEN_FAILURE : systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _currentIdentity(const RuntimeIdentity& identity) noexcept
        {
            try
            {
                const auto rebuilt = BuildCurrentUserRuntimeIdentity(
                    identity.Channel,
                    identity.SettingsRoot);
                return rebuilt.Succeeded() &&
                       rebuilt.Identity.Channel == identity.Channel &&
                       rebuilt.Identity.SettingsRoot == identity.SettingsRoot &&
                       rebuilt.Identity.UserSid == identity.UserSid &&
                       rebuilt.Identity.ProtocolIdentity == identity.ProtocolIdentity &&
                       rebuilt.Identity.InstanceMutexName == identity.InstanceMutexName &&
                       rebuilt.Identity.LaunchMutexName == identity.LaunchMutexName &&
                       rebuilt.Identity.ControlPipeName == identity.ControlPipeName;
            }
            catch (...)
            {
                return false;
            }
        }

        enum class CancellationPollStatus
        {
            NotCancelled,
            Cancelled,
            Failed,
        };

        struct CancellationPollResult
        {
            CancellationPollStatus Status{ CancellationPollStatus::NotCancelled };
            DWORD SystemError{};
        };

        CancellationPollResult _pollCancellation(
            details::IRuntimeWindowsProcessApi& api,
            const HANDLE cancellationEvent) noexcept
        {
            if (!cancellationEvent)
            {
                return {};
            }
            const auto waited = api.PollHandle(cancellationEvent);
            switch (waited.Status)
            {
            case details::RuntimeWindowsWaitStatus::Signaled:
                return { CancellationPollStatus::Cancelled, ERROR_OPERATION_ABORTED };
            case details::RuntimeWindowsWaitStatus::NotSignaled:
                return {};
            default:
                return {
                    CancellationPollStatus::Failed,
                    waited.SystemError == ERROR_SUCCESS ? ERROR_INVALID_HANDLE : waited.SystemError,
                };
            }
        }

    }

    namespace details
    {
        class RuntimeProcessLauncherImpl final : public IRuntimeProcessLauncher
        {
        public:
            explicit RuntimeProcessLauncherImpl(
                std::shared_ptr<IRuntimeWindowsProcessApi> api) noexcept :
                _api{ std::move(api) }
            {
            }

            [[nodiscard]] RuntimeProcessLaunchResult Launch(
                const RuntimeProcessLaunchRequest& request) noexcept override;

        private:
            std::shared_ptr<IRuntimeWindowsProcessApi> _api;
        };
    }

    RuntimeProcessLaunchResult details::RuntimeProcessLauncherImpl::Launch(
        const RuntimeProcessLaunchRequest& request) noexcept
    {
        try
        {
            if (!_api)
            {
                return _failure(
                    RuntimeProcessLaunchStatus::ResourceFailure,
                    RuntimeProcessLaunchPhase::Validation,
                    RuntimeParentJobStrategy::NotInJob,
                    ERROR_NOT_ENOUGH_MEMORY,
                    "Windows process services are unavailable");
            }

            const auto initialCancellation = _pollCancellation(*_api, request.CancellationEvent);
            if (initialCancellation.Status != CancellationPollStatus::NotCancelled)
            {
                return _failure(
                    initialCancellation.Status == CancellationPollStatus::Cancelled ?
                        RuntimeProcessLaunchStatus::CancelledBeforeCreate :
                        RuntimeProcessLaunchStatus::CancellationCheckFailed,
                    RuntimeProcessLaunchPhase::Validation,
                    RuntimeParentJobStrategy::NotInJob,
                    initialCancellation.SystemError,
                    "Runtime process launch cancellation could not proceed");
            }
            if (!_currentIdentity(request.Identity))
            {
                return _failure(
                    RuntimeProcessLaunchStatus::InvalidRequest,
                    RuntimeProcessLaunchPhase::Validation,
                    RuntimeParentJobStrategy::NotInJob,
                    ERROR_INVALID_PARAMETER,
                    "Runtime process launch identity is invalid");
            }
            const auto validatedPath = ValidateRuntimeExecutablePath(
                request.ExecutablePath,
                request.TrustedRoot,
                *_api);
            if (!validatedPath.Succeeded())
            {
                return _failure(
                    validatedPath.Status == RuntimeExecutablePathStatus::ResourceFailure ?
                        RuntimeProcessLaunchStatus::ResourceFailure :
                        RuntimeProcessLaunchStatus::InvalidExecutablePath,
                    RuntimeProcessLaunchPhase::Validation,
                    RuntimeParentJobStrategy::NotInJob,
                    validatedPath.SystemError,
                    validatedPath.Error);
            }

            DWORD error{};
            const std::array<std::wstring, 6> arguments{
                request.ExecutablePath.native(),
                L"--serve",
                L"--channel",
                request.Identity.Channel,
                L"--settings-root",
                request.Identity.SettingsRoot.native(),
            };
            auto commandLine = BuildRuntimeWindowsCommandLine(arguments);
            if (!commandLine.Succeeded())
            {
                const auto launchStatus =
                    commandLine.Status == RuntimeWindowsCommandLineStatus::TooLong ?
                        RuntimeProcessLaunchStatus::CommandLineTooLong :
                    commandLine.Status == RuntimeWindowsCommandLineStatus::ResourceFailure ?
                        RuntimeProcessLaunchStatus::ResourceFailure :
                        RuntimeProcessLaunchStatus::InvalidRequest;
                return _failure(
                    launchStatus,
                    RuntimeProcessLaunchPhase::Validation,
                    RuntimeParentJobStrategy::NotInJob,
                    commandLine.Status == RuntimeWindowsCommandLineStatus::ResourceFailure ?
                        ERROR_NOT_ENOUGH_MEMORY :
                        ERROR_INVALID_PARAMETER,
                    std::move(commandLine.Error));
            }

            const auto job = PlanRuntimeParentJob(*_api);
            if (!job.Ready())
            {
                return _failure(
                    job.Status == RuntimeParentJobPlanStatus::BreakawayDenied ?
                        RuntimeProcessLaunchStatus::ParentJobBreakawayDenied :
                        RuntimeProcessLaunchStatus::ParentJobQueryFailed,
                    RuntimeProcessLaunchPhase::ParentJobPreflight,
                    job.Strategy,
                    job.SystemError,
                    job.Error);
            }

            const auto finalCancellation = _pollCancellation(*_api, request.CancellationEvent);
            if (finalCancellation.Status != CancellationPollStatus::NotCancelled)
            {
                return _failure(
                    finalCancellation.Status == CancellationPollStatus::Cancelled ?
                        RuntimeProcessLaunchStatus::CancelledBeforeCreate :
                        RuntimeProcessLaunchStatus::CancellationCheckFailed,
                    RuntimeProcessLaunchPhase::ProcessCreation,
                    job.Strategy,
                    finalCancellation.SystemError,
                    "Runtime process launch was cancelled before CreateProcess");
            }

            auto observation = std::unique_ptr<RuntimeProcessObservation>{
                new RuntimeProcessObservation{ _api }
            };
            auto containment = std::make_shared<details::RuntimeProcessContainment>(_api);
            details::RuntimeWindowsCreatedProcess created;
            details::RuntimeWindowsProcessCreateRequest createRequest;
            createRequest.ApplicationPath = request.ExecutablePath;
            createRequest.MutableCommandLine =
                std::span<wchar_t>{ commandLine.MutableCommandLine };
            createRequest.CurrentDirectory = request.TrustedRoot;
            createRequest.CreationFlags = CREATE_SUSPENDED | CREATE_NO_WINDOW |
                                          CREATE_DEFAULT_ERROR_MODE |
                                          job.AdditionalCreationFlags;
            createRequest.InheritHandles = false;
            createRequest.UseStandardHandles = false;
            createRequest.UseHandleList = false;
            if (!_api->CreateProcess(createRequest, created, error) ||
                !created.Process || !created.PrimaryThread || created.ProcessId == 0)
            {
                if (created.PrimaryThread)
                {
                    _api->CloseHandle(created.PrimaryThread);
                }
                if (created.Process)
                {
                    _api->CloseHandle(created.Process);
                }
                return _failure(
                    RuntimeProcessLaunchStatus::ProcessCreationFailed,
                    RuntimeProcessLaunchPhase::ProcessCreation,
                    job.Strategy,
                    error == ERROR_SUCCESS ? ERROR_INVALID_HANDLE : error,
                    "TaskRuntime CreateProcess failed");
            }

            details::RuntimeSuspendedProcess suspended{ _api, created };
            const auto proof = ProveRuntimeChildOutsideJobs(suspended.Process(), *_api);
            if (!proof.OutsideEveryJob())
            {
                const auto rollback = suspended.Rollback();
                if (!rollback.Succeeded)
                {
                    containment->Quarantine(suspended.TakeForContainment());
                    auto result = _failure(
                        RuntimeProcessLaunchStatus::RollbackFailed,
                        RuntimeProcessLaunchPhase::Rollback,
                        job.Strategy,
                        rollback.SystemError,
                        "Suspended TaskRuntime rollback failed after Job verification");
                    result._containment = std::move(containment);
                    return result;
                }
                return _failure(
                    proof.Status == RuntimeChildJobProofStatus::StillInJob ?
                        RuntimeProcessLaunchStatus::ChildStillInJob :
                        RuntimeProcessLaunchStatus::ChildJobQueryFailed,
                    RuntimeProcessLaunchPhase::ChildJobVerification,
                    job.Strategy,
                    proof.SystemError,
                    "Suspended TaskRuntime did not prove independence from parent Jobs");
            }

            DWORD previousSuspendCount{};
            const auto resumed = _api->ResumePrimaryThread(
                    suspended.PrimaryThread(),
                    previousSuspendCount,
                    error);
            if (!resumed || previousSuspendCount > 1)
            {
                if (resumed)
                {
                    error = ERROR_INVALID_STATE;
                }
                const auto rollback = suspended.Rollback();
                if (!rollback.Succeeded)
                {
                    containment->Quarantine(suspended.TakeForContainment());
                    auto result = _failure(
                        RuntimeProcessLaunchStatus::RollbackFailed,
                        RuntimeProcessLaunchPhase::Rollback,
                        job.Strategy,
                        rollback.SystemError,
                        "Suspended TaskRuntime rollback failed after ResumeThread failure");
                    result._containment = std::move(containment);
                    return result;
                }
                return _failure(
                    RuntimeProcessLaunchStatus::ResumeFailed,
                    RuntimeProcessLaunchPhase::Resume,
                    job.Strategy,
                    error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error,
                    "TaskRuntime primary thread could not be resumed");
            }

            // ResumeThread success is the ownership commit point. Cancellation
            // after CreateProcess never rolls back or terminates a shared Runtime.
            const auto committed = suspended.Commit();
            observation->_adopt(committed.Process, committed.ProcessId);
            RuntimeProcessLaunchResult result;
            result.Status = RuntimeProcessLaunchStatus::Started;
            result.Phase = RuntimeProcessLaunchPhase::Committed;
            result.JobStrategy = job.Strategy;
            result.Process = std::move(observation);
            // A zero previous count means another same-user actor resumed the
            // child after our Job proof. User code may already be serving, so
            // this is committed and must never enter suspended rollback.
            result.SystemError = previousSuspendCount == 0 ?
                                     DWORD{ ERROR_INVALID_STATE } :
                                     DWORD{ ERROR_SUCCESS };
            return result;
        }
        catch (...)
        {
            return _failure(
                RuntimeProcessLaunchStatus::ResourceFailure,
                RuntimeProcessLaunchPhase::Validation,
                RuntimeParentJobStrategy::NotInJob,
                ERROR_NOT_ENOUGH_MEMORY,
                "Runtime process launcher exhausted local resources");
        }
    }

    std::shared_ptr<IRuntimeProcessLauncher> MakeRuntimeProcessLauncher()
    {
        try
        {
            auto api = details::MakeRuntimeWindowsProcessApi();
            return api ?
                       std::make_shared<details::RuntimeProcessLauncherImpl>(std::move(api)) :
                       std::shared_ptr<IRuntimeProcessLauncher>{};
        }
        catch (...)
        {
            return {};
        }
    }

    std::shared_ptr<IRuntimeProcessLauncher> details::MakeRuntimeProcessLauncherForTesting(
        std::shared_ptr<IRuntimeWindowsProcessApi> api)
    {
        try
        {
            return api ?
                       std::make_shared<RuntimeProcessLauncherImpl>(std::move(api)) :
                       std::shared_ptr<IRuntimeProcessLauncher>{};
        }
        catch (...)
        {
            return {};
        }
    }
}
