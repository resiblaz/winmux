// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "TaskStoreResults.h"

namespace Microsoft::Terminal::TaskCore
{
    struct TaskSessionIndexEntry { uint64_t Sequence{}; std::string ProviderSessionId; };
    struct TaskSessionIndexResult
    {
        TaskStoreCommandStatus Status{ TaskStoreCommandStatus::StorageError };
        std::vector<TaskSessionIndexEntry> Sessions;
        uint64_t AfterSequence{};
        bool HasMore{};
        std::string Error;
        bool Succeeded() const noexcept { return Status == TaskStoreCommandStatus::Succeeded; }
    };
}
