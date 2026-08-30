// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeActivationServices.h"
#include "RuntimeLaunchLockProvider.h"
#include "RuntimeProcessLaunchTypes.h"
#include "RuntimeProcessObservation.h"

#include "RuntimeControlConnector.h"

#include <optional>

namespace Microsoft::Terminal::TaskRuntime::details
{
    [[nodiscard]] RuntimeControlConnectionResult ActivationFailure(
        RuntimeTransportStatus status,
        RuntimeControlConnectFailureReason reason,
        DWORD systemError,
        std::string error,
        RuntimeControlConnectPhase phase = RuntimeControlConnectPhase::Connecting);
    [[nodiscard]] uint32_t ActivationRemaining(
        IRuntimeActivationServices& services,
        RuntimeActivationDeadline deadline) noexcept;
    [[nodiscard]] RuntimeControlConnectionResult ActivationDeadlineFailure();
    [[nodiscard]] RuntimeControlConnectionResult ActivationLockFailure(
        const RuntimeLaunchLockLeaseResult& lock);
    [[nodiscard]] RuntimeControlConnectionResult ActivationLaunchFailure(
        const RuntimeProcessLaunchResult& launched);
    [[nodiscard]] std::optional<RuntimeControlConnectionResult> ObserveRuntimeProcessExit(
        RuntimeProcessObservation& process);
}
