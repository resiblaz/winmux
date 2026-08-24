// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <span>

#include "WorkspaceCommandline.g.h"
#include "WorkspaceCommandlineResult.g.h"

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    enum class WorkspaceCommandKind : uint8_t
    {
        List,
        Open,
        Export,
        Import,
    };

    enum class WorkspaceListFormat : uint8_t
    {
        Table,
        Json,
    };

    enum class WorkspaceImportMode : uint8_t
    {
        Fail,
        Replace,
        Copy,
    };

    enum class WorkspaceCommandlineExitCode : int32_t
    {
        Succeeded = 0,
        InvalidArguments = 2,
        NotFound = 3,
        Conflict = 4,
        InvalidData = 5,
        ProfileNotFound = 6,
        Unavailable = 7,
        IoError = 8,
    };

    struct WorkspaceCommandlineRequest
    {
        WorkspaceCommandKind Kind{ WorkspaceCommandKind::List };
        WorkspaceListFormat ListFormat{ WorkspaceListFormat::Table };
        WorkspaceImportMode ImportMode{ WorkspaceImportMode::Fail };
        winrt::guid SnapshotId{};
        winrt::hstring Path;
        winrt::hstring WindowTarget;
        bool Force{};
    };

    struct WorkspaceCommandlineParseResult
    {
        bool Succeeded{};
        WorkspaceCommandlineRequest Request;
        winrt::hstring Error;
    };

    WorkspaceCommandlineParseResult ParseWorkspaceCommandline(std::span<const winrt::hstring> arguments) noexcept;

    struct WorkspaceCommandlineResult : WorkspaceCommandlineResultT<WorkspaceCommandlineResult>
    {
        WorkspaceCommandlineResult(int32_t exitCode,
                                   winrt::hstring standardOutput,
                                   winrt::hstring standardError,
                                   bool openRequested = false,
                                   const winrt::guid& snapshotId = {},
                                   winrt::hstring windowTarget = {});

        int32_t ExitCode() const noexcept;
        winrt::hstring StandardOutput() const;
        winrt::hstring StandardError() const;
        bool OpenRequested() const noexcept;
        winrt::guid SnapshotId() const noexcept;
        winrt::hstring WindowTarget() const;

    private:
        int32_t _exitCode{};
        winrt::hstring _standardOutput;
        winrt::hstring _standardError;
        bool _openRequested{};
        winrt::guid _snapshotId{};
        winrt::hstring _windowTarget;
    };

    struct WorkspaceCommandline : WorkspaceCommandlineT<WorkspaceCommandline>
    {
        WorkspaceCommandline() = default;

        Model::WorkspaceCommandlineResult Execute(
            const Windows::Foundation::Collections::IVectorView<winrt::hstring>& arguments) noexcept;
    };
}

namespace winrt::Microsoft::Terminal::Settings::Model::factory_implementation
{
    BASIC_FACTORY(WorkspaceCommandline);
}
