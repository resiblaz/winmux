// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ProtocolCapabilities.h"
#include "ProtocolValidation.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    class ProtocolValidationTests
    {
        TEST_CLASS(ProtocolValidationTests);

        TEST_METHOD(ValidatesCanonicalIdentifiersAndRuntimeIdentity);
        TEST_METHOD(AcceptsWellFormedUtf8);
        TEST_METHOD(RejectsMalformedOverlongAndSurrogateUtf8);
        TEST_METHOD(CapabilityNamesRoundTripDeterministically);
    };

    void ProtocolValidationTests::ValidatesCanonicalIdentifiersAndRuntimeIdentity()
    {
        VERIFY_IS_TRUE(IsCanonicalProtocolUuid("12345678-1234-4abc-8def-1234567890ab"));
        VERIFY_IS_FALSE(IsCanonicalProtocolUuid("12345678-1234-4ABC-8def-1234567890ab"));
        VERIFY_IS_FALSE(IsCanonicalProtocolUuid("00000000-0000-0000-0000-000000000000"));
        VERIFY_IS_TRUE(IsValidRuntimeIdentity("windows-terminal-dev:user_01"));
        VERIFY_IS_FALSE(IsValidRuntimeIdentity(""));
        VERIFY_IS_FALSE(IsValidRuntimeIdentity("runtime identity with spaces"));
        VERIFY_IS_FALSE(IsValidRuntimeIdentity(std::string(129, 'a')));
    }

    void ProtocolValidationTests::AcceptsWellFormedUtf8()
    {
        VERIFY_IS_TRUE(IsValidUtf8("plain ASCII"));
        VERIFY_IS_TRUE(IsValidUtf8("任务终端"));
        VERIFY_IS_TRUE(IsValidUtf8("emoji \xF0\x9F\x99\x82"));
        VERIFY_IS_TRUE(IsValidUtf8(std::string_view{}));
    }

    void ProtocolValidationTests::RejectsMalformedOverlongAndSurrogateUtf8()
    {
        const std::vector<std::vector<uint8_t>> invalid{
            { 0x80 },
            { 0xc0, 0x80 },
            { 0xe0, 0x80, 0x80 },
            { 0xed, 0xa0, 0x80 },
            { 0xf4, 0x90, 0x80, 0x80 },
            { 0xf0, 0x9f, 0x99 },
            { 0xe2, 0x28, 0xa1 },
        };
        for (const auto& value : invalid)
        {
            VERIFY_IS_FALSE(IsValidUtf8(value));
        }
    }

    void ProtocolValidationTests::CapabilityNamesRoundTripDeterministically()
    {
        auto capabilities = AddCapability(0, ProtocolCapability::TaskCommands);
        capabilities = AddCapability(capabilities, ProtocolCapability::SnapshotAndSubscribe);
        VERIFY_IS_TRUE(HasCapability(capabilities, ProtocolCapability::TaskCommands));
        VERIFY_IS_FALSE(HasCapability(capabilities, ProtocolCapability::TerminalStream));

        const auto names = CapabilityNames(capabilities);
        VERIFY_ARE_EQUAL(size_t{ 2 }, names.size());
        VERIFY_ARE_EQUAL(std::string{ "task_commands" }, names[0]);
        VERIFY_ARE_EQUAL(std::string{ "snapshot_and_subscribe" }, names[1]);
        VERIFY_IS_TRUE(CapabilityFromName(names[0]) == std::optional{ ProtocolCapability::TaskCommands });
        VERIFY_IS_FALSE(CapabilityFromName("future_capability").has_value());
        VERIFY_IS_TRUE(CapabilityName(static_cast<ProtocolCapability>(1ull << 60)).empty());
    }
}
