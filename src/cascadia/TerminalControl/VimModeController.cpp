// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "VimModeController.h"
#include "VimTextNavigator.h"
#include "../TerminalCore/Terminal.hpp"

namespace winrt::Microsoft::Terminal::Control::implementation
{
    namespace
    {
        // Always derive positions from the native selection, so scrollback
        // rotation cannot leave a second, stale set of coordinates behind.
        std::pair<til::point, til::point> SelectionPoints(VimModeController::Terminal& terminal)
        {
            return terminal.SelectionEndpointTarget() == VimModeController::Terminal::SelectionEndpoint::Start ?
                       std::pair{ terminal.GetSelectionEnd(), terminal.GetSelectionAnchor() } :
                       std::pair{ terminal.GetSelectionAnchor(), terminal.GetSelectionEnd() };
        }
    }

    VimMode VimModeController::State() const noexcept
    {
        return _mode;
    }

    bool VimModeController::Available(Terminal& terminal) const
    {
        return _enabled && terminal.GetBufferAndViewport().isMainBuffer;
    }

    std::optional<til::point> VimModeController::CursorPosition(Terminal& terminal) const
    {
        if (_mode == VimMode::Insert || !Available(terminal) ||
            !terminal.IsSelectionActive() || terminal.SelectionMode() != Terminal::SelectionInteractionMode::Mark)
        {
            return std::nullopt;
        }
        const VimTextNavigator navigator{ terminal.GetTextBuffer(), terminal.GetTextBufferEndPosition().y };
        auto position = SelectionPoints(terminal).second;
        if (_visualActiveGlyphEnd && terminal.SelectionEndpointTarget() == Terminal::SelectionEndpoint::End)
        {
            position = navigator.BeforeEndpoint(position);
        }
        return navigator.ClampCursor(position);
    }

    void VimModeController::SetEnabled(Terminal& terminal, const bool enabled)
    {
        _enabled = enabled;
        if (!enabled)
        {
            Exit(terminal);
        }
    }

