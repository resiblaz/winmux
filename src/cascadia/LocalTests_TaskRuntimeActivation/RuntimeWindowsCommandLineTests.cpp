// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeWindowsCommandLine.h"

#include <shellapi.h>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    namespace
    {
        std::vector<std::wstring> _parse(const RuntimeWindowsCommandLineResult& commandLine)
        {
            VERIFY_IS_TRUE(commandLine.Succeeded());
            int count{};
            auto parsed = CommandLineToArgvW(commandLine.MutableCommandLine.data(), &count);
            VERIFY_IS_NOT_NULL(parsed);
            std::vector<std::wstring> arguments;
            if (parsed)
            {
                arguments.reserve(static_cast<size_t>(count));
                for (int index = 0; index < count; ++index)
                {
                    arguments.emplace_back(parsed[index]);
                }
                LocalFree(parsed);
            }
            return arguments;
        }

        void _verifyRoundTrip(const std::vector<std::wstring>& arguments)
        {
            const auto encoded = BuildRuntimeWindowsCommandLine(arguments);
            const auto decoded = _parse(encoded);
            VERIFY_ARE_EQUAL(arguments.size(), decoded.size());
            if (arguments.size() == decoded.size())
            {
                for (size_t index = 0; index < arguments.size(); ++index)
                {
                    VERIFY_ARE_EQUAL(arguments[index], decoded[index]);
                }
            }
        }
    }

    class RuntimeWindowsCommandLineTests
    {
        TEST_CLASS(RuntimeWindowsCommandLineTests);

        TEST_METHOD(RoundTripsWindowsArgumentEdgeCases);
        TEST_METHOD(AcceptsExact32767CharacterBoundary);
        TEST_METHOD(RejectsOversizeEmptyAndEmbeddedNullArguments);
    };

    void RuntimeWindowsCommandLineTests::RoundTripsWindowsArgumentEdgeCases()
    {
        _verifyRoundTrip({
            LR"(C:\Program Files\Richer Terminal\TaskRuntime.exe)",
            L"--settings-root",
            LR"(C:\用户 数据\终端)",
            L"",
            L"plain",
            L"space separated",
            LR"(embedded"quote)",
            LR"(slashes\\\"before-quote)",
            LR"(trailing\\)",
            L"\t\r\n",
        });
        _verifyRoundTrip({ L"TaskRuntime.exe", L"\\", L"\"", L"\\\"", L"\\\\\"" });
    }

    void RuntimeWindowsCommandLineTests::AcceptsExact32767CharacterBoundary()
    {
        const std::vector arguments{ std::wstring(
            MaximumRuntimeWindowsCommandLineCharacters - 1,
            L'x') };
        const auto result = BuildRuntimeWindowsCommandLine(arguments);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(MaximumRuntimeWindowsCommandLineCharacters, result.MutableCommandLine.size());
        VERIFY_ARE_EQUAL(L'\0', result.MutableCommandLine.back());
        const auto parsed = _parse(result);
        VERIFY_ARE_EQUAL(size_t{ 1 }, parsed.size());
        if (!parsed.empty())
        {
            VERIFY_ARE_EQUAL(arguments.front(), parsed.front());
        }
    }

    void RuntimeWindowsCommandLineTests::RejectsOversizeEmptyAndEmbeddedNullArguments()
    {
        const std::vector oversized{ std::wstring(
            MaximumRuntimeWindowsCommandLineCharacters,
            L'x') };
        const auto tooLong = BuildRuntimeWindowsCommandLine(oversized);
        VERIFY_IS_TRUE(tooLong.Status == RuntimeWindowsCommandLineStatus::TooLong);
        VERIFY_IS_FALSE(tooLong.Succeeded());

        VERIFY_IS_TRUE(BuildRuntimeWindowsCommandLine(std::span<const std::wstring>{}).Status ==
                       RuntimeWindowsCommandLineStatus::InvalidArgument);
        const std::vector<std::wstring> emptyExecutable{ L"", L"argument" };
        VERIFY_IS_TRUE(BuildRuntimeWindowsCommandLine(emptyExecutable).Status ==
                       RuntimeWindowsCommandLineStatus::InvalidArgument);

        const std::vector<std::wstring> embeddedNull{
            L"TaskRuntime.exe",
            std::wstring{ L'a', L'\0', L'b' },
        };
        VERIFY_IS_TRUE(BuildRuntimeWindowsCommandLine(embeddedNull).Status ==
                       RuntimeWindowsCommandLineStatus::InvalidArgument);
    }
}
