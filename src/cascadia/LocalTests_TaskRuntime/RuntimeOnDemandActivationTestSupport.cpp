// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeOnDemandActivationTestSupport.h"

#include "RuntimeWindowsCommandLine.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <system_error>

using namespace WEX::TestExecution;

namespace TaskRuntimeLocalTests::OnDemandActivation
{
    namespace
    {
        std::optional<ActivationLaunchRecord> _parseRecord(
            const std::filesystem::path& path)
        {
            std::ifstream input{ path, std::ios::binary };
            if (!input)
            {
                return std::nullopt;
            }
            std::map<std::string, long long> values;
            std::string line;
            while (std::getline(input, line))
            {
                const auto separator = line.find('=');
                if (separator == std::string::npos)
                {
                    return std::nullopt;
                }
                try
                {
                    values[line.substr(0, separator)] =
                        std::stoll(line.substr(separator + 1));
                }
                catch (...)
                {
                    return std::nullopt;
                }
            }
            constexpr std::array keys{
                "calls",
                "status",
                "phase",
                "strategy",
                "systemError",
            };
            if (values.size() != keys.size() ||
                std::ranges::any_of(keys, [&](const auto key) {
                    return !values.contains(key);
                }))
            {
                return std::nullopt;
            }
            ActivationLaunchRecord record;
            record.Calls = static_cast<size_t>(values.at("calls"));
            record.Status = static_cast<int>(values.at("status"));
            record.Phase = static_cast<int>(values.at("phase"));
            record.Strategy = static_cast<int>(values.at("strategy"));
            record.SystemError = static_cast<DWORD>(values.at("systemError"));
            return record;
        }

        std::vector<std::wstring> _parentArguments(
            const std::filesystem::path& parent,
            const ProductRuntimeTestIdentity& identity,
            const ActivationParentOptions& options)
        {
            const auto runtime = details::TaskRuntimeExecutable();
            std::vector<std::wstring> arguments{
                parent.native(),
                L"--channel",
                identity.Channel(),
                L"--settings-root",
                identity.SettingsRoot().native(),
                L"--runtime-executable",
                runtime.native(),
                L"--trusted-root",
                runtime.parent_path().native(),
                L"--client-id",
                std::wstring{
                    options.ClientInstanceId.begin(),
                    options.ClientInstanceId.end(),
                },
            };
            if (options.StartEventName)
            {
                arguments.emplace_back(L"--start-event");
                arguments.emplace_back(*options.StartEventName);
            }
            if (options.LaunchRecord)
            {
                arguments.emplace_back(L"--launch-record");
                arguments.emplace_back(options.LaunchRecord->native());
            }
            return arguments;
        }
    }

    TemporaryActivationRecord::TemporaryActivationRecord(
        const ProductRuntimeTestIdentity& identity,
        const size_t index) :
        _path{ identity.SettingsRoot() }
    {
        _path += L".activation-" + std::to_wstring(index) + L".record";
        std::error_code ignored;
        std::filesystem::remove(_path, ignored);
    }

    TemporaryActivationRecord::~TemporaryActivationRecord()
    {
        std::error_code ignored;
        std::filesystem::remove(_path, ignored);
    }

    const std::filesystem::path& TemporaryActivationRecord::Path() const noexcept
    {
        return _path;
    }

    std::optional<ActivationLaunchRecord> TemporaryActivationRecord::Read() const
    {
        return _parseRecord(_path);
    }

    ActivationParentProcess::~ActivationParentProcess()
    {
        if (_process && WaitForSingleObject(_process.Get(), 0) == WAIT_TIMEOUT)
        {
            if (TerminateProcess(_process.Get(), RuntimeCleanupExitCode))
            {
                (void)WaitForSingleObject(
                    _process.Get(),
                    ActivationParentExitTimeoutMilliseconds);
            }
        }
    }

    ActivationParentProcess::operator bool() const noexcept
    {
        return static_cast<bool>(_process) && _processId != 0;
    }

    HANDLE ActivationParentProcess::Handle() const noexcept
    {
        return _process.Get();
    }

    DWORD ActivationParentProcess::ProcessId() const noexcept
    {
        return _processId;
    }

    bool ActivationParentProcess::WaitForExit(
        const DWORD timeoutMilliseconds,
        DWORD& exitCode) noexcept
    {
        return _process &&
               WaitForSingleObject(_process.Get(), timeoutMilliseconds) == WAIT_OBJECT_0 &&
               GetExitCodeProcess(_process.Get(), &exitCode);
    }

    bool ActivationParentLaunchResult::Succeeded() const noexcept
    {
        return static_cast<bool>(Process);
    }

