// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitSourceSnapshot.h"

#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskGit::details
{
    [[nodiscard]] std::optional<std::string> ComputeGitSourceStatusDigest(
        std::string_view statusOutput);
    [[nodiscard]] std::optional<std::string> ComputeGitSourceSnapshotDigest(
        const GitSourceSnapshot& snapshot);
}
