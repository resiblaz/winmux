// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeRtaskCapabilityToken.h"

#include <deque>
#include <stdexcept>

namespace TaskRuntimeLocalTests
{
    class FakeRuntimeRtaskCapabilityTokenSource final :
        public Microsoft::Terminal::TaskRuntime::IRuntimeRtaskCapabilityTokenSource
    {
    public:
        [[nodiscard]] std::optional<std::string> Generate() override
        {
            ++Calls;
            if (ThrowOnGenerate)
            {
                throw std::runtime_error{ "fake capability token failure" };
            }
            if (Tokens.empty())
            {
                return std::nullopt;
            }
            auto token = std::move(Tokens.front());
            Tokens.pop_front();
            return token;
        }

        std::deque<std::optional<std::string>> Tokens;
        size_t Calls{};
        bool ThrowOnGenerate{};
    };
}
