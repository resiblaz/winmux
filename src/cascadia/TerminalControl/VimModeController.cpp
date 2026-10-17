// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "VimModeController.h"
#include "../TerminalCore/Terminal.hpp"

namespace winrt::Microsoft::Terminal::Control::implementation
{
    VimMode VimModeController::State() const noexcept
    {
        return _mode;
    }

    bool VimModeController::Available(Terminal& terminal) const
    {
        return _enabled && terminal.GetBufferAndViewport().isMainBuffer;
    }

    void VimModeController::SetEnabled(Terminal& terminal, const bool enabled)
    {
        _enabled = enabled;
        if (!enabled)
        {
            Exit(terminal);
        }
    }

    void VimModeController::Reconcile(Terminal& terminal)
    {
        if (_mode != VimMode::Insert &&
            (!Available(terminal) || !terminal.IsSelectionActive() || terminal.SelectionMode() != Terminal::SelectionInteractionMode::Mark))
        {
            // A mouse selection or a buffer switch may already have replaced
            // our selection. Only clear it when we still own a Mark selection.
            Exit(terminal);
        }
        else if (_mode != VimMode::Insert &&
                 (terminal.IsBlockSelection() ||
                  (_mode == VimMode::Normal &&
                   (terminal.GetSelectionAnchor() != terminal.GetSelectionEnd() ||
                    terminal.SelectionEndpointTarget() != static_cast<Terminal::SelectionEndpoint>(3)))))
        {
            // An existing selection action took ownership. Keep its selection,
            // but stop interpreting it as Vim navigation.
            _mode = VimMode::Insert;
        }
    }

    bool VimModeController::Enter(Terminal& terminal)
    {
        Reconcile(terminal);
        if (!Available(terminal))
        {
            return false;
        }
        if (_mode != VimMode::Insert)
        {
            return true;
        }
        if (terminal.IsSelectionActive())
        {
            return false;
        }
        terminal.ToggleMarkMode();
        _mode = VimMode::Normal;
        return true;
    }

    void VimModeController::Exit(Terminal& terminal)
    {
        if (_mode != VimMode::Insert)
        {
            if (terminal.SelectionMode() == Terminal::SelectionInteractionMode::Mark)
            {
                terminal.ClearSelection();
            }
            _mode = VimMode::Insert;
        }
    }

    void VimModeController::ReturnToNormal(Terminal& terminal)
    {
        terminal.CollapseSelectionToActiveEndpoint();
        _mode = VimMode::Normal;
    }

    VimKeyResult VimModeController::HandleKey(Terminal& terminal, const WORD vkey, const Modifiers modifiers)
    {
        Reconcile(terminal);
        if (!Available(terminal) || modifiers.IsCtrlPressed() || modifiers.IsAltPressed() || modifiers.IsWinPressed())
        {
            return {};
        }

        if (_mode == VimMode::Insert)
        {
            // Inline applications need Escape too. Entry is an explicit,
            // configurable action, independent of the application's screen.
            return {};
        }

        if (!Terminal::IsInputKey(vkey))
        {
            return {};
        }
        if (modifiers.IsShiftPressed())
        {
            return { .Handled = true };
        }

        std::optional<Terminal::SelectionDirection> direction;
        switch (vkey)
        {
        case 'H':
        case VK_LEFT:
            direction = Terminal::SelectionDirection::Left;
            break;
        case 'J':
        case VK_DOWN:
            direction = Terminal::SelectionDirection::Down;
            break;
        case 'K':
        case VK_UP:
            direction = Terminal::SelectionDirection::Up;
            break;
        case 'L':
        case VK_RIGHT:
            direction = Terminal::SelectionDirection::Right;
            break;
        case 'I':
        case 'A':
            Exit(terminal);
            terminal.TrySnapOnInput();
            return { .Handled = true, .SelectionChanged = true };
        case 'V':
            if (_mode == VimMode::Normal)
            {
                terminal.SwitchSelectionEndpoint();
                _mode = VimMode::Visual;
            }
            else
            {
                ReturnToNormal(terminal);
            }
            return { .Handled = true, .SelectionChanged = true };
        case VK_ESCAPE:
            if (_mode == VimMode::Visual)
            {
                ReturnToNormal(terminal);
                return { .Handled = true, .SelectionChanged = true };
            }
            break;
        case 'Y':
            return { .Handled = true,
                     .CopySelection = _mode == VimMode::Visual && terminal.GetSelectionAnchor() != terminal.GetSelectionEnd() };
        default:
            break;
        }

        if (direction)
        {
            terminal.UpdateSelection(*direction, Terminal::SelectionExpansion::Char, {});
            return { .Handled = true, .SelectionChanged = true };
        }
        return { .Handled = true };
    }
}
