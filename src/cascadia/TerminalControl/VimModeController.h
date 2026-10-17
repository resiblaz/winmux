// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "../TerminalCore/ControlKeyStates.hpp"
#include <winrt/Microsoft.Terminal.Control.h>

namespace Microsoft::Terminal::Core
{
    class Terminal;
}

namespace winrt::Microsoft::Terminal::Control::implementation
{
    struct VimKeyResult
    {
        bool Handled{};
        bool SelectionChanged{};
        bool CopySelection{};
    };

    // Operates on the existing Mark selection, under the caller's terminal lock.
    // Clipboard delivery, keyboard event lifetimes and XAML remain with the host.
    class VimModeController
    {
    public:
        using Terminal = ::Microsoft::Terminal::Core::Terminal;
        using Modifiers = ::Microsoft::Terminal::Core::ControlKeyStates;

        VimMode State() const noexcept;
        bool Available(Terminal& terminal) const;
        void SetEnabled(Terminal& terminal, bool enabled);
        void Reconcile(Terminal& terminal);
        bool Enter(Terminal& terminal);
        void Exit(Terminal& terminal);
        void ReturnToNormal(Terminal& terminal);
        VimKeyResult HandleKey(Terminal& terminal, WORD vkey, Modifiers modifiers);

    private:
        bool _enabled{};
        VimMode _mode{ VimMode::Insert };
    };
}
