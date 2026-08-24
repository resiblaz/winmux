#include <algorithm>
#include <cstddef>
#include <string>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wil/stl.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>

namespace
{
    constexpr std::wstring_view WorkspaceCommand{ L"workspace" };
    constexpr std::wstring_view HeadlessWorkspaceSentinel{ L"--headless-workspace" };
    constexpr std::wstring_view InternalWorkspaceOpenSentinel{ L"--internal-workspace-open" };
    constexpr DWORD WorkspaceCommandTimeoutMilliseconds{ 120000 };
    constexpr int WorkspaceLauncherErrorExitCode{ 8 };

    void AppendQuotedArgument(std::wstring& commandline, const std::wstring_view argument)
    {
        if (!commandline.empty())
        {
            commandline.push_back(L' ');
        }
        commandline.push_back(L'"');
        size_t backslashes{};
        for (const auto character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'"')
            {
                commandline.append(backslashes * 2 + 1, L'\\');
                commandline.push_back(character);
                backslashes = 0;
                continue;
            }
            commandline.append(backslashes, L'\\');
            backslashes = 0;
            commandline.push_back(character);
        }
        commandline.append(backslashes * 2, L'\\');
        commandline.push_back(L'"');
    }

    wil::unique_handle DuplicateStandardHandle(const DWORD standardHandle) noexcept
    {
        const auto source = GetStdHandle(standardHandle);
        if (!source || source == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        HANDLE duplicate{};
        if (!DuplicateHandle(GetCurrentProcess(),
                             source,
                             GetCurrentProcess(),
                             &duplicate,
                             0,
                             TRUE,
                             DUPLICATE_SAME_ACCESS))
        {
            return {};
        }
        return wil::unique_handle{ duplicate };
    }

    wil::unique_handle OpenNullHandle(const DWORD desiredAccess) noexcept
    {
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;
        const auto handle = CreateFileW(L"NUL",
                                        desiredAccess,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &securityAttributes,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        return handle == INVALID_HANDLE_VALUE ? wil::unique_handle{} : wil::unique_handle{ handle };
    }

    void WriteLauncherError(const HANDLE handle, const std::wstring_view message) noexcept
    try
    {
        if (!handle || handle == INVALID_HANDLE_VALUE || message.empty())
        {
            OutputDebugStringW(std::wstring{ message }.c_str());
            return;
        }

        DWORD consoleMode{};
        if (GetConsoleMode(handle, &consoleMode))
        {
            size_t offset{};
            while (offset < message.size())
            {
                DWORD written{};
                const auto remaining = static_cast<DWORD>(std::min<size_t>(message.size() - offset, std::numeric_limits<DWORD>::max()));
                if (!WriteConsoleW(handle, message.data() + offset, remaining, &written, nullptr) || written == 0)
                {
                    break;
                }
                offset += written;
            }
            return;
        }

        const auto byteCount = WideCharToMultiByte(CP_UTF8,
                                                   WC_ERR_INVALID_CHARS,
                                                   message.data(),
                                                   static_cast<int>(message.size()),
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   nullptr);
        if (byteCount <= 0)
        {
            return;
        }
        std::string utf8(static_cast<size_t>(byteCount), '\0');
        if (WideCharToMultiByte(CP_UTF8,
                               WC_ERR_INVALID_CHARS,
                               message.data(),
                               static_cast<int>(message.size()),
                               utf8.data(),
                               byteCount,
                               nullptr,
                               nullptr) <= 0)
        {
            return;
        }
        size_t offset{};
        while (offset < utf8.size())
        {
            DWORD written{};
            const auto remaining = static_cast<DWORD>(std::min<size_t>(utf8.size() - offset, std::numeric_limits<DWORD>::max()));
            if (!WriteFile(handle, utf8.data() + offset, remaining, &written, nullptr) || written == 0)
            {
                break;
            }
            offset += written;
        }
    }
    catch (...)
    {
        OutputDebugStringW(L"wt workspace: launcher diagnostics could not be written.\n");
    }

    class AttributeList final
    {
    public:
        AttributeList()
        {
            SIZE_T bytes{};
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
            {
                return;
            }
            _storage.resize(bytes);
            _list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(_storage.data());
            if (!InitializeProcThreadAttributeList(_list, 1, 0, &bytes))
            {
                _list = nullptr;
                _storage.clear();
            }
        }

        ~AttributeList()
        {
            if (_list)
            {
                DeleteProcThreadAttributeList(_list);
            }
        }

        AttributeList(const AttributeList&) = delete;
        AttributeList& operator=(const AttributeList&) = delete;

        bool SetHandles(const std::vector<HANDLE>& handles) noexcept
        {
            return _list &&
                   !handles.empty() &&
                   UpdateProcThreadAttribute(_list,
                                             0,
                                             PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                             const_cast<HANDLE*>(handles.data()),
                                             handles.size() * sizeof(HANDLE),
                                             nullptr,
                                             nullptr);
        }

        PPROC_THREAD_ATTRIBUTE_LIST get() const noexcept
        {
            return _list;
        }

    private:
        std::vector<std::byte> _storage;
        PPROC_THREAD_ATTRIBUTE_LIST _list{};
    };

    int LaunchAndWait(const std::filesystem::path& module, std::wstring& commandline)
    {
        auto input = DuplicateStandardHandle(STD_INPUT_HANDLE);
        auto output = DuplicateStandardHandle(STD_OUTPUT_HANDLE);
        auto error = DuplicateStandardHandle(STD_ERROR_HANDLE);
        if (!input)
        {
            input = OpenNullHandle(GENERIC_READ);
        }
        if (!error)
        {
            OutputDebugStringW(L"wt workspace: stderr is unavailable.\n");
            return WorkspaceLauncherErrorExitCode;
        }
        if (!output)
        {
            WriteLauncherError(error.get(), L"wt workspace: stdout is unavailable.\n");
            return WorkspaceLauncherErrorExitCode;
        }
        if (!input)
        {
            WriteLauncherError(error.get(), L"wt workspace: stdin and the NUL fallback are unavailable.\n");
            return WorkspaceLauncherErrorExitCode;
        }

        std::vector<HANDLE> inheritedHandles{ input.get(), output.get(), error.get() };
        AttributeList attributes;
        if (!attributes.SetHandles(inheritedHandles))
        {
            WriteLauncherError(error.get(), L"wt workspace: standard handles could not be isolated for the child process.\n");
            return WorkspaceLauncherErrorExitCode;
        }

        wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
        jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job ||
            !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)))
        {
            WriteLauncherError(error.get(), L"wt workspace: the child process containment job could not be created.\n");
            return WorkspaceLauncherErrorExitCode;
        }

        STARTUPINFOEXW startupInfo{};
        startupInfo.StartupInfo.cb = sizeof(startupInfo);
        GetStartupInfoW(&startupInfo.StartupInfo);
        startupInfo.StartupInfo.cb = sizeof(startupInfo);
        startupInfo.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        startupInfo.StartupInfo.hStdInput = input.get();
        startupInfo.StartupInfo.hStdOutput = output.get();
        startupInfo.StartupInfo.hStdError = error.get();
        startupInfo.lpAttributeList = attributes.get();

        wil::unique_process_information processInformation;
        if (!CreateProcessW(module.c_str(),
                            commandline.data(),
                            nullptr,
                            nullptr,
                            TRUE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                            nullptr,
                            nullptr,
                            &startupInfo.StartupInfo,
                            &processInformation))
        {
            WriteLauncherError(error.get(), L"wt workspace: WindowsTerminal.exe could not be started.\n");
            return WorkspaceLauncherErrorExitCode;
        }
        if (!AssignProcessToJobObject(job.get(), processInformation.hProcess))
        {
            TerminateProcess(processInformation.hProcess, WorkspaceLauncherErrorExitCode);
            WriteLauncherError(error.get(), L"wt workspace: WindowsTerminal.exe could not be contained safely.\n");
            return WorkspaceLauncherErrorExitCode;
        }
        if (ResumeThread(processInformation.hThread) == static_cast<DWORD>(-1))
        {
            TerminateJobObject(job.get(), WorkspaceLauncherErrorExitCode);
            WriteLauncherError(error.get(), L"wt workspace: WindowsTerminal.exe could not be resumed.\n");
            return WorkspaceLauncherErrorExitCode;
        }

        const auto waitResult = WaitForSingleObject(processInformation.hProcess, WorkspaceCommandTimeoutMilliseconds);
        if (waitResult == WAIT_TIMEOUT)
        {
            TerminateJobObject(job.get(), ERROR_TIMEOUT);
            WaitForSingleObject(processInformation.hProcess, 5000);
            WriteLauncherError(error.get(), L"wt workspace: command timed out after 120 seconds.\n");
            return WorkspaceLauncherErrorExitCode;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            TerminateJobObject(job.get(), WorkspaceLauncherErrorExitCode);
            WaitForSingleObject(processInformation.hProcess, 5000);
            WriteLauncherError(error.get(), L"wt workspace: waiting for WindowsTerminal.exe failed.\n");
            return WorkspaceLauncherErrorExitCode;
        }

        DWORD exitCode{};
        if (!GetExitCodeProcess(processInformation.hProcess, &exitCode))
        {
            WriteLauncherError(error.get(), L"wt workspace: the child exit code could not be read.\n");
            return WorkspaceLauncherErrorExitCode;
        }
        return static_cast<int>(exitCode);
    }

    bool LaunchWithoutWaiting(const std::filesystem::path& module, std::wstring& commandline) noexcept
    {
        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        GetStartupInfoW(&startupInfo);
        wil::unique_process_information processInformation;
        return CreateProcessW(module.c_str(),
                              commandline.data(),
                              nullptr,
                              nullptr,
                              FALSE,
                              0,
                              nullptr,
                              nullptr,
                              &startupInfo,
                              &processInformation) != FALSE;
    }
}

