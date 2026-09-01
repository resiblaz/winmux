// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProviderTypes.h"

namespace Microsoft::Terminal::TaskProvider
{
    struct TaskProviderPreparedRunContext
    {
        bool Succeeded{};
        std::string Instructions;
        std::vector<TaskProviderEnvironmentEntry> Environment;
        std::string Error;
    };

    [[nodiscard]] TaskProviderPreparedRunContext PrepareTaskProviderRunContext(
        const TaskProviderRunContext& context) noexcept;
}
