// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "WorkspaceSnapshot.g.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    enum class WorkspaceSnapshotStoreScope
    {
        Standard,
        Elevated,
    };

    enum class WorkspaceSnapshotStoreStatus
    {
        Succeeded,
        Unchanged,
        NotFound,
        Conflict,
        Invalid,
        Corrupt,
        Oversized,
        CapacityExceeded,
        ScopeUnavailable,
        IoError,
    };

    struct WorkspaceSnapshotStoreRecord
    {
        WorkspaceSnapshotStoreStatus Status{ WorkspaceSnapshotStoreStatus::NotFound };
        Model::WorkspaceSnapshot Snapshot{ nullptr };

        explicit operator bool() const noexcept
        {
            return Status == WorkspaceSnapshotStoreStatus::Succeeded ||
                   Status == WorkspaceSnapshotStoreStatus::Unchanged;
        }
    };

    class WorkspaceSnapshotStore final
    {
    public:
        WorkspaceSnapshotStore(const std::filesystem::path& stateRoot,
                               WorkspaceSnapshotStoreScope scope) noexcept;

        WorkspaceSnapshotStoreRecord Load(const winrt::guid& id) const noexcept;
        std::vector<Model::WorkspaceSnapshot> List() const noexcept;
        WorkspaceSnapshotStoreRecord Commit(const Model::WorkspaceSnapshot& snapshot,
                                            std::optional<uint64_t> expectedRevision) noexcept;
        WorkspaceSnapshotStoreRecord TouchLastOpened(const winrt::guid& id,
                                                     const winrt::guid& workspaceId,
                                                     int64_t timestamp) noexcept;
        WorkspaceSnapshotStoreStatus Remove(const winrt::guid& id,
                                            uint64_t expectedRevision) noexcept;
        WorkspaceSnapshotStoreStatus Clear() noexcept;

        const std::filesystem::path& StoreRoot() const noexcept;
        WorkspaceSnapshotStoreScope Scope() const noexcept;

    private:
        std::filesystem::path _stateRoot;
        std::filesystem::path _storeRoot;
        std::wstring _mutexName;
        WorkspaceSnapshotStoreScope _scope;

        bool _available() const noexcept;
        WorkspaceSnapshotStoreStatus _ensureRoot(bool create) const noexcept;
        std::filesystem::path _snapshotDirectory(const winrt::guid& id) const;
        std::filesystem::path _snapshotPath(const winrt::guid& id) const;
        WorkspaceSnapshotStoreRecord _loadUnlocked(const winrt::guid& id) const noexcept;
        WorkspaceSnapshotStoreStatus _writeUnlocked(const Model::WorkspaceSnapshot& snapshot) const noexcept;
        size_t _countSnapshotsUnlocked() const noexcept;
    };
}
