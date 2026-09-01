// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProviderTypes.h"

#include <json/json.h>

namespace Microsoft::Terminal::TaskProvider
{
    struct CodexPendingApproval
    {
        Json::Value RequestId;
        std::string Method;
        std::string SessionId;
        std::string RunId;
        std::string ApprovalId;
        Json::Value RequestedPermissions;
    };

    struct CodexEventParseResult : TaskProviderResult
    {
        std::optional<TaskProviderEvent> Event;
        std::optional<CodexPendingApproval> Approval;
    };

    [[nodiscard]] CodexEventParseResult ParseCodexAppServerEvent(
        const Json::Value& message,
        uint64_t sequence) noexcept;
    [[nodiscard]] Json::Value CodexApprovalResponse(
        const CodexPendingApproval& approval,
        TaskProviderApprovalDecision decision) noexcept;
}
