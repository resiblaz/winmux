// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRuntimeClientTypes.h"

#include "RuntimeControlConnector.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime::details
{
    [[nodiscard]] std::shared_ptr<IRuntimeControlConnector>
        MakeRuntimeClientProductConnector(
            const TaskRuntimeClientConfiguration& configuration);
}
