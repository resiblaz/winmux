// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreCommandTypes.h"

#include <optional>

namespace Microsoft::Terminal::TaskRuntime
{
    [[nodiscard]] int64_t RuntimeUnixMilliseconds() noexcept;
    [[nodiscard]] std::optional<Microsoft::Terminal::TaskCore::TaskCommandMetadata>
    MakeRuntimeTaskCommandMetadata(
        std::string_view domain,
        const Microsoft::Terminal::TaskCore::TaskId& task,
        std::string_view operationIdentity,
        uint64_t expectedTaskVersion,
        int64_t minimumTimestamp,
        std::string eventPayloadJson = "{}",
        std::string receiptResultJson = "{}");
}
