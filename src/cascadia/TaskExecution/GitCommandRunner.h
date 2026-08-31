// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitCommandTypes.h"

namespace Microsoft::Terminal::TaskExecution
{
    class GitCommandRunner final
    {
    public:
        [[nodiscard]] GitCommandResult Run(
            const GitCommandRequest& request) const noexcept;
    };
}