    bool VimModeController::Reconcile(Terminal& terminal)
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
        if (_mode == VimMode::Insert)
        {
            _preferredColumn.reset();
            _visualActiveGlyphEnd = false;
            _visualAnchorGlyphEnd = false;
        }
        else if (_mode == VimMode::Visual && (_visualActiveGlyphEnd || _visualAnchorGlyphEnd))
        {
            // Existing selection actions can switch the active endpoint.
            // In an inclusive range, whichever endpoint is End represents
            // the boundary after its glyph, regardless of who switched it.
            _visualActiveGlyphEnd = terminal.SelectionEndpointTarget() == Terminal::SelectionEndpoint::End;
            _visualAnchorGlyphEnd = terminal.SelectionEndpointTarget() == Terminal::SelectionEndpoint::Start;
        }
        else if (_mode == VimMode::Normal)
        {
            // Output can erase or shorten the row under a stationary cursor.
            // Keep it on text even when no navigation key has been pressed.
            const VimTextNavigator navigator{ terminal.GetTextBuffer(), terminal.GetTextBufferEndPosition().y };
            const auto position = terminal.GetSelectionAnchor();
            const auto constrained = navigator.ClampCursor(position);
            if (constrained != position)
            {
                terminal.SetMarkSelection(constrained, constrained, false);
                return true;
            }
        }
        return false;
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
        _preferredColumn.reset();
        _visualActiveGlyphEnd = false;
        _visualAnchorGlyphEnd = false;
        _mode = VimMode::Normal;
        Reconcile(terminal);
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
        _preferredColumn.reset();
        _visualActiveGlyphEnd = false;
        _visualAnchorGlyphEnd = false;
    }

    void VimModeController::ReturnToNormal(Terminal& terminal)
    {
        const VimTextNavigator navigator{ terminal.GetTextBuffer(), terminal.GetTextBufferEndPosition().y };
        auto position = SelectionPoints(terminal).second;
        if (_visualActiveGlyphEnd && terminal.SelectionEndpointTarget() == Terminal::SelectionEndpoint::End)
        {
            position = navigator.BeforeEndpoint(position);
        }
        position = navigator.ClampCursor(position);
        terminal.SetMarkSelection(position, position, false);
        _preferredColumn.reset();
        _visualActiveGlyphEnd = false;
        _visualAnchorGlyphEnd = false;
        _mode = VimMode::Normal;
    }

    void VimModeController::_setInclusiveSelection(Terminal& terminal, const VimTextNavigator& navigator, til::point anchor, til::point position)
    {
        _visualActiveGlyphEnd = position >= anchor;
        _visualAnchorGlyphEnd = position < anchor;
        position = _visualActiveGlyphEnd ? navigator.AfterGlyph(position) : position;
        anchor = _visualAnchorGlyphEnd ? navigator.AfterGlyph(anchor) : anchor;
        terminal.SetMarkSelection(anchor, position, true);
    }

    VimKeyResult VimModeController::HandleKey(Terminal& terminal, const WORD vkey, const Modifiers modifiers, const std::wstring_view wordDelimiters)
    {
        const auto reconciled = Reconcile(terminal);
        if (!Available(terminal) || modifiers.IsCtrlPressed() || modifiers.IsAltPressed() || modifiers.IsWinPressed())
        {
            return { .SelectionChanged = reconciled };
        }

        if (_mode == VimMode::Insert)
        {
            // Inline applications need Escape too. Entry is an explicit,
            // configurable action, independent of the application's screen.
            return { .SelectionChanged = reconciled };
        }

        if (!Terminal::IsInputKey(vkey))
        {
            return { .SelectionChanged = reconciled };
        }
        if (modifiers.IsShiftPressed())
        {
            return { .Handled = true, .SelectionChanged = reconciled };
        }

        const VimTextNavigator navigator{ terminal.GetTextBuffer(), terminal.GetTextBufferEndPosition().y, wordDelimiters };
        auto [anchor, position] = SelectionPoints(terminal);
        bool clamped = reconciled;
        if (_mode == VimMode::Visual)
        {
            const auto constrainedAnchor = navigator.ClampEndpoint(anchor);
            const auto constrainedPosition = navigator.ClampEndpoint(position);
            clamped = constrainedAnchor != anchor || constrainedPosition != position;
            anchor = constrainedAnchor;
            position = constrainedPosition;
            if (clamped)
            {
                terminal.SetMarkSelection(anchor, position, true);
            }
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
                position = navigator.ClampEndpoint(position);
                terminal.SetMarkSelection(position, position, !navigator.Empty());
                if (!navigator.Empty())
                {
                    _mode = VimMode::Visual;
                }
                _preferredColumn.reset();
                _visualActiveGlyphEnd = false;
                _visualAnchorGlyphEnd = false;
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
                     .SelectionChanged = clamped,
                     .CopySelection = _mode == VimMode::Visual && terminal.GetSelectionAnchor() != terminal.GetSelectionEnd() };
        case 'B':
        case 'W':
        case 'E':
        {
            // Word motions include both endpoint glyphs in Visual ranges.
            // The native end remains exclusive; remember only the endpoint
            // interpretation, never a coordinate that rotation could invalidate.
            const auto cursor = _visualActiveGlyphEnd && terminal.SelectionEndpointTarget() == Terminal::SelectionEndpoint::End ?
                                    navigator.BeforeEndpoint(position) : position;
            const auto target = vkey == 'B' ? navigator.WordStart(cursor) :
                                vkey == 'W' ? navigator.NextWordStart(cursor) : navigator.WordEnd(cursor);
            if (_visualAnchorGlyphEnd)
            {
                anchor = navigator.BeforeEndpoint(anchor);
            }
            if (_mode == VimMode::Visual)
            {
                _setInclusiveSelection(terminal, navigator, anchor, target);
            }
            else
            {
                terminal.SetMarkSelection(target, target, false);
            }
            _preferredColumn.reset();
            return { .Handled = true, .SelectionChanged = true };
        }
        default:
            break;
        }

        if (direction)
        {
            const auto visual = _mode == VimMode::Visual;
            const auto inclusive = visual && (_visualActiveGlyphEnd || _visualAnchorGlyphEnd);
            if (visual)
            {
                if (_visualActiveGlyphEnd)
                {
                    position = navigator.BeforeEndpoint(position);
                }
                if (_visualAnchorGlyphEnd)
                {
                    anchor = navigator.BeforeEndpoint(anchor);
                }
            }
            if (*direction == Terminal::SelectionDirection::Up || *direction == Terminal::SelectionDirection::Down)
            {
                if (!_preferredColumn)
                {
                    _preferredColumn = position.x;
                }
                position = navigator.Vertical(position, *direction == Terminal::SelectionDirection::Up ? -1 : 1, *_preferredColumn);
            }
            else
            {
                position = navigator.Horizontal(position, *direction == Terminal::SelectionDirection::Right);
                _preferredColumn.reset();
            }
            if (inclusive)
            {
                _setInclusiveSelection(terminal, navigator, anchor, navigator.ClampCursor(position));
            }
            else if (visual)
            {
                terminal.SetMarkSelection(anchor, position, true);
            }
            else
            {
                position = navigator.ClampCursor(position);
                terminal.SetMarkSelection(position, position, false);
            }
            return { .Handled = true, .SelectionChanged = true };
        }
        return { .Handled = true, .SelectionChanged = clamped };
    }
}
