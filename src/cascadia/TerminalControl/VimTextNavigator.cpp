// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "VimTextNavigator.h"
#include "../../buffer/out/textBuffer.hpp"

namespace winrt::Microsoft::Terminal::Control::implementation
{
    VimTextNavigator::VimTextNavigator(const TextBuffer& buffer, const til::CoordType bottom, const std::wstring_view delimiters) :
        _buffer{ buffer }, _delimiters{ delimiters }
    {
        const auto size = buffer.GetSize();
        const auto viewport = ::Microsoft::Console::Types::Viewport::FromDimensions(
            { 0, 0 }, { size.Width(), std::clamp(bottom, 0, size.BottomInclusive()) + 1 });
        _lastRow = buffer.GetLastNonSpaceCharacter(&viewport).y;
        while (_lastRow > 0 && buffer.GetRowByOffset(_lastRow).GetLastNonSpaceColumn() == 0)
        {
            --_lastRow;
        }
    }

    bool VimTextNavigator::Empty() const
    {
        return _lastRow == 0 && _lineEnd(0) == 0;
    }

    til::CoordType VimTextNavigator::_lineEnd(const til::CoordType y) const
    {
        if (y == _cachedRow)
        {
            return _cachedEnd;
        }
        const auto& row = _buffer.GetRowByOffset(y);
        // Spaces at a soft wrap belong to the logical line. Only trim them
        // when they are at the end of all output.
        const auto end = y == _lastRow ? row.GetLastNonSpaceColumn() : row.MeasureRight();
        _cachedRow = y;
        _cachedEnd = _buffer.BufferToScreenPosition({ std::min(end, _buffer.GetLineWidth(y)), y }).x;
        return _cachedEnd;
    }

    til::point VimTextNavigator::ClampEndpoint(til::point position) const
    {
        if (position.y > _lastRow)
        {
            return { _lineEnd(_lastRow), _lastRow };
        }
        position.y = std::max(position.y, 0);
        const auto end = _lineEnd(position.y);
        position.x = std::clamp(position.x, 0, end);
        if (position.x < end)
        {
            auto bufferPosition = _buffer.ScreenToBufferPosition(position);
            bufferPosition.x = _buffer.GetRowByOffset(position.y).AdjustToGlyphStart(bufferPosition.x);
            position = _buffer.BufferToScreenPosition(bufferPosition);
        }
        return position;
    }

    til::point VimTextNavigator::ClampCursor(til::point position) const
    {
        position = ClampEndpoint(position);
        if (position.x > 0 && position.x == _lineEnd(position.y))
        {
            position = BeforeEndpoint(position);
        }
        return position;
    }

    til::point VimTextNavigator::AfterGlyph(til::point position) const
    {
        position = ClampEndpoint(position);
        if (position.x < _lineEnd(position.y))
        {
            auto bufferPosition = _buffer.ScreenToBufferPosition(position);
            bufferPosition.x = _buffer.GetRowByOffset(position.y).AdjustToGlyphEnd(bufferPosition.x + 1);
            position = _buffer.BufferToScreenPosition(bufferPosition);
        }
        return position;
    }

    til::point VimTextNavigator::BeforeEndpoint(til::point position) const
    {
        position = ClampEndpoint(position);
        if (position.x > 0)
        {
            auto bufferPosition = _buffer.ScreenToBufferPosition(position);
            bufferPosition.x = _buffer.GetRowByOffset(position.y).AdjustToGlyphStart(bufferPosition.x - 1);
            position = _buffer.BufferToScreenPosition(bufferPosition);
        }
        return position;
    }

    til::point VimTextNavigator::Horizontal(const til::point position, const bool forward) const
    {
        return forward ? AfterGlyph(position) : BeforeEndpoint(position);
    }

    til::point VimTextNavigator::Vertical(til::point position, const int delta, const til::CoordType preferredColumn) const
    {
        position = ClampEndpoint(position);
        position.y = std::clamp(position.y + delta, 0, _lastRow);
        position.x = preferredColumn;
        return ClampEndpoint(position);
    }

