// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    class IRuntimeClientRequestIdSource
    {
    public:
        virtual ~IRuntimeClientRequestIdSource() = default;
        [[nodiscard]] virtual std::optional<std::string> Next() noexcept = 0;
    };

    class IRuntimeClientBackoffPolicy
    {
    public:
        virtual ~IRuntimeClientBackoffPolicy() = default;
        [[nodiscard]] virtual uint32_t DelayForFailure(uint32_t consecutiveFailures) noexcept = 0;
    };

    [[nodiscard]] std::shared_ptr<IRuntimeClientRequestIdSource>
        MakeRuntimeClientRequestIdSource();
    [[nodiscard]] std::shared_ptr<IRuntimeClientBackoffPolicy>
        MakeRuntimeClientBackoffPolicy();
}
