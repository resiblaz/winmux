// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ClientRequestPolicy.h"

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    static_assert(static_cast<uint8_t>(ControlMethod::Count) == 8,
                  "Review disconnect/replay policy whenever a control method is added");

    bool IsReadOnlyControlMethod(const ControlMethod method) noexcept
    {
        switch (method)
        {
        case ControlMethod::Ping:
        case ControlMethod::QueryTask:
        case ControlMethod::RuntimeStatus:
            return true;
        case ControlMethod::SnapshotAndSubscribe:
        case ControlMethod::Unsubscribe:
        case ControlMethod::SubmitCommand:
        case ControlMethod::AcquireInputLease:
        case ControlMethod::ReleaseInputLease:
        case ControlMethod::Count:
            return false;
        default:
            return false;
        }
    }

    ClientDisconnectAction EvaluateClientDisconnect(
        const ControlMethod method,
        const ClientRequestTransmission transmission) noexcept
    {
        switch (method)
        {
        case ControlMethod::SnapshotAndSubscribe:
            return ClientDisconnectAction::Resnapshot;
        case ControlMethod::Unsubscribe:
            return ClientDisconnectAction::FailDisconnected;
        case ControlMethod::Ping:
        case ControlMethod::QueryTask:
        case ControlMethod::RuntimeStatus:
            return ClientDisconnectAction::RetryWithNewRequestId;
        case ControlMethod::SubmitCommand:
        case ControlMethod::AcquireInputLease:
        case ControlMethod::ReleaseInputLease:
            switch (transmission)
            {
            case ClientRequestTransmission::NotSent:
                return ClientDisconnectAction::RetryWithNewRequestId;
            case ClientRequestTransmission::WriteStarted:
            case ClientRequestTransmission::WriteCompleted:
                return ClientDisconnectAction::OutcomeUnknown;
            default:
                return ClientDisconnectAction::OutcomeUnknown;
            }
        case ControlMethod::Count:
        default:
            return ClientDisconnectAction::OutcomeUnknown;
        }
    }
}
