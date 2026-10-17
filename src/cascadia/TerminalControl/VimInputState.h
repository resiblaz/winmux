// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <bitset>
#include <cstdint>
#include <optional>
#include <string_view>

namespace winrt::Microsoft::Terminal::Control::implementation
{
    // UI-thread state for one physical key's down/character/up lifecycle.
    // No terminal, connection, clipboard or XAML dependencies.
    class VimInputState
    {
    public:
        bool IsConsumed(const uint16_t key) const noexcept
        {
            return key < _consumed.size() && _consumed[key];
        }

        void Consume(const uint16_t key, const uint16_t scanCode) noexcept
        {
            if (key < _consumed.size())
            {
                _consumed.set(key);
                _pendingText = PendingText{ key, scanCode };
            }
        }

        bool Release(const uint16_t key) noexcept
        {
            const auto consumed = IsConsumed(key);
            if (consumed)
            {
                _consumed.reset(key);
            }
            return consumed;
        }

        void UnhandledKeyDown() noexcept
        {
            _pendingText.reset();
        }

        bool SuppressCharacter(const wchar_t character, const uint16_t scanCode, const bool navigating) noexcept
        {
            if (navigating)
            {
                return true;
            }
            if (_pendingText && ((scanCode != 0 && scanCode == _pendingText->ScanCode) || _matches(character)))
            {
                _pendingText.reset();
                return true;
            }
            return false;
        }

        bool SuppressText(const std::wstring_view text, const bool navigating) noexcept
        {
            if (navigating)
            {
                return true;
            }
            if (text.size() == 1 && _pendingText && _matches(text.front()))
            {
                _pendingText.reset();
                return true;
            }
            return false;
        }

        void Reset() noexcept
        {
            _consumed.reset();
            _pendingText.reset();
        }

        static bool IsMovement(const uint16_t key) noexcept
        {
            return key == 'H' || key == 'J' || key == 'K' || key == 'L' || key == 'B' || key == 'W' || key == 'E' || (key >= 0x25 && key <= 0x28);
        }

    private:
        struct PendingText
        {
            uint16_t Key;
            uint16_t ScanCode;
        };

        bool _matches(const wchar_t character) const noexcept
        {
            const auto key = _pendingText->Key;
            return character == key || (key >= 'A' && key <= 'Z' && character == key + (L'a' - L'A'));
        }

        std::bitset<256> _consumed;
        std::optional<PendingText> _pendingText;
    };
}
