// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::TaskProvider
{
    inline constexpr size_t MaximumProviderIdentifierBytes{ 256 };
    inline constexpr size_t MaximumProviderTextBytes{ 64 * 1024 };
    inline constexpr size_t MaximumProviderPayloadBytes{ 256 * 1024 };
    inline constexpr size_t MaximumProviderDiagnosticBytes{ 4096 };
    inline constexpr size_t MaximumProviderEnvironmentEntries{ 64 };
    inline constexpr size_t MaximumProviderEnvironmentBytes{ 64 * 1024 };
    inline constexpr size_t MaximumProviderQueuedEvents{ 4096 };

    enum class TaskProviderStatus : uint8_t
    {
        Succeeded,
        AlreadyApplied,
        InvalidRequest,
        Unavailable,
        UnsupportedVersion,
        UnsupportedCapability,
        NotFound,
        Conflict,
        Rejected,
        Timeout,
        Cancelled,
        TransportClosed,
        MalformedResponse,
        OutputLimitExceeded,
        ResourceFailure,
    };

    enum class TaskProviderCapability : uint64_t
    {
        ThreadStart = 1ull << 0,
        ThreadResume = 1ull << 1,
        TurnStart = 1ull << 2,
        TurnSteer = 1ull << 3,
        TurnInterrupt = 1ull << 4,
        ThreadUnsubscribe = 1ull << 5,
        ApprovalResponse = 1ull << 6,
        StructuredEvents = 1ull << 7,
    };

    using TaskProviderCapabilitySet = uint64_t;

    inline constexpr TaskProviderCapabilitySet RequiredTaskProviderCapabilities{
        static_cast<uint64_t>(TaskProviderCapability::ThreadStart) |
        static_cast<uint64_t>(TaskProviderCapability::ThreadResume) |
        static_cast<uint64_t>(TaskProviderCapability::TurnStart) |
        static_cast<uint64_t>(TaskProviderCapability::TurnSteer) |
        static_cast<uint64_t>(TaskProviderCapability::TurnInterrupt) |
        static_cast<uint64_t>(TaskProviderCapability::ThreadUnsubscribe) |
        static_cast<uint64_t>(TaskProviderCapability::ApprovalResponse) |
        static_cast<uint64_t>(TaskProviderCapability::StructuredEvents)
    };

    enum class TaskProviderSessionState : uint8_t
    {
        Starting,
        Ready,
        Busy,
        Suspended,
        Closing,
        ClosePending,
        Closed,
        Poisoned,
    };

    enum class TaskProviderRunState : uint8_t
    {
        Starting,
        Running,
        Completed,
        Failed,
        Interrupted,
    };

    enum class TaskProviderEventKind : uint8_t
    {
        SessionStarted,
        SessionResumed,
        RunStarted,
        AgentMessageDelta,
        AgentMessageCompleted,
        ToolStarted,
        ToolProgress,
        ToolCompleted,
        FileChanged,
        ApprovalRequested,
        ApprovalResolved,
        PlanUpdated,
        DiffUpdated,
        TokenUsage,
        RunCompleted,
        RunFailed,
        RunInterrupted,
        ProviderError,
        UnknownOptional,
    };

    enum class TaskProviderApprovalDecision : uint8_t
    {
        Approved,
        Denied,
        Cancelled,
    };

    enum class TaskProviderFailureClass : uint8_t
    {
        None,
        RuntimeRecovery,
        TransientNetwork,
        InactivityTimeout,
        Authentication,
        Authorization,
        Quota,
        Configuration,
        ContextOverflow,
        InvalidRequest,
        ProviderProtocol,
        Unclassified,
    };

    enum class TaskProviderResumeSafety : uint8_t
    {
        Safe,
        FreshSessionRequired,
        Unsafe,
    };

    struct TaskProviderEnvironmentEntry
    {
        std::wstring Name;
        std::wstring Value;
    };

    struct TaskProviderCapabilities
    {
        std::string Provider;
        std::string ProviderVersion;
        uint32_t ProtocolMajor{};
        uint32_t ProtocolMinor{};
        TaskProviderCapabilitySet Capabilities{};
    };

    struct TaskProviderRunContext
    {
        std::string TaskId;
        std::string RunId;
        std::filesystem::path WorkingDirectory;
        std::string Model;
        std::string Instructions;
        std::filesystem::path RtaskExecutable;
        std::string RtaskEndpoint;
        std::string RtaskCapability;
        std::vector<TaskProviderEnvironmentEntry> Environment;
    };

    struct TaskProviderSessionRequest
    {
        std::string TaskId;
        std::filesystem::path WorkingDirectory;
        std::string Model;
    };

    struct TaskProviderRunRequest
    {
        std::string SessionId;
        std::string Prompt;
        TaskProviderRunContext Context;
    };

    struct TaskProviderSession
    {
        std::string SessionId;
        TaskProviderSessionState State{ TaskProviderSessionState::Starting };
    };

    struct TaskProviderRun
    {
        std::string SessionId;
        std::string RunId;
        TaskProviderRunState State{ TaskProviderRunState::Starting };
    };

    struct TaskProviderEvent
    {
        uint64_t Sequence{};
        TaskProviderEventKind Kind{ TaskProviderEventKind::UnknownOptional };
        std::string SessionId;
        std::string RunId;
        std::string ItemId;
        std::string ApprovalId;
        std::string Text;
        std::string PayloadJson{ "{}" };
        bool Redacted{};
        bool Truncated{};
        size_t OriginalBytes{};
    };

    struct TaskProviderFailureDisposition
    {
        TaskProviderFailureClass Classification{ TaskProviderFailureClass::Unclassified };
        TaskProviderResumeSafety ResumeSafety{ TaskProviderResumeSafety::Unsafe };
        bool AutomaticRetryAllowed{};
        uint32_t MaximumExecutions{ 1 };
        std::string ReasonCode{ "provider-unclassified" };
    };

    struct TaskProviderResult
    {
        TaskProviderStatus Status{ TaskProviderStatus::ResourceFailure };
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == TaskProviderStatus::Succeeded ||
                   Status == TaskProviderStatus::AlreadyApplied;
        }
    };

    struct TaskProviderCapabilitiesResult : TaskProviderResult
    {
        std::optional<TaskProviderCapabilities> Value;
    };

    struct TaskProviderSessionResult : TaskProviderResult
    {
        std::optional<TaskProviderSession> Value;
    };

    struct TaskProviderRunResult : TaskProviderResult
    {
        std::optional<TaskProviderRun> Value;
    };

    struct TaskProviderEventResult : TaskProviderResult
    {
        std::optional<TaskProviderEvent> Value;
        bool NoEvent{};
    };

    [[nodiscard]] bool HasTaskProviderCapability(
        TaskProviderCapabilitySet capabilities,
        TaskProviderCapability capability) noexcept;
    [[nodiscard]] std::string_view TaskProviderEventKindName(
        TaskProviderEventKind kind) noexcept;
}
