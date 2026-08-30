// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientSubscriptions.h"

#include <unordered_map>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeClientSubscriptionRequestRecord
    {
        RuntimeClientLogicalSubscriptionId LogicalId{};
        RuntimeClientSubscriptionRevision Revision{};
        RuntimeClientEpoch Epoch{};
        RuntimeClientSubscriptionRequestKind Kind{ RuntimeClientSubscriptionRequestKind::Snapshot };
    };

    struct RuntimeClientSubscriptionRecord
    {
        RuntimeClientSubscriptionRevision Revision{};
        RuntimeClientSubscriptionPhase Phase{ RuntimeClientSubscriptionPhase::Desired };
        RuntimeClientEpoch Epoch{};
        std::string RequestId;
        Microsoft::Terminal::TaskRuntimeProtocol::SubscriptionCursor Cursor;
    };

    struct RuntimeClientSubscriptionRegistry::Impl
    {
        RuntimeClientSubscriptionConfiguration Configuration;
        bool IsValid{};
        std::unordered_map<RuntimeClientLogicalSubscriptionId, RuntimeClientSubscriptionRecord> Records;
        std::unordered_map<std::string, RuntimeClientSubscriptionRequestRecord> RequestBindings;
        std::unordered_map<std::string, RuntimeClientLogicalSubscriptionId> WireOwners;

        explicit Impl(const RuntimeClientSubscriptionConfiguration configuration) :
            Configuration{ configuration },
            IsValid{ configuration.MaximumSubscriptions != 0 &&
                     configuration.MaximumSubscriptions <= AbsoluteMaximumRuntimeClientSubscriptions }
        {
            if (IsValid)
            {
                try
                {
                    Records.reserve(Configuration.MaximumSubscriptions);
                    RequestBindings.reserve(Configuration.MaximumSubscriptions);
                    WireOwners.reserve(Configuration.MaximumSubscriptions);
                }
                catch (...)
                {
                    IsValid = false;
                }
            }
        }

        void RemoveRequest(RuntimeClientSubscriptionRecord& record) noexcept
        {
            if (!record.RequestId.empty())
            {
                RequestBindings.erase(record.RequestId);
                record.RequestId.clear();
            }
        }

        void RemoveWire(RuntimeClientSubscriptionRecord& record) noexcept
        {
            if (!record.Cursor.SubscriptionId().empty())
            {
                WireOwners.erase(record.Cursor.SubscriptionId());
                record.Cursor.Reset();
            }
        }
    };
}
