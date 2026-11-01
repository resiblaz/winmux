// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TermControl.h"
#include "ControlCore.h"

namespace winrt::Microsoft::Terminal::Control::implementation
{
    bool TermControl::EnterVimMode()
    {
        return _core.EnterVimMode();
    }

    bool TermControl::_TryHandleVimKey(const WORD vkey, const WORD scanCode,
                                       const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
                                       const bool keyDown)
    {
        if (!keyDown)
        {
            return _vimInputState.Release(vkey);
        }
        if (!_core.VimModeAvailable())
        {
            // Finish a gesture consumed before disabling the feature, but
            // never suppress a fresh key-down in the alternate screen.
            _vimInputState.Release(vkey);
            _vimInputState.UnhandledKeyDown();
            return false;
        }
        if (modifiers.IsCtrlPressed() || modifiers.IsAltPressed() || modifiers.IsWinPressed())
        {
            // Modified shortcuts cancel a partially entered Vim command.
            _core.TryVimModeKeybinding(vkey, modifiers);
            _vimInputState.Release(vkey);
            _vimInputState.UnhandledKeyDown();
            return false;
        }

        if (_vimInputState.IsConsumed(vkey) &&
            (!VimInputState::IsMovement(vkey) || _core.VimModeState() == VimMode::Insert))
        {
            return true;
        }

        if (_core.TryVimModeKeybinding(vkey, modifiers))
        {
            _vimInputState.Consume(vkey, scanCode);
            _ClearKeyboardState(vkey, scanCode);
            if (vkey == VK_OEM_2 && !modifiers.IsShiftPressed())
            {
                _OpenVimSearch();
            }
            return true;
        }

        _vimInputState.UnhandledKeyDown();
        return false;
    }

    void TermControl::_OpenVimSearch()
    {
        if (!winrt::get_self<ControlCore>(_core)->BeginVimSearch())
        {
            return;
        }
        if (const auto searchBox = FindName(L"SearchBox").try_as<Control::SearchBoxControl>())
        {
            _searchBox.copy_from(winrt::get_self<SearchBoxControl>(searchBox));
            _vimSearchActive = true;
            _searchBox->GoForwardButton().IsChecked(true);
            _searchBox->GoBackwardButton().IsChecked(false);
            _searchBox->PopulateTextbox(L"");
            _searchBox->ClearStatus();
            _searchBox->Open([weakThis = get_weak()]() {
                if (const auto self = weakThis.get(); self && !self->_IsClosing() && self->_vimSearchActive)
                {
                    self->_searchBox->SetFocusOnTextbox();
                    self->_refreshSearch();
                }
            });
        }
        else
        {
            winrt::get_self<ControlCore>(_core)->CancelVimSearch();
        }
    }

    bool TermControl::_SuppressVimCharacter(const wchar_t character, const WORD scanCode)
    {
        const auto navigating = _core.VimModeAvailable() && _core.VimModeState() != VimMode::Insert;
        return _vimInputState.SuppressCharacter(character, scanCode, navigating);
    }

    bool TermControl::_SuppressVimText(const std::wstring_view text)
    {
        const auto navigating = _core.VimModeAvailable() && _core.VimModeState() != VimMode::Insert;
        return _vimInputState.SuppressText(text, navigating);
    }

    safe_void_coroutine TermControl::_VimModeChanged(IInspectable /*sender*/, IInspectable /*args*/)
    {
        const auto weakThis = get_weak();
        co_await winrt::resume_foreground(Dispatcher());
        if (const auto self = weakThis.get(); self && !self->_IsClosing())
        {
            const auto available = self->_core.VimModeAvailable();
            if (self->_vimSearchActive && (!available || self->_core.VimModeState() == VimMode::Insert))
            {
                self->_CloseSearchBoxControl(nullptr, nullptr);
            }
            self->VimModeIndicator().Visibility(available ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed);
            if (!available)
            {
                co_return;
            }
            const auto mode = self->_core.VimModeState();
            self->VimModeText().Text(mode == VimMode::Normal ? L"NORMAL" : mode == VimMode::Visual ? L"VISUAL" : L"INSERT");
        }
    }
}
