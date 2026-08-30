// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

namespace Microsoft::Terminal::TaskCore
{
    enum class TaskStoreOpenStatus
    {
        Ready,
        UnsupportedSchema,
        ForeignDatabase,
        RecoveryRequired,
        IOError,
    };
}
