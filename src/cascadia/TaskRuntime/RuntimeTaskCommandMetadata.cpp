// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskCommandMetadata.h"

#include "RuntimeCrypto.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;

    int64_t RuntimeUnixMilliseconds() noexcept
    {
        FILETIME fileTime{};
        GetSystemTimeAsFileTime(&fileTime);
        ULARGE_INTEGER ticks{};
        ticks.LowPart = fileTime.dwLowDateTime;
        ticks.HighPart = fileTime.dwHighDateTime;
        constexpr uint64_t epoch{ 116444736000000000ull };
        return ticks.QuadPart < epoch ? 0 : static_cast<int64_t>((ticks.QuadPart - epoch) / 10'000);
    }

    std::optional<TaskCommandMetadata> MakeRuntimeTaskCommandMetadata(
        const std::string_view domain,
        const TaskId& task,
        const std::string_view operationIdentity,
        const uint64_t expectedTaskVersion,
        const int64_t minimumTimestamp,
        std::string eventPayloadJson,
        std::string receiptResultJson)
    {
        try
        {
            const auto command = NewRuntimeUuid();
            const auto event = NewRuntimeUuid();
            std::vector<uint8_t> digestInput;
            const auto append = [&](const std::string_view value) {
                digestInput.insert(digestInput.end(), value.begin(), value.end());
                digestInput.push_back(0);
            };
            append(domain);
            append(task.Value());
            append(operationIdentity);
            append(eventPayloadJson);
            const auto digest = RuntimeSha256Hex(digestInput);
            if (!command || !event || !digest || domain.empty() || operationIdentity.empty() ||
                eventPayloadJson.empty() || eventPayloadJson.size() > MaximumTaskStoreJsonBytes ||
                receiptResultJson.empty() || receiptResultJson.size() > MaximumTaskStoreJsonBytes)
            {
                return std::nullopt;
            }
            TaskCommandMetadata result;
            result.Command = CommandId{ *command };
            result.Event = TaskEventId{ *event };
            result.ExpectedTaskVersion = expectedTaskVersion;
            result.RequestDigest = *digest;
            result.EventPayloadJson = std::move(eventPayloadJson);
            result.ReceiptResultJson = std::move(receiptResultJson);
            result.TimestampUnixMilliseconds = (std::max)(RuntimeUnixMilliseconds(), minimumTimestamp);
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
