// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>

namespace Microsoft::Terminal::TaskExecution
{
    inline constexpr uint32_t GitRepositoryLockTimeoutMilliseconds{ 30000 };

    enum class TaskDirectoryLockStatus
    {
        Acquired,
        Cancelled,
        TimedOut,
        InvalidIdentity,
        InvalidCancellation,
        InvalidTimeout,
        ReentrantAcquire,
        SecurityFailure,
        SystemError,
        ResourceFailure,
    };

    enum class TaskDirectoryLockReleaseStatus
    {
        Released,
        NotOwned,
        WrongThread,
        SystemError,
    };
}
