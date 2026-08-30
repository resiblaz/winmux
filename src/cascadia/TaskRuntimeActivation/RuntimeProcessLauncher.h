// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeProcessLaunchTypes.h"

#include <memory>

namespace Microsoft::Terminal::TaskRuntime
{
    class IRuntimeProcessLauncher
    {
    public:
        virtual ~IRuntimeProcessLauncher() = default;
        [[nodiscard]] virtual RuntimeProcessLaunchResult Launch(
            const RuntimeProcessLaunchRequest& request) noexcept = 0;
    };

    [[nodiscard]] std::shared_ptr<IRuntimeProcessLauncher>
        MakeRuntimeProcessLauncher();
}
