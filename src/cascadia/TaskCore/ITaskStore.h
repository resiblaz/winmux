// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreStatus.h"

#include <cstdint>
#include <string>

namespace Microsoft::Terminal::TaskCore
{
    class ITaskStore
    {
    public:
        virtual ~ITaskStore() = default;

        [[nodiscard]] virtual bool IsReady() const noexcept = 0;
        [[nodiscard]] virtual TaskStoreOpenStatus OpenStatus() const noexcept = 0;
        [[nodiscard]] virtual int32_t SchemaVersion() const noexcept = 0;
        [[nodiscard]] virtual const std::string& LastError() const noexcept = 0;
    };
}
