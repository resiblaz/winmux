// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeAgentCatalogSource.h"

namespace winrt::TerminalApp::implementation::agents
{
    using namespace ::Microsoft::Terminal::TaskRuntime;
    using namespace protocol;

    namespace
    {
        template<typename T, typename TDecode>
        winrt::fire_and_forget _send(
            std::shared_ptr<TaskRuntimeClient> client, Windows::UI::Core::CoreDispatcher dispatcher,
            ControlMethod method, ProtocolRpcJsonEncodeResult encoded, TDecode decode,
            std::function<void(CatalogReply<T>)> completed,
            std::optional<std::pair<std::string, std::string>> expected = std::nullopt)
        {
            CatalogReply<T> reply;
            const bool writes = method == ControlMethod::AgentCommand || method == ControlMethod::SubmitCommand;
            try
            {
                co_await winrt::resume_background();
                if (!encoded.Succeeded)
                {
                    reply.Error = encoded.Error;
                    reply.ErrorCode = ProtocolRpcErrorCode::InvalidRequest;
                }
                else for (size_t attempt = 0; attempt < 2; ++attempt)
                {
                    auto pending = client->Submit(method, encoded.Payload);
                    if (!pending.Completion.valid()) { reply.Error = "Runtime did not accept the request"; break; }
                    if (pending.Completion.wait_for(std::chrono::seconds{ 15 }) != std::future_status::ready)
                    { reply.Error = "Runtime response timed out; retry to resolve this operation"; reply.OutcomeUnknown = writes && pending.Accepted; break; }
                    const auto result = pending.Completion.get();
                    if (result.Outcome == RuntimeClientRequestOutcome::Succeeded && result.Response && result.Response->Succeeded)
                    {
                        auto decoded = decode(result.Response->ResultJson);
                        reply.Succeeded = decoded.Succeeded();
                        reply.Error = decoded.Error;
                        reply.OutcomeUnknown = writes && !reply.Succeeded;
                        if constexpr (std::is_same_v<T, AgentCatalogCommandResult> || std::is_same_v<T, TaskUiCommandResult>)
                        {
                            if (reply.Succeeded && expected)
                            {
                                const auto target = [&] {
                                    if constexpr (std::is_same_v<T, AgentCatalogCommandResult>) return decoded.Value.AgentId;
                                    else return decoded.Value.TaskId;
                                }();
                                if (decoded.Value.CommandId != expected->first || target != expected->second)
                                { reply.Succeeded = false; reply.OutcomeUnknown = true; reply.Error = "Runtime returned an unrelated command result"; }
                            }
                        }
                        if (reply.Succeeded) reply.Value = std::move(decoded.Value);
                        break;
                    }
                    if (result.Response && result.Response->Error)
                    {
                        reply.ErrorCode = result.Response->Error->Code;
                        reply.Error = result.Response->Error->Message;
                        reply.OutcomeUnknown = reply.OutcomeUnknown || (writes && reply.ErrorCode == ProtocolRpcErrorCode::InternalError);
                        break;
                    }
                    reply.Error = result.Diagnostic.empty() ? "Runtime connection is unavailable" : result.Diagnostic;
                    reply.OutcomeUnknown = reply.OutcomeUnknown || (writes && result.Outcome == RuntimeClientRequestOutcome::OutcomeUnknown);
                    if (result.Outcome != RuntimeClientRequestOutcome::Disconnected && result.Outcome != RuntimeClientRequestOutcome::OutcomeUnknown) break;
                    // The payload retains the durable command ID when the wire
                    // request ID changes after reconnect. Replaying is safe.
                }
            }
            catch (...)
            {
                reply.Error = "Agent request could not be completed";
                reply.OutcomeUnknown = writes;
            }
            try
            {
                if (dispatcher) co_await winrt::resume_foreground(dispatcher);
                if (completed) completed(std::move(reply));
            }
            catch (...) { LOG_CAUGHT_EXCEPTION(); }
        }

        class RuntimeAgentCatalogSource final : public IAgentCatalogSource
        {
        public:
            RuntimeAgentCatalogSource(std::shared_ptr<TaskRuntimeClient> client, Windows::UI::Core::CoreDispatcher dispatcher) :
                _client{ std::move(client) }, _dispatcher{ std::move(dispatcher) } {}
            void Query(AgentCatalogQueryRequest request, std::function<void(CatalogReply<AgentCatalogQueryResult>)> completed) override
            { _send(_client, _dispatcher, ControlMethod::QueryAgents, EncodeAgentCatalogQueryRequest(request), DecodeAgentCatalogQueryResult, std::move(completed)); }
            void Execute(AgentCatalogCommandRequest request, std::function<void(CatalogReply<AgentCatalogCommandResult>)> completed) override
            { _send(_client, _dispatcher, ControlMethod::AgentCommand, EncodeAgentCatalogCommandRequest(request), DecodeAgentCatalogCommandResult, std::move(completed), std::pair{ request.CommandId, request.AgentId }); }
            void ExecuteTask(TaskUiCommandRequest request, std::function<void(CatalogReply<TaskUiCommandResult>)> completed) override
            { _send(_client, _dispatcher, ControlMethod::SubmitCommand, EncodeTaskUiCommandRequest(request), DecodeTaskUiCommandResult, std::move(completed), std::pair{ request.CommandId, request.TaskId }); }
            void QueryTask(TaskProjectionQueryRequest request, std::function<void(CatalogReply<TaskProjectionQueryResult>)> completed) override
            { _send(_client, _dispatcher, ControlMethod::QueryTask, EncodeTaskProjectionQueryRequest(request), DecodeTaskProjectionQueryResult, std::move(completed)); }
        private:
            std::shared_ptr<TaskRuntimeClient> _client;
            Windows::UI::Core::CoreDispatcher _dispatcher{ nullptr };
        };
    }

    std::shared_ptr<IAgentCatalogSource> MakeRuntimeAgentCatalogSource(std::shared_ptr<TaskRuntimeClient> client, Windows::UI::Core::CoreDispatcher dispatcher)
    {
        return std::make_shared<RuntimeAgentCatalogSource>(std::move(client), std::move(dispatcher));
    }
}
