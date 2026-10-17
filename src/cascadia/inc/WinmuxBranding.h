// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <string_view>

// The downstream product uses the existing Dev build channel. Keep executable
// names shared by the GUI, console launcher, elevation helper and shell extension.
namespace TerminalBranding
{
#if defined(WT_BRANDING_DEV)
    inline constexpr std::wstring_view Executable{ L"winmux.exe" };
    inline constexpr std::wstring_view DevExecutionAlias{ L"wmx.exe" };
#else
    inline constexpr std::wstring_view Executable{ L"WindowsTerminal.exe" };
    inline constexpr std::wstring_view DevExecutionAlias{ L"wtd.exe" };
#endif
}
