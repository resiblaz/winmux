// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProjection.h"
#include "TaskProjectionRpc.h"

namespace winrt::TerminalApp::implementation::tasks
{
    [[nodiscard]] std::optional<TaskProjection> MapTaskProjection(
        const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionRecord& source) noexcept;
    [[nodiscard]] std::optional<std::vector<TaskProjection>> MapTaskProjectionSnapshot(
        const ::Microsoft::Terminal::TaskRuntimeProtocol::TaskProjectionQueryResult& source) noexcept;
}
