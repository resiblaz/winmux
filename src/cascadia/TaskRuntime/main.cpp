// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeBootstrap.h"
#include "RuntimeCommandLine.h"
#include "RuntimeConsoleStop.h"
#include "RuntimeProductHost.h"

#include <cstdio>

using namespace Microsoft::Terminal::TaskRuntime;

namespace
{
    constexpr std::wstring_view Usage{
        L"Usage:\n"
        L"  TaskRuntime --bootstrap-check --channel <channel> --settings-root <absolute-path>\n"
        L"  TaskRuntime --serve --channel <channel> --settings-root <absolute-path>\n"
    };

    int _exit(const RuntimeProcessExitCode code) noexcept
    {
        return static_cast<int>(code);
    }

    int _bootstrapCheck(const RuntimeCommandLine& commandLine)
    {
        const auto bootstrap = BootstrapCurrentUserRuntime(
            commandLine.Channel,
            commandLine.SettingsRoot);
        if (!bootstrap.Ready())
        {
            std::fprintf(stderr, "Task Runtime bootstrap failed: %s\n", bootstrap.Error.c_str());
            return _exit(bootstrap.Status == RuntimeBootstrapStatus::AlreadyRunning ?
                             RuntimeProcessExitCode::AlreadyRunning :
                             RuntimeProcessExitCode::BootstrapFailure);
        }
        std::printf("%s\n", bootstrap.Identity.ProtocolIdentity.c_str());
        return _exit(RuntimeProcessExitCode::Success);
    }

    int _serve(const RuntimeCommandLine& commandLine)
    {
        RuntimeConsoleStop consoleStop;
        if (!consoleStop)
        {
            std::fprintf(stderr, "Task Runtime could not install its console stop signal\n");
            return _exit(RuntimeProcessExitCode::StopInfrastructureFailure);
        }

        auto started = StartRuntimeProductHost(commandLine.Channel, commandLine.SettingsRoot);
        if (!started.Started())
        {
            std::fprintf(stderr, "Task Runtime host failed to start: %s\n", started.Error.c_str());
            switch (started.Status)
            {
            case RuntimeProductHostStartStatus::AlreadyRunning:
                return _exit(RuntimeProcessExitCode::AlreadyRunning);
            case RuntimeProductHostStartStatus::RuntimeInstanceIdFailure:
                return _exit(RuntimeProcessExitCode::RuntimeInstanceIdFailure);
            case RuntimeProductHostStartStatus::ServerStartFailure:
            case RuntimeProductHostStartStatus::ResourceFailure:
                return _exit(RuntimeProcessExitCode::ServerStartFailure);
            default:
                return _exit(RuntimeProcessExitCode::BootstrapFailure);
            }
        }

        auto host = std::move(started.Host);
        const auto identity = host.Identity();
        if (!identity || !host.StoppedHandle())
        {
            host.Stop();
            std::fprintf(stderr, "Task Runtime host returned invalid lifecycle state\n");
            return _exit(RuntimeProcessExitCode::ServerStartFailure);
        }

        std::printf(
            "READY %s %.*s\n",
            identity->ProtocolIdentity.c_str(),
            static_cast<int>(host.RuntimeInstanceId().size()),
            host.RuntimeInstanceId().data());
        std::fflush(stdout);

        HANDLE testReady{};
        HANDLE testShutdown{};
        if (commandLine.TestReadyHandle)
        {
            testReady = reinterpret_cast<HANDLE>(*commandLine.TestReadyHandle);
            testShutdown = reinterpret_cast<HANDLE>(*commandLine.TestShutdownHandle);
            DWORD flags{};
            if (!GetHandleInformation(testReady, &flags) ||
                !GetHandleInformation(testShutdown, &flags) ||
                !SetEvent(testReady))
            {
                host.Stop();
                std::fprintf(stderr, "Task Runtime test lifecycle handles are invalid\n");
                return _exit(RuntimeProcessExitCode::StopInfrastructureFailure);
            }
        }

        std::array<HANDLE, 3> handles{
            consoleStop.Handle(),
            testShutdown,
            host.StoppedHandle(),
        };
        const DWORD handleCount = testShutdown ? 3 : 2;
        if (!testShutdown)
        {
            handles[1] = handles[2];
        }
        const auto waited = WaitForMultipleObjects(handleCount, handles.data(), FALSE, INFINITE);
        if (waited == WAIT_OBJECT_0 || (testShutdown && waited == WAIT_OBJECT_0 + 1))
        {
            host.Stop();
            return _exit(RuntimeProcessExitCode::Success);
        }
        if (waited == WAIT_OBJECT_0 + handleCount - 1)
        {
            const auto terminalStatus = host.TerminalStatus();
            const auto terminalError = host.TerminalError();
            (void)host.Wait(0);
            if (terminalStatus == RuntimeProductHostTerminalStatus::Stopped)
            {
                return _exit(RuntimeProcessExitCode::Success);
            }
            std::fprintf(
                stderr,
                "Task Runtime host stopped unexpectedly: %s\n",
                terminalError.empty() ? "control server failure" : terminalError.c_str());
            return _exit(RuntimeProcessExitCode::UnexpectedServerStop);
        }

        const auto systemError = waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_FUNCTION;
        host.Stop();
        std::fprintf(stderr, "Task Runtime lifecycle wait failed: %lu\n", systemError);
        return _exit(RuntimeProcessExitCode::StopInfrastructureFailure);
    }
}

int __cdecl wmain(const int argc, wchar_t** argv)
{
    const auto commandLine = ParseRuntimeCommandLine(argc, argv);
    if (!commandLine.Succeeded)
    {
        if (!commandLine.Error.empty())
        {
            std::fwprintf(stderr, L"Task Runtime command line is invalid: %ls\n", commandLine.Error.c_str());
        }
        std::fwprintf(stderr, L"%.*ls", static_cast<int>(Usage.size()), Usage.data());
        return _exit(RuntimeProcessExitCode::InvalidCommandLine);
    }
    if (commandLine.Mode == RuntimeCommandMode::Help)
    {
        std::wprintf(L"%.*ls", static_cast<int>(Usage.size()), Usage.data());
        return _exit(RuntimeProcessExitCode::Success);
    }
    return commandLine.Mode == RuntimeCommandMode::BootstrapCheck ?
               _bootstrapCheck(commandLine) :
               _serve(commandLine);
}
