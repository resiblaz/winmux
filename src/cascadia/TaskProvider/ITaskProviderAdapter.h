// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProviderTypes.h"

namespace Microsoft::Terminal::TaskProvider
{
    class ITaskProviderAdapter
    {
    public:
        virtual ~ITaskProviderAdapter() = default;

        [[nodiscard]] virtual TaskProviderCapabilitiesResult ProbeCapabilities(
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderSessionResult StartSession(
            const TaskProviderSessionRequest& request,
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderSessionResult ResumeSession(
            std::string_view sessionId,
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderRunResult StartRun(
            const TaskProviderRunRequest& request,
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderResult SteerRun(
            std::string_view sessionId,
            std::string_view runId,
            std::string_view message,
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderResult InterruptRun(
            std::string_view sessionId,
            std::string_view runId,
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderEventResult NextEvent(
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderResult RespondToApproval(
            std::string_view sessionId,
            std::string_view runId,
            std::string_view approvalId,
            TaskProviderApprovalDecision decision,
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderResult SuspendSession(
            std::string_view sessionId,
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderResult CloseSessionProjection(
            std::string_view sessionId,
            HANDLE cancellationEvent) = 0;
        [[nodiscard]] virtual TaskProviderFailureDisposition
            ClassifyFailureAndResumeSafety(
                TaskProviderStatus status,
                std::string_view providerCode) const noexcept = 0;
        virtual void Stop() noexcept = 0;
    };
}
