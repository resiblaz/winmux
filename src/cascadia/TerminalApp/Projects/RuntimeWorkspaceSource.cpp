// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "RuntimeWorkspaceSource.h"

namespace winrt::TerminalApp::implementation::projects
{
    using namespace ::Microsoft::Terminal::TaskRuntime;
    using namespace protocol;
    namespace
    {
        template<typename T, typename Decode>
        winrt::fire_and_forget send(std::shared_ptr<TaskRuntimeClient> client, Windows::UI::Core::CoreDispatcher dispatcher,
                                   ControlMethod method, ProtocolRpcJsonEncodeResult encoded, Decode decode,
                                   std::function<void(WorkspaceReply<T>)> completed, std::string commandId = {}, std::string taskId = {})
        {
            WorkspaceReply<T> reply;
            const auto writes = method == ControlMethod::ProjectCommand || method == ControlMethod::TaskBoardCommand;
            try
            {
                co_await winrt::resume_background();
                if (!encoded.Succeeded) { reply.Error = encoded.Error; reply.ErrorCode = ProtocolRpcErrorCode::InvalidRequest; }
                else for (size_t attempt = 0; attempt < 2; ++attempt)
                {
                    auto pending = client->Submit(method, encoded.Payload);
                    if (!pending.Completion.valid()) { reply.Error = "Runtime did not accept the request"; break; }
                    if (pending.Completion.wait_for(std::chrono::seconds{ 15 }) != std::future_status::ready)
                    { reply.Error = "Runtime response timed out; retry this operation"; reply.OutcomeUnknown = writes && pending.Accepted; break; }
                    const auto result = pending.Completion.get();
                    if (result.Outcome == RuntimeClientRequestOutcome::Succeeded && result.Response && result.Response->Succeeded)
                    {
                        auto decoded = decode(result.Response->ResultJson);
                        reply.Succeeded = decoded.Succeeded(); reply.Error = decoded.Error; reply.OutcomeUnknown = writes && !reply.Succeeded;
                        if constexpr (std::is_same_v<T, ProjectCatalogCommandResult> || std::is_same_v<T, TaskBoardCommandResult>)
                        {
                            if (reply.Succeeded && !commandId.empty() && decoded.Value.CommandId != commandId)
                            { reply.Succeeded = false; reply.Error = "Runtime returned an unrelated command result"; reply.OutcomeUnknown = writes; }
                        }
                        if constexpr (std::is_same_v<T, TaskBoardCommandResult>)
                        {
                            if (reply.Succeeded && !taskId.empty() && decoded.Value.TaskId != taskId)
                            { reply.Succeeded = false; reply.Error = "Runtime returned a result for another Task"; reply.OutcomeUnknown = writes; }
                        }
                        if (reply.Succeeded) reply.Value = std::move(decoded.Value);
                        break;
                    }
                    if (result.Response && result.Response->Error)
                    {
                        reply.ErrorCode = result.Response->Error->Code; reply.Error = result.Response->Error->Message;
                        reply.OutcomeUnknown = reply.OutcomeUnknown || (writes && reply.ErrorCode == ProtocolRpcErrorCode::InternalError);
                        break;
                    }
                    reply.Error = result.Diagnostic.empty() ? "Runtime connection is unavailable" : result.Diagnostic;
                    reply.OutcomeUnknown = reply.OutcomeUnknown || (writes && result.Outcome == RuntimeClientRequestOutcome::OutcomeUnknown);
                    if (result.Outcome != RuntimeClientRequestOutcome::Disconnected && result.Outcome != RuntimeClientRequestOutcome::OutcomeUnknown) break;
                    // Retries change the transport request identity and keep the durable command identity.
                }
            }
            catch (...) { reply.Error = "Project or board request could not be completed"; reply.OutcomeUnknown = writes; }
            try
            {
                if (dispatcher) co_await winrt::resume_foreground(dispatcher);
                if (completed) completed(std::move(reply));
            }
            catch (...) { LOG_CAUGHT_EXCEPTION(); }
        }
        class RuntimeWorkspaceSource final : public IWorkspaceSource
        {
        public:
            RuntimeWorkspaceSource(std::shared_ptr<TaskRuntimeClient> client, Windows::UI::Core::CoreDispatcher dispatcher) :
                _client{ std::move(client) }, _dispatcher{ std::move(dispatcher) } {}
            bool SupportsProjects() const noexcept override { return HasCapability(_client->NegotiatedCapabilities(), ProtocolCapability::ProjectCatalog); }
            bool SupportsBoard() const noexcept override { return HasCapability(_client->NegotiatedCapabilities(), ProtocolCapability::TaskBoard); }
            std::string Epoch() const override { return _client->RuntimeInstanceId(); }
            bool Connected() const noexcept override { return _client->State() == TaskRuntimeClientState::Ready; }
            void QueryProjects(ProjectCatalogQueryRequest request, std::function<void(WorkspaceReply<ProjectCatalogQueryResult>)> completed) override
            { send(_client, _dispatcher, ControlMethod::QueryProjects, EncodeProjectCatalogQueryRequest(request), DecodeProjectCatalogQueryResult, std::move(completed)); }
            void ExecuteProject(ProjectCatalogCommandRequest request, std::function<void(WorkspaceReply<ProjectCatalogCommandResult>)> completed) override
            { send(_client, _dispatcher, ControlMethod::ProjectCommand, EncodeProjectCatalogCommandRequest(request), DecodeProjectCatalogCommandResult, std::move(completed), request.CommandId); }
            void QuerySummaries(TaskSummaryQueryRequest request, std::function<void(WorkspaceReply<TaskSummaryQueryResult>)> completed) override
            { send(_client, _dispatcher, ControlMethod::QueryTaskSummaries, EncodeTaskSummaryQueryRequest(request), DecodeTaskSummaryQueryResult, std::move(completed)); }
            void ExecuteBoard(TaskBoardCommandRequest request, std::function<void(WorkspaceReply<TaskBoardCommandResult>)> completed) override
            { send(_client, _dispatcher, ControlMethod::TaskBoardCommand, EncodeTaskBoardCommandRequest(request), DecodeTaskBoardCommandResult, std::move(completed), request.CommandId, request.TaskId); }
            void QueryOperation(TaskBoardOperationQueryRequest request, std::function<void(WorkspaceReply<TaskBoardCommandResult>)> completed) override
            { send(_client, _dispatcher, ControlMethod::QueryBoardOperation, EncodeTaskBoardOperationQueryRequest(request), DecodeTaskBoardCommandResult, std::move(completed), request.OperationId); }
        private:
            std::shared_ptr<TaskRuntimeClient> _client;
            Windows::UI::Core::CoreDispatcher _dispatcher{ nullptr };
        };
    }
    std::shared_ptr<IWorkspaceSource> MakeRuntimeWorkspaceSource(std::shared_ptr<TaskRuntimeClient> client, Windows::UI::Core::CoreDispatcher dispatcher)
    { return std::make_shared<RuntimeWorkspaceSource>(std::move(client), std::move(dispatcher)); }
}
