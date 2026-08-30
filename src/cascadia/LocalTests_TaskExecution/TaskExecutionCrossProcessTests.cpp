// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskExecutionTestSupport.h"

using namespace WEX::TestExecution;
using namespace TaskExecutionLocalTests::TestSupport;

namespace TaskExecutionLocalTests
{
    namespace
    {
        struct NamedEvent
        {
            std::wstring Name;
            UniqueHandle Handle;
        };

        class ProcessOwner final
        {
        public:
            ProcessOwner() noexcept = default;

            ~ProcessOwner()
            {
                if (Process)
                {
                    if (WaitForSingleObject(Process.Get(), 0) == WAIT_TIMEOUT)
                    {
                        TerminateProcess(Process.Get(), ERROR_PROCESS_ABORTED);
                        WaitForSingleObject(Process.Get(), 5000);
                    }
                }
            }

            ProcessOwner(const ProcessOwner&) = delete;
            ProcessOwner& operator=(const ProcessOwner&) = delete;
            ProcessOwner(ProcessOwner&&) noexcept = default;
            ProcessOwner& operator=(ProcessOwner&&) noexcept = delete;

            UniqueHandle Process;
            UniqueHandle Thread;
        };

        std::wstring _uniqueKernelName(const std::wstring_view suffix)
        {
            static std::atomic_uint64_t sequence{};
            return L"Local\\WT.TaskExecution.Test." +
                   std::to_wstring(GetCurrentProcessId()) + L"." +
                   std::to_wstring(GetTickCount64()) + L"." +
                   std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) +
                   L"." + std::wstring{ suffix };
        }

        NamedEvent _namedEvent(const std::wstring_view suffix)
        {
            NamedEvent result;
            result.Name = _uniqueKernelName(suffix);
            SetLastError(ERROR_SUCCESS);
            result.Handle = UniqueHandle{ CreateEventW(
                nullptr,
                TRUE,
                FALSE,
                result.Name.c_str()) };
            VERIFY_IS_TRUE(static_cast<bool>(result.Handle));
            VERIFY_ARE_NOT_EQUAL(DWORD{ ERROR_ALREADY_EXISTS }, GetLastError());
            return result;
        }

        std::filesystem::path _helperPath()
        {
            HMODULE module{};
            VERIFY_IS_TRUE(GetModuleHandleExW(
                               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&_helperPath),
                               &module) != FALSE);
            std::wstring value(32768, L'\0');
            const auto written = GetModuleFileNameW(
                module,
                value.data(),
                static_cast<DWORD>(value.size()));
            VERIFY_IS_GREATER_THAN(written, DWORD{ 0 });
            VERIFY_IS_LESS_THAN(written, static_cast<DWORD>(value.size()));
            value.resize(written);
            return std::filesystem::path{ value }.parent_path() /
                   L"TaskExecutionLockChild.exe";
        }

        std::wstring _quote(const std::wstring_view value)
        {
            std::wstring result{ L"\"" };
            size_t backslashes{};
            for (const auto character : value)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (character == L'\"')
                {
                    result.append(backslashes * 2 + 1, L'\\');
                    result.push_back(L'\"');
                    backslashes = 0;
                    continue;
                }
                result.append(backslashes, L'\\');
                backslashes = 0;
                result.push_back(character);
            }
            result.append(backslashes * 2, L'\\');
            result.push_back(L'\"');
            return result;
        }

        ProcessOwner _startChild(
            const std::wstring_view mode,
            const std::filesystem::path& path,
            const std::wstring_view runtimeIdentity,
            const NamedEvent& acquired,
            const NamedEvent& release)
        {
            const auto helper = _helperPath();
            VERIFY_IS_TRUE(std::filesystem::is_regular_file(helper));
            auto commandLine =
                _quote(helper.native()) + L" --mode " + _quote(mode) +
                L" --path " + _quote(path.native()) +
                L" --runtime-id " + _quote(runtimeIdentity) +
                L" --acquired-event " + _quote(acquired.Name) +
                L" --release-event " + _quote(release.Name);
            STARTUPINFOW startup{ sizeof(startup) };
            PROCESS_INFORMATION process{};
            VERIFY_IS_TRUE(CreateProcessW(
                               helper.c_str(),
                               commandLine.data(),
                               nullptr,
                               nullptr,
                               FALSE,
                               CREATE_NO_WINDOW,
                               nullptr,
                               nullptr,
                               &startup,
                               &process) != FALSE);
            ProcessOwner result;
            result.Process = UniqueHandle{ process.hProcess };
            result.Thread = UniqueHandle{ process.hThread };
            return result;
        }

        void _verifyChildExitedSuccessfully(ProcessOwner& child)
        {
            VERIFY_ARE_EQUAL(
                DWORD{ WAIT_OBJECT_0 },
                WaitForSingleObject(child.Process.Get(), 5000));
            DWORD exitCode{};
            VERIFY_IS_TRUE(GetExitCodeProcess(child.Process.Get(), &exitCode) != FALSE);
            VERIFY_ARE_EQUAL(DWORD{ 0 }, exitCode);
        }

        void _verifyCrossProcessContention(const std::wstring_view mode)
        {
            TemporaryDirectory temporary;
            auto firstAcquired = _namedEvent(L"first-acquired");
            auto firstRelease = _namedEvent(L"first-release");
            auto secondAcquired = _namedEvent(L"second-acquired");
            auto secondRelease = _namedEvent(L"second-release");

            auto first = _startChild(
                mode,
                temporary.Path(),
                L"registered-runtime-alpha",
                firstAcquired,
                firstRelease);
            VERIFY_ARE_EQUAL(
                DWORD{ WAIT_OBJECT_0 },
                WaitForSingleObject(firstAcquired.Handle.Get(), 5000));

            auto second = _startChild(
                mode,
                temporary.Path(),
                L"portable-runtime-beta",
                secondAcquired,
                secondRelease);
            VERIFY_ARE_EQUAL(
                DWORD{ WAIT_TIMEOUT },
                WaitForSingleObject(secondAcquired.Handle.Get(), 100));

            VERIFY_IS_TRUE(SetEvent(firstRelease.Handle.Get()) != FALSE);
            _verifyChildExitedSuccessfully(first);
            VERIFY_ARE_EQUAL(
                DWORD{ WAIT_OBJECT_0 },
                WaitForSingleObject(secondAcquired.Handle.Get(), 5000));
            VERIFY_IS_TRUE(SetEvent(secondRelease.Handle.Get()) != FALSE);
            _verifyChildExitedSuccessfully(second);
        }
    }

    class TaskExecutionCrossProcessTests
    {
        TEST_CLASS(TaskExecutionCrossProcessTests);

        TEST_METHOD(DirectLeaseSerializesDifferentRuntimeIdentitiesAcrossProcesses);
        TEST_METHOD(GitLockSerializesDifferentRuntimeIdentitiesAcrossProcesses);
    };

    void TaskExecutionCrossProcessTests::DirectLeaseSerializesDifferentRuntimeIdentitiesAcrossProcesses()
    {
        _verifyCrossProcessContention(L"direct");
    }

    void TaskExecutionCrossProcessTests::GitLockSerializesDifferentRuntimeIdentitiesAcrossProcesses()
    {
        _verifyCrossProcessContention(L"git");
    }
}
