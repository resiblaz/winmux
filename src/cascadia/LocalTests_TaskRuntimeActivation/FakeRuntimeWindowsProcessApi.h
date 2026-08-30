// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeWindowsProcessApiInternal.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace TaskRuntimeActivationLocalTests
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntime::details;

    struct FakeRuntimeProcessCreateCall
    {
        std::filesystem::path ApplicationPath;
        std::wstring CommandLine;
        std::filesystem::path CurrentDirectory;
        DWORD CreationFlags{};
        bool InheritHandles{};
        bool UseStandardHandles{};
        bool UseHandleList{};
    };

    struct FakeRuntimePathAttributes
    {
        bool Succeeded{ true };
        DWORD Attributes{};
        DWORD SystemError{};
    };

    class FakeRuntimeWindowsProcessApi final : public IRuntimeWindowsProcessApi
    {
    public:
        bool GetPathAttributes(
            const std::filesystem::path& path,
            DWORD& attributes,
            DWORD& systemError) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            PathAttributeCalls.emplace_back(path);
            const auto configured = PathAttributes.find(path);
            const auto filename = path.filename().native();
            const auto runtimeExecutable = CompareStringOrdinal(
                                               filename.c_str(),
                                               static_cast<int>(filename.size()),
                                               L"TaskRuntime.exe",
                                               -1,
                                               TRUE) == CSTR_EQUAL;
            const auto value = configured != PathAttributes.end() ?
                                   configured->second :
                                   FakeRuntimePathAttributes{
                                       true,
                                       runtimeExecutable ?
                                           DefaultExecutableAttributes :
                                           DefaultDirectoryAttributes,
                                       ERROR_SUCCESS,
                                   };
            attributes = value.Attributes;
            systemError = value.SystemError;
            return value.Succeeded;
        }

        bool CurrentProcessInJob(bool& inJob, DWORD& systemError) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            ++CurrentProcessJobCalls;
            inJob = CurrentProcessIsInJob;
            systemError = CurrentProcessJobError;
            return CurrentProcessJobSucceeded;
        }

        bool CurrentJobLimitFlags(DWORD& limitFlags, DWORD& systemError) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            ++CurrentJobLimitCalls;
            limitFlags = CurrentJobLimits;
            systemError = CurrentJobLimitError;
            return CurrentJobLimitSucceeded;
        }

        bool CreateProcess(
            const RuntimeWindowsProcessCreateRequest& request,
            RuntimeWindowsCreatedProcess& process,
            DWORD& systemError) noexcept override
        {
            std::function<void()> hook;
            {
                std::scoped_lock lock{ _mutex };
                FakeRuntimeProcessCreateCall call;
                call.ApplicationPath = request.ApplicationPath;
                const auto terminator = std::ranges::find(request.MutableCommandLine, L'\0');
                call.CommandLine.assign(request.MutableCommandLine.begin(), terminator);
                call.CurrentDirectory = request.CurrentDirectory;
                call.CreationFlags = request.CreationFlags;
                call.InheritHandles = request.InheritHandles;
                call.UseStandardHandles = request.UseStandardHandles;
                call.UseHandleList = request.UseHandleList;
                CreateCalls.emplace_back(std::move(call));
                process = CreatedProcess;
                systemError = CreateProcessError;
                hook = CreateProcessHook;
            }
            if (hook)
            {
                hook();
            }
            return CreateProcessSucceeded;
        }

        bool ProcessInAnyJob(
            const HANDLE process,
            bool& inJob,
            DWORD& systemError) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            ChildJobQueryHandles.emplace_back(process);
            inJob = ChildProcessIsInJob;
            systemError = ChildJobQueryError;
            return ChildJobQuerySucceeded;
        }

        bool ResumePrimaryThread(
            const HANDLE thread,
            DWORD& previousSuspendCount,
            DWORD& systemError) noexcept override
        {
            std::function<void()> hook;
            {
                std::scoped_lock lock{ _mutex };
                ResumeHandles.emplace_back(thread);
                previousSuspendCount = ResumePreviousSuspendCount;
                systemError = ResumeError;
                hook = ResumeHook;
            }
            if (hook)
            {
                hook();
            }
            return ResumeSucceeded;
        }

        bool TerminateProcessBeforeCommit(
            const HANDLE process,
            const DWORD exitCode,
            DWORD& systemError) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            TerminateHandles.emplace_back(process);
            TerminateExitCodes.emplace_back(exitCode);
            systemError = TerminateError;
            return TerminateSucceeded;
        }

        RuntimeWindowsWaitResult PollHandle(const HANDLE handle) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            PollHandles.emplace_back(handle);
            if (PollResults.empty())
            {
                return DefaultPollResult;
            }
            const auto result = PollResults.front();
            PollResults.pop_front();
            return result;
        }

        RuntimeWindowsWaitResult WaitForProcess(
            const HANDLE process,
            const DWORD timeoutMilliseconds) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            WaitProcessHandles.emplace_back(process);
            WaitProcessTimeouts.emplace_back(timeoutMilliseconds);
            if (!WaitProcessResults.empty())
            {
                const auto result = WaitProcessResults.front();
                WaitProcessResults.pop_front();
                return result;
            }
            return WaitProcessResult;
        }

        bool ProcessExitCode(
            const HANDLE process,
            DWORD& exitCode,
            DWORD& systemError) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            ExitCodeHandles.emplace_back(process);
            exitCode = ConfiguredExitCode;
            systemError = ExitCodeError;
            return ExitCodeSucceeded;
        }

        void CloseHandle(const HANDLE handle) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            ClosedHandles.emplace_back(handle);
        }

        [[nodiscard]] std::vector<FakeRuntimeProcessCreateCall> CreateCallSnapshot() const
        {
            std::scoped_lock lock{ _mutex };
            return CreateCalls;
        }

        [[nodiscard]] std::vector<HANDLE> ClosedHandleSnapshot() const
        {
            std::scoped_lock lock{ _mutex };
            return ClosedHandles;
        }

        DWORD DefaultDirectoryAttributes{ FILE_ATTRIBUTE_DIRECTORY };
        DWORD DefaultExecutableAttributes{ FILE_ATTRIBUTE_ARCHIVE };
        std::map<std::filesystem::path, FakeRuntimePathAttributes> PathAttributes;
        bool CurrentProcessJobSucceeded{ true };
        bool CurrentProcessIsInJob{};
        DWORD CurrentProcessJobError{};
        bool CurrentJobLimitSucceeded{ true };
        DWORD CurrentJobLimits{};
        DWORD CurrentJobLimitError{};
        bool CreateProcessSucceeded{ true };
        RuntimeWindowsCreatedProcess CreatedProcess{
            reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1010)),
            reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x2020)),
            4242,
        };
        DWORD CreateProcessError{};
        std::function<void()> CreateProcessHook;
        bool ChildJobQuerySucceeded{ true };
        bool ChildProcessIsInJob{};
        DWORD ChildJobQueryError{};
        bool ResumeSucceeded{ true };
        DWORD ResumePreviousSuspendCount{ 1 };
        DWORD ResumeError{};
        std::function<void()> ResumeHook;
        bool TerminateSucceeded{ true };
        DWORD TerminateError{};
        RuntimeWindowsWaitResult DefaultPollResult{
            RuntimeWindowsWaitStatus::NotSignaled,
            ERROR_SUCCESS,
        };
        std::deque<RuntimeWindowsWaitResult> PollResults;
        RuntimeWindowsWaitResult WaitProcessResult{
            RuntimeWindowsWaitStatus::Signaled,
            ERROR_SUCCESS,
        };
        std::deque<RuntimeWindowsWaitResult> WaitProcessResults;
        bool ExitCodeSucceeded{ true };
        DWORD ConfiguredExitCode{};
        DWORD ExitCodeError{};

        std::vector<std::filesystem::path> PathAttributeCalls;
        size_t CurrentProcessJobCalls{};
        size_t CurrentJobLimitCalls{};
        std::vector<FakeRuntimeProcessCreateCall> CreateCalls;
        std::vector<HANDLE> ChildJobQueryHandles;
        std::vector<HANDLE> ResumeHandles;
        std::vector<HANDLE> TerminateHandles;
        std::vector<DWORD> TerminateExitCodes;
        std::vector<HANDLE> PollHandles;
        std::vector<HANDLE> WaitProcessHandles;
        std::vector<DWORD> WaitProcessTimeouts;
        std::vector<HANDLE> ExitCodeHandles;
        std::vector<HANDLE> ClosedHandles;

    private:
        mutable std::mutex _mutex;
    };
}
