// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProviderTypes.h"

namespace Microsoft::Terminal::TaskProvider
{
    [[nodiscard]] TaskProviderFailureDisposition ClassifyTaskProviderFailure(
        TaskProviderStatus status,
        std::string_view providerCode) noexcept;
}
