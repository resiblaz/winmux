// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Microsoft::Terminal::TaskCore
{
    template<typename Tag>
    class StrongId final
    {
    public:
        StrongId() = default;
        explicit StrongId(std::string value) :
            _value{ std::move(value) }
        {
        }

        [[nodiscard]] const std::string& Value() const noexcept
        {
            return _value;
        }

        [[nodiscard]] bool Empty() const noexcept
        {
            return _value.empty();
        }

        explicit operator bool() const noexcept
        {
            return !_value.empty();
        }

        auto operator<=>(const StrongId&) const = default;

    private:
        std::string _value;
    };

    struct TaskIdTag;
    struct WorkspaceIdTag;
    struct TaskRunIdTag;
    struct TaskTerminalIdTag;
    struct ExecutionContextIdTag;
    struct AgentSessionIdTag;
    struct TaskMessageIdTag;
    struct TaskEventIdTag;
    struct CommandIdTag;
    struct TaskIntentIdTag;

    using TaskId = StrongId<TaskIdTag>;
    using WorkspaceId = StrongId<WorkspaceIdTag>;
    using TaskRunId = StrongId<TaskRunIdTag>;
    using TaskTerminalId = StrongId<TaskTerminalIdTag>;
    using ExecutionContextId = StrongId<ExecutionContextIdTag>;
    using AgentSessionId = StrongId<AgentSessionIdTag>;
    using TaskMessageId = StrongId<TaskMessageIdTag>;
    using TaskEventId = StrongId<TaskEventIdTag>;
    using CommandId = StrongId<CommandIdTag>;
    using TaskIntentId = StrongId<TaskIntentIdTag>;

    [[nodiscard]] bool IsCanonicalUuid(std::string_view value) noexcept;

    template<typename TId>
    [[nodiscard]] std::optional<TId> ParseId(const std::string_view value)
    {
        if (!IsCanonicalUuid(value))
        {
            return std::nullopt;
        }
        return TId{ std::string{ value } };
    }
}
