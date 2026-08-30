// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRuntimeProcessTestFixtures.h"
#include "TaskRuntimeProcessWin32Support.h"

#include "RuntimeProcessLaunchTypes.h"

#include <filesystem>
#include <optional>
#include <string>

namespace TaskRuntimeLocalTests::OnDemandActivation
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace TaskRuntimeLocalTests::ProcessFixtures;

    inline constexpr DWORD ActivationParentExitTimeoutMilliseconds{ 30000 };
    inline constexpr DWORD RuntimeCleanupExitCode{ 0xe000d2d0 };

    struct ActivationLaunchRecord
    {
        size_t Calls{};
        int Status{ -1 };
        int Phase{ -1 };
        int Strategy{ -1 };
        DWORD SystemError{};
    };

    struct ActivationParentOptions;
    struct ActivationParentLaunchResult;

    class TemporaryActivationRecord final
    {
    public:
        TemporaryActivationRecord(
            const ProductRuntimeTestIdentity& identity,
            size_t index);
        ~TemporaryActivationRecord();

        TemporaryActivationRecord(const TemporaryActivationRecord&) = delete;
        TemporaryActivationRecord& operator=(const TemporaryActivationRecord&) = delete;

        [[nodiscard]] const std::filesystem::path& Path() const noexcept;
        [[nodiscard]] std::optional<ActivationLaunchRecord> Read() const;

    private:
        std::filesystem::path _path;
    };

    class ActivationParentProcess final
    {
    public:
        ActivationParentProcess() noexcept = default;
        ~ActivationParentProcess();

        ActivationParentProcess(const ActivationParentProcess&) = delete;
        ActivationParentProcess& operator=(const ActivationParentProcess&) = delete;
        ActivationParentProcess(ActivationParentProcess&&) noexcept = default;
        ActivationParentProcess& operator=(ActivationParentProcess&&) noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] HANDLE Handle() const noexcept;
        [[nodiscard]] DWORD ProcessId() const noexcept;
        [[nodiscard]] bool WaitForExit(DWORD timeoutMilliseconds, DWORD& exitCode) noexcept;

    private:
        friend ActivationParentLaunchResult LaunchActivationParent(
            const ProductRuntimeTestIdentity& identity,
            ActivationParentOptions options);
        details::HandleOwner _process;
        DWORD _processId{};
    };

    struct ActivationParentLaunchResult
    {
        ActivationParentProcess Process;
        DWORD SystemError{};

        [[nodiscard]] bool Succeeded() const noexcept;
    };

    class ActivationTestJob final
    {
    public:
        explicit ActivationTestJob(DWORD limitFlags) noexcept;

        ActivationTestJob(const ActivationTestJob&) = delete;
        ActivationTestJob& operator=(const ActivationTestJob&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] HANDLE Handle() const noexcept;
        [[nodiscard]] DWORD SystemError() const noexcept;

    private:
        details::HandleOwner _job;
        DWORD _systemError{};
    };

    struct ActivationParentOptions
    {
        std::string ClientInstanceId;
        std::optional<std::wstring> StartEventName;
        std::optional<std::filesystem::path> LaunchRecord;
        HANDLE Job{};
        DWORD AdditionalCreationFlags{};
    };

    [[nodiscard]] ActivationParentLaunchResult LaunchActivationParent(
        const ProductRuntimeTestIdentity& identity,
        ActivationParentOptions options);
    [[nodiscard]] std::filesystem::path ActivationParentExecutable();
    [[nodiscard]] std::string ActivationClientInstanceId(size_t index);
    [[nodiscard]] std::wstring ActivationStartEventName(
        const ProductRuntimeTestIdentity& identity);

    struct ActiveRuntimeProbe
    {
        ProductControlClient Client;
        DWORD ProcessId{};
    };

    [[nodiscard]] ActiveRuntimeProbe RequireActiveRuntime(
        const ProductRuntimeTestIdentity& identity,
        std::string_view clientInstanceId);

    class ActiveRuntimeProcessGuard final
    {
    public:
        explicit ActiveRuntimeProcessGuard(DWORD processId) noexcept;
        ~ActiveRuntimeProcessGuard();

        ActiveRuntimeProcessGuard(const ActiveRuntimeProcessGuard&) = delete;
        ActiveRuntimeProcessGuard& operator=(const ActiveRuntimeProcessGuard&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] HANDLE Handle() const noexcept;
        [[nodiscard]] DWORD ProcessId() const noexcept;
        [[nodiscard]] bool Running() const noexcept;
        [[nodiscard]] bool TerminateAndWait(DWORD exitCode = RuntimeCleanupExitCode) noexcept;

    private:
        details::HandleOwner _process;
        DWORD _processId{};
    };
}
