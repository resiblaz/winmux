// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <winrt/base.h>

namespace winrt::TerminalApp::implementation
{
    class AgentHistoryStore;

    struct AgentHistoryInboxWorkspace
    {
        winrt::guid Id{};
        winrt::hstring Title;
        winrt::hstring Cwd;
    };

    struct AgentHistoryInboxImportResult
    {
        size_t EntriesInspected{};
        size_t FilesVisited{};
        size_t Committed{};
        size_t Deleted{};
        size_t Deferred{};
        size_t Quarantined{};
        size_t Retained{};
        bool LimitReached{};
    };

    // Imports bounded, schema-validated history events from one or more
    // explicitly supplied directories. Supplying paths in the constructor keeps
    // the filesystem boundary deterministic in tests.
    class AgentHistoryInbox final
    {
    public:
        static constexpr size_t MaximumEventBytes{ 64 * 1024 };
        static constexpr size_t MaximumEntriesPerDirectory{ 512 };
        static constexpr size_t MaximumFilesPerDirectory{ 256 };
        static constexpr size_t MaximumQuarantineFiles{ 256 };
        static constexpr uintmax_t MaximumQuarantineBytes{ 8 * 1024 * 1024 };
        static constexpr size_t EmergencySlotCount{ 32 };

        AgentHistoryInbox(AgentHistoryStore& store, std::vector<std::filesystem::path> inboxPaths);

        [[nodiscard]] AgentHistoryInboxImportResult Import(std::span<const AgentHistoryInboxWorkspace> workspaces) noexcept;

        [[nodiscard]] static std::filesystem::path SharedPathForLocalAppData(const std::filesystem::path& localAppDataRoot) noexcept;
        [[nodiscard]] static std::filesystem::path PrivatePathForSettingsFile(const std::filesystem::path& settingsFile) noexcept;
        [[nodiscard]] static std::filesystem::path QuarantinePath(const std::filesystem::path& inboxPath) noexcept;
        [[nodiscard]] static std::vector<std::filesystem::path> DefaultPaths() noexcept;

    private:
        AgentHistoryStore& _store;
        std::vector<std::filesystem::path> _inboxPaths;
    };
}
