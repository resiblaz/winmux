// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    [[nodiscard]] std::filesystem::path FindRuntimeProductSibling(const wchar_t* name);
    [[nodiscard]] std::filesystem::path FindGitExecutable();
    [[nodiscard]] std::string NarrowRuntimeAscii(std::wstring_view value);
}
