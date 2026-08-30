// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <optional>
#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    class IRuntimeRtaskCapabilityTokenSource
    {
    public:
        virtual ~IRuntimeRtaskCapabilityTokenSource() = default;
        [[nodiscard]] virtual std::optional<std::string> Generate() = 0;
    };

    class SystemRuntimeRtaskCapabilityTokenSource final :
        public IRuntimeRtaskCapabilityTokenSource
    {
    public:
        [[nodiscard]] std::optional<std::string> Generate() override;
    };
}
