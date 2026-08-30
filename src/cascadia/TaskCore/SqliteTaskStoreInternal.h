// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "SqliteTaskStore.h"

#include <winsqlite/winsqlite3.h>

#include <mutex>

namespace Microsoft::Terminal::TaskCore
{
    struct SqliteTaskStore::Impl
    {
        sqlite3* Database{};
        TaskStoreOpenStatus Status{ TaskStoreOpenStatus::IOError };
        int32_t Version{};
        std::string Error;
        mutable std::mutex DatabaseMutex;

        ~Impl();
    };
}
