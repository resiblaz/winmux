// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeProcessTestFixtures.h"
#include "TaskRuntimeProcessWin32Support.h"

#include <span>
#include <stdexcept>
#include <system_error>

namespace TaskRuntimeLocalTests::ProcessFixtures::details
{
    namespace
    {
        [[noreturn]] void _throwLastError(const char* const operation)
        {
            throw std::system_error{
                static_cast<int>(GetLastError()),
                std::system_category(),
                operation };
        }

        std::filesystem::path _testModuleDirectory()
        {
            HMODULE module{};
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&_testModuleDirectory),
                    &module))
            {
                _throwLastError("GetModuleHandleExW(test module)");
            }

            std::vector<wchar_t> buffer(MAX_PATH);
            for (;;)
            {
                SetLastError(ERROR_SUCCESS);
                const auto copied = GetModuleFileNameW(
                    module,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()));
                if (copied == 0)
                {
                    _throwLastError("GetModuleFileNameW(test module)");
                }
                if (copied < buffer.size() - 1 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                {
                    return std::filesystem::path{ std::wstring_view{ buffer.data(), copied } }.parent_path();
                }
                if (buffer.size() >= 32768)
                {
                    throw std::runtime_error{ "Test module path exceeds the Windows path limit" };
                }
                buffer.resize((std::min)(buffer.size() * 2, size_t{ 32768 }));
            }
        }

        std::wstring _quoteWindowsArgument(const std::wstring_view argument)
        {
            if (!argument.empty() &&
                argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
            {
                return std::wstring{ argument };
            }

            std::wstring quoted{ L'\"' };
            size_t backslashes{};
            for (const auto character : argument)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'\"')
                {
                    quoted.append(backslashes * 2 + 1, L'\\');
                    quoted.push_back(L'\"');
                    backslashes = 0;
                    continue;
                }
                quoted.append(backslashes, L'\\');
                backslashes = 0;
                quoted.push_back(character);
            }
            quoted.append(backslashes * 2, L'\\');
            quoted.push_back(L'\"');
            return quoted;
        }

        std::vector<wchar_t> _commandLine(const std::span<const std::wstring> arguments)
        {
            std::wstring commandLine;
            for (const auto& argument : arguments)
            {
                if (!commandLine.empty())
                {
                    commandLine.push_back(L' ');
                }
                commandLine.append(_quoteWindowsArgument(argument));
            }
            std::vector<wchar_t> mutableCommandLine{ commandLine.begin(), commandLine.end() };
            mutableCommandLine.push_back(L'\0');
            return mutableCommandLine;
        }

        std::wstring _wideDiagnostic(const std::string_view text)
        {
            if (text.empty())
            {
                return L"<empty>";
            }
            const auto required = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                text.data(),
                static_cast<int>(text.size()),
                nullptr,
                0);
            if (required <= 0)
            {
                return std::wstring{ text.begin(), text.end() };
            }
            std::wstring result(static_cast<size_t>(required), L'\0');
            (void)MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                text.data(),
                static_cast<int>(text.size()),
                result.data(),
                required);
            return result;
        }

        std::wstring _processDiagnostic(
            const std::wstring_view prefix,
            const DWORD processId,
            const std::string_view standardError)
        {
            return std::wstring{ prefix } + L" (pid=" + std::to_wstring(processId) +
                   L"); stderr: " + _wideDiagnostic(standardError);
        }

        std::wstring _handleArgument(const HANDLE handle)
        {
            return std::to_wstring(
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)));
        }
    }

    std::filesystem::path TaskRuntimeExecutable()
    {
        const auto executable = _testModuleDirectory() / L"TaskRuntime.exe";
        if (!std::filesystem::is_regular_file(executable))
        {
            throw std::runtime_error{ "TaskRuntime.exe is not beside TaskRuntime.LocalTests.dll" };
        }
        return executable;
    }

    std::vector<wchar_t> BuildTaskRuntimeCommandLine(
        const std::filesystem::path& executable,
        const ProductRuntimeTestIdentity& identity,
        const HANDLE readyEvent,
        const HANDLE shutdownEvent)
    {
        const std::array<std::wstring, 10> arguments{
            executable.native(),
            L"--serve",
            L"--channel",
            identity.Channel(),
            L"--settings-root",
            identity.SettingsRoot().native(),
            L"--internal-test-ready-handle",
            _handleArgument(readyEvent),
            L"--internal-test-shutdown-handle",
            _handleArgument(shutdownEvent),
        };
        return _commandLine(arguments);
    }

    void LogProcessDiagnostic(
        const std::wstring_view prefix,
        const DWORD processId,
        const std::string_view standardError)
    {
        const auto message = _processDiagnostic(prefix, processId, standardError);
        WEX::Logging::Log::Comment(message.c_str());
    }

    void LogProcessErrorNoThrow(
        const std::wstring_view prefix,
        const DWORD processId,
        const std::string_view standardError) noexcept
    {
        try
        {
            const auto message = _processDiagnostic(prefix, processId, standardError);
            WEX::Logging::Log::Error(message.c_str());
        }
        catch (...)
        {
            try
            {
                WEX::Logging::Log::Error(L"TaskRuntime process cleanup failed without diagnostics");
            }
            catch (...)
            {
            }
        }
    }
}