    int VimTextNavigator::_classAt(const til::point position) const
    {
        if (position.x >= _lineEnd(position.y))
        {
            return static_cast<int>(DelimiterClass::ControlChar);
        }
        const auto bufferPosition = _buffer.ScreenToBufferPosition(position);
        return static_cast<int>(_buffer.GetRowByOffset(position.y).DelimiterClassAt(bufferPosition.x, _delimiters));
    }

    bool VimTextNavigator::_sameLogicalLine(const til::point left, const til::point right) const
    {
        return left.y == right.y || (left.y + 1 == right.y && _buffer.GetRowByOffset(left.y).WasWrapForced());
    }

    std::optional<til::point> VimTextNavigator::_previous(til::point position) const
    {
        if (position.x > 0)
        {
            return BeforeEndpoint(position);
        }
        if (position.y == 0)
        {
            return std::nullopt;
        }
        --position.y;
        position.x = _lineEnd(position.y);
        return BeforeEndpoint(position);
    }

    std::optional<til::point> VimTextNavigator::_next(til::point position) const
    {
        position = AfterGlyph(position);
        if (position.x < _lineEnd(position.y))
        {
            return position;
        }
        if (position.y == _lastRow)
        {
            return std::nullopt;
        }
        return til::point{ 0, position.y + 1 };
    }

    til::point VimTextNavigator::Paragraph(til::point position, const bool forward) const
    {
        position = ClampCursor(position);
        const auto emptyLine = [this](const til::CoordType row) {
            // An empty continuation row still belongs to its soft-wrapped
            // logical line and must not split a paragraph.
            return _lineEnd(row) == 0 && (row == 0 || !_buffer.GetRowByOffset(row - 1).WasWrapForced());
        };
        auto seenText = !emptyLine(position.y);
        const auto delta = forward ? 1 : -1;
        for (auto row = position.y + delta; row >= 0 && row <= _lastRow; row += delta)
        {
            if (emptyLine(row))
            {
                if (seenText)
                {
                    return { 0, row };
                }
            }
            else
            {
                seenText = true;
            }
        }
        return forward ? ClampCursor({ _lineEnd(_lastRow), _lastRow }) : til::point{ 0, 0 };
    }

    til::point VimTextNavigator::WordStart(til::point position) const
    {
        position = ClampEndpoint(position);
        auto previous = _previous(position);
        while (previous && _classAt(*previous) == static_cast<int>(DelimiterClass::ControlChar))
        {
            position = *previous;
            previous = _previous(position);
        }
        if (!previous)
        {
            return position;
        }
        position = *previous;
        const auto wordClass = _classAt(position);
        while ((previous = _previous(position)) && _sameLogicalLine(*previous, position) && _classAt(*previous) == wordClass)
        {
            position = *previous;
        }
        return position;
    }

    til::point VimTextNavigator::NextWordStart(til::point position) const
    {
        position = ClampCursor(position);
        const auto whitespace = static_cast<int>(DelimiterClass::ControlChar);
        const auto wordClass = _classAt(position);
        auto next = _next(position);
        if (wordClass != whitespace)
        {
            while (next && _sameLogicalLine(position, *next) && _classAt(*next) == wordClass)
            {
                position = *next;
                next = _next(position);
            }
        }
        // Empty interior rows are word stops in Vim. Spaces inside a logical
        // line are skipped, including those on either side of a soft wrap.
        while (next && _lineEnd(next->y) != 0 && _classAt(*next) == whitespace)
        {
            position = *next;
            next = _next(position);
        }
        return next.value_or(ClampCursor(position));
    }

    til::point VimTextNavigator::WordEnd(til::point position) const
    {
        position = ClampEndpoint(position);
        auto next = _next(position);
        while (next && _classAt(*next) == static_cast<int>(DelimiterClass::ControlChar))
        {
            next = _next(*next);
        }
        if (!next)
        {
            return ClampCursor(position);
        }
        position = *next;
        const auto wordClass = _classAt(position);
        while ((next = _next(position)) && _sameLogicalLine(position, *next) && _classAt(*next) == wordClass)
        {
            position = *next;
        }
        return position;
    }
}
