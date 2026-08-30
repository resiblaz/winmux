// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RtaskCommand.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    namespace
    {
        constexpr std::string_view CommandId{
            "a1000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view RunId{
            "a2000000-0000-4000-8000-000000000001"
        };

        RtaskCommandRequest _request(const RtaskMethod method, std::string text = {})
        {
            return {
                std::string{ CommandId },
                std::string{ RunId },
                std::string(RtaskCapabilityBytes, 'a'),
                method,
                std::move(text),
            };
        }

        std::string _unicodeText(const size_t characters)
        {
            std::string value;
            value.reserve(characters * 4);
            for (size_t index = 0; index < characters; ++index)
            {
                value.append("\xf0\x9f\x98\x80", 4);
            }
            return value;
        }
    }

    class RtaskCommandTests
    {
        TEST_CLASS(RtaskCommandTests);

        TEST_METHOD(MethodsAndSetsAreExactAndBounded);
        TEST_METHOD(FourCommandShapesEnforceScopedText);
        TEST_METHOD(Utf8TextLimitsAreByteAndCharacterBounded);
        TEST_METHOD(IdentityCapabilityAndResultsFailClosed);
        TEST_METHOD(DiagnosticSummaryRedactsCapabilityAndText);
    };

    void RtaskCommandTests::MethodsAndSetsAreExactAndBounded()
    {
        VERIFY_ARE_EQUAL(
            std::string_view{ "status_in_progress" },
            RtaskMethodName(RtaskMethod::StatusInProgress));
        VERIFY_ARE_EQUAL(
            std::string_view{ "status_in_review" },
            RtaskMethodName(RtaskMethod::StatusInReview));
        VERIFY_ARE_EQUAL(
            std::string_view{ "status_blocked" },
            RtaskMethodName(RtaskMethod::StatusBlocked));
        VERIFY_ARE_EQUAL(
            std::string_view{ "result" },
            RtaskMethodName(RtaskMethod::Result));
        VERIFY_IS_TRUE(RtaskMethodName(RtaskMethod::Count).empty());
        VERIFY_IS_FALSE(RtaskMethodFromName("status_done").has_value());

        RtaskMethodSet methods{};
        for (uint8_t value{}; value < static_cast<uint8_t>(RtaskMethod::Count); ++value)
        {
            const auto method = static_cast<RtaskMethod>(value);
            methods |= RtaskMethodBit(method);
            VERIFY_IS_TRUE(HasRtaskMethod(methods, method));
        }
        VERIFY_ARE_EQUAL(AllRtaskMethods, methods);
        VERIFY_IS_TRUE(IsValidRtaskMethodSet(methods));
        VERIFY_IS_FALSE(IsValidRtaskMethodSet(0));
        VERIFY_IS_FALSE(IsValidRtaskMethodSet(AllRtaskMethods | (RtaskMethodSet{ 1 } << 8)));
    }

    void RtaskCommandTests::FourCommandShapesEnforceScopedText()
    {
        std::string error;
        VERIFY_IS_TRUE(ValidateRtaskCommand(
            _request(RtaskMethod::StatusInProgress),
            error));
        VERIFY_IS_TRUE(ValidateRtaskCommand(
            _request(RtaskMethod::StatusInReview),
            error));
        VERIFY_IS_TRUE(ValidateRtaskCommand(
            _request(RtaskMethod::StatusBlocked, "waiting for credentials"),
            error));
        VERIFY_IS_TRUE(ValidateRtaskCommand(
            _request(RtaskMethod::Result, "implemented and tested"),
            error));

        VERIFY_IS_FALSE(ValidateRtaskCommand(
            _request(RtaskMethod::StatusInProgress, "unexpected"),
            error));
        VERIFY_IS_FALSE(ValidateRtaskCommand(
            _request(RtaskMethod::StatusInReview, "unexpected"),
            error));
        VERIFY_IS_FALSE(ValidateRtaskCommand(
            _request(RtaskMethod::StatusBlocked),
            error));
        VERIFY_IS_FALSE(ValidateRtaskCommand(
            _request(RtaskMethod::Result),
            error));
        VERIFY_IS_FALSE(ValidateRtaskCommand(
            _request(RtaskMethod::Count),
            error));
    }

    void RtaskCommandTests::Utf8TextLimitsAreByteAndCharacterBounded()
    {
        std::string error;
        auto blocked = _request(
            RtaskMethod::StatusBlocked,
            _unicodeText(MaximumRtaskBlockedReasonCharacters));
        VERIFY_ARE_EQUAL(MaximumRtaskBlockedReasonBytes, blocked.Text.size());
        VERIFY_IS_TRUE(ValidateRtaskCommand(blocked, error));
        blocked.Text.append("\xf0\x9f\x98\x80", 4);
        VERIFY_IS_FALSE(ValidateRtaskCommand(blocked, error));

        auto result = _request(
            RtaskMethod::Result,
            _unicodeText(MaximumRtaskResultCharacters));
        VERIFY_ARE_EQUAL(MaximumRtaskResultBytes, result.Text.size());
        VERIFY_IS_TRUE(ValidateRtaskCommand(result, error));
        result.Text.push_back('x');
        VERIFY_IS_FALSE(ValidateRtaskCommand(result, error));

        auto malformed = _request(RtaskMethod::Result, std::string{ "\xc0\x80", 2 });
        VERIFY_IS_FALSE(ValidateRtaskCommand(malformed, error));
        auto embeddedNull = _request(RtaskMethod::Result, std::string{ "a\0b", 3 });
        VERIFY_IS_FALSE(ValidateRtaskCommand(embeddedNull, error));
    }

    void RtaskCommandTests::IdentityCapabilityAndResultsFailClosed()
    {
        std::string error;
        auto request = _request(RtaskMethod::StatusInProgress);
        request.CommandId = "not-a-command";
        VERIFY_IS_FALSE(ValidateRtaskCommand(request, error));
        request = _request(RtaskMethod::StatusInProgress);
        request.RunId = "not-a-run";
        VERIFY_IS_FALSE(ValidateRtaskCommand(request, error));
        request = _request(RtaskMethod::StatusInProgress);
        request.Capability = std::string(RtaskCapabilityBytes, '0');
        VERIFY_IS_FALSE(ValidateRtaskCommand(request, error));
        request.Capability = std::string(RtaskCapabilityBytes, 'A');
        VERIFY_IS_FALSE(ValidateRtaskCommand(request, error));

        RtaskCommandResult result{
            std::string{ CommandId },
            RtaskCommandDisposition::Applied,
            7,
            9,
        };
        VERIFY_IS_TRUE(ValidateRtaskCommandResult(result, error));
        result.Disposition = RtaskCommandDisposition::Count;
        VERIFY_IS_FALSE(ValidateRtaskCommandResult(result, error));
        result.Disposition = RtaskCommandDisposition::Replayed;
        result.TaskVersion = 0;
        VERIFY_IS_FALSE(ValidateRtaskCommandResult(result, error));
        result.TaskVersion = 7;
        result.EventSequence = 0;
        VERIFY_IS_FALSE(ValidateRtaskCommandResult(result, error));
    }

    void RtaskCommandTests::DiagnosticSummaryRedactsCapabilityAndText()
    {
        const auto request = _request(
            RtaskMethod::Result,
            "private result that must not enter diagnostics");
        const auto summary = RtaskCommandDiagnosticSummary(request);
        VERIFY_IS_TRUE(summary.find(request.CommandId) != std::string::npos);
        VERIFY_IS_TRUE(summary.find(request.RunId) != std::string::npos);
        VERIFY_IS_TRUE(summary.find("method=result") != std::string::npos);
        VERIFY_IS_TRUE(summary.find("capability=<redacted>") != std::string::npos);
        VERIFY_IS_TRUE(summary.find(request.Capability) == std::string::npos);
        VERIFY_IS_TRUE(summary.find(request.Text) == std::string::npos);
        VERIFY_IS_LESS_THAN(summary.size(), size_t{ 256 });

        auto invalid = request;
        invalid.CommandId = std::string(4096, 'x');
        invalid.RunId = std::string(4096, 'y');
        const auto invalidSummary = RtaskCommandDiagnosticSummary(invalid);
        VERIFY_IS_TRUE(invalidSummary.find(std::string(64, 'x')) == std::string::npos);
        VERIFY_IS_TRUE(invalidSummary.find(std::string(64, 'y')) == std::string::npos);
        VERIFY_IS_LESS_THAN(invalidSummary.size(), size_t{ 192 });
    }
}
