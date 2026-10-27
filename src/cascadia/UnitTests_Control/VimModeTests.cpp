// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../TerminalControl/ControlCore.h"
#include "../TerminalControl/VimInputState.h"
#include "../../renderer/base/renderer.hpp"
#include "MockControlSettings.h"

using namespace Microsoft::Console;
using namespace WEX::TestExecution;
using namespace winrt;
using namespace winrt::Microsoft::Terminal;

namespace ControlUnitTests
{
    namespace
    {
        struct RecordingConnection : winrt::implements<RecordingConnection, TerminalConnection::ITerminalConnection>
        {
            void Initialize(const winrt::Windows::Foundation::Collections::ValueSet&) {}
            void Start() noexcept {}
            void Resize(uint32_t, uint32_t) noexcept {}
            void Close() noexcept {}
            winrt::guid SessionId() const noexcept { return {}; }
            TerminalConnection::ConnectionState State() const noexcept { return TerminalConnection::ConnectionState::Connected; }
            void WriteInput(const winrt::array_view<const char16_t> input) { Input.append(input.begin(), input.end()); }
            void Output(const std::wstring_view text) { TerminalOutput.raise(winrt_wstring_to_array_view(text)); }

            std::wstring Input;
            til::event<TerminalConnection::TerminalOutputHandler> TerminalOutput;
            til::typed_event<TerminalConnection::ITerminalConnection, IInspectable> StateChanged;
        };
    }

    class VimModeTests
    {
        BEGIN_TEST_CLASS(VimModeTests)
            TEST_CLASS_PROPERTY(L"TestTimeout", L"0:0:15")
        END_TEST_CLASS()

        TEST_CLASS_SETUP(Setup)
        {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            return true;
        }
        TEST_CLASS_CLEANUP(Cleanup)
        {
            winrt::uninit_apartment();
            return true;
        }

        struct Fixture
        {
            winrt::com_ptr<MockControlSettings> Settings;
            winrt::com_ptr<RecordingConnection> Connection;
            winrt::com_ptr<Control::implementation::ControlCore> Core;
        };

        Fixture Create(const bool enabled = true, const std::wstring_view text = L"abcdef\r")
        {
            Fixture fixture{ winrt::make_self<MockControlSettings>(), winrt::make_self<RecordingConnection>(), nullptr };
            fixture.Settings->VimModeEnabled(enabled);
            fixture.Settings->HistorySize(0);
            fixture.Core = winrt::make_self<Control::implementation::ControlCore>(*fixture.Settings, *fixture.Settings, *fixture.Connection);
            fixture.Core->_inUnitTests = true;
            VERIFY_IS_TRUE(fixture.Core->Initialize(270, 380, 1.0f));
            fixture.Connection->Output(text);
            return fixture;
        }

        void Key(const Fixture& fixture, const WORD key, const ::Microsoft::Terminal::Core::ControlKeyStates modifiers = {})
        {
            VERIFY_IS_TRUE(fixture.Core->TryVimModeKeybinding(key, modifiers));
        }

        void Paragraph(const Fixture& fixture, const bool forward)
        {
            Key(fixture, forward ? VK_OEM_6 : VK_OEM_4, ::Microsoft::Terminal::Core::ControlKeyStates::ShiftPressed);
        }

        void Enter(const Fixture& fixture)
        {
            VERIFY_IS_TRUE(fixture.Core->EnterVimMode());
        }

        til::point Anchor(const Fixture& fixture)
        {
            const auto lock = fixture.Core->_terminal->LockForReading();
            return fixture.Core->_terminal->GetSelectionAnchor();
        }

        til::point Active(const Fixture& fixture)
        {
            const auto lock = fixture.Core->_terminal->LockForReading();
            const auto& terminal = *fixture.Core->_terminal;
            return terminal.SelectionEndpointTarget() == Control::implementation::VimModeController::Terminal::SelectionEndpoint::Start ?
                       terminal.GetSelectionAnchor() : terminal.GetSelectionEnd();
        }

        void Position(const Fixture& fixture, const til::point position)
        {
            const auto lock = fixture.Core->_terminal->LockForWriting();
            fixture.Core->_terminal->SetMarkSelection(position, position, false);
        }

        std::optional<::Microsoft::Console::Render::CursorOverride> DisplayCursor(const Fixture& fixture)
        {
            const auto lock = fixture.Core->_terminal->LockForReading();
            return fixture.Core->_renderer->GetCursorOverride();
        }

