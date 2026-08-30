// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRuntimeProcessOptions.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TaskRuntimeLocalTests::ProcessFixtures
{
    class ProductRuntimeTestIdentity;
}

namespace TaskRuntimeLocalTests::ProcessFixtures::details
{
    class HandleOwner final
    {
    public:
        HandleOwner() noexcept = default;
        explicit HandleOwner(const HANDLE value) noexcept :
            _value{ value }
        {
        }

        ~HandleOwner()
        {
            Reset();
        }

        HandleOwner(const HandleOwner&) = delete;
        HandleOwner& operator=(const HandleOwner&) = delete;

        HandleOwner(HandleOwner&& other) noexcept :
            _value{ other.Release() }
        {
        }

        HandleOwner& operator=(HandleOwner&& other) noexcept
        {
            if (this != &other)
            {
                Reset(other.Release());
            }
            return *this;
        }

        [[nodiscard]] HANDLE Get() const noexcept
        {
            return _value;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return _value && _value != INVALID_HANDLE_VALUE;
        }

        [[nodiscard]] HANDLE Release() noexcept
        {
            return std::exchange(_value, nullptr);
        }

        void Reset(const HANDLE value = nullptr) noexcept
        {
            if (*this)
            {
                CloseHandle(_value);
            }
            _value = value;
        }

    private:
        HANDLE _value{};
    };

    class StderrCaptureFile final
    {
    public:
        explicit StderrCaptureFile(std::filesystem::path path);
        ~StderrCaptureFile();

        StderrCaptureFile(const StderrCaptureFile&) = delete;
        StderrCaptureFile& operator=(const StderrCaptureFile&) = delete;

        [[nodiscard]] std::string Snapshot() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    struct LaunchedTaskRuntimeProcess
    {
        HandleOwner ReadyEvent;
        HandleOwner ShutdownEvent;
        HandleOwner Process;
        DWORD ProcessId{};
        std::shared_ptr<StderrCaptureFile> StderrCapture;
    };

    [[nodiscard]] LaunchedTaskRuntimeProcess LaunchTaskRuntimeProcess(
        const ProductRuntimeTestIdentity& identity,
        TaskRuntimeProcessOptions options);

    [[nodiscard]] std::filesystem::path TaskRuntimeExecutable();
    [[nodiscard]] std::vector<wchar_t> BuildTaskRuntimeCommandLine(
        const std::filesystem::path& executable,
        const ProductRuntimeTestIdentity& identity,
        HANDLE readyEvent,
        HANDLE shutdownEvent,
        const TaskRuntimeProcessOptions& options);

    void LogProcessDiagnostic(
        std::wstring_view prefix,
        DWORD processId,
        std::string_view standardError);
    void LogProcessErrorNoThrow(
        std::wstring_view prefix,
        DWORD processId,
        std::string_view standardError) noexcept;
}
