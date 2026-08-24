// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <winrt/base.h>

namespace winrt::TerminalApp::implementation
{
    // EventAt and ReceivedAt are UTC Unix timestamps in milliseconds.
    struct AgentHistoryRecord
    {
        winrt::guid WorkspaceId{};
        winrt::hstring WorkspaceTitle;
        winrt::hstring Provider;
        winrt::hstring SessionId;
        winrt::hstring TurnId;
        winrt::hstring Cwd;
        winrt::hstring Title;
        winrt::hstring Prompt;
        winrt::hstring FinalMarkdown;
        winrt::hstring Model;
        int64_t EventAt{};
        int64_t ReceivedAt{};
        bool Pinned{};
    };

    // Canonical history.ingest payload fields. Values that Terminal derives
    // locally (workspace title, fallback cwd, received time) are deliberately
    // excluded so an otherwise identical retry remains identical.
    struct AgentHistoryEventReceipt
    {
        winrt::guid EventId{};
        winrt::hstring Cwd;
        winrt::hstring Title;
        winrt::hstring Prompt;
        winrt::hstring FinalMarkdown;
        winrt::hstring Model;
        std::optional<int64_t> EventAt;
    };

    // Privacy-bounded session metadata obtained from a provider's supported
    // summary API (for example Codex App Server thread/list). CreatedAt,
    // UpdatedAt, and SeenAt are UTC Unix timestamps in milliseconds. Provider
    // identifies the local adapter (for example "codex"); Model is a concrete
    // model name only and must remain empty when the source exposes only a
    // model-provider name. HasLocalTurns is derived by ListSessionSummaries and
    // must be false on input to UpsertSessionSummary.
    struct AgentHistorySessionRecord
    {
        winrt::guid WorkspaceId{};
        winrt::hstring WorkspaceTitle;
        winrt::hstring Provider;
        winrt::hstring SessionId;
        winrt::hstring Cwd;
        winrt::hstring Model;
        winrt::hstring DisplayName;
        winrt::hstring Preview;
        winrt::hstring SourceKind;
        int64_t CreatedAt{};
        int64_t UpdatedAt{};
        int64_t SeenAt{};
        bool HasLocalTurns{};
    };

    // Zero disables the corresponding limit; zero ReceiptTombstoneDays keeps
    // tombstones indefinitely. Receipt tombstones default to 30 days so a
    // bounded history cannot be recreated immediately by a late producer retry
    // after automatic or explicit deletion.
    struct AgentHistoryRetentionPolicy
    {
        uint32_t RetentionDays{};
        uint64_t MaximumTurns{};
        uint64_t MaximumPayloadBytes{};
        uint32_t ReceiptTombstoneDays{ 30 };
    };

    struct AgentHistoryCleanupResult
    {
        bool Succeeded{};
        uint64_t TurnsDeleted{};
        uint64_t SessionsDeleted{};
        uint64_t ReceiptsDeleted{};
        uint64_t RemainingTurns{};
        uint64_t RemainingPayloadBytes{};
        // Pinned rows, an intentionally bounded cleanup batch, or both may
        // leave a soft age/count/size target unsatisfied.
        bool SoftLimitUnsatisfied{};
    };

    struct AgentHistoryQuery
    {
        std::optional<winrt::guid> WorkspaceId;
        winrt::hstring Provider;
        winrt::hstring SessionId;
        // Inclusive local-receipt time bounds in UTC Unix milliseconds.
        std::optional<int64_t> ReceivedFrom;
        std::optional<int64_t> ReceivedTo;
        winrt::hstring Search;
        uint32_t Limit{ 1000 };
    };

    class AgentHistoryStore final
    {
    public:
        explicit AgentHistoryStore(const std::filesystem::path& path,
                                   bool enableFullTextSearch = true) noexcept;
        ~AgentHistoryStore();

        AgentHistoryStore(const AgentHistoryStore&) = delete;
        AgentHistoryStore& operator=(const AgentHistoryStore&) = delete;
        AgentHistoryStore(AgentHistoryStore&&) = delete;
        AgentHistoryStore& operator=(AgentHistoryStore&&) = delete;

        [[nodiscard]] bool IsAvailable() const noexcept;
        // Prompt and final events may arrive independently. Both operations are
        // idempotent and merge into the same logical turn. When eventId is
        // supplied, its receipt and the turn merge are committed atomically.
        // Replaying the same identity and canonical payload is acknowledged
        // without modifying the turn; changing either for an existing event ID
        // is rejected.
        [[nodiscard]] bool UpsertPrompt(const AgentHistoryRecord& record,
                                        std::optional<AgentHistoryEventReceipt> event = std::nullopt) noexcept;
        [[nodiscard]] bool UpsertFinal(const AgentHistoryRecord& record,
                                       std::optional<AgentHistoryEventReceipt> event = std::nullopt) noexcept;
        // Compatibility entry point for callers that already have a completed
        // turn. This has the same validation and behavior as UpsertFinal.
        [[nodiscard]] bool UpsertTurn(const AgentHistoryRecord& record) noexcept;
        // App-server summaries are supplemental. They may create a summary-only
        // session and refresh display fields, but never replace routing metadata
        // once a local Hook turn exists. A subsequent Hook turn recomputes the
        // authoritative Workspace/cwd/model/activity metadata transactionally.
        [[nodiscard]] bool UpsertSessionSummary(const AgentHistorySessionRecord& record) noexcept;
        [[nodiscard]] std::vector<AgentHistorySessionRecord> ListSessionSummaries(std::optional<winrt::guid> workspaceId,
                                                                                  std::wstring_view search,
                                                                                  uint32_t limit = 1000) noexcept;
        [[nodiscard]] std::vector<AgentHistoryRecord> ListTurns(std::optional<winrt::guid> workspaceId,
                                                                 std::wstring_view search,
                                                                 uint32_t limit = 1000) noexcept;
        [[nodiscard]] std::vector<AgentHistoryRecord> ListTurns(const AgentHistoryQuery& query) noexcept;
        [[nodiscard]] bool SetTurnPinned(std::wstring_view provider,
                                         std::wstring_view sessionId,
                                         std::wstring_view turnId,
                                         bool pinned) noexcept;
        [[nodiscard]] bool DeleteTurn(std::wstring_view provider,
                                      std::wstring_view sessionId,
                                      std::wstring_view turnId) noexcept;
        [[nodiscard]] bool DeleteSession(std::wstring_view provider,
                                         std::wstring_view sessionId) noexcept;
        [[nodiscard]] bool DeleteWorkspace(const winrt::guid& workspaceId) noexcept;
        [[nodiscard]] bool DeleteAllUnpinned() noexcept;
        [[nodiscard]] AgentHistoryCleanupResult Cleanup(const AgentHistoryRetentionPolicy& policy,
                                                        int64_t nowUnixMilliseconds) noexcept;

        [[nodiscard]] static std::filesystem::path DefaultPath();

    private:
        [[nodiscard]] bool _UpsertEvent(const AgentHistoryRecord& record,
                                        bool isFinal,
                                        const std::optional<AgentHistoryEventReceipt>& event) noexcept;
        [[nodiscard]] bool _DeleteUnpinned(std::optional<winrt::guid> workspaceId) noexcept;

        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