        TEST_METHOD(VimBlockCursorPreservesApplicationCursor)
        {
            const auto fixture = Create(true, L"alpha beta\r\x1b[6 q\x1b[?25l");
            VERIFY_IS_FALSE(DisplayCursor(fixture).has_value());
            Enter(fixture);
            Key(fixture, 'W');
            auto display = DisplayCursor(fixture);
            VERIFY_IS_TRUE(display.has_value());
            VERIFY_ARE_EQUAL(til::point(6, 0), display->position);
            VERIFY_IS_TRUE(display->cursorType == CursorType::FullBox);
            VERIFY_IS_FALSE(display->blinking);
            {
                const auto lock = fixture.Core->_terminal->LockForReading();
                const auto& application = fixture.Core->_terminal->GetTextBuffer().GetCursor();
                VERIFY_ARE_EQUAL(til::point(0, 0), application.GetPosition());
                VERIFY_IS_TRUE(application.GetType() == CursorType::VerticalBar);
                VERIFY_IS_FALSE(application.IsVisible());
            }
            // VT output continues at the shell cursor and may change its style
            // while the independent browsing cursor stays in Normal.
            fixture.Connection->Output(L"zz\x1b[4 q\x1b[?25h");
            display = DisplayCursor(fixture);
            VERIFY_ARE_EQUAL(til::point(6, 0), display->position);
            VERIFY_IS_TRUE(display->cursorType == CursorType::FullBox);
            Key(fixture, 'I');
            VERIFY_IS_FALSE(DisplayCursor(fixture).has_value());
            {
                const auto lock = fixture.Core->_terminal->LockForReading();
                const auto& application = fixture.Core->_terminal->GetTextBuffer().GetCursor();
                VERIFY_ARE_EQUAL(til::point(2, 0), application.GetPosition());
                VERIFY_IS_TRUE(application.GetType() == CursorType::Underscore);
                VERIFY_IS_TRUE(application.IsVisible());
            }
        }

        TEST_METHOD(VimBlockCursorTracksVisualGlyphs)
        {
            const auto fixture = Create(true, L"\x4e2d abc def\r");
            Enter(fixture);
            VERIFY_ARE_EQUAL(til::point(0, 0), DisplayCursor(fixture)->position);
            Key(fixture, 'W');
            Key(fixture, 'V');
            Key(fixture, 'E');
            VERIFY_ARE_EQUAL(til::point(6, 0), Active(fixture));
            VERIFY_ARE_EQUAL(til::point(5, 0), DisplayCursor(fixture)->position);
            Key(fixture, 'W');
            VERIFY_ARE_EQUAL(til::point(7, 0), DisplayCursor(fixture)->position);
            VERIFY_IS_TRUE(fixture.Core->SwitchSelectionEndpoint());
            VERIFY_ARE_EQUAL(til::point(3, 0), DisplayCursor(fixture)->position);
            Key(fixture, 'B');
            VERIFY_ARE_EQUAL(til::point(0, 0), DisplayCursor(fixture)->position);
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(til::point(0, 0), DisplayCursor(fixture)->position);

            const auto doubled = Create(true, L"\x1b#6abc\r");
            Enter(doubled);
            Key(doubled, 'L');
            VERIFY_ARE_EQUAL(til::point(2, 0), Anchor(doubled));
            VERIFY_ARE_EQUAL(til::point(1, 0), DisplayCursor(doubled)->position);
        }

        TEST_METHOD(VimBlockCursorEndsWithNavigation)
        {
            const auto fixture = Create();
            Enter(fixture);
            fixture.Connection->Output(L"\x1b[?1049h");
            VERIFY_IS_FALSE(DisplayCursor(fixture).has_value());
            fixture.Connection->Output(L"\x1b[?1049l");
            VERIFY_IS_FALSE(DisplayCursor(fixture).has_value());
            Enter(fixture);
            fixture.Settings->VimModeEnabled(false);
            fixture.Core->UpdateSettings(*fixture.Settings, *fixture.Settings);
            VERIFY_IS_FALSE(DisplayCursor(fixture).has_value());
            fixture.Settings->VimModeEnabled(true);
            fixture.Core->UpdateSettings(*fixture.Settings, *fixture.Settings);
            Enter(fixture);
            fixture.Core->SizeChanged(300, 380);
            VERIFY_IS_FALSE(DisplayCursor(fixture).has_value());
            fixture.Core->ToggleMarkMode();
            VERIFY_IS_FALSE(DisplayCursor(fixture).has_value());
            fixture.Core->ClearSelection();
            Enter(fixture);
            fixture.Core->ClearSelection();
            VERIFY_IS_FALSE(DisplayCursor(fixture).has_value());
        }

