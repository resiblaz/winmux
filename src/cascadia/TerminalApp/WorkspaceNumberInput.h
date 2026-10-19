// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <bitset>
#include <cstdint>
#include <optional>

namespace winrt::TerminalApp::implementation
{
    // Accumulate one or two top-row digits while a single modifier is held.
    // Keep consumed key-downs until their releases, even after committing or
    // cancelling, so that no part of the number is forwarded to a terminal.
    class WorkspaceNumberInput final
    {
    public:
        static constexpr uint32_t MaximumNumber{ 99 };

        enum class Target
        {
            Workspace,
            Surface,
        };

        struct Selection
        {
            Target Kind;
            uint32_t Number;
        };

        std::optional<Target> PendingTarget() const noexcept
        {
            return _target;
        }

        bool IsDigitHeld(const uint32_t digit) const noexcept
        {
            return digit <= 9 && _heldDigits.test(digit);
        }

        void PressDigit(const Target target, const uint32_t digit) noexcept
        {
            if (digit > 9 || _heldDigits.test(digit))
            {
                return;
            }
            _heldDigits.set(digit);

            if (!_target)
            {
                _target = target;
            }
            if (_target != target || ++_digitCount > 2 || (_digitCount == 1 && digit == 0))
            {
                _cancelled = true;
            }
            if (!_cancelled)
            {
                _number = _number * 10 + digit;
            }
        }

        bool ReleaseDigit(const uint32_t digit) noexcept
        {
            if (digit > 9 || !_heldDigits.test(digit))
            {
                return false;
            }
            _heldDigits.reset(digit);
            return true;
        }

        bool Escape(const bool down) noexcept
        {
            if (down && (_target || _heldEscape))
            {
                Cancel();
                _heldEscape = true;
                return true;
            }
            if (!down && _heldEscape)
            {
                _heldEscape = false;
                return true;
            }
            return false;
        }

        void Cancel() noexcept
        {
            if (_target)
            {
                _cancelled = true;
            }
        }

        std::optional<Selection> Finish(const Target target) noexcept
        {
            if (_target != target)
            {
                return std::nullopt;
            }

            std::optional<Selection> selection;
            if (!_cancelled && _number > 0 && _number <= MaximumNumber)
            {
                selection = Selection{ target, _number };
            }
            _target.reset();
            _number = 0;
            _digitCount = 0;
            _cancelled = false;
            return selection;
        }

        void Reset() noexcept
        {
            *this = {};
        }

    private:
        std::optional<Target> _target;
        std::bitset<10> _heldDigits;
        uint32_t _number{};
        uint32_t _digitCount{};
        bool _cancelled{};
        bool _heldEscape{};
    };
}
