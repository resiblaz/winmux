// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ControlCore.h"
#include "VimTextNavigator.h"
#include "EventArgs.h"
#include "../../renderer/base/renderer.hpp"

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
        const auto selectionChanged = initialized && _vimModeController.Reconcile(*_terminal);
        if (initialized && _vimModeController.State() == Control::VimMode::Insert && _vimSearchActive)
        {
            _clearVimSearchUnderLock();
        }
        std::optional<::Microsoft::Console::Render::CursorOverride> cursor;
        if (initialized)
        {
            if (const auto position = _vimModeController.CursorPosition(*_terminal))
            {
                cursor = ::Microsoft::Console::Render::CursorOverride{
                    .position = _terminal->GetTextBuffer().ScreenToBufferPosition(*position)
                };
            }
        }
        _renderer->SetCursorOverride(cursor);
        const auto mode = _vimModeController.State();
        const auto available = initialized && !_IsClosing() && _vimModeController.Available(*_terminal);
        if (mode == _reportedVimMode && available == _reportedVimModeAvailable)
        {
            return selectionChanged;
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
            if (result.SearchDirection && !_vimSearchRequest.Text.empty())
            {
                auto request = _vimSearchRequest;
                request.GoForward = result.SearchDirection > 0 ? request.GoForward : !request.GoForward;
                _searchVimUnderLock(request, result.Count, true);
                result.SelectionChanged = true;
            }
            if (result.CopyLineCount)
            {
                const VimTextNavigator navigator{ _terminal->GetTextBuffer(), _terminal->GetTextBufferEndPosition().y };
                if (!navigator.Empty())
                {
                    const auto position = *_vimModeController.CursorPosition(*_terminal);
                    const auto start = navigator.LineStart(position);
                    const auto end = navigator.AfterGlyph(navigator.LineEnd(position, result.CopyLineCount));
                    _terminal->SetMarkSelection(start, end, true);
                    const auto formats = _settings.CopyFormatting();
                    payload = _terminal->RetrieveSelectedTextFromBuffer(false, false, WI_IsFlagSet(formats, CopyFormat::HTML), WI_IsFlagSet(formats, CopyFormat::RTF));
                    // A linewise yank includes a final newline, including on blank lines.
                    payload.plainText.append(L"\r\n");
                    _terminal->SetMarkSelection(position, position, false);
                    result.SelectionChanged = true;
                }
                else
                {
                    result.CopyLineCount = 0;
                }
            }
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
        if (result.CopySelection || result.CopyLineCount)
        {
            WriteToClipboard.raise(*this, winrt::make<WriteToClipboardEventArgs>(
                                             winrt::hstring{ payload.plainText }, std::move(payload.html), std::move(payload.rtf)));
        }
        return result.Handled;
    }

    bool ControlCore::BeginVimSearch()
    {
        const auto lock = _terminal->LockForWriting();
        _refreshVimModeUnderLock();
        if (_vimModeController.State() == Control::VimMode::Insert ||
            !_initializedTerminal.load(std::memory_order_relaxed) || _IsClosing() || !_vimModeController.Available(*_terminal))
        {
            return false;
        }
        _vimModeController.ResetCommand();
        _vimSearchActive = true;
        return true;
    }

    SearchResults ControlCore::SearchVim(const SearchRequest& request, const bool commit)
    {
        const auto lock = _terminal->LockForWriting();
        _refreshVimModeUnderLock();
        if (!_vimSearchActive || _vimModeController.State() == Control::VimMode::Insert)
        {
            return {};
        }
        auto effectiveRequest = request;
        if (commit && effectiveRequest.Text.empty())
        {
            effectiveRequest.Text = _vimSearchRequest.Text;
        }
        const auto results = _searchVimUnderLock(effectiveRequest, 1, commit);
        if (commit && !results.SearchRegexInvalid && !effectiveRequest.Text.empty())
        {
            _vimSearchRequest = effectiveRequest;
        }
        if (commit)
        {
            _refreshVimModeUnderLock();
            _updateSelectionUI();
        }
        return results;
    }

    SearchResults ControlCore::_searchVimUnderLock(const SearchRequest& request, const uint32_t count, const bool move)
    {
        const auto cursor = _vimModeController.CursorPosition(*_terminal);
        if (!cursor)
        {
            return {};
        }
        _vimSearchActive = true;
        if (request.Text.empty())
        {
            _clearVimSearchUnderLock();
            _vimSearchActive = true;
            return { .SearchInvalidated = true };
        }
        SearchFlag flags{};
        WI_SetFlagIf(flags, SearchFlag::CaseInsensitive, !request.CaseSensitive);
        WI_SetFlagIf(flags, SearchFlag::RegularExpression, request.RegularExpression);
        const auto invalidated = _searcher.IsStale(*_terminal, request.Text, flags);
        std::vector<til::point_span> oldResults;
        if (invalidated)
        {
            oldResults = _searcher.ExtractResults();
            _searcher.Reset(*_terminal, request.Text, flags, !request.GoForward);
            _terminal->SetSearchHighlights(_searcher.Results());
        }
        auto matches = std::span{ _searcher.Results() };
        // The text provider maps a zero-width match at the absolute EOF back
        // to (0,0). Ignore that trailing sentinel when it breaks buffer order.
        if (matches.size() > 1 && matches.back().start == matches.back().end &&
            matches.back().start < matches[matches.size() - 2].start)
        {
            matches = matches.first(matches.size() - 1);
        }
        size_t index = 0;
        if (!matches.empty())
        {
            const auto& buffer = _terminal->GetTextBuffer();
            const VimTextNavigator navigator{ buffer, _terminal->GetTextBufferEndPosition().y };
            // Compare browsing characters, so a zero-width end-of-line match
            // or a match inside a wide glyph cannot trap n on the same character.
            const auto positionOf = [&](const auto& match) { return navigator.ClampCursor(buffer.BufferToScreenPosition(match.start)); };
            const auto first = std::lower_bound(matches.begin(), matches.end(), *cursor, [&](const auto& match, const auto point) { return positionOf(match) < point; });
            if (request.GoForward)
            {
                auto next = first;
                while (next != matches.end() && positionOf(*next) == *cursor)
                {
                    ++next;
                }
                index = (static_cast<size_t>(next - matches.begin()) + (count - 1) % matches.size()) % matches.size();
            }
            else
            {
                const auto offset = static_cast<size_t>(first - matches.begin());
                index = (offset + matches.size() - 1 - (count - 1) % matches.size()) % matches.size();
            }
            _terminal->SetSearchHighlightFocused(index);
            if (move)
            {
                _vimModeController.MoveTo(*_terminal, positionOf(matches[index]));
            }
        }
        _renderer->TriggerSearchHighlight(oldResults);
        return {
            .TotalMatches = gsl::narrow<int32_t>(matches.size()),
            .CurrentMatch = gsl::narrow<int32_t>(index),
            .SearchInvalidated = invalidated,
            .SearchRegexInvalid = !_searcher.IsOk(),
        };
    }

    void ControlCore::_clearVimSearchUnderLock()
    {
        _terminal->SetSearchHighlights({});
        _terminal->SetSearchHighlightFocused(0);
        _renderer->TriggerSearchHighlight(_searcher.Results());
        _searcher = {};
        _vimSearchActive = false;
    }

    void ControlCore::CancelVimSearch()
    {
        const auto lock = _terminal->LockForWriting();
        if (_vimSearchActive)
        {
            _clearVimSearchUnderLock();
        }
        _vimModeController.ResetCommand();
    }
}
