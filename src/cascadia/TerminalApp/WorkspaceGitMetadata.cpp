// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceGitMetadata.h"

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr DWORD ProcessCleanupMilliseconds{ 1000 };

        bool _equalsIgnoreCase(const std::wstring_view lhs, const std::wstring_view rhs) noexcept
        {
            if (lhs.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                rhs.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }
            return CompareStringOrdinal(lhs.data(),
                                        static_cast<int>(lhs.size()),
                                        rhs.data(),
                                        static_cast<int>(rhs.size()),
                                        TRUE) == CSTR_EQUAL;
        }

        bool _startsWithIgnoreCase(const std::wstring_view value, const std::wstring_view prefix) noexcept
        {
            return value.size() >= prefix.size() && _equalsIgnoreCase(value.substr(0, prefix.size()), prefix);
        }

        bool _hasControlCharacter(const std::wstring_view value) noexcept
        {
            return std::ranges::any_of(value, [](const wchar_t character) {
                return character < L' ' || character == 0x7f;
            });
        }

        bool _validPathSyntax(const std::filesystem::path& path) noexcept
        {
            try
            {
                const auto& value = path.native();
                return !value.empty() &&
                       value.size() <= WorkspaceGitMetadata::MaximumPathCharacters &&
                       value.find(L'\0') == std::wstring::npos &&
                       !_hasControlCharacter(value) &&
                       path.is_absolute();
            }
            catch (...)
            {
                return false;
            }
        }

        std::optional<std::filesystem::path> _canonicalDirectory(const std::filesystem::path& path) noexcept
        {
            try
            {
                if (!_validPathSyntax(path))
                {
                    return std::nullopt;
                }

                std::error_code error;
                auto canonical = std::filesystem::canonical(path, error);
                if (error || !std::filesystem::is_directory(canonical, error) || error || !_validPathSyntax(canonical))
                {
                    return std::nullopt;
                }
                return canonical.lexically_normal();
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        bool _samePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs) noexcept
        {
            try
            {
                return _equalsIgnoreCase(lhs.lexically_normal().native(), rhs.lexically_normal().native());
            }
            catch (...)
            {
                return false;
            }
        }

        bool _validHead(const std::wstring_view value) noexcept
        {
            if (value.size() != 40 && value.size() != 64)
            {
                return false;
            }
            return std::ranges::all_of(value, [](const wchar_t character) {
                return (character >= L'0' && character <= L'9') ||
                       (character >= L'a' && character <= L'f') ||
                       (character >= L'A' && character <= L'F');
            });
        }

        bool _validBranchRef(const std::wstring_view value) noexcept
        {
            return value.empty() ||
                   (value.size() <= WorkspaceGitMetadata::MaximumRefCharacters &&
                    value.starts_with(L"refs/") &&
                    !_hasControlCharacter(value));
        }

        std::optional<std::wstring> _strictUtf8(const std::string_view bytes) noexcept
        {
            try
            {
                auto value = bytes;
                if (value.size() >= 3 &&
                    static_cast<uint8_t>(value[0]) == 0xef &&
                    static_cast<uint8_t>(value[1]) == 0xbb &&
                    static_cast<uint8_t>(value[2]) == 0xbf)
                {
                    value.remove_prefix(3);
                }
                if (value.empty())
                {
                    return std::wstring{};
                }
                if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                {
                    return std::nullopt;
                }

                const auto sourceLength = static_cast<int>(value.size());
                const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength, nullptr, 0);
                if (required <= 0)
                {
                    return std::nullopt;
                }
                std::wstring result(static_cast<size_t>(required), L'\0');
                if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength, result.data(), required) != required)
                {
                    return std::nullopt;
                }
                return result;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::optional<winrt::hstring> _singleLine(const WorkspaceGitCommandResult& result,
                                                  const bool allowEmpty = false) noexcept
        {
            try
            {
                if (result.Stdout.size() > WorkspaceGitMetadata::MaximumStdoutBytes ||
                    result.Stderr.size() > WorkspaceGitMetadata::MaximumStderrBytes)
                {
                    return std::nullopt;
                }

                const auto converted = _strictUtf8(result.Stdout);
                if (!converted)
                {
                    return std::nullopt;
                }
                auto value = converted.value();
                if (!value.empty() && value.back() == L'\n')
                {
                    value.pop_back();
                    if (!value.empty() && value.back() == L'\r')
                    {
                        value.pop_back();
                    }
                }
                if ((!allowEmpty && value.empty()) ||
                    value.size() > WorkspaceGitMetadata::MaximumPathCharacters ||
                    _hasControlCharacter(value))
                {
                    return std::nullopt;
                }
                return winrt::hstring{ value };
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::optional<std::filesystem::path> _directoryFromResult(const WorkspaceGitCommandResult& result) noexcept
        {
            try
            {
                const auto line = _singleLine(result);
                if (!line)
                {
                    return std::nullopt;
                }
                return _canonicalDirectory(std::filesystem::path{ std::wstring_view{ line.value() } });
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::optional<std::filesystem::path> _resolveGitExecutable() noexcept
        {
            try
            {
                const auto pathLength = GetEnvironmentVariableW(L"PATH", nullptr, 0);
                if (pathLength == 0 || pathLength > WorkspaceGitMetadata::MaximumPathCharacters)
                {
                    return std::nullopt;
                }

                std::wstring searchPath(pathLength, L'\0');
                const auto written = GetEnvironmentVariableW(L"PATH", searchPath.data(), pathLength);
                if (written == 0 || written >= pathLength)
                {
                    return std::nullopt;
                }
                searchPath.resize(written);

                size_t offset{};
                while (offset <= searchPath.size())
                {
                    const auto separator = searchPath.find(L';', offset);
                    auto entry = searchPath.substr(offset, separator == std::wstring::npos ? std::wstring::npos : separator - offset);
                    if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"')
                    {
                        entry = entry.substr(1, entry.size() - 2);
                    }

                    const std::filesystem::path directory{ entry };
                    if (!entry.empty() && directory.is_absolute())
                    {
                        const auto candidate = (directory / L"git.exe").lexically_normal();
                        const auto attributes = GetFileAttributesW(candidate.c_str());
                        if (attributes != INVALID_FILE_ATTRIBUTES &&
                            WI_IsFlagClear(attributes, FILE_ATTRIBUTE_DIRECTORY) &&
                            _validPathSyntax(candidate))
                        {
                            return candidate;
                        }
                    }

                    if (separator == std::wstring::npos)
                    {
                        break;
                    }
                    offset = separator + 1;
                }
            }
            catch (...)
            {
            }
            return std::nullopt;
        }

        std::vector<wchar_t> _gitEnvironment()
        {
            std::vector<std::wstring> entries;
            const auto environment = GetEnvironmentStringsW();
            if (!environment)
            {
                return {};
            }
            const auto freeEnvironment = wil::scope_exit([&]() noexcept { FreeEnvironmentStringsW(environment); });

            for (auto current = environment; *current != L'\0';)
            {
                const std::wstring_view entry{ current };
                const auto equals = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
                const auto name = entry.substr(0, equals);
                if (!_startsWithIgnoreCase(name, L"GIT_") &&
                    !_equalsIgnoreCase(name, L"GCM_INTERACTIVE"))
                {
                    entries.emplace_back(entry);
                }
                current += entry.size() + 1;
            }
            entries.emplace_back(L"GCM_INTERACTIVE=Never");
            entries.emplace_back(L"GIT_OPTIONAL_LOCKS=0");
            entries.emplace_back(L"GIT_TERMINAL_PROMPT=0");

            std::ranges::sort(entries, [](const auto& lhs, const auto& rhs) {
                return CompareStringOrdinal(lhs.c_str(), -1, rhs.c_str(), -1, TRUE) == CSTR_LESS_THAN;
            });

            size_t characters{ 1 };
            for (const auto& entry : entries)
            {
                characters += entry.size() + 1;
            }
            std::vector<wchar_t> block;
            block.reserve(characters);
            for (const auto& entry : entries)
            {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            return block;
        }

        std::wstring _commandLine(const WorkspaceGitCommand command, const std::wstring_view objectId)
        {
            if ((command == WorkspaceGitCommand::IsAncestor && !_validHead(objectId)) ||
                (command != WorkspaceGitCommand::IsAncestor && !objectId.empty()))
            {
                return {};
            }
            constexpr std::wstring_view prefix{ L"git.exe -c core.quotepath=false " };
            std::wstring result{ prefix };
            switch (command)
            {
            case WorkspaceGitCommand::ShowTopLevel:
                result.append(L"rev-parse --path-format=absolute --show-toplevel");
                break;
            case WorkspaceGitCommand::ShowCommonGitDirectory:
                result.append(L"rev-parse --path-format=absolute --git-common-dir");
                break;
            case WorkspaceGitCommand::ShowGitDirectory:
                result.append(L"rev-parse --path-format=absolute --git-dir");
                break;
            case WorkspaceGitCommand::VerifyHead:
                result.append(L"rev-parse --verify HEAD");
                break;
            case WorkspaceGitCommand::SymbolicHead:
                result.append(L"symbolic-ref --quiet HEAD");
                break;
            case WorkspaceGitCommand::IsAncestor:
                result.append(L"merge-base --is-ancestor ");
                result.append(objectId);
                result.append(L" HEAD");
                break;
            }
            return result;
        }

        WorkspaceGitCommandResult _runGitCommand(const WorkspaceGitCommand command,
                                                  const std::filesystem::path& workingDirectory,
                                                  const std::chrono::milliseconds timeout,
                                                  const std::wstring_view objectId) noexcept
        {
            WorkspaceGitCommandResult result;
            try
            {
                if (timeout <= std::chrono::milliseconds::zero() || timeout > WorkspaceGitMetadata::MaximumTimeout)
                {
                    return result;
                }
                const auto deadline = std::chrono::steady_clock::now() + timeout;
                const auto canonicalWorkingDirectory = _canonicalDirectory(workingDirectory);
                const auto gitExecutable = _resolveGitExecutable();
                auto environment = _gitEnvironment();
                if (!canonicalWorkingDirectory || !gitExecutable || environment.empty())
                {
                    return result;
                }

                SECURITY_ATTRIBUTES securityAttributes{
                    .nLength = sizeof(SECURITY_ATTRIBUTES),
                    .lpSecurityDescriptor = nullptr,
                    .bInheritHandle = TRUE,
                };
                wil::unique_handle stdoutRead;
                wil::unique_handle stdoutWrite;
                wil::unique_handle stderrRead;
                wil::unique_handle stderrWrite;
                wil::unique_handle nullInput{ CreateFileW(L"NUL",
                                                           GENERIC_READ,
                                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                           &securityAttributes,
                                                           OPEN_EXISTING,
                                                           FILE_ATTRIBUTE_NORMAL,
                                                           nullptr) };
                if (!nullInput ||
                    !CreatePipe(stdoutRead.addressof(), stdoutWrite.addressof(), &securityAttributes, 0) ||
                    !CreatePipe(stderrRead.addressof(), stderrWrite.addressof(), &securityAttributes, 0) ||
                    !SetHandleInformation(stdoutRead.get(), HANDLE_FLAG_INHERIT, 0) ||
                    !SetHandleInformation(stderrRead.get(), HANDLE_FLAG_INHERIT, 0))
                {
                    return result;
                }

                STARTUPINFOEXW startupInfo{};
                startupInfo.StartupInfo.cb = sizeof(startupInfo);
                startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                startupInfo.StartupInfo.hStdInput = nullInput.get();
                startupInfo.StartupInfo.hStdOutput = stdoutWrite.get();
                startupInfo.StartupInfo.hStdError = stderrWrite.get();

                SIZE_T attributeListSize{};
                InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);
                if (attributeListSize == 0)
                {
                    return result;
                }
                std::vector<std::byte> attributeListStorage(attributeListSize);
                startupInfo.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());
                if (!InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &attributeListSize))
                {
                    return result;
                }
                const auto deleteAttributeList = wil::scope_exit([&]() noexcept {
                    DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
                });

                HANDLE inheritedHandles[]{ nullInput.get(), stdoutWrite.get(), stderrWrite.get() };
                if (!UpdateProcThreadAttribute(startupInfo.lpAttributeList,
                                               0,
                                               PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               inheritedHandles,
                                               sizeof(inheritedHandles),
                                               nullptr,
                                               nullptr))
                {
                    return result;
                }

                wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
                if (!job)
                {
                    return result;
                }
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
                jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)))
                {
                    return result;
                }

                auto commandLine = _commandLine(command, objectId);
                if (commandLine.empty())
                {
                    return result;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    result.Status = WorkspaceGitCommandStatus::TimedOut;
                    return result;
                }
                PROCESS_INFORMATION processInformation{};
                if (!CreateProcessW(gitExecutable->c_str(),
                                    commandLine.data(),
                                    nullptr,
                                    nullptr,
                                    TRUE,
                                    CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                    environment.data(),
                                    canonicalWorkingDirectory->c_str(),
                                    &startupInfo.StartupInfo,
                                    &processInformation))
                {
                    return result;
                }

                wil::unique_handle process{ processInformation.hProcess };
                wil::unique_handle thread{ processInformation.hThread };
                stdoutWrite.reset();
                stderrWrite.reset();
                nullInput.reset();
                if (!AssignProcessToJobObject(job.get(), process.get()))
                {
                    TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
                    WaitForSingleObject(process.get(), ProcessCleanupMilliseconds);
                    return result;
                }
                if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
                {
                    TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
                    WaitForSingleObject(process.get(), ProcessCleanupMilliseconds);
                    return result;
                }

                std::atomic_bool outputExceeded{};
                const auto pump = [&](const HANDLE pipe, std::string& output, const size_t maximumBytes) noexcept {
                    try
                    {
                        std::array<char, 4096> buffer{};
                        for (;;)
                        {
                            DWORD bytesRead{};
                            if (!ReadFile(pipe, buffer.data(), gsl::narrow<DWORD>(buffer.size()), &bytesRead, nullptr) || bytesRead == 0)
                            {
                                return;
                            }
                            if (bytesRead > maximumBytes - std::min(output.size(), maximumBytes))
                            {
                                outputExceeded.store(true, std::memory_order_release);
                                TerminateJobObject(job.get(), ERROR_BUFFER_OVERFLOW);
                                return;
                            }
                            output.append(buffer.data(), bytesRead);
                        }
                    }
                    catch (...)
                    {
                        outputExceeded.store(true, std::memory_order_release);
                        TerminateJobObject(job.get(), ERROR_NOT_ENOUGH_MEMORY);
                    }
                };

                std::thread stdoutThread;
                std::thread stderrThread;
                try
                {
                    stdoutThread = std::thread{ pump,
                                                stdoutRead.get(),
                                                std::ref(result.Stdout),
                                                WorkspaceGitMetadata::MaximumStdoutBytes };
                    stderrThread = std::thread{ pump,
                                                stderrRead.get(),
                                                std::ref(result.Stderr),
                                                WorkspaceGitMetadata::MaximumStderrBytes };
                }
                catch (...)
                {
                    TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
                    if (stdoutThread.joinable())
                    {
                        CancelSynchronousIo(stdoutThread.native_handle());
                        stdoutThread.join();
                    }
                    WaitForSingleObject(process.get(), ProcessCleanupMilliseconds);
                    return result;
                }

                const auto now = std::chrono::steady_clock::now();
                const auto remaining = now < deadline ?
                                           std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                                                    std::chrono::milliseconds{ 1 }) :
                                           std::chrono::milliseconds::zero();
                const auto waitMilliseconds = gsl::narrow<DWORD>(remaining.count());
                const auto wait = WaitForSingleObject(process.get(), waitMilliseconds);
                DWORD exitCode{ ERROR_PROCESS_ABORTED };
                if (wait == WAIT_OBJECT_0)
                {
                    GetExitCodeProcess(process.get(), &exitCode);
                }
                TerminateJobObject(job.get(), wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_PROCESS_ABORTED);
                WaitForSingleObject(process.get(), ProcessCleanupMilliseconds);

                if (stdoutThread.joinable())
                {
                    stdoutThread.join();
                }
                if (stderrThread.joinable())
                {
                    stderrThread.join();
                }

                result.ExitCode = exitCode;
                if (outputExceeded.load(std::memory_order_acquire))
                {
                    result.Status = WorkspaceGitCommandStatus::OutputLimitExceeded;
                }
                else if (wait == WAIT_TIMEOUT || remaining == std::chrono::milliseconds::zero())
                {
                    result.Status = WorkspaceGitCommandStatus::TimedOut;
                }
                else if (wait == WAIT_OBJECT_0)
                {
                    result.Status = WorkspaceGitCommandStatus::Completed;
                }
                return result;
            }
            catch (...)
            {
                return result;
            }
        }
    }

    WorkspaceGitMetadata::WorkspaceGitMetadata(WorkspaceGitCommandRunner runner) :
        _runner{ runner ? std::move(runner) : WorkspaceGitCommandRunner{ _runGitCommand } }
    {
    }

    WorkspaceGitMetadataResult WorkspaceGitMetadata::Query(const std::filesystem::path& path,
                                                            const std::chrono::milliseconds timeout) const noexcept
    {
        WorkspaceGitMetadataResult result;
        try
        {
            if (!_validPathSyntax(path) || timeout <= std::chrono::milliseconds::zero() || timeout > MaximumTimeout)
            {
                result.Status = WorkspaceGitMetadataStatus::Invalid;
                return result;
            }

            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error)
            {
                result.Status = error == std::errc::no_such_file_or_directory ?
                                    WorkspaceGitMetadataStatus::MissingWorktree :
                                    WorkspaceGitMetadataStatus::Unverified;
                return result;
            }
            if (status.type() == std::filesystem::file_type::not_found)
            {
                result.Status = WorkspaceGitMetadataStatus::MissingWorktree;
                return result;
            }
            const auto workingDirectory = _canonicalDirectory(path);
            if (!workingDirectory)
            {
                result.Status = WorkspaceGitMetadataStatus::Invalid;
                return result;
            }

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            WorkspaceGitMetadataStatus failure{ WorkspaceGitMetadataStatus::Succeeded };
            const auto invoke = [&](const WorkspaceGitCommand command) -> std::optional<WorkspaceGitCommandResult> {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                {
                    failure = WorkspaceGitMetadataStatus::TimedOut;
                    return std::nullopt;
                }
                const auto remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                                                std::chrono::milliseconds{ 1 });
                WorkspaceGitCommandResult commandResult;
                try
                {
                    commandResult = _runner(command, workingDirectory.value(), remaining, {});
                }
                catch (...)
                {
                    failure = WorkspaceGitMetadataStatus::Unverified;
                    return std::nullopt;
                }
                if (std::chrono::steady_clock::now() >= deadline || commandResult.Status == WorkspaceGitCommandStatus::TimedOut)
                {
                    failure = WorkspaceGitMetadataStatus::TimedOut;
                    return std::nullopt;
                }
                if (commandResult.Status != WorkspaceGitCommandStatus::Completed ||
                    commandResult.Stdout.size() > MaximumStdoutBytes ||
                    commandResult.Stderr.size() > MaximumStderrBytes ||
                    !_strictUtf8(commandResult.Stderr))
                {
                    failure = WorkspaceGitMetadataStatus::Unverified;
                    return std::nullopt;
                }
                return commandResult;
            };

            const auto topLevelResult = invoke(WorkspaceGitCommand::ShowTopLevel);
            if (!topLevelResult || topLevelResult->ExitCode != 0)
            {
                result.Status = failure == WorkspaceGitMetadataStatus::Succeeded ? WorkspaceGitMetadataStatus::Unverified : failure;
                return result;
            }
            const auto worktreePath = _directoryFromResult(topLevelResult.value());

            const auto commonResult = invoke(WorkspaceGitCommand::ShowCommonGitDirectory);
            if (!commonResult || commonResult->ExitCode != 0)
            {
                result.Status = failure == WorkspaceGitMetadataStatus::Succeeded ? WorkspaceGitMetadataStatus::Unverified : failure;
                return result;
            }
            const auto commonGitDirectory = _directoryFromResult(commonResult.value());

            const auto gitDirectoryResult = invoke(WorkspaceGitCommand::ShowGitDirectory);
            if (!gitDirectoryResult || gitDirectoryResult->ExitCode != 0)
            {
                result.Status = failure == WorkspaceGitMetadataStatus::Succeeded ? WorkspaceGitMetadataStatus::Unverified : failure;
                return result;
            }
            const auto gitDirectory = _directoryFromResult(gitDirectoryResult.value());

            const auto headResult = invoke(WorkspaceGitCommand::VerifyHead);
            if (!headResult || headResult->ExitCode != 0)
            {
                result.Status = failure == WorkspaceGitMetadataStatus::Succeeded ? WorkspaceGitMetadataStatus::Unverified : failure;
                return result;
            }
            const auto head = _singleLine(headResult.value());

            const auto branchResult = invoke(WorkspaceGitCommand::SymbolicHead);
            if (!branchResult)
            {
                result.Status = failure;
                return result;
            }
            std::optional<winrt::hstring> branch;
            if (branchResult->ExitCode == 0)
            {
                branch = _singleLine(branchResult.value());
            }
            else if (branchResult->ExitCode == 1)
            {
                branch = _singleLine(branchResult.value(), true);
                if (branch && !branch->empty())
                {
                    branch.reset();
                }
            }

            if (!worktreePath || !commonGitDirectory || !gitDirectory || !head || !branch ||
                !_validHead(std::wstring_view{ head.value() }) ||
                !_validBranchRef(std::wstring_view{ branch.value() }))
            {
                result.Status = WorkspaceGitMetadataStatus::Unverified;
                return result;
            }
            const auto repositoryRoot = _canonicalDirectory(commonGitDirectory->parent_path());
            if (!repositoryRoot)
            {
                result.Status = WorkspaceGitMetadataStatus::Unverified;
                return result;
            }

            result.Binding = {
                .RepositoryRoot = repositoryRoot.value(),
                .WorktreePath = worktreePath.value(),
                .CommonGitDirectory = commonGitDirectory.value(),
                .GitDirectory = gitDirectory.value(),
                .BranchRef = branch.value(),
                .Head = head.value(),
                .CapturedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count(),
            };
            result.Status = IsValidBinding(result.Binding) ? WorkspaceGitMetadataStatus::Succeeded : WorkspaceGitMetadataStatus::Unverified;
        }
        catch (...)
        {
            result.Status = WorkspaceGitMetadataStatus::Unverified;
        }
        return result;
    }

    WorkspaceGitValidationResult WorkspaceGitMetadata::Validate(const WorkspaceGitBinding& binding,
                                                                const std::filesystem::path& path,
                                                                const std::chrono::milliseconds timeout) const noexcept
    {
        WorkspaceGitValidationResult result;
        try
        {
            if (!IsValidBinding(binding) || !_validPathSyntax(path) ||
                timeout <= std::chrono::milliseconds::zero() || timeout > MaximumTimeout)
            {
                result.Status = WorkspaceGitValidationStatus::Invalid;
                return result;
            }

            const auto validationStarted = std::chrono::steady_clock::now();
            const auto current = Query(path, timeout);
            if (current.Status != WorkspaceGitMetadataStatus::Succeeded)
            {
                result.Status = current.Status == WorkspaceGitMetadataStatus::MissingWorktree ?
                                    WorkspaceGitValidationStatus::MissingWorktree :
                                    (current.Status == WorkspaceGitMetadataStatus::Invalid ?
                                         WorkspaceGitValidationStatus::Invalid :
                                         WorkspaceGitValidationStatus::Unverified);
                return result;
            }
            result.Current = current.Binding;

            if (!_samePath(binding.RepositoryRoot, current.Binding.RepositoryRoot) ||
                !_samePath(binding.CommonGitDirectory, current.Binding.CommonGitDirectory))
            {
                result.Status = WorkspaceGitValidationStatus::DifferentRepository;
            }
            else if (!_samePath(binding.WorktreePath, current.Binding.WorktreePath) ||
                     !_samePath(binding.GitDirectory, current.Binding.GitDirectory))
            {
                result.Status = WorkspaceGitValidationStatus::WorktreeMismatch;
            }
            else if (std::wstring_view{ binding.BranchRef } != std::wstring_view{ current.Binding.BranchRef })
            {
                result.Status = WorkspaceGitValidationStatus::BranchMismatch;
            }
            else if (_equalsIgnoreCase(std::wstring_view{ binding.Head }, std::wstring_view{ current.Binding.Head }))
            {
                result.Status = WorkspaceGitValidationStatus::ValidExact;
            }
            else
            {
                const auto elapsed = std::chrono::steady_clock::now() - validationStarted;
                if (elapsed >= timeout)
                {
                    result.Status = WorkspaceGitValidationStatus::Unverified;
                    return result;
                }
                const auto remaining = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(timeout - elapsed),
                                                std::chrono::milliseconds{ 1 });
                const auto ancestry = _runner(WorkspaceGitCommand::IsAncestor,
                                              current.Binding.WorktreePath,
                                              remaining,
                                              std::wstring_view{ binding.Head });
                if (ancestry.Status != WorkspaceGitCommandStatus::Completed ||
                    ancestry.Stdout.size() > MaximumStdoutBytes ||
                    ancestry.Stderr.size() > MaximumStderrBytes ||
                    !_strictUtf8(ancestry.Stdout) || !_strictUtf8(ancestry.Stderr))
                {
                    result.Status = WorkspaceGitValidationStatus::Unverified;
                }
                else if (ancestry.ExitCode == 0)
                {
                    result.Status = WorkspaceGitValidationStatus::SameRepositoryHeadAdvanced;
                }
                else if (ancestry.ExitCode == 1)
                {
                    result.Status = WorkspaceGitValidationStatus::HeadDiverged;
                }
                else
                {
                    result.Status = WorkspaceGitValidationStatus::Unverified;
                }
            }
        }
        catch (...)
        {
            result.Status = WorkspaceGitValidationStatus::Unverified;
        }
        return result;
    }

    bool WorkspaceGitMetadata::IsValidBinding(const WorkspaceGitBinding& binding) noexcept
    {
        try
        {
            return _validPathSyntax(binding.RepositoryRoot) &&
                   _validPathSyntax(binding.WorktreePath) &&
                   _validPathSyntax(binding.CommonGitDirectory) &&
                   _validPathSyntax(binding.GitDirectory) &&
                   _samePath(binding.RepositoryRoot, binding.CommonGitDirectory.parent_path()) &&
                   _validBranchRef(std::wstring_view{ binding.BranchRef }) &&
                   _validHead(std::wstring_view{ binding.Head }) &&
                   binding.CapturedAt >= 0 &&
                   binding.CapturedAt <= MaximumUnixMilliseconds;
        }
        catch (...)
        {
            return false;
        }
    }
}
