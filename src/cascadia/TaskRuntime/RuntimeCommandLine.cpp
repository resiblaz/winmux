// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeCommandLine.h"

#include <cerrno>
#include <cwchar>
#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        std::optional<uintptr_t> _parseHandle(const std::wstring_view text) noexcept
        {
            if (text.empty() || !std::ranges::all_of(text, [](const wchar_t value) {
                    return value >= L'0' && value <= L'9';
                }))
            {
                return std::nullopt;
            }

            errno = 0;
            wchar_t* end{};
            const auto value = std::wcstoull(text.data(), &end, 10);
            if (errno == ERANGE || end != text.data() + text.size() || value == 0 ||
                value > std::numeric_limits<uintptr_t>::max())
            {
                return std::nullopt;
            }
            return static_cast<uintptr_t>(value);
        }

        RuntimeCommandLine _invalid(std::wstring error) noexcept
        {
            RuntimeCommandLine result;
            result.Error = std::move(error);
            return result;
        }
    }

    RuntimeCommandLine ParseRuntimeCommandLine(const int argc, wchar_t* const* argv) noexcept
    {
        try
        {
            if (argc == 2 && argv && argv[1] && std::wstring_view{ argv[1] } == L"--help")
            {
                RuntimeCommandLine result;
                result.Succeeded = true;
                result.Mode = RuntimeCommandMode::Help;
                return result;
            }
            if (argc < 2 || !argv || !argv[1])
            {
                return _invalid(L"A Runtime mode is required");
            }

            RuntimeCommandLine result;
            const std::wstring_view mode{ argv[1] };
            if (mode == L"--bootstrap-check")
            {
                result.Mode = RuntimeCommandMode::BootstrapCheck;
            }
            else if (mode == L"--serve")
            {
                result.Mode = RuntimeCommandMode::Serve;
            }
            else
            {
                return _invalid(L"The Runtime mode is unknown");
            }

            bool channelSeen{};
            bool settingsRootSeen{};
            bool readyHandleSeen{};
            bool shutdownHandleSeen{};
            for (int index = 2; index < argc; index += 2)
            {
                if (index + 1 >= argc || !argv[index] || !argv[index + 1])
                {
                    return _invalid(L"Every Runtime option requires a value");
                }

                const std::wstring_view name{ argv[index] };
                const std::wstring_view value{ argv[index + 1] };
                if (name == L"--channel")
                {
                    if (channelSeen || value.empty())
                    {
                        return _invalid(L"The Runtime channel must be specified exactly once");
                    }
                    result.Channel = value;
                    channelSeen = true;
                }
                else if (name == L"--settings-root")
                {
                    if (settingsRootSeen || value.empty())
                    {
                        return _invalid(L"The settings root must be specified exactly once");
                    }
                    result.SettingsRoot = std::filesystem::path{ value };
                    if (!result.SettingsRoot.is_absolute())
                    {
                        return _invalid(L"The settings root must be an absolute path");
                    }
                    settingsRootSeen = true;
                }
                else if (name == L"--internal-test-ready-handle")
                {
                    if (result.Mode != RuntimeCommandMode::Serve || readyHandleSeen)
                    {
                        return _invalid(L"The test ready handle is not valid for this invocation");
                    }
                    result.TestReadyHandle = _parseHandle(value);
                    if (!result.TestReadyHandle)
                    {
                        return _invalid(L"The test ready handle is invalid");
                    }
                    readyHandleSeen = true;
                }
                else if (name == L"--internal-test-shutdown-handle")
                {
                    if (result.Mode != RuntimeCommandMode::Serve || shutdownHandleSeen)
                    {
                        return _invalid(L"The test shutdown handle is not valid for this invocation");
                    }
                    result.TestShutdownHandle = _parseHandle(value);
                    if (!result.TestShutdownHandle)
                    {
                        return _invalid(L"The test shutdown handle is invalid");
                    }
                    shutdownHandleSeen = true;
                }
                else
                {
                    return _invalid(L"An unknown Runtime option was supplied");
                }
            }

            if (!channelSeen || !settingsRootSeen)
            {
                return _invalid(L"Both --channel and --settings-root are required");
            }
            if (readyHandleSeen != shutdownHandleSeen)
            {
                return _invalid(L"The test ready and shutdown handles must be supplied together");
            }

            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            // Preserve the noexcept contract even under allocation failure.
            // The caller will still emit the stable usage text.
            return {};
        }
    }
}
