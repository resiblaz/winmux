// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeProductExecutableDiscovery.h"

namespace Microsoft::Terminal::TaskRuntime
{
    std::filesystem::path FindRuntimeProductSibling(const wchar_t* name)
    {
        if (!name || *name == L'\0')
        {
            return {};
        }
        std::wstring module(32'768, L'\0');
        const auto written = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (written == 0 || written >= module.size())
        {
            return {};
        }
        module.resize(written);
        return std::filesystem::path{ std::move(module) }.parent_path() / name;
    }

    std::filesystem::path FindGitExecutable()
    {
        std::wstring path(32'768, L'\0');
        const auto written = SearchPathW(nullptr, L"git.exe", nullptr, static_cast<DWORD>(path.size()), path.data(), nullptr);
        if (written == 0 || written >= path.size())
        {
            return {};
        }
        path.resize(written);
        return std::filesystem::path{ std::move(path) };
    }

    std::string NarrowRuntimeAscii(const std::wstring_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (const auto character : value)
        {
            if (character > 0x7f)
            {
                return {};
            }
            result.push_back(static_cast<char>(character));
        }
        return result;
    }
}
