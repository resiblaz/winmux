// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProviderTypes.h"

#include <json/json.h>

namespace Microsoft::Terminal::TaskProvider
{
    [[nodiscard]] TaskProviderCapabilitiesResult ParseCodexAppServerCapabilities(
        const Json::Value& initializeResult) noexcept;
    [[nodiscard]] TaskProviderCapabilitiesResult ValidateCodexAppServerSchema(
        std::string_view schemaJson) noexcept;
}