        TEST_METHOD(VimNormalBoundsAndPreferredColumn)
        {
            const auto fixture = Create(true, L"abcdef\r\nxy\r\n\r\n  z  q   \r\n");
            Enter(fixture);
            VERIFY_ARE_EQUAL(til::point(5, 3), Anchor(fixture));
            Position(fixture, { 5, 0 });
            for (const auto expected : { til::point(1, 1), til::point(0, 2), til::point(5, 3) })
            {
                Key(fixture, 'J');
                VERIFY_ARE_EQUAL(expected, Anchor(fixture));
            }
            for (int i = 0; i < 20; ++i)
            {
                Key(fixture, VK_RIGHT);
                Key(fixture, VK_DOWN);
            }
            VERIFY_ARE_EQUAL(til::point(5, 3), Anchor(fixture));
            for (const auto expected : { til::point(0, 2), til::point(1, 1), til::point(5, 0) })
            {
                Key(fixture, VK_UP);
                VERIFY_ARE_EQUAL(expected, Anchor(fixture));
            }
            for (int i = 0; i < 20; ++i)
            {
                Key(fixture, 'H');
                Key(fixture, 'K');
            }
            VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));
            VERIFY_IS_TRUE(fixture.Connection->Input.empty());
        }

        TEST_METHOD(VimNormalEntryOutputAndSelectionReturn)
        {
            const auto fixture = Create(true, L"abcdef   \r\n");
            std::wstring copied;
            auto markerUpdates = 0;
            fixture.Core->UpdateSelectionMarkers([&](auto&&, auto&&) { ++markerUpdates; });
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
            Enter(fixture);
            VERIFY_ARE_EQUAL(til::point(5, 0), Anchor(fixture));
            Key(fixture, 'V');
            Key(fixture, 'L');
            Key(fixture, VK_ESCAPE);
            VERIFY_ARE_EQUAL(til::point(5, 0), Anchor(fixture));
            Key(fixture, 'V');
            Key(fixture, 'L');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"f"), copied);
            VERIFY_ARE_EQUAL(til::point(5, 0), Anchor(fixture));

            const auto markersBeforeErase = markerUpdates;
            fixture.Connection->Output(L"\x1b[H\x1b[Kxy\r");
            VERIFY_ARE_EQUAL(til::point(1, 0), Anchor(fixture));
            VERIFY_IS_TRUE(markerUpdates > markersBeforeErase);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Normal);
            fixture.Connection->Output(L"\x1b[H\x1b[K\r");
            VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));
            for (const auto key : std::wstring_view{ L"VWLJY" })
            {
                Key(fixture, static_cast<WORD>(key));
            }
            VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));
            VERIFY_ARE_EQUAL(std::wstring(L"f"), copied);

            fixture.Core->ExitVimMode();
            fixture.Connection->Output(L"a\x4e2d\r\n");
            Enter(fixture);
            VERIFY_ARE_EQUAL(til::point(1, 0), Anchor(fixture));
            Key(fixture, 'L');
            VERIFY_ARE_EQUAL(til::point(1, 0), Anchor(fixture));
            Key(fixture, 'H');
            VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));
        }

        TEST_METHOD(VimNextWordStarts)
        {
            const auto fixture = Create(true, L"foo_bar::baz  qux\r");
            Enter(fixture);
            for (const auto column : { 7, 9, 14, 16, 16 })
            {
                Key(fixture, 'W');
                VERIFY_ARE_EQUAL(til::point(column, 0), Anchor(fixture));
            }
            Position(fixture, { 3, 0 });
            Key(fixture, 'W');
            VERIFY_ARE_EQUAL(til::point(7, 0), Anchor(fixture));
            Position(fixture, { 12, 0 });
            Key(fixture, 'W');
            VERIFY_ARE_EQUAL(til::point(14, 0), Anchor(fixture));
            fixture.Core->ExitVimMode();
            fixture.Settings->WordDelimiters(L" ");
            fixture.Core->UpdateSettings(*fixture.Settings, *fixture.Settings);
            Enter(fixture);
            Key(fixture, 'W');
            VERIFY_ARE_EQUAL(til::point(14, 0), Anchor(fixture));
            VERIFY_IS_TRUE(fixture.Connection->Input.empty());
        }

        TEST_METHOD(VimNextWordLinesAndWhitespace)
        {
            const auto fixture = Create(true, L"alpha  beta\r\n  gamma\r\n\r\n\r\nend\r");
            Enter(fixture);
            Position(fixture, { 0, 0 });
            for (const auto expected : { til::point(7, 0), til::point(2, 1), til::point(0, 2),
                                         til::point(0, 3), til::point(0, 4), til::point(2, 4), til::point(2, 4) })
            {
                Key(fixture, 'W');
                VERIFY_ARE_EQUAL(expected, Anchor(fixture));
            }
            Position(fixture, { 0, 1 });
            Key(fixture, 'W');
            VERIFY_ARE_EQUAL(til::point(2, 1), Anchor(fixture));
        }

        TEST_METHOD(VimNextWordVisualRange)
        {
            const auto fixture = Create(true, L"one two three\r");
            std::wstring copied;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
            Enter(fixture);
            Key(fixture, 'V');
            Key(fixture, 'W');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"one t"), copied);
            VERIFY_ARE_EQUAL(til::point(4, 0), Anchor(fixture));
            Position(fixture, { 6, 0 });
            Key(fixture, 'V');
            Key(fixture, 'B');
            Key(fixture, 'W');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"o t"), copied);
            VERIFY_ARE_EQUAL(til::point(8, 0), Anchor(fixture));
            Key(fixture, 'V');
            Key(fixture, 'W');
            Key(fixture, 'W');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"three"), copied);
            VERIFY_ARE_EQUAL(til::point(12, 0), Anchor(fixture));
        }

        TEST_METHOD(VimNextWordWrapAndWideGlyphs)
        {
            const auto fixture = Create(true, L"");
            const auto width = fixture.Core->_terminal->GetTextBuffer().GetSize().Width();
            fixture.Connection->Output(std::wstring(width + 4, L'a') + L" zz\r");
            Enter(fixture);
            Position(fixture, { 0, 0 });
            Key(fixture, 'W');
            VERIFY_ARE_EQUAL(til::point(5, 1), Anchor(fixture));
            Key(fixture, 'W');
            VERIFY_ARE_EQUAL(til::point(6, 1), Anchor(fixture));
            fixture.Core->ExitVimMode();
            fixture.Connection->Output(L"\x1b[2J\x1b[H" + std::wstring(width, L'a') + L"\r\nbbbb\r");
            Enter(fixture);
            Position(fixture, { 0, 0 });
            Key(fixture, 'W');
            VERIFY_ARE_EQUAL(til::point(0, 1), Anchor(fixture));
            fixture.Core->ExitVimMode();
            fixture.Connection->Output(L"\x1b[2J\x1b[H" + std::wstring(width - 1, L'a') + L"  word\r");
            Enter(fixture);
            Position(fixture, { 0, 0 });
            Key(fixture, 'W');
            VERIFY_ARE_EQUAL(til::point(1, 1), Anchor(fixture));

            const auto wide = Create(true, L"\x4e2d z\r");
            Enter(wide);
            Key(wide, 'W');
            VERIFY_ARE_EQUAL(til::point(3, 0), Anchor(wide));
            Key(wide, 'W');
            VERIFY_ARE_EQUAL(til::point(3, 0), Anchor(wide));
        }

        TEST_METHOD(VimParagraphMotionsAndBounds)
        {
            const auto fixture = Create(true, L"alpha\r\nbeta\r\n\r\n\r\n  gamma\r\n   \r\ndelta\r\n\r\nend\r\n\r\n");
            Enter(fixture);
            Position(fixture, { 2, 1 });
            for (const auto expected : { til::point(0, 2), til::point(0, 5), til::point(0, 7), til::point(2, 8), til::point(2, 8) })
            {
                Paragraph(fixture, true);
                VERIFY_ARE_EQUAL(expected, Anchor(fixture));
            }
            for (const auto expected : { til::point(0, 7), til::point(0, 5), til::point(0, 3), til::point(0, 0), til::point(0, 0) })
            {
                Paragraph(fixture, false);
                VERIFY_ARE_EQUAL(expected, Anchor(fixture));
            }
            // Paragraph jumps reset the column remembered by vertical motion.
            Position(fixture, { 4, 0 });
            Key(fixture, 'J');
            Paragraph(fixture, true);
            Key(fixture, 'J');
            Key(fixture, 'J');
            VERIFY_ARE_EQUAL(til::point(0, 4), Anchor(fixture));
            VERIFY_IS_TRUE(fixture.Connection->Input.empty());
        }

        TEST_METHOD(VimParagraphVisualRanges)
        {
            const auto text = std::wstring(L"one\r\n\r\ntwo\r\n\r\n\x4e2d\U0001f600");
            const auto fixture = Create(true, text + L"\r");
            std::wstring copied;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
            Enter(fixture);
            Position(fixture, { 0, 0 });
            Key(fixture, 'V');
            Paragraph(fixture, true);
            VERIFY_ARE_EQUAL(til::point(0, 1), DisplayCursor(fixture)->position);
            Paragraph(fixture, true);
            Paragraph(fixture, true);
            VERIFY_ARE_EQUAL(til::point(2, 4), DisplayCursor(fixture)->position);
            VERIFY_ARE_EQUAL(til::point(4, 4), Active(fixture));
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(text, copied);
            VERIFY_ARE_EQUAL(til::point(2, 4), Anchor(fixture));

            Key(fixture, 'V');
            Paragraph(fixture, false);
            Paragraph(fixture, false);
            Paragraph(fixture, false);
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(text, copied);
            VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));

            // Cross the anchor in both directions after an inclusive word motion.
            Position(fixture, { 0, 2 });
            Key(fixture, 'V');
            Key(fixture, 'E');
            Paragraph(fixture, false);
            VERIFY_ARE_EQUAL(til::point(0, 1), Active(fixture));
            Paragraph(fixture, true);
            VERIFY_ARE_EQUAL(til::point(0, 3), Active(fixture));
            Paragraph(fixture, false);
            Key(fixture, VK_ESCAPE);
            VERIFY_ARE_EQUAL(til::point(0, 1), Anchor(fixture));
        }

        TEST_METHOD(VimParagraphSoftWrapAndSpaces)
        {
            const auto fixture = Create(true, L"");
            const auto width = fixture.Core->_terminal->GetTextBuffer().GetSize().Width();
            fixture.Connection->Output(std::wstring(width, L'a') + std::wstring(width, L' ') + L"tail\r\n\r\nend\r");
            Enter(fixture);
            Position(fixture, { 0, 0 });
            Paragraph(fixture, true);
            VERIFY_ARE_EQUAL(til::point(0, 3), Anchor(fixture));
            Paragraph(fixture, false);
            VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));

            // The last continuation row can be empty after trimming spaces.
            fixture.Core->ExitVimMode();
            fixture.Connection->Output(L"\x1b[2J\x1b[H" + std::wstring(width, L'a') + L"   \r\nnext\r\n\r\nend\r");
            Enter(fixture);
            Position(fixture, { 0, 0 });
            Paragraph(fixture, true);
            VERIFY_ARE_EQUAL(til::point(0, 3), Anchor(fixture));
            Paragraph(fixture, false);
            VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));
        }

        TEST_METHOD(VimParagraphKeysRespectModesAndModifiers)
        {
            using Modifiers = ::Microsoft::Terminal::Core::ControlKeyStates;
            const auto fixture = Create(true, L"one\r\n\r\ntwo\r");
            for (const auto key : std::array<WORD, 2>{ VK_OEM_4, VK_OEM_6 })
            {
                VERIFY_IS_FALSE(fixture.Core->TryVimModeKeybinding(key, Modifiers::ShiftPressed));
            }
            Enter(fixture);
            Position(fixture, { 0, 0 });
            Key(fixture, VK_OEM_4);
            Key(fixture, VK_OEM_6);
            Key(fixture, 'L', Modifiers::ShiftPressed);
            VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));
            for (const auto modifier : std::array<DWORD, 3>{ LEFT_CTRL_PRESSED, LEFT_ALT_PRESSED, LEFT_WIN_PRESSED })
            {
                for (const auto key : std::array<WORD, 2>{ VK_OEM_4, VK_OEM_6 })
                {
                    VERIFY_IS_FALSE(fixture.Core->TryVimModeKeybinding(key, Modifiers{ modifier | SHIFT_PRESSED }));
                }
            }
            VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));
            Paragraph(fixture, true);
            VERIFY_ARE_EQUAL(til::point(0, 1), Anchor(fixture));
            fixture.Connection->Output(L"\x1b[?1049h");
            for (const auto key : std::array<WORD, 2>{ VK_OEM_4, VK_OEM_6 })
            {
                VERIFY_IS_FALSE(fixture.Core->TryVimModeKeybinding(key, Modifiers::ShiftPressed));
            }
        }

        TEST_METHOD(VimWordMotionsAndDelimiters)
        {
            const auto fixture = Create(true, L"foo_bar::baz  qux\r");
            Enter(fixture);
            for (const auto column : { 6, 8, 11, 16, 16 })
            {
                Key(fixture, 'E');
                VERIFY_ARE_EQUAL(til::point(column, 0), Anchor(fixture));
            }
            for (const auto column : { 14, 9, 7, 0, 0 })
            {
                Key(fixture, 'B');
                VERIFY_ARE_EQUAL(til::point(column, 0), Anchor(fixture));
            }
            fixture.Core->ExitVimMode();
            fixture.Settings->WordDelimiters(L" ");
            fixture.Core->UpdateSettings(*fixture.Settings, *fixture.Settings);
            Enter(fixture);
            Key(fixture, 'E');
            VERIFY_ARE_EQUAL(til::point(11, 0), Anchor(fixture));
            VERIFY_IS_TRUE(fixture.Connection->Input.empty());
        }

        TEST_METHOD(VimVisualSelectsSinglePeriod)
        {
            for (const auto period : { std::wstring_view{ L"." }, std::wstring_view{ L"\x3002" } })
            {
                const auto fixture = Create(true, std::wstring{ period } + L"\r");
                std::wstring copied;
                fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
                Enter(fixture);
                Key(fixture, 'V');
                VERIFY_IS_TRUE(fixture.Core->HasSelection());
                Key(fixture, 'Y');
                VERIFY_ARE_EQUAL(std::wstring{ period }, copied);
                VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Normal);
                VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));
                VERIFY_IS_TRUE(fixture.Connection->Input.empty());
            }
        }

        TEST_METHOD(VimVisualCharacterMotionsIncludePeriod)
        {
            for (const auto period : { std::wstring_view{ L"." }, std::wstring_view{ L"\x3002" } })
            {
                const auto text = std::wstring{ L"ab" } + std::wstring{ period };
                const auto fixture = Create(true, text + L"\r");
                std::wstring copied;
                fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
                Enter(fixture);
                Key(fixture, 'V');
                Key(fixture, 'L');
                Key(fixture, VK_RIGHT);
                VERIFY_ARE_EQUAL(til::point(2, 0), DisplayCursor(fixture)->position);
                Key(fixture, 'Y');
                VERIFY_ARE_EQUAL(text, copied);
                VERIFY_ARE_EQUAL(til::point(2, 0), Anchor(fixture));

                Key(fixture, 'V');
                Key(fixture, 'H');
                Key(fixture, VK_LEFT);
                Key(fixture, 'Y');
                VERIFY_ARE_EQUAL(text, copied);
                VERIFY_ARE_EQUAL(til::point(0, 0), Anchor(fixture));

                Position(fixture, { 2, 0 });
                Key(fixture, 'V');
                Key(fixture, 'H');
                Key(fixture, 'L');
                Key(fixture, 'Y');
                VERIFY_ARE_EQUAL(std::wstring{ period }, copied);
                VERIFY_ARE_EQUAL(til::point(2, 0), Anchor(fixture));
            }
        }

        TEST_METHOD(VimWordSelectionIncludesEndpoints)
        {
            const auto fixture = Create(true, L"hello world\r");
            std::wstring copied;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
            Enter(fixture);
            Key(fixture, 'V');
            Key(fixture, 'E');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"hello"), copied);
            VERIFY_ARE_EQUAL(til::point(4, 0), Anchor(fixture));
            Key(fixture, 'E');
            Key(fixture, 'V');
            Key(fixture, 'B');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"world"), copied);
            VERIFY_ARE_EQUAL(til::point(6, 0), Anchor(fixture));

            Key(fixture, 'V');
            Key(fixture, 'B');
            VERIFY_ARE_EQUAL(til::point(0, 0), Active(fixture));
            Key(fixture, 'E');
            VERIFY_ARE_EQUAL(til::point(4, 0), Active(fixture));
            Key(fixture, 'E');
            Key(fixture, 'E'); // End of output is stable under repeat.
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"world"), copied);
            VERIFY_ARE_EQUAL(til::point(10, 0), Anchor(fixture));
            Position(fixture, { 4, 0 });
            Key(fixture, 'V');
            Key(fixture, 'B');
            for (int i = 0; i < 5; ++i)
            {
                Key(fixture, 'L');
            }
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"o "), copied);
            VERIFY_ARE_EQUAL(til::point(5, 0), Anchor(fixture));
            Position(fixture, { 0, 0 });
            Key(fixture, 'V');
            Key(fixture, 'E');
            VERIFY_IS_TRUE(fixture.Core->SwitchSelectionEndpoint());
            Key(fixture, 'E');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"o"), copied);
        }

        TEST_METHOD(VimVisualBoundsAndPreferredColumn)
        {
            const auto fixture = Create(true, L"abcdef\r\nxy\r\n\r\n  z  q   \r");
            Enter(fixture);
            Position(fixture, { 5, 0 });
            Key(fixture, 'V');
            Key(fixture, 'J');
            VERIFY_ARE_EQUAL(til::point(2, 1), Active(fixture));
            Key(fixture, 'J');
            VERIFY_ARE_EQUAL(til::point(0, 2), Active(fixture));
            Key(fixture, 'J');
            VERIFY_ARE_EQUAL(til::point(6, 3), Active(fixture));
            VERIFY_ARE_EQUAL(til::point(5, 3), DisplayCursor(fixture)->position);
            for (int i = 0; i < 4; ++i)
            {
                Key(fixture, 'L');
                Key(fixture, 'J');
            }
            VERIFY_ARE_EQUAL(til::point(6, 3), Active(fixture));
            Key(fixture, 'K');
            VERIFY_ARE_EQUAL(til::point(0, 2), Active(fixture));
            Key(fixture, 'K');
            VERIFY_ARE_EQUAL(til::point(2, 1), Active(fixture));
            Key(fixture, 'K');
            VERIFY_ARE_EQUAL(til::point(6, 0), Active(fixture));
        }

        TEST_METHOD(VimVisualEntryAndInteriorWhitespace)
        {
            const auto fixture = Create(true, L"  a  b\r\n\r\nc  \r");
            std::wstring copied;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
            Enter(fixture);
            Position(fixture, { 15, 4 });
            Key(fixture, 'V');
            VERIFY_ARE_EQUAL(til::point(1, 2), Active(fixture));
            VERIFY_ARE_EQUAL(til::point(0, 2), DisplayCursor(fixture)->position);
            Key(fixture, 'L');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"c"), copied);
            Position(fixture, { 0, 0 });
            Key(fixture, 'V');
            Key(fixture, 'J');
            Key(fixture, 'J');
            Key(fixture, 'L');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"  a  b\r\n\r\nc"), copied);
        }

        TEST_METHOD(VimEmptyAndErasedOutput)
        {
            const auto fixture = Create(true, L"");
            auto copies = 0;
            std::wstring copied;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) {
                ++copies;
                copied = args.Plain();
            });
            Enter(fixture);
            Paragraph(fixture, false);
            Paragraph(fixture, true);
            Key(fixture, 'V');
            Key(fixture, 'B');
            Key(fixture, 'E');
            Key(fixture, 'Y');
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Normal);
            VERIFY_ARE_EQUAL(0, copies);
            fixture.Connection->Output(L"abcdef\r");
            Key(fixture, 'V');
            for (int i = 0; i < 6; ++i)
            {
                Key(fixture, 'L');
            }
            fixture.Connection->Output(L"\x1b[H\x1b[Kxy\r");
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"xy"), copied);
            VERIFY_ARE_EQUAL(1, copies);
        }

        TEST_METHOD(VimWordAndVisualGlyphBoundaries)
        {
            const auto fixture = Create(true, L"a\x4e2d\U0001f600" L"e\x0301 z\r");
            std::wstring copied;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
            Enter(fixture);
            Key(fixture, 'V');
            Key(fixture, 'E');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"a\x4e2d\U0001f600" L"e\x0301"), copied);
            Position(fixture, { 2, 0 }); // Trailing cell of a wide glyph.
            Key(fixture, 'V');
            VERIFY_ARE_EQUAL(til::point(3, 0), Active(fixture));
            VERIFY_ARE_EQUAL(til::point(1, 0), DisplayCursor(fixture)->position);
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"\x4e2d"), copied);
            Key(fixture, 'V');
            Key(fixture, 'L');
            VERIFY_ARE_EQUAL(til::point(5, 0), Active(fixture));
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"\x4e2d\U0001f600"), copied);
        }

        TEST_METHOD(VimWordSoftWrapAndHardNewline)
        {
            const auto fixture = Create(true, L"");
            const auto width = fixture.Core->_terminal->GetTextBuffer().GetSize().Width();
            const auto longWord = std::wstring(width + 4, L'a');
            fixture.Connection->Output(longWord + L" zz\r");
            std::wstring copied;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
            Enter(fixture);
            Position(fixture, { 0, 0 });
            Key(fixture, 'E');
            VERIFY_ARE_EQUAL(til::point(3, 1), Anchor(fixture));
            Key(fixture, 'V');
            Key(fixture, 'B');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(longWord, copied);
            fixture.Core->ExitVimMode();
            fixture.Connection->Output(L"\x1b[2J\x1b[H" + std::wstring(width, L'a') + L"\r\nbbbb\r");
            Enter(fixture);
            Position(fixture, { 0, 0 });
            Key(fixture, 'E');
            VERIFY_ARE_EQUAL(til::point(width - 1, 0), Anchor(fixture));
            Key(fixture, 'E');
            VERIFY_ARE_EQUAL(til::point(3, 1), Anchor(fixture));
        }

        TEST_METHOD(VimWordEndpointSurvivesBufferRotation)
        {
            const auto fixture = Create(true, L"");
            const auto height = fixture.Core->_terminal->GetTextBufferEndPosition().y + 1;
            for (int i = 0; i < height - 1; ++i)
            {
                fixture.Connection->Output(L"one two\r\n");
            }
            std::wstring copied;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
            Enter(fixture);
            Position(fixture, { 0, height - 2 });
            Key(fixture, 'V');
            Key(fixture, 'E');
            fixture.Connection->Output(L"tail\r\n");
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"one"), copied);
            VERIFY_ARE_EQUAL(til::point(2, height - 3), Anchor(fixture));
        }

        TEST_METHOD(VimModesNavigateAndCopy)
        {
            const auto fixture = Create();
            std::wstring copied;
            auto copies = 0;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) {
                copied = args.Plain();
                ++copies;
            });
            Enter(fixture);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Normal);
            Key(fixture, 'L');
            Key(fixture, 'L');
            VERIFY_ARE_EQUAL(til::point(2, 0), Anchor(fixture));
            Key(fixture, 'V');
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Visual);
            Key(fixture, 'L');
            Key(fixture, 'L');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"cde"), copied);
            VERIFY_ARE_EQUAL(1, copies);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Normal);
            VERIFY_ARE_EQUAL(til::point(4, 0), Anchor(fixture));
            Key(fixture, 'V');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"e"), copied);
            VERIFY_ARE_EQUAL(2, copies);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Normal);
            VERIFY_ARE_EQUAL(til::point(4, 0), Anchor(fixture));
            Key(fixture, 'X');
            Key(fixture, 'W');
            VERIFY_IS_TRUE(fixture.Connection->Input.empty());
            Key(fixture, 'I');
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Insert);
            VERIFY_IS_FALSE(fixture.Core->HasSelection());
            fixture.Core->SendCharEvent(L'x', 0, {});
            VERIFY_ARE_EQUAL(std::wstring(L"x"), fixture.Connection->Input);
        }

        TEST_METHOD(VimReverseSelectionRetainsPosition)
        {
            const auto fixture = Create();
            std::wstring copied;
            fixture.Core->WriteToClipboard([&](auto&&, const Control::WriteToClipboardEventArgs& args) { copied = args.Plain(); });
            Enter(fixture);
            for (int index = 0; index < 3; ++index)
            {
                Key(fixture, 'L');
            }
            Key(fixture, 'V');
            Key(fixture, 'H');
            Key(fixture, 'H');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"bcd"), copied);
            VERIFY_ARE_EQUAL(til::point(1, 0), Anchor(fixture));
            Key(fixture, 'V');
            Key(fixture, 'L');
            Key(fixture, VK_ESCAPE);
            VERIFY_ARE_EQUAL(til::point(2, 0), Anchor(fixture));
            Key(fixture, 'H');
            VERIFY_ARE_EQUAL(til::point(1, 0), Anchor(fixture));
        }

        TEST_METHOD(VimAlternateScreenPassesInputThrough)
        {
            const auto fixture = Create();
            Enter(fixture);
            fixture.Connection->Output(L"\x1b[?1049h");
            VERIFY_IS_FALSE(fixture.Core->VimModeAvailable());
            VERIFY_IS_FALSE(fixture.Core->EnterVimMode());
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Insert);
            for (const auto key : std::array<WORD, 12>{ VK_ESCAPE, 'I', 'A', 'V', 'Y', 'H', 'J', 'K', 'L', 'B', 'W', 'E' })
            {
                VERIFY_IS_FALSE(fixture.Core->TryVimModeKeybinding(static_cast<WORD>(key), {}));
            }
            for (const auto character : std::wstring_view{ L"\x1biavyhjklbwe" })
            {
                fixture.Core->SendCharEvent(character, 0, {});
            }
            VERIFY_ARE_EQUAL(std::wstring(L"\x1biavyhjklbwe"), fixture.Connection->Input);
            fixture.Connection->Output(L"\x1b[?1049l");
            VERIFY_IS_TRUE(fixture.Core->VimModeAvailable());
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Insert);
            Enter(fixture);
        }

        TEST_METHOD(VimSettingsReloadAndPaneIsolation)
        {
            const auto first = Create();
            const auto second = Create(false);
            VERIFY_IS_FALSE(second.Core->VimModeAvailable());
            VERIFY_IS_FALSE(second.Core->EnterVimMode());
            VERIFY_IS_FALSE(second.Core->TryVimModeKeybinding(VK_ESCAPE, {}));
            Enter(first);
            Key(first, 'V');
            Key(first, 'L');
            VERIFY_IS_TRUE(second.Core->VimModeState() == Control::VimMode::Insert);
            first.Settings->VimModeEnabled(false);
            first.Core->UpdateSettings(*first.Settings, *first.Settings);
            VERIFY_IS_FALSE(first.Core->VimModeAvailable());
            VERIFY_IS_TRUE(first.Core->VimModeState() == Control::VimMode::Insert);
            VERIFY_IS_FALSE(first.Core->HasSelection());
            first.Settings->VimModeEnabled(true);
            first.Core->UpdateSettings(*first.Settings, *first.Settings);
            VERIFY_IS_TRUE(first.Core->VimModeAvailable());
            VERIFY_IS_TRUE(first.Core->VimModeState() == Control::VimMode::Insert);
            Enter(first);
            VERIFY_IS_FALSE(second.Core->VimModeAvailable());
        }

        TEST_METHOD(VimLegacySelectionResizeAndRotation)
        {
            const auto fixture = Create();
            fixture.Core->ToggleMarkMode();
            VERIFY_IS_TRUE(fixture.Core->TryMarkModeKeybinding(VK_DOWN, {}));
            VERIFY_ARE_EQUAL(til::point(0, 1), Anchor(fixture));
            VERIFY_IS_FALSE(fixture.Core->EnterVimMode());
            VERIFY_IS_FALSE(fixture.Core->TryVimModeKeybinding(VK_ESCAPE, {}));
            VERIFY_IS_TRUE(fixture.Core->TryMarkModeKeybinding(VK_ESCAPE, {}));
            Enter(fixture);
            fixture.Core->SizeChanged(300, 380);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Insert);
            Enter(fixture);
            for (int index = 0; index < 40; ++index)
            {
                fixture.Connection->Output(L"line\r\n");
            }
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Insert);
            VERIFY_IS_FALSE(fixture.Core->HasSelection());
        }

        TEST_METHOD(VimPasteAndProtocolResponsesStillWork)
        {
            const auto fixture = Create();
            Enter(fixture);
            fixture.Connection->Output(L"\x1b[5n");
            VERIFY_ARE_EQUAL(std::wstring(L"\x1b[0n"), fixture.Connection->Input);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Normal);
            fixture.Connection->Input.clear();
            fixture.Core->PasteText(L"paste");
            VERIFY_ARE_EQUAL(std::wstring(L"paste"), fixture.Connection->Input);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Insert);
        }

        TEST_METHOD(VimInsertEscapePassesThrough)
        {
            const auto fixture = Create();
            for (int index = 0; index < 2; ++index)
            {
                VERIFY_IS_FALSE(fixture.Core->TryVimModeKeybinding(VK_ESCAPE, {}));
                // An unhandled key-down delegates to the Windows character
                // event; it must not be mistaken for a consumed Vim command.
                VERIFY_IS_FALSE(fixture.Core->TrySendKeyEvent(VK_ESCAPE, 1, {}, true));
                VERIFY_IS_TRUE(fixture.Core->SendCharEvent(L'\x1b', 1, {}));
                fixture.Core->TrySendKeyEvent(VK_ESCAPE, 1, {}, false);
            }
            VERIFY_ARE_EQUAL(std::wstring(L"\x1b\x1b"), fixture.Connection->Input);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Insert);
            VERIFY_IS_FALSE(fixture.Core->TryVimModeKeybinding('W', {}));
            fixture.Core->SendCharEvent(L'w', 17, {});
            VERIFY_ARE_EQUAL(std::wstring(L"\x1b\x1bw"), fixture.Connection->Input);
            Enter(fixture);
            Key(fixture, 'L');
            Enter(fixture);
            VERIFY_ARE_EQUAL(til::point(1, 0), Anchor(fixture));
            Key(fixture, 'V');
            Enter(fixture);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Visual);
        }

        TEST_METHOD(VimConsumedKeyLifecycle)
        {
            Control::implementation::VimInputState input;
            VERIFY_IS_TRUE(Control::implementation::VimInputState::IsMovement('B'));
            VERIFY_IS_TRUE(Control::implementation::VimInputState::IsMovement('W'));
            VERIFY_IS_TRUE(Control::implementation::VimInputState::IsMovement('E'));
            VERIFY_IS_TRUE(Control::implementation::VimInputState::IsMovement(VK_OEM_4));
            VERIFY_IS_TRUE(Control::implementation::VimInputState::IsMovement(VK_OEM_6));
            input.Consume(VK_OEM_4, 26);
            VERIFY_IS_TRUE(input.SuppressCharacter(L'{', 0, false));
            VERIFY_IS_TRUE(input.Release(VK_OEM_4));
            input.Consume(VK_OEM_6, 27);
            VERIFY_IS_TRUE(input.SuppressText(L"}", false));
            VERIFY_IS_TRUE(input.Release(VK_OEM_6));
            input.Consume('W', 17);
            VERIFY_IS_TRUE(input.SuppressCharacter(L'w', 17, true));
            VERIFY_IS_TRUE(input.Release('W'));
            input.Consume('I', 23);
            VERIFY_IS_TRUE(input.IsConsumed('I'));
            VERIFY_IS_TRUE(input.Release('I'));
            VERIFY_IS_TRUE(input.SuppressCharacter(L'i', 23, false));
            VERIFY_IS_FALSE(input.SuppressCharacter(L'i', 23, false));
            input.Consume('A', 30);
            VERIFY_IS_TRUE(input.SuppressText(L"a", false));
            VERIFY_IS_TRUE(input.Release('A'));
            input.Consume('I', 23);
            input.Release('I');
            input.UnhandledKeyDown();
            VERIFY_IS_FALSE(input.SuppressText(L"i", false));
            VERIFY_IS_TRUE(input.SuppressText(L"text", true));
            input.Consume('V', 47);
            input.Reset();
            VERIFY_IS_FALSE(input.IsConsumed('V'));
            VERIFY_IS_FALSE(input.SuppressText(L"v", false));
            VERIFY_IS_FALSE(input.Release('V'));
        }
    };
}
