// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ProtocolRpc.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    enum class ClientRequestTransmission
    {
        NotSent,
        WriteStarted,
        WriteCompleted,
    };

    enum class ClientDisconnectAction
    {
        RetryWithNewRequestId,
        Resnapshot,
        FailDisconnected,
        OutcomeUnknown,
    };

    [[nodiscard]] bool IsReadOnlyControlMethod(ControlMethod method) noexcept;
    [[nodiscard]] ClientDisconnectAction EvaluateClientDisconnect(
        ControlMethod method,
        ClientRequestTransmission transmission) noexcept;
}
