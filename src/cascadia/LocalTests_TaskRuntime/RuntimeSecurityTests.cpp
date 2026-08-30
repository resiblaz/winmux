// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        PSID _parseSid(const std::wstring& value)
        {
            PSID sid{};
            VERIFY_IS_TRUE(ConvertStringSidToSidW(value.c_str(), &sid));
            return sid;
        }

        std::vector<uint8_t> _wellKnownSid(const WELL_KNOWN_SID_TYPE type)
        {
            DWORD size{ SECURITY_MAX_SID_SIZE };
            std::vector<uint8_t> sid(size);
            VERIFY_IS_TRUE(CreateWellKnownSid(type, nullptr, sid.data(), &size));
            sid.resize(size);
            return sid;
        }
    }

    class RuntimeSecurityTests
    {
        TEST_CLASS(RuntimeSecurityTests);

        TEST_METHOD(DescriptorIsProtectedNonInheritableAndOwnedByCurrentUser);
        TEST_METHOD(DaclGrantsOnlyCurrentUserAndLocalSystem);
        TEST_METHOD(MandatoryLabelRejectsLowIntegrityWrites);
        TEST_METHOD(RejectsMalformedUserSid);
    };

    void RuntimeSecurityTests::DescriptorIsProtectedNonInheritableAndOwnedByCurrentUser()
    {
        const auto sidText = RequireCurrentUserSid();
        const auto expectedSid = _parseSid(sidText);
        auto security = RequireSecurity(sidText);
        VERIFY_IS_TRUE(static_cast<bool>(security));
        VERIFY_IS_NOT_NULL(security.Attributes());
        VERIFY_ARE_EQUAL(DWORD{ sizeof(SECURITY_ATTRIBUTES) }, security.Attributes()->nLength);
        VERIFY_IS_FALSE(security.Attributes()->bInheritHandle != FALSE);

        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision{};
        VERIFY_IS_TRUE(GetSecurityDescriptorControl(security.Descriptor(), &control, &revision));
        VERIFY_IS_TRUE((control & SE_DACL_PROTECTED) != 0);

        PSID owner{};
        BOOL ownerDefaulted{};
        VERIFY_IS_TRUE(GetSecurityDescriptorOwner(security.Descriptor(), &owner, &ownerDefaulted));
        VERIFY_IS_TRUE(EqualSid(expectedSid, owner));
        LocalFree(expectedSid);
    }

    void RuntimeSecurityTests::DaclGrantsOnlyCurrentUserAndLocalSystem()
    {
        const auto sidText = RequireCurrentUserSid();
        const auto userSid = _parseSid(sidText);
        auto systemSid = _wellKnownSid(WinLocalSystemSid);
        auto security = RequireSecurity(sidText);

        PACL dacl{};
        BOOL present{};
        BOOL defaulted{};
        VERIFY_IS_TRUE(GetSecurityDescriptorDacl(security.Descriptor(), &present, &dacl, &defaulted));
        VERIFY_IS_TRUE(present != FALSE);
        VERIFY_IS_NOT_NULL(dacl);
        VERIFY_ARE_EQUAL(DWORD{ 2 }, dacl->AceCount);

        size_t userEntries{};
        size_t systemEntries{};
        for (DWORD index = 0; index < dacl->AceCount; ++index)
        {
            void* rawAce{};
            VERIFY_IS_TRUE(GetAce(dacl, index, &rawAce));
            const auto ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            VERIFY_ARE_EQUAL(static_cast<uint8_t>(ACCESS_ALLOWED_ACE_TYPE), ace->Header.AceType);
            VERIFY_IS_TRUE((ace->Mask & GENERIC_ALL) != 0);
            const auto aceSid = const_cast<DWORD*>(&ace->SidStart);
            userEntries += EqualSid(userSid, aceSid) ? 1 : 0;
            systemEntries += EqualSid(systemSid.data(), aceSid) ? 1 : 0;
        }
        VERIFY_ARE_EQUAL(size_t{ 1 }, userEntries);
        VERIFY_ARE_EQUAL(size_t{ 1 }, systemEntries);
        LocalFree(userSid);
    }

    void RuntimeSecurityTests::MandatoryLabelRejectsLowIntegrityWrites()
    {
        auto security = RequireSecurity(RequireCurrentUserSid());
        PACL sacl{};
        BOOL present{};
        BOOL defaulted{};
        VERIFY_IS_TRUE(GetSecurityDescriptorSacl(security.Descriptor(), &present, &sacl, &defaulted));
        VERIFY_IS_TRUE(present != FALSE);
        VERIFY_IS_NOT_NULL(sacl);
        VERIFY_ARE_EQUAL(DWORD{ 1 }, sacl->AceCount);

        void* rawAce{};
        VERIFY_IS_TRUE(GetAce(sacl, 0, &rawAce));
        const auto ace = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(rawAce);
        VERIFY_ARE_EQUAL(static_cast<uint8_t>(SYSTEM_MANDATORY_LABEL_ACE_TYPE), ace->Header.AceType);
        VERIFY_IS_TRUE((ace->Mask & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP) != 0);
        auto mediumSid = _wellKnownSid(WinMediumLabelSid);
        VERIFY_IS_TRUE(EqualSid(mediumSid.data(), const_cast<DWORD*>(&ace->SidStart)));
    }

    void RuntimeSecurityTests::RejectsMalformedUserSid()
    {
        const auto result = CreateRuntimeSecurityAttributes(L"not-a-sid");
        VERIFY_IS_FALSE(result.Succeeded());
        VERIFY_IS_TRUE(result.Status == RuntimeSecurityStatus::InvalidUserSid);
    }
}
