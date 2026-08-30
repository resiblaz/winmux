// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProcessLaunchTypes.h"

#include "RuntimeProcessContainmentInternal.h"
#include "RuntimeProcessObservation.h"

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeProcessLaunchResult::RuntimeProcessLaunchResult() noexcept = default;
    RuntimeProcessLaunchResult::~RuntimeProcessLaunchResult() = default;
    RuntimeProcessLaunchResult::RuntimeProcessLaunchResult(RuntimeProcessLaunchResult&&) noexcept = default;
    RuntimeProcessLaunchResult& RuntimeProcessLaunchResult::operator=(RuntimeProcessLaunchResult&&) noexcept = default;

    bool RuntimeProcessLaunchResult::Started() const noexcept
    {
        return Status == RuntimeProcessLaunchStatus::Started &&
               Phase == RuntimeProcessLaunchPhase::Committed && Process;
    }
}
