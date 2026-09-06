// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentProfile.h"

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        bool _decode(const std::string& text, std::wstring& wide)
        {
            if (text.find('\0') != std::string::npos) return false;
            if (text.empty()) { wide.clear(); return true; }
            const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (size <= 0) return false;
            wide.resize(size);
            return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size) == size;
        }

        bool _space(const wchar_t c) noexcept
        {
            return (c >= 9 && c <= 13) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 ||
                   (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
        }
    }

    bool NormalizeAgentProfileContent(AgentProfileContent& content, std::string& error)
    {
        if (content.Provider != "codex" || content.Name.size() > 1024 ||
            content.Description.size() > MaximumAgentProfileDescriptionBytes ||
            content.Instructions.size() > MaximumAgentProfileInstructionsBytes ||
            content.Model.size() > MaximumAgentProfileModelBytes)
        {
            error = "Agent fields exceed their limits or the Provider is unsupported";
            return false;
        }
        std::wstring name, text;
        if (!_decode(content.Name, name) || !_decode(content.Description, text) ||
            !_decode(content.Instructions, text) || !_decode(content.Model, text))
        {
            error = "Agent fields must contain valid UTF-8 without NUL";
            return false;
        }
        for (const auto c : text)
        {
            if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) { error = "Model contains control characters"; return false; }
        }
        const auto first = name.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos) name.clear();
        while (!name.empty() && _space(name.front())) name.erase(name.begin());
        while (!name.empty() && _space(name.back())) name.pop_back();
        size_t scalars{};
        for (const auto c : name)
        {
            if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) { error = "Agent name contains control characters"; return false; }
            if (c < 0xdc00 || c > 0xdfff) ++scalars;
        }
        if (!scalars || scalars > MaximumAgentProfileNameScalars)
        {
            error = "Agent name must contain 1 to 80 Unicode characters";
            return false;
        }
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) { error = "Agent name could not be encoded"; return false; }
        content.Name.resize(size);
        return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), content.Name.data(), size, nullptr, nullptr) == size;
    }
}
