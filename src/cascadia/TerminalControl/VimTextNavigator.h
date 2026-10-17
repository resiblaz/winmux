// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <optional>
#include <string_view>
#include <til/point.h>

class TextBuffer;

namespace winrt::Microsoft::Terminal::Control::implementation
{
    // A read-only view used while the caller holds the terminal lock. Positions
    // use the native selection's screen columns and absolute buffer rows.
    class VimTextNavigator
    {
    public:
        VimTextNavigator(const TextBuffer& buffer, til::CoordType bottom, std::wstring_view delimiters = {});

        bool Empty() const;
        til::point ClampEndpoint(til::point position) const;
        til::point ClampCursor(til::point position) const;
        til::point Horizontal(til::point position, bool forward) const;
        til::point Vertical(til::point position, int delta, til::CoordType preferredColumn) const;
        til::point WordStart(til::point position) const;
        til::point WordEnd(til::point position) const;
        til::point AfterGlyph(til::point position) const;
        til::point BeforeEndpoint(til::point position) const;

    private:
        til::CoordType _lineEnd(til::CoordType row) const;
        int _classAt(til::point position) const;
        bool _sameLogicalLine(til::point left, til::point right) const;
        std::optional<til::point> _previous(til::point position) const;
        std::optional<til::point> _next(til::point position) const;

        const TextBuffer& _buffer;
        std::wstring_view _delimiters;
        til::CoordType _lastRow{};
        mutable til::CoordType _cachedRow{ -1 };
        mutable til::CoordType _cachedEnd{};
    };
}
