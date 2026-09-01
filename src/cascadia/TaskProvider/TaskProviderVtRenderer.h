// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProviderTypes.h"

namespace Microsoft::Terminal::TaskProvider
{
    struct TaskProviderVtRenderResult
    {
        bool Succeeded{};
        std::string Bytes;
        std::string Error;
    };

    [[nodiscard]] TaskProviderVtRenderResult RenderTaskProviderEventAsVt(
        const TaskProviderEvent& event) noexcept;
}
