// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include "WorkspaceSnapshotStore.h"
#include "WorkspaceSnapshotValidation.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    enum class WorkspaceSnapshotImportMode
    {
        Fail,
        Replace,
        Copy,
    };

    enum class WorkspaceSnapshotExchangeStatus
    {
        Succeeded,
        Unchanged,
        NotFound,
        DestinationExists,
        Conflict,
        MalformedJson,
        UnsupportedFormat,
        UnsupportedFormatVersion,
        UnknownField,
        InvalidEnvelope,
        InvalidSnapshot,
        UnsafeAction,
        ProfileLookupUnavailable,
        ProfileNotFound,
        Oversized,
        CapacityExceeded,
        ScopeUnavailable,
        IoError,
    };

    struct WorkspaceSnapshotExchangeResult
    {
        WorkspaceSnapshotExchangeStatus Status{ WorkspaceSnapshotExchangeStatus::IoError };
        WorkspaceSnapshotStoreStatus UnderlyingStoreStatus{ WorkspaceSnapshotStoreStatus::IoError };
        WorkspaceSnapshotValidationError ValidationError{ WorkspaceSnapshotValidationError::None };
        Model::WorkspaceSnapshot Snapshot{ nullptr };
        winrt::hstring MissingProfile;

        explicit operator bool() const noexcept
        {
            return Status == WorkspaceSnapshotExchangeStatus::Succeeded ||
                   Status == WorkspaceSnapshotExchangeStatus::Unchanged;
        }
    };

    class WorkspaceSnapshotExchange final
    {
    public:
        using ProfileExistsCallback = std::function<bool(const winrt::hstring&)>;

        static constexpr std::string_view Format{ "com.microsoft.windows-terminal.workspace" };
        static constexpr uint32_t FormatVersion{ 1 };
        static constexpr size_t MaximumEnvelopeBytes{ 512 * 1024 };

        static WorkspaceSnapshotExchangeResult ExportToFile(const WorkspaceSnapshotStore& store,
                                                             const winrt::guid& snapshotId,
                                                             const std::filesystem::path& destination,
                                                             bool force = false,
                                                             std::optional<int64_t> exportedAt = std::nullopt) noexcept;

        static WorkspaceSnapshotExchangeResult ImportFromFile(WorkspaceSnapshotStore& store,
                                                               const std::filesystem::path& source,
                                                               WorkspaceSnapshotImportMode mode,
                                                               const ProfileExistsCallback& profileExists,
                                                               std::optional<int64_t> importedAt = std::nullopt) noexcept;
    };
}
