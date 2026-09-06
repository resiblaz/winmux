// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentCatalogRpc.h"
#include "ClientRequestPolicy.h"
#include "ProtocolHandshake.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;

namespace TaskRuntimeProtocolLocalTests
{
    class AgentCatalogRpcTests
    {
        TEST_CLASS(AgentCatalogRpcTests);
        TEST_METHOD(CommandsAndQueriesRoundTripWithStrictBounds);
        TEST_METHOD(AgentCapabilityIsOptInAndRequiresMinorThree);
        TEST_METHOD(CatalogReadsRetryAndWritesRetainUnknownOutcomes);
    };

    void AgentCatalogRpcTests::CommandsAndQueriesRoundTripWithStrictBounds()
    {
        AgentCatalogCommandRequest command;
        command.CommandId = "10000000-0000-4000-8000-000000000001";
        command.AgentId = "20000000-0000-4000-8000-000000000002";
        command.Configuration = AgentProfileConfiguration{ "Reviewer", "Display", "codex", "", "Review the changes." };
        const auto encoded = EncodeAgentCatalogCommandRequest(command); VERIFY_IS_TRUE(encoded.Succeeded);
        const auto decoded = DecodeAgentCatalogCommandRequest(encoded.Payload); VERIFY_IS_TRUE(decoded.Succeeded());
        VERIFY_ARE_EQUAL(command.Configuration->Instructions, decoded.Value.Configuration->Instructions);
        auto unknown = encoded.Payload; unknown.insert(1, "\"unknown\":1,");
        VERIFY_IS_FALSE(DecodeAgentCatalogCommandRequest(unknown).Succeeded());
        command.Configuration->Instructions.assign(MaximumAgentInstructionsBytes + 1, 'a');
        VERIFY_IS_FALSE(EncodeAgentCatalogCommandRequest(command).Succeeded);
        command.Configuration.reset(); command.Operation = AgentCatalogOperation::SetWorkspaceDefault;
        command.WorkspaceId = "30000000-0000-4000-8000-000000000003"; command.AgentId.clear();
        VERIFY_IS_TRUE(DecodeAgentCatalogCommandRequest(EncodeAgentCatalogCommandRequest(command).Payload).Succeeded());
        command.Operation = static_cast<AgentCatalogOperation>(200); VERIFY_IS_FALSE(EncodeAgentCatalogCommandRequest(command).Succeeded);
        AgentCatalogQueryRequest query; query.Limit = MaximumAgentCatalogPageSize;
        VERIFY_IS_TRUE(DecodeAgentCatalogQueryRequest(EncodeAgentCatalogQueryRequest(query).Payload).Succeeded());
        ++query.Limit; VERIFY_IS_FALSE(EncodeAgentCatalogQueryRequest(query).Succeeded);
        AgentCatalogQueryResult result;
        result.Generation = 3; result.Default = WorkspaceAgentDefaultProjection{ *command.WorkspaceId, std::nullopt, 2 };
        AgentProfileProjection profile;
        profile.Id = "20000000-0000-4000-8000-000000000002"; profile.Version = 3; profile.Revision = 2;
        profile.Configuration = { "Reviewer", "Display", "codex", "model-a", "Keep unchanged" }; profile.CreatedAtUnixMilliseconds = 1000; profile.UpdatedAtUnixMilliseconds = 2000;
        result.Profiles.push_back(profile);
        const auto roundtrip = DecodeAgentCatalogQueryResult(EncodeAgentCatalogQueryResult(result).Payload);
        VERIFY_IS_TRUE(roundtrip.Succeeded()); VERIFY_ARE_EQUAL(uint64_t{ 2 }, roundtrip.Value.Profiles.front().Revision);
        result.Profiles.push_back(profile); VERIFY_IS_FALSE(EncodeAgentCatalogQueryResult(result).Succeeded);
    }

    void AgentCatalogRpcTests::AgentCapabilityIsOptInAndRequiresMinorThree()
    {
        HelloRequest request;
        request.ClientInstanceId = "10000000-0000-4000-8000-000000000001";
        request.ExpectedRuntimeIdentity = "wt-task-runtime-v1:" + std::string(64, 'a');
        RuntimeHandshakeConfiguration runtime;
        runtime.RuntimeIdentity = request.ExpectedRuntimeIdentity; runtime.RuntimeInstanceId = "20000000-0000-4000-8000-000000000002";
        runtime.Capabilities = AllProtocolCapabilities;
        VERIFY_IS_FALSE(HasCapability(NegotiateHandshake(request, runtime).Response.Capabilities, ProtocolCapability::AgentProfiles));
        request.OptionalCapabilities = static_cast<uint64_t>(ProtocolCapability::AgentProfiles);
        auto accepted = NegotiateHandshake(request, runtime); VERIFY_IS_TRUE(accepted.Accepted);
        VERIFY_IS_TRUE(HasCapability(accepted.Response.Capabilities, ProtocolCapability::AgentProfiles));
        request.Versions.PreferredMinor = 2;
        accepted = NegotiateHandshake(request, runtime); VERIFY_IS_TRUE(accepted.Accepted);
        VERIFY_IS_FALSE(HasCapability(accepted.Response.Capabilities, ProtocolCapability::AgentProfiles));
        request.RequiredCapabilities = request.OptionalCapabilities; request.OptionalCapabilities = 0;
        VERIFY_IS_TRUE(NegotiateHandshake(request, runtime).Error == HandshakeError::MissingRequiredCapability);
    }

    void AgentCatalogRpcTests::CatalogReadsRetryAndWritesRetainUnknownOutcomes()
    {
        VERIFY_IS_TRUE(IsReadOnlyControlMethod(ControlMethod::QueryAgents));
        VERIFY_IS_FALSE(IsReadOnlyControlMethod(ControlMethod::AgentCommand));
        VERIFY_IS_TRUE(EvaluateClientDisconnect(ControlMethod::QueryAgents, ClientRequestTransmission::WriteCompleted) == ClientDisconnectAction::RetryWithNewRequestId);
        VERIFY_IS_TRUE(EvaluateClientDisconnect(ControlMethod::AgentCommand, ClientRequestTransmission::WriteCompleted) == ClientDisconnectAction::OutcomeUnknown);
        VERIFY_ARE_EQUAL(static_cast<uint64_t>(ProtocolCapability::AgentProfiles), RequiredCapabilities(ControlMethod::AgentCommand));
    }
}
