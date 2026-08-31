// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr DWORD ChildSleepMilliseconds{ 30000 };

    std::optional<std::wstring_view> _valueAfter(
        const int argc,
        wchar_t* const* const argv,
        const std::wstring_view name)
    {
        for (int index = 1; index + 1 < argc; ++index)
        {
            if (std::wstring_view{ argv[index] } == name)
            {
                return std::wstring_view{ argv[index + 1] };
            }
        }
        return std::nullopt;
    }

    std::optional<uint32_t> _unsignedValue(
        const std::optional<std::wstring_view> value)
    {
        if (!value || value->empty())
        {
            return std::nullopt;
        }
        uint64_t parsed{};
        for (const auto character : *value)
        {
            if (character < L'0' || character > L'9')
            {
                return std::nullopt;
            }
            parsed = parsed * 10 + static_cast<uint64_t>(character - L'0');
            if (parsed > UINT32_MAX)
            {
                return std::nullopt;
            }
        }
        return static_cast<uint32_t>(parsed);
    }

    std::string _hexWide(const std::wstring_view value)
    {
        constexpr std::string_view digits{ "0123456789abcdef" };
        std::string result;
        result.reserve(value.size() * 4);
        for (const auto character : value)
        {
            const auto codeUnit = static_cast<uint16_t>(character);
            result.push_back(digits[(codeUnit >> 12) & 0x0f]);
            result.push_back(digits[(codeUnit >> 8) & 0x0f]);
            result.push_back(digits[(codeUnit >> 4) & 0x0f]);
            result.push_back(digits[codeUnit & 0x0f]);
        }
        return result;
    }

    bool _write(const HANDLE handle, const std::string_view value)
    {
        size_t offset{};
        while (offset < value.size())
        {
            DWORD written{};
            const auto remaining = value.size() - offset;
            const auto chunk = static_cast<DWORD>(
                (std::min)(remaining, static_cast<size_t>(UINT32_MAX)));
            if (!WriteFile(
                    handle,
                    value.data() + offset,
                    chunk,
                    &written,
                    nullptr) ||
                written == 0)
            {
                return false;
            }
            offset += written;
        }
        return true;
    }

    std::optional<std::wstring> _environment(const wchar_t* name)
    {
        const auto required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0 || required > 4097)
        {
            return std::nullopt;
        }
        std::wstring value(required, L'\0');
        const auto written = GetEnvironmentVariableW(
            name,
            value.data(),
            required);
        if (written == 0 || written >= required)
        {
            return std::nullopt;
        }
        value.resize(written);
        return value;
    }

    bool _writeEcho(const int argc, wchar_t* const* const argv)
    {
        const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
        std::wstring currentDirectory(32768, L'\0');
        const auto currentLength = GetCurrentDirectoryW(
            static_cast<DWORD>(currentDirectory.size()),
            currentDirectory.data());
        if (currentLength == 0 || currentLength >= currentDirectory.size())
        {
            return false;
        }
        currentDirectory.resize(currentLength);
        if (!_write(output, "CWD=" + _hexWide(currentDirectory) + "\n"))
        {
            return false;
        }
        for (int index = 0; index < argc; ++index)
        {
            const auto line = "ARG" + std::to_string(index) + "=" +
                              _hexWide(argv[index]) + "\n";
            if (!_write(output, line))
            {
                return false;
            }
        }
        constexpr std::array<const wchar_t*, 8> names{
            L"GIT_DIR",
            L"GIT_WORK_TREE",
            L"GIT_TERMINAL_PROMPT",
            L"GCM_INTERACTIVE",
            L"GIT_CONFIG_COUNT",
            L"GIT_OPTIONAL_LOCKS",
            L"GIT_AUTHOR_NAME",
            L"GIT_AUTHOR_EMAIL",
        };
        for (const auto name : names)
        {
            const auto value = _environment(name);
            const auto line = "ENV_" + _hexWide(name) + "=" +
                              (value ? _hexWide(*value) : "MISSING") + "\n";
            if (!_write(output, line))
            {
                return false;
            }
        }
        return true;
    }

    std::wstring _quote(const std::wstring_view value)
    {
        std::wstring result{ L"\"" };
        size_t backslashes{};
        for (const auto character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    int _grandchild(const std::wstring_view eventName)
    {
        const auto event = OpenEventW(
            EVENT_MODIFY_STATE,
            FALSE,
            std::wstring{ eventName }.c_str());
        if (!event)
        {
            return 20;
        }
        const auto signaled = SetEvent(event);
        CloseHandle(event);
        if (!signaled)
        {
            return 21;
        }
        Sleep(ChildSleepMilliseconds);
        return 0;
    }

    int _spawnTree(const std::wstring_view eventName)
    {
        std::wstring executable(32768, L'\0');
        const auto written = GetModuleFileNameW(
            nullptr,
            executable.data(),
            static_cast<DWORD>(executable.size()));
        if (written == 0 || written >= executable.size())
        {
            return 22;
        }
        executable.resize(written);
        auto commandLine = _quote(executable) +
                           L" --test-mode grandchild --started-event " +
                           _quote(eventName);
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(
                executable.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &child))
        {
            return 23;
        }
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        const auto line = "GRANDCHILD_PID=" +
                          std::to_string(child.dwProcessId) + "\n";
        if (!_write(GetStdHandle(STD_OUTPUT_HANDLE), line))
        {
            return 24;
        }
        Sleep(ChildSleepMilliseconds);
        return 0;
    }
}

int wmain(const int argc, wchar_t* const* const argv)
{
    const auto mode = _valueAfter(argc, argv, L"--test-mode");
    if (!mode)
    {
        return 2;
    }
    if (*mode == L"echo")
    {
        return _writeEcho(argc, argv) ? 0 : 3;
    }
    if (*mode == L"sleep")
    {
        const auto milliseconds = _unsignedValue(
            _valueAfter(argc, argv, L"--milliseconds"));
        if (!milliseconds)
        {
            return 4;
        }
        Sleep(*milliseconds);
        return 0;
    }
    if (*mode == L"exit")
    {
        const auto code = _unsignedValue(_valueAfter(argc, argv, L"--code"));
        return code ? static_cast<int>(*code) : 5;
    }
    if (*mode == L"output")
    {
        const auto stdoutBytes = _unsignedValue(
            _valueAfter(argc, argv, L"--stdout-bytes"));
        const auto stderrBytes = _unsignedValue(
            _valueAfter(argc, argv, L"--stderr-bytes"));
        if (!stdoutBytes || !stderrBytes)
        {
            return 6;
        }
        const std::string stdoutValue(*stdoutBytes, 'o');
        const std::string stderrValue(*stderrBytes, 'e');
        return _write(GetStdHandle(STD_OUTPUT_HANDLE), stdoutValue) &&
                       _write(GetStdHandle(STD_ERROR_HANDLE), stderrValue) ?
                   0 :
                   7;
    }
    if (*mode == L"invalid-utf8")
    {
        const std::string value{ "\xff", 1 };
        return _write(GetStdHandle(STD_OUTPUT_HANDLE), value) ? 0 : 8;
    }
    if (*mode == L"tree" || *mode == L"grandchild")
    {
        const auto event = _valueAfter(argc, argv, L"--started-event");
        if (!event)
        {
            return 9;
        }
        return *mode == L"tree" ? _spawnTree(*event) : _grandchild(*event);
    }
    return 10;
}