    ActivationTestJob::ActivationTestJob(const DWORD limitFlags) noexcept
    {
        _job.Reset(CreateJobObjectW(nullptr, nullptr));
        if (!_job)
        {
            _systemError = GetLastError();
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = limitFlags;
        if (!SetInformationJobObject(
                _job.Get(),
                JobObjectExtendedLimitInformation,
                &limits,
                sizeof(limits)))
        {
            _systemError = GetLastError();
            _job.Reset();
        }
    }

    ActivationTestJob::operator bool() const noexcept
    {
        return static_cast<bool>(_job);
    }

    HANDLE ActivationTestJob::Handle() const noexcept
    {
        return _job.Get();
    }

    DWORD ActivationTestJob::SystemError() const noexcept
    {
        return _systemError;
    }

    ActivationParentLaunchResult LaunchActivationParent(
        const ProductRuntimeTestIdentity& identity,
        ActivationParentOptions options)
    {
        ActivationParentLaunchResult result;
        try
        {
            const auto executable = ActivationParentExecutable();
            const auto arguments = _parentArguments(executable, identity, options);
            auto commandLine = BuildRuntimeWindowsCommandLine(arguments);
            if (!commandLine.Succeeded())
            {
                result.SystemError = ERROR_INVALID_PARAMETER;
                return result;
            }

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            auto flags = CREATE_NO_WINDOW | options.AdditionalCreationFlags;
            if (options.Job)
            {
                flags |= CREATE_SUSPENDED;
            }
            const auto workingDirectory = executable.parent_path().native();
            if (!CreateProcessW(
                    executable.c_str(),
                    commandLine.MutableCommandLine.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    flags,
                    nullptr,
                    workingDirectory.c_str(),
                    &startup,
                    &process))
            {
                result.SystemError = GetLastError();
                return result;
            }

            details::HandleOwner processHandle{ process.hProcess };
            details::HandleOwner threadHandle{ process.hThread };
            if (options.Job && !AssignProcessToJobObject(options.Job, processHandle.Get()))
            {
                result.SystemError = GetLastError();
                (void)TerminateProcess(processHandle.Get(), RuntimeCleanupExitCode);
                (void)WaitForSingleObject(
                    processHandle.Get(),
                    ActivationParentExitTimeoutMilliseconds);
                return result;
            }
            if (options.Job && ResumeThread(threadHandle.Get()) == static_cast<DWORD>(-1))
            {
                result.SystemError = GetLastError();
                (void)TerminateProcess(processHandle.Get(), RuntimeCleanupExitCode);
                (void)WaitForSingleObject(
                    processHandle.Get(),
                    ActivationParentExitTimeoutMilliseconds);
                return result;
            }

            result.Process._process = std::move(processHandle);
            result.Process._processId = process.dwProcessId;
            return result;
        }
        catch (...)
        {
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            return result;
        }
    }

    std::filesystem::path ActivationParentExecutable()
    {
        const auto executable =
            details::TaskRuntimeExecutable().parent_path() /
            L"TaskRuntimeActivationParent.exe";
        if (!std::filesystem::is_regular_file(executable))
        {
            throw std::runtime_error{
                "TaskRuntimeActivationParent.exe is not beside TaskRuntime.LocalTests.dll"
            };
        }
        return executable;
    }

    std::string ActivationClientInstanceId(const size_t index)
    {
        std::array<char, 37> value{};
        sprintf_s(
            value.data(),
            value.size(),
            "91000000-0000-4000-8000-%012llu",
            static_cast<unsigned long long>(index + 1));
        return value.data();
    }

    std::wstring ActivationStartEventName(
        const ProductRuntimeTestIdentity& identity)
    {
        return L"Local\\WT.TaskRuntime.ActivationTest." + identity.Channel();
    }

    ActiveRuntimeProbe RequireActiveRuntime(
        const ProductRuntimeTestIdentity& identity,
        const std::string_view clientInstanceId)
    {
        ActiveRuntimeProbe result;
        result.Client = RequireProductControlClient(identity, clientInstanceId);
        if (result.Client.Channel)
        {
            VERIFY_IS_TRUE(GetNamedPipeServerProcessId(
                result.Client.Channel.Handle(),
                &result.ProcessId));
        }
        VERIFY_ARE_NOT_EQUAL(DWORD{ 0 }, result.ProcessId);
        return result;
    }

    ActiveRuntimeProcessGuard::ActiveRuntimeProcessGuard(
        const DWORD processId) noexcept :
        _processId{ processId }
    {
        _process.Reset(OpenProcess(
            SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            processId));
        if (!_process || GetProcessId(_process.Get()) != processId)
        {
            _process.Reset();
            _processId = 0;
        }
    }

    ActiveRuntimeProcessGuard::~ActiveRuntimeProcessGuard()
    {
        (void)TerminateAndWait();
    }

    ActiveRuntimeProcessGuard::operator bool() const noexcept
    {
        return static_cast<bool>(_process) && _processId != 0;
    }

    HANDLE ActiveRuntimeProcessGuard::Handle() const noexcept
    {
        return _process.Get();
    }

    DWORD ActiveRuntimeProcessGuard::ProcessId() const noexcept
    {
        return _processId;
    }

    bool ActiveRuntimeProcessGuard::Running() const noexcept
    {
        return _process && WaitForSingleObject(_process.Get(), 0) == WAIT_TIMEOUT;
    }

    bool ActiveRuntimeProcessGuard::TerminateAndWait(const DWORD exitCode) noexcept
    {
        if (!_process)
        {
            return true;
        }
        if (WaitForSingleObject(_process.Get(), 0) == WAIT_OBJECT_0)
        {
            return true;
        }
        return TerminateProcess(_process.Get(), exitCode) &&
               WaitForSingleObject(
                   _process.Get(),
                   ProductProcessExitTimeoutMilliseconds) == WAIT_OBJECT_0;
    }
}
