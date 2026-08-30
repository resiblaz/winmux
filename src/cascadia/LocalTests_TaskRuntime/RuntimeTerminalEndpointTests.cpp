// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimePipe.h"
#include "RuntimeTerminalEndpoint.h"
#include "TaskRuntimeTestFixtures.h"

#include <sddl.h>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr std::string_view TerminalSessionOne{
            "93000000-0000-4000-8000-000000000001"
        };
        constexpr std::string_view TerminalSessionTwo{
            "93000000-0000-4000-8000-000000000002"
        };
    }

    class RuntimeTerminalEndpointTests
    {
        TEST_CLASS(RuntimeTerminalEndpointTests);

        TEST_METHOD(EndpointDerivationBindsRuntimeSessionAndGeneration);
        TEST_METHOD(EndpointValidationRejectsDriftAndMalformedIdentity);
        TEST_METHOD(TerminalPipeUsesCurrentUserSecurityAndFirstInstanceGuard);
    };

    void RuntimeTerminalEndpointTests::EndpointDerivationBindsRuntimeSessionAndGeneration()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        const auto first = BuildRuntimeTerminalEndpoint(
            identity,
            TerminalSessionOne,
            7);
        const auto repeated = BuildRuntimeTerminalEndpoint(
            identity,
            TerminalSessionOne,
            7);
        const auto nextGeneration = BuildRuntimeTerminalEndpoint(
            identity,
            TerminalSessionOne,
            8);
        const auto otherSession = BuildRuntimeTerminalEndpoint(
            identity,
            TerminalSessionTwo,
            7);

        VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_IS_TRUE(repeated.Succeeded());
        VERIFY_IS_TRUE(nextGeneration.Succeeded());
        VERIFY_IS_TRUE(otherSession.Succeeded());
        VERIFY_ARE_EQUAL(
            first.Endpoint.PipeName,
            repeated.Endpoint.PipeName);
        VERIFY_IS_TRUE(
            first.Endpoint.PipeName != nextGeneration.Endpoint.PipeName);
        VERIFY_IS_TRUE(first.Endpoint.PipeName != otherSession.Endpoint.PipeName);
        VERIFY_ARE_EQUAL(identity.ProtocolIdentity, first.Endpoint.RuntimeIdentity);
        VERIFY_ARE_EQUAL(std::string{ TerminalSessionOne }, first.Endpoint.TerminalSessionId);
        VERIFY_ARE_EQUAL(uint64_t{ 7 }, first.Endpoint.Generation);
        VERIFY_IS_TRUE(ValidateRuntimeTerminalEndpoint(first.Endpoint));
        VERIFY_IS_TRUE(first.Endpoint.PipeName.size() <= 240);
    }

    void RuntimeTerminalEndpointTests::EndpointValidationRejectsDriftAndMalformedIdentity()
    {
        UniqueRuntimeRoot root;
        auto identity = RequireIdentity(root.Path());
        auto endpoint = BuildRuntimeTerminalEndpoint(
            identity,
            TerminalSessionOne,
            1);
        VERIFY_IS_TRUE(endpoint.Succeeded());

        auto drifted = endpoint.Endpoint;
        drifted.Generation = 2;
        VERIFY_IS_FALSE(ValidateRuntimeTerminalEndpoint(drifted));
        drifted = endpoint.Endpoint;
        drifted.TerminalSessionId = std::string{ TerminalSessionTwo };
        VERIFY_IS_FALSE(ValidateRuntimeTerminalEndpoint(drifted));
        drifted = endpoint.Endpoint;
        drifted.RuntimeIdentity.back() = 'g';
        VERIFY_IS_FALSE(ValidateRuntimeTerminalEndpoint(drifted));
        drifted = endpoint.Endpoint;
        drifted.PipeName.append(L".extra");
        VERIFY_IS_FALSE(ValidateRuntimeTerminalEndpoint(drifted));

        VERIFY_IS_TRUE(
            BuildRuntimeTerminalEndpoint(identity, TerminalSessionOne, 0).Status ==
            RuntimeTerminalEndpointStatus::InvalidGeneration);
        VERIFY_IS_TRUE(
            BuildRuntimeTerminalEndpoint(identity, "not-a-session", 1).Status ==
            RuntimeTerminalEndpointStatus::InvalidTerminalSessionId);
        identity.ControlPipeName.append(L".drift");
        VERIFY_IS_TRUE(
            BuildRuntimeTerminalEndpoint(identity, TerminalSessionOne, 1).Status ==
            RuntimeTerminalEndpointStatus::InvalidRuntimeIdentity);
    }

    void RuntimeTerminalEndpointTests::TerminalPipeUsesCurrentUserSecurityAndFirstInstanceGuard()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        const auto endpoint = BuildRuntimeTerminalEndpoint(
            identity,
            TerminalSessionOne,
            11);
        VERIFY_IS_TRUE(endpoint.Succeeded());
        auto security = RequireSecurity(identity.UserSid);
        auto first = CreateRuntimeTerminalPipe(
            endpoint.Endpoint,
            security.Attributes(),
            true);
        VERIFY_IS_TRUE(first.Created());
        VERIFY_ARE_EQUAL(endpoint.Endpoint.PipeName, first.Pipe.Name());

        DWORD handleFlags{};
        VERIFY_IS_TRUE(GetHandleInformation(first.Pipe.Handle(), &handleFlags));
        VERIFY_ARE_EQUAL(DWORD{ 0 }, handleFlags & HANDLE_FLAG_INHERIT);
        DWORD pipeFlags{};
        VERIFY_IS_TRUE(GetNamedPipeInfo(
            first.Pipe.Handle(),
            &pipeFlags,
            nullptr,
            nullptr,
            nullptr));
        VERIFY_IS_TRUE((pipeFlags & PIPE_SERVER_END) != 0);

        PSID owner{};
        PSECURITY_DESCRIPTOR descriptor{};
        VERIFY_ARE_EQUAL(
            DWORD{ ERROR_SUCCESS },
            GetSecurityInfo(
                first.Pipe.Handle(),
                SE_KERNEL_OBJECT,
                OWNER_SECURITY_INFORMATION,
                &owner,
                nullptr,
                nullptr,
                nullptr,
                &descriptor));
        PSID expectedOwner{};
        VERIFY_IS_TRUE(ConvertStringSidToSidW(
            identity.UserSid.c_str(),
            &expectedOwner));
        if (owner && expectedOwner)
        {
            VERIFY_IS_TRUE(EqualSid(owner, expectedOwner));
        }
        if (expectedOwner)
        {
            LocalFree(expectedOwner);
        }
        if (descriptor)
        {
            LocalFree(descriptor);
        }

        const auto duplicate = CreateRuntimeTerminalPipe(
            endpoint.Endpoint,
            security.Attributes(),
            true);
        VERIFY_IS_TRUE(duplicate.Status == RuntimePipeStatus::AlreadyExists);

        auto invalid = endpoint.Endpoint;
        invalid.Generation += 1;
        VERIFY_IS_TRUE(
            CreateRuntimeTerminalPipe(invalid, security.Attributes()).Status ==
            RuntimePipeStatus::InvalidConfiguration);
    }
}
