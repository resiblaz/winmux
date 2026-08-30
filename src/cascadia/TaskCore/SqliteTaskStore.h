// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ITaskStore.h"

#include <filesystem>
#include <memory>

namespace Microsoft::Terminal::TaskCore
{
    class SqliteTaskStore final : public ITaskStore
    {
    public:
        explicit SqliteTaskStore(const std::filesystem::path& path);
        ~SqliteTaskStore() override;

        SqliteTaskStore(const SqliteTaskStore&) = delete;
        SqliteTaskStore& operator=(const SqliteTaskStore&) = delete;
        SqliteTaskStore(SqliteTaskStore&&) = delete;
        SqliteTaskStore& operator=(SqliteTaskStore&&) = delete;

        [[nodiscard]] bool IsReady() const noexcept override;
        [[nodiscard]] TaskStoreOpenStatus OpenStatus() const noexcept override;
        [[nodiscard]] int32_t SchemaVersion() const noexcept override;
        [[nodiscard]] const std::string& LastError() const noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
