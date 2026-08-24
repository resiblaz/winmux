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

    class AgentHistoryStore final
    {
    public:
        explicit AgentHistoryStore(const std::filesystem::path& path) noexcept;
        ~AgentHistoryStore();

        AgentHistoryStore(const AgentHistoryStore&) = delete;
        AgentHistoryStore& operator=(const AgentHistoryStore&) = delete;
        AgentHistoryStore(AgentHistoryStore&&) = delete;
        AgentHistoryStore& operator=(AgentHistoryStore&&) = delete;

        [[nodiscard]] bool IsAvailable() const noexcept;
        [[nodiscard]] bool UpsertTurn(const AgentHistoryRecord& record) noexcept;
        [[nodiscard]] std::vector<AgentHistoryRecord> ListTurns(std::optional<winrt::guid> workspaceId,
                                                                 std::wstring_view search,
                                                                 uint32_t limit = 1000) noexcept;
        [[nodiscard]] bool SetTurnPinned(std::wstring_view provider,
                                         std::wstring_view sessionId,
                                         std::wstring_view turnId,
                                         bool pinned) noexcept;
        [[nodiscard]] bool DeleteTurn(std::wstring_view provider,
                                      std::wstring_view sessionId,
                                      std::wstring_view turnId) noexcept;
        [[nodiscard]] bool DeleteSession(std::wstring_view provider,
                                         std::wstring_view sessionId) noexcept;

        [[nodiscard]] static std::filesystem::path DefaultPath();

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };
}
