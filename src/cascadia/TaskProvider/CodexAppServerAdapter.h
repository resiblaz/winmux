// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "CodexAppServerEventParser.h"
#include "CodexAppServerJsonRpc.h"
#include "ITaskProviderAdapter.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace Microsoft::Terminal::TaskProvider
{
    struct CodexAppServerTimeouts
    {
        uint32_t InitializeMilliseconds{ 10'000 };
        uint32_t SessionMilliseconds{ 15'000 };
        uint32_t RunMilliseconds{ 15'000 };
        uint32_t CommandMilliseconds{ 5'000 };

        [[nodiscard]] bool Valid() const noexcept;
    };

    class CodexAppServerAdapter final : public ITaskProviderAdapter
    {
    public:
        explicit CodexAppServerAdapter(
            CodexAppServerTransportFactory transportFactory,
            CodexAppServerTimeouts timeouts = {});
        ~CodexAppServerAdapter() override;

        CodexAppServerAdapter(const CodexAppServerAdapter&) = delete;
        CodexAppServerAdapter& operator=(const CodexAppServerAdapter&) = delete;

        [[nodiscard]] TaskProviderCapabilitiesResult ProbeCapabilities(HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderSessionResult StartSession(const TaskProviderSessionRequest& request, HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderSessionResult ResumeSession(std::string_view sessionId, HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderRunResult StartRun(const TaskProviderRunRequest& request, HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderResult SteerRun(std::string_view sessionId, std::string_view runId, std::string_view message, HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderResult InterruptRun(std::string_view sessionId, std::string_view runId, HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderEventResult NextEvent(uint32_t timeoutMilliseconds, HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderResult RespondToApproval(std::string_view sessionId, std::string_view runId, std::string_view approvalId, TaskProviderApprovalDecision decision, HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderResult SuspendSession(std::string_view sessionId, HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderResult CloseSessionProjection(std::string_view sessionId, HANDLE cancellationEvent) override;
        [[nodiscard]] TaskProviderFailureDisposition ClassifyFailureAndResumeSafety(TaskProviderStatus status, std::string_view providerCode) const noexcept override;
        void Stop() noexcept override;

    private:
        CodexAppServerJsonRpc _rpc;
        CodexAppServerTimeouts _timeouts;
        std::mutex _stateMutex;
        std::mutex _initializeMutex;
        bool _initialized{};
        bool _stopped{};
        uint64_t _nextEventSequence{ 1 };
        std::optional<TaskProviderCapabilities> _capabilities;
        std::unordered_map<std::string, CodexPendingApproval> _approvals;
        std::unordered_map<std::string, std::string> _initialRunBySession;
        std::unordered_set<std::string> _pendingUnarchive;

        [[nodiscard]] TaskProviderResult _ensureInitialized(HANDLE cancellationEvent);
        [[nodiscard]] TaskProviderResult _unsubscribe(std::string_view sessionId, HANDLE cancellationEvent);
    };

    [[nodiscard]] std::unique_ptr<ITaskProviderAdapter> MakeCodexAppServerAdapter(
        const std::filesystem::path& executable,
        CodexAppServerTimeouts timeouts = {});
}
