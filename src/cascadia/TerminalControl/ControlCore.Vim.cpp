// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ControlCore.h"
#include "EventArgs.h"

namespace winrt::Microsoft::Terminal::Control::implementation
{
    Control::VimMode ControlCore::VimModeState() const
    {
        const auto lock = _terminal->LockForReading();
        return _vimModeController.State();
    }

    bool ControlCore::VimModeAvailable() const
    {
        const auto lock = _terminal->LockForReading();
        return _initializedTerminal.load(std::memory_order_relaxed) && !_IsClosing() && _vimModeController.Available(*_terminal);
    }

    bool ControlCore::_refreshVimModeUnderLock()
    {
        const auto initialized = _initializedTerminal.load(std::memory_order_relaxed);
        if (initialized)
        {
            _vimModeController.Reconcile(*_terminal);
        }
        const auto mode = _vimModeController.State();
        const auto available = initialized && !_IsClosing() && _vimModeController.Available(*_terminal);
        if (mode == _reportedVimMode && available == _reportedVimModeAvailable)
        {
            return false;
        }
        _reportedVimMode = mode;
        _reportedVimModeAvailable = available;
        // Never invoke consumers synchronously while holding the terminal lock.
        _dispatcher.TryEnqueue([weakThis = get_weak()]() {
            if (const auto self = weakThis.get(); self && !self->_IsClosing())
            {
                self->VimModeChanged.raise(*self, nullptr);
            }
        });
        return true;
    }

    bool ControlCore::EnterVimMode()
    {
        const auto lock = _terminal->LockForWriting();
        if (!_initializedTerminal.load(std::memory_order_relaxed) || _IsClosing())
        {
            return false;
        }
        const auto entered = _vimModeController.Enter(*_terminal);
        if (entered || _refreshVimModeUnderLock())
        {
            _updateSelectionUI();
        }
        return entered;
    }

    void ControlCore::ExitVimMode()
    {
        const auto lock = _terminal->LockForWriting();
        if (_vimModeController.State() != Control::VimMode::Insert)
        {
            _vimModeController.Exit(*_terminal);
            _updateSelectionUI();
        }
    }

    bool ControlCore::TryVimModeKeybinding(const WORD vkey, const ::Microsoft::Terminal::Core::ControlKeyStates modifiers)
    {
        ::Microsoft::Terminal::Core::Terminal::TextCopyData payload;
        VimKeyResult result;
        {
            const auto lock = _terminal->LockForWriting();
            if (!_initializedTerminal.load(std::memory_order_relaxed) || _IsClosing())
            {
                return false;
            }

            const auto wordDelimiters = _settings.WordDelimiters();
            result = _vimModeController.HandleKey(*_terminal, vkey, modifiers, { wordDelimiters.c_str(), wordDelimiters.size() });
            if (result.CopySelection)
            {
                const auto formats = _settings.CopyFormatting();
                payload = _terminal->RetrieveSelectedTextFromBuffer(false, false,
                                                                    WI_IsFlagSet(formats, CopyFormat::HTML),
                                                                    WI_IsFlagSet(formats, CopyFormat::RTF));
                // Capture and collapse the same selection atomically. Deliver
                // the clipboard request only after releasing the lock.
                _vimModeController.ReturnToNormal(*_terminal);
                result.SelectionChanged = true;
            }
            if (result.SelectionChanged || _refreshVimModeUnderLock())
            {
                _updateSelectionUI();
            }
        }
        if (result.CopySelection)
        {
            WriteToClipboard.raise(*this, winrt::make<WriteToClipboardEventArgs>(
                                             winrt::hstring{ payload.plainText }, std::move(payload.html), std::move(payload.rtf)));
        }
        return result.Handled;
    }
}
