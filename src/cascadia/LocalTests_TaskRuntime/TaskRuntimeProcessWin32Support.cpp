// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeProcessTestFixtures.h"
#include "TaskRuntimeProcessWin32Support.h"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <system_error>

namespace TaskRuntimeLocalTests::ProcessFixtures::details
{
    namespace
    {
        class ProcessAttributeList final
        {
        public:
            ProcessAttributeList()
            {
                SIZE_T bytes{};
                (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
                if (bytes == 0)
                {
                    throw std::system_error{
                        static_cast<int>(GetLastError()),
                        std::system_category(),
                        "InitializeProcThreadAttributeList(size)" };
                }
                _storage.resize(bytes);
                _list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(_storage.data());
                if (!InitializeProcThreadAttributeList(_list, 1, 0, &bytes))
                {
                    throw std::system_error{
                        static_cast<int>(GetLastError()),
                        std::system_category(),
                        "InitializeProcThreadAttributeList" };
                }
            }

            ~ProcessAttributeList()
            {
                if (_list)
                {
                    DeleteProcThreadAttributeList(_list);
                }
            }

            ProcessAttributeList(const ProcessAttributeList&) = delete;
            ProcessAttributeList& operator=(const ProcessAttributeList&) = delete;

            void AllowHandles(const std::span<HANDLE> handles)
            {
                if (!UpdateProcThreadAttribute(
                        _list,
                        0,
                        PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                        handles.data(),
                        handles.size_bytes(),
                        nullptr,
                        nullptr))
                {
                    throw std::system_error{
                        static_cast<int>(GetLastError()),
                        std::system_category(),
                        "UpdateProcThreadAttribute(HANDLE_LIST)" };
                }
            }

            [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept
            {
                return _list;
            }

        private:
            std::vector<std::byte> _storage;
            LPPROC_THREAD_ATTRIBUTE_LIST _list{};
        };

        [[noreturn]] void _throwLastError(const char* const operation)
        {
            throw std::system_error{
                static_cast<int>(GetLastError()),
                std::system_category(),
                operation };
        }

        SECURITY_ATTRIBUTES _inheritableSecurityAttributes() noexcept
        {
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.bInheritHandle = TRUE;
            return attributes;
        }

        HandleOwner _openInheritableNull(
            const DWORD access,
            SECURITY_ATTRIBUTES& inheritable)
        {
            const auto raw = CreateFileW(
                L"NUL",
                access,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                &inheritable,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (raw == INVALID_HANDLE_VALUE)
            {
                _throwLastError("CreateFileW(NUL)");
            }
            return HandleOwner{ raw };
        }

        struct StderrFile
        {
            std::shared_ptr<StderrCaptureFile> Capture;
            HandleOwner Write;
        };

        StderrFile _createStderrFile(SECURITY_ATTRIBUTES& inheritable)
        {
            std::array<wchar_t, MAX_PATH> temporaryDirectory{};
            const auto directoryLength = GetTempPathW(
                static_cast<DWORD>(temporaryDirectory.size()),
                temporaryDirectory.data());
            if (directoryLength == 0 || directoryLength >= temporaryDirectory.size())
            {
                _throwLastError("GetTempPathW(stderr)");
            }

            std::array<wchar_t, MAX_PATH> temporaryFile{};
            if (!GetTempFileNameW(
                    temporaryDirectory.data(),
                    L"rte",
                    0,
                    temporaryFile.data()))
            {
                _throwLastError("GetTempFileNameW(stderr)");
            }
            auto capture = std::make_shared<StderrCaptureFile>(
                std::filesystem::path{ temporaryFile.data() });
            const auto rawWrite = CreateFileW(
                temporaryFile.data(),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                &inheritable,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_TEMPORARY,
                nullptr);
            if (rawWrite == INVALID_HANDLE_VALUE)
            {
                _throwLastError("CreateFileW(stderr)");
            }
            return { std::move(capture), HandleOwner{ rawWrite } };
        }
    }

    LaunchedTaskRuntimeProcess LaunchTaskRuntimeProcess(
        const ProductRuntimeTestIdentity& identity)
    {
        auto inheritable = _inheritableSecurityAttributes();
        HandleOwner readyEvent{ CreateEventW(&inheritable, TRUE, FALSE, nullptr) };
        if (!readyEvent)
        {
            _throwLastError("CreateEventW(ready)");
        }
        HandleOwner shutdownEvent{ CreateEventW(&inheritable, TRUE, FALSE, nullptr) };
        if (!shutdownEvent)
        {
            _throwLastError("CreateEventW(shutdown)");
        }
        auto stderrFile = _createStderrFile(inheritable);
        auto standardInput = _openInheritableNull(GENERIC_READ, inheritable);
        auto standardOutput = _openInheritableNull(GENERIC_WRITE, inheritable);

        std::array<HANDLE, 5> inheritedHandles{
            readyEvent.Get(),
            shutdownEvent.Get(),
            standardInput.Get(),
            standardOutput.Get(),
            stderrFile.Write.Get(),
        };
        ProcessAttributeList attributes;
        attributes.AllowHandles(inheritedHandles);

        const auto executable = TaskRuntimeExecutable();
        auto commandLine = BuildTaskRuntimeCommandLine(
            executable,
            identity,
            readyEvent.Get(),
            shutdownEvent.Get());

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = standardInput.Get();
        startup.StartupInfo.hStdOutput = standardOutput.Get();
        startup.StartupInfo.hStdError = stderrFile.Write.Get();
        startup.lpAttributeList = attributes.Get();

        PROCESS_INFORMATION processInformation{};
        const auto workingDirectory = executable.parent_path().native();
        if (!CreateProcessW(
                executable.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                nullptr,
                workingDirectory.c_str(),
                &startup.StartupInfo,
                &processInformation))
        {
            _throwLastError("CreateProcessW(TaskRuntime.exe)");
        }

        HandleOwner process{ processInformation.hProcess };
        HandleOwner thread{ processInformation.hThread };
        stderrFile.Write.Reset();
        return {
            std::move(readyEvent),
            std::move(shutdownEvent),
            std::move(process),
            processInformation.dwProcessId,
            std::move(stderrFile.Capture),
        };
    }
}
