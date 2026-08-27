// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceSnapshot.g.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    enum class WorkspaceSnapshotValidationError
    {
        None,
        NullSnapshot,
        UnsupportedVersion,
        InvalidRevision,
        InvalidIdentity,
        InvalidName,
        InvalidRootDirectory,
        InvalidTimestamp,
        InvalidActions,
        InvalidAgentResume,
        InvalidGitBinding,
        SerializedSizeExceeded,
    };

    struct WorkspaceSnapshotValidationResult
    {
        WorkspaceSnapshotValidationError Error{ WorkspaceSnapshotValidationError::None };

        explicit operator bool() const noexcept
        {
            return Error == WorkspaceSnapshotValidationError::None;
        }
    };

    class WorkspaceSnapshotValidator final
    {
    public:
        static constexpr uint32_t CurrentVersion{ 2 };
        static constexpr uint32_t LegacyVersion{ 1 };
        static constexpr uint32_t MaximumSnapshots{ 128 };
        static constexpr uint32_t MaximumActions{ 256 };
        static constexpr uint32_t MaximumSurfaces{ 32 };
        static constexpr uint32_t MaximumAgentResumeEntries{ 256 };
        static constexpr size_t MaximumNameCharacters{ 160 };
        static constexpr size_t MaximumTextCharacters{ 4096 };
        static constexpr size_t MaximumBranchCharacters{ 1024 };
        static constexpr size_t MaximumIdentifierCharacters{ 128 };
        static constexpr size_t MaximumSerializedBytes{ 256 * 1024 };
        static constexpr int64_t MaximumUnixMilliseconds{ 253402300799999ll };

        static WorkspaceSnapshotValidationResult Validate(const Model::WorkspaceSnapshot& snapshot,
                                                          bool allowZeroRevision = false) noexcept;
        static bool IsSafeActions(const Windows::Foundation::Collections::IVector<Model::ActionAndArgs>& actions) noexcept;
        static Model::WorkspaceSnapshot DeepCopy(const Model::WorkspaceSnapshot& snapshot) noexcept;
        static Model::WorkspaceSnapshot UpgradeLegacy(const Model::WorkspaceSnapshot& snapshot) noexcept;
        static bool EquivalentPortableContent(const Model::WorkspaceSnapshot& left,
                                              const Model::WorkspaceSnapshot& right) noexcept;
    };
}
