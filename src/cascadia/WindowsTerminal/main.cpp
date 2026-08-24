// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WindowEmperor.h"
#include "resource.h"
#include "../types/inc/User32Utils.hpp"
#include <WilErrorReporting.h>

using namespace winrt;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::Foundation::Numerics;

// Note: Generate GUID using TlgGuid.exe tool - seriously, it won't work if you
// just generate an arbitrary GUID
TRACELOGGING_DEFINE_PROVIDER(
    g_hWindowsTerminalProvider,
    "Microsoft.Windows.Terminal.Win32Host",
    // {56c06166-2e2e-5f4d-7ff3-74f4b78c87d6}
    (0x56c06166, 0x2e2e, 0x5f4d, 0x7f, 0xf3, 0x74, 0xf4, 0xb7, 0x8c, 0x87, 0xd6),
    TraceLoggingOptionMicrosoftTelemetry());

// !! BODGY !!
// Manually use the resources from TerminalApp as our resources.
// The WindowsTerminal project doesn't actually build a Resources.resw file, but
// we still need to be able to localize strings for the notification icon menu. Anything
// you want localized for WindowsTerminal.exe should be stuck in
// ...\TerminalApp\Resources\en-US\Resources.resw
UTILS_DEFINE_LIBRARY_RESOURCE_SCOPE(L"TerminalApp/Resources");

// Routine Description:
// - Takes an image architecture and locates a string resource that maps to that architecture.
// Arguments:
// - imageArchitecture - An IMAGE_FILE_MACHINE architecture enum value
//                     - See https://docs.microsoft.com/en-us/windows/win32/sysinfo/image-file-machine-constants
// Return Value:
// - A string value representing the human-readable name of this architecture.
static std::wstring ImageArchitectureToString(USHORT imageArchitecture)
{
    // clang-format off
    const auto id = imageArchitecture == IMAGE_FILE_MACHINE_I386 ? IDS_X86_ARCHITECTURE :
                    imageArchitecture == IMAGE_FILE_MACHINE_AMD64 ? IDS_AMD64_ARCHITECTURE :
                    imageArchitecture == IMAGE_FILE_MACHINE_ARM64 ? IDS_ARM64_ARCHITECTURE :
                    imageArchitecture == IMAGE_FILE_MACHINE_ARM ? IDS_ARM_ARCHITECTURE :
                    IDS_UNKNOWN_ARCHITECTURE;
    // clang-format on

    return GetStringResource(id);
}

// Routine Description:
// - Builds an error message if the process architecture doesn't match the
//   system platform native architecture.
// - This is because the conhost.exe must match the condrv.sys on the system and the PTY
//   infrastructure that powers everything won't work if we have a mismatch.
// Arguments:
// - <none>
// Return Value:
// - The localized mismatch message, or nullopt when the architecture matches.
static std::optional<std::wstring> NativeArchitectureError()
{
    USHORT processMachine{};
    USHORT nativeMachine{};
    THROW_IF_WIN32_BOOL_FALSE(IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine));
    if (processMachine != IMAGE_FILE_MACHINE_UNKNOWN && processMachine != nativeMachine)
    {
        const auto formatPattern = GetStringResource(IDS_ERROR_ARCHITECTURE_FORMAT);

        const auto nativeArchitecture = ImageArchitectureToString(nativeMachine);
        const auto processArchitecture = ImageArchitectureToString(processMachine);

        return wil::str_printf<std::wstring>(formatPattern.data(), nativeArchitecture.data(), processArchitecture.data());
    }
    return std::nullopt;
}

namespace
{
    constexpr std::wstring_view HeadlessWorkspaceSentinel{ L"--headless-workspace" };
    constexpr std::wstring_view InternalWorkspaceOpenSentinel{ L"--internal-workspace-open" };
    constexpr int32_t WorkspaceIoErrorExitCode{ 8 };

    std::vector<winrt::hstring> CommandlineArguments()
    {
        int argumentCount{};
        const wil::unique_hlocal_ptr<LPWSTR> arguments{ CommandLineToArgvW(GetCommandLineW(), &argumentCount) };
        std::vector<winrt::hstring> result;
        if (!arguments || argumentCount <= 0)
        {
            return result;
        }
        result.reserve(static_cast<size_t>(argumentCount));
        for (int index = 0; index < argumentCount; ++index)
        {
            result.emplace_back(arguments.get()[index]);
        }
        return result;
    }

