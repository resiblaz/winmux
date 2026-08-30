// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RtaskCommandLine.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::Rtask;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        RtaskCommandLine _parse(std::initializer_list<std::wstring_view> values)
        {
            return ParseRtaskCommandLine(std::vector<std::wstring_view>{ values });
        }
    }

    class RtaskCommandLineTests
    {
        TEST_CLASS(RtaskCommandLineTests);

        TEST_METHOD(ParsesTheFourAcceptedCommands);
        TEST_METHOD(HelpIsTheOnlyNonCommandMode);
        TEST_METHOD(RejectsExtraScopeSecretsAndMalformedForms);
    };

    void RtaskCommandLineTests::ParsesTheFourAcceptedCommands()
    {
        auto parsed = _parse({ L"status", L"in-progress" });
        VERIFY_IS_TRUE(parsed.Succeeded);
        VERIFY_IS_TRUE(parsed.Method == RtaskMethod::StatusInProgress);
        VERIFY_IS_FALSE(parsed.ReadsStdin);

        parsed = _parse({ L"status", L"in-review" });
        VERIFY_IS_TRUE(parsed.Succeeded);
        VERIFY_IS_TRUE(parsed.Method == RtaskMethod::StatusInReview);
        VERIFY_IS_FALSE(parsed.ReadsStdin);

        parsed = _parse({ L"status", L"blocked", L"--reason-stdin" });
        VERIFY_IS_TRUE(parsed.Succeeded);
        VERIFY_IS_TRUE(parsed.Method == RtaskMethod::StatusBlocked);
        VERIFY_IS_TRUE(parsed.ReadsStdin);

        parsed = _parse({ L"result", L"--stdin" });
        VERIFY_IS_TRUE(parsed.Succeeded);
        VERIFY_IS_TRUE(parsed.Method == RtaskMethod::Result);
        VERIFY_IS_TRUE(parsed.ReadsStdin);
    }

    void RtaskCommandLineTests::HelpIsTheOnlyNonCommandMode()
    {
        const auto help = _parse({ L"--help" });
        VERIFY_IS_TRUE(help.Succeeded);
        VERIFY_IS_TRUE(help.Help);
        const auto shortHelp = _parse({ L"-h" });
        VERIFY_IS_TRUE(shortHelp.Succeeded);
        VERIFY_IS_TRUE(shortHelp.Help);
        VERIFY_IS_FALSE(_parse({}).Succeeded);
        VERIFY_IS_FALSE(_parse({ L"help" }).Succeeded);
    }

    void RtaskCommandLineTests::RejectsExtraScopeSecretsAndMalformedForms()
    {
        const std::array invalid{
            _parse({ L"status" }),
            _parse({ L"status", L"blocked" }),
            _parse({ L"status", L"blocked", L"reason" }),
            _parse({ L"status", L"done" }),
            _parse({ L"result" }),
            _parse({ L"result", L"inline-result" }),
            _parse({ L"result", L"--stdin", L"--task", L"id" }),
            _parse({ L"status", L"in-progress", L"--capability", L"secret" }),
            _parse({ L"cancel" }),
            _parse({ L"delete" }),
        };
        for (const auto& parsed : invalid)
        {
            VERIFY_IS_FALSE(parsed.Succeeded);
            VERIFY_IS_FALSE(parsed.Error.empty());
        }
    }
}
