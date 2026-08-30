// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeCommandLine.h"

#include <optional>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        RuntimeCommandLine _parse(const std::vector<std::wstring>& arguments)
        {
            std::vector<std::wstring> storage{ arguments };
            std::vector<wchar_t*> argv;
            argv.reserve(storage.size());
            for (auto& argument : storage)
            {
                argv.emplace_back(argument.data());
            }
            return ParseRuntimeCommandLine(
                gsl::narrow<int>(argv.size()),
                argv.data());
        }

        void _logCase(const std::wstring_view name)
        {
            const auto message = L"Runtime command-line case: " + std::wstring{ name };
            WEX::Logging::Log::Comment(message.c_str());
        }
    }

    class RuntimeCommandLineTests
    {
        TEST_CLASS(RuntimeCommandLineTests);

        TEST_METHOD(AcceptsLegalModesAndPairedInternalHandles);
        TEST_METHOD(RejectsMalformedOptionsTable);
    };

    void RuntimeCommandLineTests::AcceptsLegalModesAndPairedInternalHandles()
    {
        struct LegalCase
        {
            std::wstring_view Name;
            std::vector<std::wstring> Arguments;
            RuntimeCommandMode Mode;
            std::wstring_view Channel;
            std::wstring_view SettingsRoot;
            std::optional<uintptr_t> ReadyHandle;
            std::optional<uintptr_t> ShutdownHandle;
            std::optional<uint32_t> IdleExitMilliseconds;
        };

        const std::vector<LegalCase> cases{
            {
                L"help",
                { L"TaskRuntime.exe", L"--help" },
                RuntimeCommandMode::Help,
                {},
                {},
                std::nullopt,
                std::nullopt,
                std::nullopt,
            },
            {
                L"bootstrap",
                {
                    L"TaskRuntime.exe",
                    L"--bootstrap-check",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\Task Runtime Tests",
                },
                RuntimeCommandMode::BootstrapCheck,
                L"dev",
                L"C:\\Task Runtime Tests",
                std::nullopt,
                std::nullopt,
                std::nullopt,
            },
            {
                L"serve options reordered",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--settings-root",
                    L"C:\\Task Runtime Tests",
                    L"--channel",
                    L"preview",
                },
                RuntimeCommandMode::Serve,
                L"preview",
                L"C:\\Task Runtime Tests",
                std::nullopt,
                std::nullopt,
                std::nullopt,
            },
            {
                L"serve paired internal handles",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\Task Runtime Tests",
                    L"--internal-test-ready-handle",
                    L"123",
                    L"--internal-test-shutdown-handle",
                    L"456",
                    L"--internal-test-idle-exit-ms",
                    L"250",
                },
                RuntimeCommandMode::Serve,
                L"dev",
                L"C:\\Task Runtime Tests",
                uintptr_t{ 123 },
                uintptr_t{ 456 },
                uint32_t{ 250 },
            },
        };

        for (const auto& test : cases)
        {
            _logCase(test.Name);
            const auto parsed = _parse(test.Arguments);
            VERIFY_IS_TRUE(parsed.Succeeded);
            VERIFY_IS_TRUE(parsed.Error.empty());
            VERIFY_IS_TRUE(parsed.Mode == test.Mode);
            VERIFY_ARE_EQUAL(std::wstring{ test.Channel }, parsed.Channel);
            VERIFY_ARE_EQUAL(
                std::filesystem::path{ test.SettingsRoot }.native(),
                parsed.SettingsRoot.native());
            VERIFY_IS_TRUE(parsed.TestReadyHandle == test.ReadyHandle);
            VERIFY_IS_TRUE(parsed.TestShutdownHandle == test.ShutdownHandle);
            VERIFY_IS_TRUE(
                parsed.TestIdleExitMilliseconds == test.IdleExitMilliseconds);
        }
    }

    void RuntimeCommandLineTests::RejectsMalformedOptionsTable()
    {
        struct InvalidCase
        {
            std::wstring_view Name;
            std::vector<std::wstring> Arguments;
        };

        const std::vector<InvalidCase> cases{
            {
                L"unknown mode",
                { L"TaskRuntime.exe", L"--bad" },
            },
            {
                L"missing required settings root",
                { L"TaskRuntime.exe", L"--serve", L"--channel", L"dev" },
            },
            {
                L"option missing value",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                },
            },
            {
                L"unknown option",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--unknown",
                    L"value",
                },
            },
            {
                L"duplicate channel",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--channel",
                    L"preview",
                },
            },
            {
                L"relative settings root",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"relative\\settings",
                },
            },
            {
                L"ready handle without shutdown handle",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-ready-handle",
                    L"123",
                },
            },
            {
                L"shutdown handle without ready handle",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-shutdown-handle",
                    L"456",
                },
            },
            {
                L"zero handle",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-ready-handle",
                    L"0",
                    L"--internal-test-shutdown-handle",
                    L"456",
                },
            },
            {
                L"positive sign handle",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-ready-handle",
                    L"+123",
                    L"--internal-test-shutdown-handle",
                    L"456",
                },
            },
            {
                L"negative sign handle",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-ready-handle",
                    L"-1",
                    L"--internal-test-shutdown-handle",
                    L"456",
                },
            },
            {
                L"overflow handle",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-ready-handle",
                    L"18446744073709551616",
                    L"--internal-test-shutdown-handle",
                    L"456",
                },
            },
            {
                L"bootstrap rejects internal handles",
                {
                    L"TaskRuntime.exe",
                    L"--bootstrap-check",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-ready-handle",
                    L"123",
                    L"--internal-test-shutdown-handle",
                    L"456",
                },
            },
            {
                L"idle exit requires lifecycle handles",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-idle-exit-ms",
                    L"250",
                },
            },
            {
                L"zero idle exit",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-ready-handle",
                    L"123",
                    L"--internal-test-shutdown-handle",
                    L"456",
                    L"--internal-test-idle-exit-ms",
                    L"0",
                },
            },
            {
                L"duplicate idle exit",
                {
                    L"TaskRuntime.exe",
                    L"--serve",
                    L"--channel",
                    L"dev",
                    L"--settings-root",
                    L"C:\\settings",
                    L"--internal-test-ready-handle",
                    L"123",
                    L"--internal-test-shutdown-handle",
                    L"456",
                    L"--internal-test-idle-exit-ms",
                    L"250",
                    L"--internal-test-idle-exit-ms",
                    L"251",
                },
            },
        };

        for (const auto& test : cases)
        {
            _logCase(test.Name);
            const auto parsed = _parse(test.Arguments);
            VERIFY_IS_FALSE(parsed.Succeeded);
            VERIFY_IS_FALSE(parsed.Error.empty());
        }
    }
}
