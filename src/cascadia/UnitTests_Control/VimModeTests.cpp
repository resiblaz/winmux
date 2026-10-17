// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "../TerminalControl/ControlCore.h"
#include "../TerminalControl/VimInputState.h"
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

        void Key(const Fixture& fixture, const WORD key)
        {
            VERIFY_IS_TRUE(fixture.Core->TryVimModeKeybinding(key, {}));
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
            VERIFY_ARE_EQUAL(til::point(5, 3), Active(fixture));
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
            Key(fixture, 'H');
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
            VERIFY_ARE_EQUAL(til::point(1, 0), Active(fixture));
            Key(fixture, 'L');
            VERIFY_ARE_EQUAL(til::point(3, 0), Active(fixture));
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"\x4e2d"), copied);
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
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(0, copies);
            VERIFY_IS_TRUE(fixture.Core->VimModeState() == Control::VimMode::Visual);
            Key(fixture, 'L');
            Key(fixture, 'L');
            Key(fixture, 'Y');
            VERIFY_ARE_EQUAL(std::wstring(L"cd"), copied);
            VERIFY_ARE_EQUAL(1, copies);
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
            VERIFY_ARE_EQUAL(std::wstring(L"bc"), copied);
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
            for (const auto key : std::array<WORD, 11>{ VK_ESCAPE, 'I', 'A', 'V', 'Y', 'H', 'J', 'K', 'L', 'B', 'E' })
            {
                VERIFY_IS_FALSE(fixture.Core->TryVimModeKeybinding(static_cast<WORD>(key), {}));
            }
            for (const auto character : std::wstring_view{ L"\x1biavyhjklbe" })
            {
                fixture.Core->SendCharEvent(character, 0, {});
            }
            VERIFY_ARE_EQUAL(std::wstring(L"\x1biavyhjklbe"), fixture.Connection->Input);
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
            VERIFY_IS_TRUE(Control::implementation::VimInputState::IsMovement('E'));
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
