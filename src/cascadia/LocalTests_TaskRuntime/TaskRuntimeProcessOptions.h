// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <optional>

namespace TaskRuntimeLocalTests::ProcessFixtures
{
    struct TaskRuntimeProcessOptions
    {
        std::optional<uint32_t> IdleExitMilliseconds;
    };
}
