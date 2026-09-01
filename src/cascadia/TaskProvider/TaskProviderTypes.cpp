// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProviderTypes.h"

namespace Microsoft::Terminal::TaskProvider
{
    bool HasTaskProviderCapability(
        const TaskProviderCapabilitySet capabilities,
        const TaskProviderCapability capability) noexcept
    {
        return (capabilities & static_cast<uint64_t>(capability)) != 0;
    }

    std::string_view TaskProviderEventKindName(
        const TaskProviderEventKind kind) noexcept
    {
        switch (kind)
        {
        case TaskProviderEventKind::SessionStarted: return "session_started";
        case TaskProviderEventKind::SessionResumed: return "session_resumed";
        case TaskProviderEventKind::RunStarted: return "run_started";
        case TaskProviderEventKind::AgentMessageDelta: return "agent_message_delta";
        case TaskProviderEventKind::AgentMessageCompleted: return "agent_message_completed";
        case TaskProviderEventKind::ToolStarted: return "tool_started";
        case TaskProviderEventKind::ToolProgress: return "tool_progress";
        case TaskProviderEventKind::ToolCompleted: return "tool_completed";
        case TaskProviderEventKind::FileChanged: return "file_changed";
        case TaskProviderEventKind::ApprovalRequested: return "approval_requested";
        case TaskProviderEventKind::ApprovalResolved: return "approval_resolved";
        case TaskProviderEventKind::PlanUpdated: return "plan_updated";
        case TaskProviderEventKind::DiffUpdated: return "diff_updated";
        case TaskProviderEventKind::TokenUsage: return "token_usage";
        case TaskProviderEventKind::RunCompleted: return "run_completed";
        case TaskProviderEventKind::RunFailed: return "run_failed";
        case TaskProviderEventKind::RunInterrupted: return "run_interrupted";
        case TaskProviderEventKind::ProviderError: return "provider_error";
        case TaskProviderEventKind::UnknownOptional: return "unknown_optional";
        default: return {};
        }
    }
}