int __cdecl wmain(const int argc, wchar_t** argv)
try
{
    std::filesystem::path module{ wil::GetModuleFileNameW<std::wstring>(nullptr) };

    // Cache our name (wt, wtd), then locate WindowsTerminal.exe beside us.
    const std::wstring ourFilename{ module.filename() };
    module.replace_filename(L"WindowsTerminal.exe");

    const auto workspaceCommand = argc >= 2 && argv[1] == WorkspaceCommand;
    std::wstring commandline;
    AppendQuotedArgument(commandline, ourFilename);
    if (workspaceCommand)
    {
        AppendQuotedArgument(commandline, HeadlessWorkspaceSentinel);
    }
    for (int index = 1; index < argc; ++index)
    {
        AppendQuotedArgument(commandline, argv[index]);
    }

    if (workspaceCommand)
    {
        const auto exitCode = LaunchAndWait(module, commandline);
        if (exitCode != 0 || argc < 3 || std::wstring_view{ argv[2] } != L"open")
        {
            return exitCode;
        }

        // The validating process is job-contained. Launch the long-lived UI
        // from the shim only after validation succeeds, so it never needs to
        // break away from that containment job.
        std::wstring uiCommandline;
        AppendQuotedArgument(uiCommandline, ourFilename);
        AppendQuotedArgument(uiCommandline, InternalWorkspaceOpenSentinel);
        for (int index = 1; index < argc; ++index)
        {
            AppendQuotedArgument(uiCommandline, argv[index]);
        }
        if (!LaunchWithoutWaiting(module, uiCommandline))
        {
            WriteLauncherError(GetStdHandle(STD_ERROR_HANDLE), L"wt workspace: the validated workspace UI could not be started.\n");
            return WorkspaceLauncherErrorExitCode;
        }
        return 0;
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    GetStartupInfoW(&startupInfo);
    wil::unique_process_information processInformation;
    return !CreateProcessW(module.c_str(),
                           commandline.data(),
                           nullptr,
                           nullptr,
                           FALSE,
                           0,
                           nullptr,
                           nullptr,
                           &startupInfo,
                           &processInformation);
}
catch (...)
{
    WriteLauncherError(GetStdHandle(STD_ERROR_HANDLE), L"wt workspace: launcher initialization failed.\n");
    return WorkspaceLauncherErrorExitCode;
}
