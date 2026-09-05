// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace winrt::TerminalApp::implementation::tasks
{
    struct TaskTerminalInputBatch
    {
        std::wstring Echo;
        std::vector<std::wstring> Messages;
        bool Interrupt{};
    };

    class TaskTerminalInput final
    {
    public:
        [[nodiscard]] TaskTerminalInputBatch Feed(const std::wstring_view data)
        {
            TaskTerminalInputBatch batch;
            // TerminalControl emits an entire key sequence in one callback.
            // Only a standalone Escape key requests interruption.
            if (_escape == EscapeState::Text && data == L"\x1b")
            {
                batch.Interrupt = true;
                return batch;
            }
            for (const auto value : data)
            {
                if (_escape != EscapeState::Text)
                {
                    switch (_escape)
                    {
                    case EscapeState::Escape:
                        _escape = value == L'[' ? EscapeState::Csi :
                                  value == L'O' ? EscapeState::Ss3 :
                                  value == L']' ? EscapeState::Osc : EscapeState::Text;
                        break;
                    case EscapeState::Csi:
                    case EscapeState::Ss3:
                        if ((value >= 0x40 && value <= 0x7e) || value < 0x20 || value > 0x7e)
                            _escape = EscapeState::Text;
                        break;
                    case EscapeState::Osc:
                        if (value == 0x07) _escape = EscapeState::Text;
                        else if (value == 0x1b) _escape = EscapeState::OscEscape;
                        break;
                    case EscapeState::OscEscape:
                        _escape = value == L'\\' ? EscapeState::Text : EscapeState::Osc;
                        break;
                    default:
                        break;
                    }
                    continue;
                }
                if (value == 0x1b)
                {
                    _escape = EscapeState::Escape;
                    continue;
                }
                if (value == L'\r' || value == L'\n')
                {
                    if (value == L'\n' && std::exchange(_lastCarriageReturn, false))
                        continue;
                    _lastCarriageReturn = value == L'\r';
                    if (!_line.empty())
                        batch.Messages.push_back(std::exchange(_line, {}));
                    batch.Echo.append(L"\r\n");
                    continue;
                }
                _lastCarriageReturn = false;
                if (value == L'\b' || value == 0x7f)
                {
                    if (!_line.empty())
                    {
                        const auto last = _line.back();
                        _line.pop_back();
                        if (last >= 0xdc00 && last <= 0xdfff && !_line.empty() &&
                            _line.back() >= 0xd800 && _line.back() <= 0xdbff)
                            _line.pop_back();
                        batch.Echo.append(L"\b \b");
                    }
                }
                else if (value >= 0x20 || value == L'\t')
                {
                    _line.push_back(value);
                    batch.Echo.push_back(value);
                }
            }
            return batch;
        }

    private:
        enum class EscapeState { Text, Escape, Csi, Ss3, Osc, OscEscape };
        std::wstring _line;
        EscapeState _escape{ EscapeState::Text };
        bool _lastCarriageReturn{};
    };
}
