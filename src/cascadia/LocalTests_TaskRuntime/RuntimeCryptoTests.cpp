// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeCrypto.h"

#include "ProtocolValidation.h"

#include <set>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeLocalTests
{
    class RuntimeCryptoTests
    {
        TEST_CLASS(RuntimeCryptoTests);

        TEST_METHOD(Sha256MatchesKnownVectorAndEmptyInput);
        TEST_METHOD(RandomUuidsAreCanonicalAndUnique);
    };

    void RuntimeCryptoTests::Sha256MatchesKnownVectorAndEmptyInput()
    {
        constexpr std::array<uint8_t, 3> abc{ 'a', 'b', 'c' };
        const auto digest = RuntimeSha256Hex(abc);
        VERIFY_IS_TRUE(digest.has_value());
        VERIFY_ARE_EQUAL(
            std::string{ "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
            *digest);
        const auto empty = RuntimeSha256Hex(std::span<const uint8_t>{});
        VERIFY_IS_TRUE(empty.has_value());
        VERIFY_ARE_EQUAL(
            std::string{ "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
            *empty);
    }

    void RuntimeCryptoTests::RandomUuidsAreCanonicalAndUnique()
    {
        std::set<std::string> values;
        for (size_t index = 0; index < 256; ++index)
        {
            const auto value = NewRuntimeUuid();
            VERIFY_IS_TRUE(value.has_value());
            if (!value)
            {
                return;
            }
            VERIFY_IS_TRUE(IsCanonicalProtocolUuid(*value));
            VERIFY_ARE_EQUAL('4', (*value)[14]);
            VERIFY_IS_TRUE((*value)[19] == '8' || (*value)[19] == '9' ||
                           (*value)[19] == 'a' || (*value)[19] == 'b');
            VERIFY_IS_TRUE(values.emplace(*value).second);
        }
        VERIFY_ARE_EQUAL(size_t{ 256 }, values.size());
    }
}
