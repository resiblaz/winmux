// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlConnector.h"

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeDirectConnectDisposition
    {
        Connected,
        MayLaunch,
        RetryConnectOnly,
        Cancelled,
        Fatal,
    };

    [[nodiscard]] RuntimeDirectConnectDisposition EvaluateRuntimeDirectConnectDisposition(
        const RuntimeControlConnectionResult& result) noexcept;
}
