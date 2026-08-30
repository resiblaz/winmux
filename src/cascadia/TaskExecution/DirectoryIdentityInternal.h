// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "DirectoryIdentity.h"

namespace Microsoft::Terminal::TaskExecution::details
{
    [[nodiscard]] DirectoryIdentityStatus ClassifyDirectoryAttributes(
        DWORD attributes) noexcept;
}