    bool WriteStandardText(const DWORD standardHandle, const winrt::hstring& text) noexcept
    try
    {
        if (text.empty())
        {
            return true;
        }
        const auto handle = GetStdHandle(standardHandle);
        if (!handle || handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD consoleMode{};
        if (GetConsoleMode(handle, &consoleMode))
        {
            size_t offset{};
            while (offset < text.size())
            {
                DWORD written{};
                const auto remaining = std::min<size_t>(text.size() - offset, std::numeric_limits<DWORD>::max());
                if (!WriteConsoleW(handle, text.c_str() + offset, gsl::narrow<DWORD>(remaining), &written, nullptr) || written == 0)
                {
                    return false;
                }
                offset += written;
            }
            return true;
        }

        const auto utf8 = til::u16u8(text);
        size_t offset{};
        while (offset < utf8.size())
        {
            DWORD written{};
            const auto remaining = std::min<size_t>(utf8.size() - offset, std::numeric_limits<DWORD>::max());
            if (!WriteFile(handle, utf8.data() + offset, gsl::narrow<DWORD>(remaining), &written, nullptr) || written == 0)
            {
                return false;
            }
            offset += written;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }

}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int nCmdShow)
try
{
    TraceLoggingRegister(g_hWindowsTerminalProvider);
    ::Microsoft::Console::ErrorReporting::EnableFallbackFailureReporting(g_hWindowsTerminalProvider);

    TraceLoggingWrite(
        g_hWindowsTerminalProvider,
        "ExeCreated",
        TraceLoggingDescription("Event emitted when the terminal process is started"),
        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
        TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

    // If Terminal is spawned by a shortcut that requests that it run in a new process group
    // while attached to a console session, that request is nonsense. That request will, however,
    // cause WT to start with Ctrl-C disabled. This wouldn't matter, because it's a Windows-subsystem
    // application. Unfortunately, that state is heritable. In short, if you start WT using cmd in
    // a weird way, ^C stops working _inside_ the terminal. Mad.
    SetConsoleCtrlHandler(NULL, FALSE);

    const auto processArguments = CommandlineArguments();
    const auto workspaceMode = processArguments.size() >= 2 &&
                               (processArguments[1] == HeadlessWorkspaceSentinel ||
                                processArguments[1] == InternalWorkspaceOpenSentinel);

    // Block the user from starting if they launched the incorrect architecture version of the project.
    // This should only be applicable to developer versions. The package installation process
    // should choose and install the correct one from the bundle. Workspace CLI
    // invocations report through stderr and never show a modal dialog.
    if (const auto architectureError = NativeArchitectureError())
    {
        if (workspaceMode)
        {
            std::wstring message{ architectureError.value() };
            message.push_back(L'\n');
            WriteStandardText(STD_ERROR_HANDLE, winrt::hstring{ message });
            TraceLoggingUnregister(g_hWindowsTerminalProvider);
            return WorkspaceIoErrorExitCode;
        }
        MessageBoxW(nullptr,
                    architectureError->c_str(),
                    GetStringResource(IDS_ERROR_DIALOG_TITLE).data(),
                    MB_OK | MB_ICONERROR);
        TraceLoggingUnregister(g_hWindowsTerminalProvider);
        return 0;
    }

    // !!! LOAD BEARING !!!
    // We must initialize the main thread as a single-threaded apartment before
    // constructing any Xaml objects. Failing to do so will cause some issues
    // in accessibility somewhere down the line when a UIAutomation object will
    // be queried on the wrong thread at the wrong time.
    // We used to initialize as STA only _after_ initializing the application
    // host, which loaded the settings. The settings needed to be loaded in MTA
    // because we were using the Windows.Storage APIs. Since we're no longer
    // doing that, we can safely init as STA before any WinRT dispatches.
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    if (workspaceMode)
    {
        try
        {
            if (processArguments[1] == InternalWorkspaceOpenSentinel &&
                (processArguments.size() < 4 || processArguments[2] != L"workspace" || processArguments[3] != L"open"))
            {
                WriteStandardText(STD_ERROR_HANDLE, L"The internal workspace route accepts only workspace open.\n");
                TraceLoggingUnregister(g_hWindowsTerminalProvider);
                return WorkspaceIoErrorExitCode;
            }

            std::vector<winrt::hstring> workspaceArguments{ processArguments.begin() + 2, processArguments.end() };
            const auto argumentView = winrt::single_threaded_vector<winrt::hstring>(std::move(workspaceArguments)).GetView();
            const auto result = winrt::Microsoft::Terminal::Settings::Model::WorkspaceCommandline{}.Execute(argumentView);
            const auto standardOutput = result.StandardOutput();
            const auto standardError = result.StandardError();
            if (!standardOutput.empty() && !WriteStandardText(STD_OUTPUT_HANDLE, standardOutput))
            {
                WriteStandardText(STD_ERROR_HANDLE, L"Workspace command output could not be written.\n");
                TraceLoggingUnregister(g_hWindowsTerminalProvider);
                return WorkspaceIoErrorExitCode;
            }
            if (!standardError.empty() && !WriteStandardText(STD_ERROR_HANDLE, standardError))
            {
                OutputDebugStringW(L"Workspace command diagnostics could not be written.\n");
            }
            if (result.ExitCode() != 0)
            {
                TraceLoggingUnregister(g_hWindowsTerminalProvider);
                return result.ExitCode();
            }

            if (processArguments[1] == HeadlessWorkspaceSentinel)
            {
                // wt.exe owns the transition to the internal GUI route after
                // this validating process exits successfully. This process is
                // job-contained and must not create a long-lived descendant.
                TraceLoggingUnregister(g_hWindowsTerminalProvider);
                return 0;
            }

            if (!result.OpenRequested())
            {
                WriteStandardText(STD_ERROR_HANDLE, L"The internal workspace route accepts only workspace open.\n");
                TraceLoggingUnregister(g_hWindowsTerminalProvider);
                return WorkspaceIoErrorExitCode;
            }

            // The UI process starts only after the headless process validated
            // the snapshot and returned a real exit code to wt.exe.
            EnableMouseInPointer(true);
            const auto emperor = std::make_shared<::WindowEmperor>();
            emperor->HandleWorkspaceOpen(result.SnapshotId(), result.WindowTarget(), nCmdShow);
            return 0;
        }
        catch (...)
        {
            WriteStandardText(STD_ERROR_HANDLE, L"Workspace command initialization failed.\n");
            TraceLoggingUnregister(g_hWindowsTerminalProvider);
            return WorkspaceIoErrorExitCode;
        }
    }

    // Make sure to call this so we get WM_POINTER messages.
    EnableMouseInPointer(true);

    const auto emperor = std::make_shared<::WindowEmperor>();
    emperor->HandleCommandlineArgs(nCmdShow);
    return 0;
}
catch (...)
{
    WriteStandardText(STD_ERROR_HANDLE, L"Windows Terminal initialization failed.\n");
    TraceLoggingUnregister(g_hWindowsTerminalProvider);
    return WorkspaceIoErrorExitCode;
}
