// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskProviderVtRenderer.h"

namespace Microsoft::Terminal::TaskProvider
{
    TaskProviderVtRenderResult RenderTaskProviderEventAsVt(
        const TaskProviderEvent& event) noexcept
    {
        TaskProviderVtRenderResult result;
        try
        {
            if (event.Text.size() > MaximumProviderTextBytes ||
                event.Text.find('\0') != std::string::npos)
            {
                result.Error = "Task provider event text is invalid";
                return result;
            }
            switch (event.Kind)
            {
            case TaskProviderEventKind::AgentMessageDelta:
                result.Bytes = event.Text;
                break;
            case TaskProviderEventKind::AgentMessageCompleted:
                result.Bytes = event.Text + "\r\n";
                break;
            case TaskProviderEventKind::ToolStarted:
                result.Bytes = "\x1b[36m[tool] " + event.Text + "\x1b[0m\r\n";
                break;
            case TaskProviderEventKind::ToolProgress:
                result.Bytes = "\x1b[2m[tool] " + event.Text + "\x1b[0m\r\n";
                break;
            case TaskProviderEventKind::ToolCompleted:
                result.Bytes = "\x1b[32m[tool] " + event.Text + "\x1b[0m\r\n";
                break;
            case TaskProviderEventKind::ApprovalRequested:
                result.Bytes = "\x1b[33m[approval required] " + event.Text + "\x1b[0m\r\n";
                break;
            case TaskProviderEventKind::RunFailed:
            case TaskProviderEventKind::ProviderError:
                result.Bytes = "\x1b[31m[error] " + event.Text + "\x1b[0m\r\n";
                break;
            case TaskProviderEventKind::RunInterrupted:
                result.Bytes = "\x1b[33m[interrupted]\x1b[0m\r\n";
                break;
            case TaskProviderEventKind::PlanUpdated:
                result.Bytes = "\x1b[35m[plan] " + event.Text + "\x1b[0m\r\n";
                break;
            case TaskProviderEventKind::DiffUpdated:
                result.Bytes = "\x1b[34m[diff updated]\x1b[0m\r\n";
                break;
            case TaskProviderEventKind::RunCompleted:
                result.Bytes = "\x1b[32m[completed]\x1b[0m\r\n";
                break;
            case TaskProviderEventKind::UnknownOptional:
            case TaskProviderEventKind::TokenUsage:
                break;
            default:
                result.Bytes = "\x1b[2m[" + std::string{ TaskProviderEventKindName(event.Kind) } + "]\x1b[0m\r\n";
                break;
            }
            if (result.Bytes.size() > MaximumProviderPayloadBytes)
            {
                result.Error = "Task provider VT projection exceeds its bound";
                result.Bytes.clear();
                return result;
            }
            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            result.Error = "Task provider VT projection exhausted local resources";
            return result;
        }
    }
}
