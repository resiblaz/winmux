// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlConnector.h"

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeClientConnectDisposition
    {
        Retry,
        Fatal,
    };

    [[nodiscard]] RuntimeClientConnectDisposition EvaluateRuntimeClientConnectDisposition(
        const RuntimeControlConnectionResult& result) noexcept;
}
