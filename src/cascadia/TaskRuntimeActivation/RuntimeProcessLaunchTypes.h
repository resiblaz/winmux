// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    namespace details
    {
        class RuntimeProcessContainment;
        class RuntimeProcessLauncherImpl;
    }

    class RuntimeProcessObservation;

    enum class RuntimeParentJobStrategy
    {
        NotInJob,
        SilentBreakaway,
        ExplicitBreakaway,
    };

    enum class RuntimeProcessLaunchPhase
    {
        Validation,
        ParentJobPreflight,
        ProcessCreation,
        ChildJobVerification,
        Resume,
        Rollback,
        Committed,
    };

    enum class RuntimeProcessLaunchStatus
    {
        Started,
        CancelledBeforeCreate,
        InvalidRequest,
        InvalidExecutablePath,
        CommandLineTooLong,
        CancellationCheckFailed,
        ParentJobQueryFailed,
        ParentJobBreakawayDenied,
        ProcessCreationFailed,
        ChildJobQueryFailed,
        ChildStillInJob,
        ResumeFailed,
        RollbackFailed,
        ResourceFailure,
    };

    struct RuntimeProcessLaunchRequest
    {
        std::filesystem::path ExecutablePath;
        std::filesystem::path TrustedRoot;
        RuntimeIdentity Identity;
        HANDLE CancellationEvent{};
    };

    struct RuntimeProcessLaunchResult
    {
        RuntimeProcessLaunchResult() noexcept;
        ~RuntimeProcessLaunchResult();

        RuntimeProcessLaunchResult(const RuntimeProcessLaunchResult&) = delete;
        RuntimeProcessLaunchResult& operator=(const RuntimeProcessLaunchResult&) = delete;
        RuntimeProcessLaunchResult(RuntimeProcessLaunchResult&&) noexcept;
        RuntimeProcessLaunchResult& operator=(RuntimeProcessLaunchResult&&) noexcept;

        RuntimeProcessLaunchStatus Status{ RuntimeProcessLaunchStatus::InvalidRequest };
        RuntimeProcessLaunchPhase Phase{ RuntimeProcessLaunchPhase::Validation };
        RuntimeParentJobStrategy JobStrategy{ RuntimeParentJobStrategy::NotInJob };
        std::unique_ptr<RuntimeProcessObservation> Process;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Started() const noexcept;

    private:
        friend class details::RuntimeProcessLauncherImpl;
        std::shared_ptr<details::RuntimeProcessContainment> _containment;
    };
}
