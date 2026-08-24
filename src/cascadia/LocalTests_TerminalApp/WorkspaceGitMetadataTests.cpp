// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/WorkspaceGitMetadata.h"

#include <fstream>

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;

using winrt::TerminalApp::implementation::WorkspaceGitBinding;
using winrt::TerminalApp::implementation::WorkspaceGitCommand;
using winrt::TerminalApp::implementation::WorkspaceGitCommandResult;
using winrt::TerminalApp::implementation::WorkspaceGitCommandStatus;
using winrt::TerminalApp::implementation::WorkspaceGitMetadata;
using winrt::TerminalApp::implementation::WorkspaceGitMetadataStatus;
using winrt::TerminalApp::implementation::WorkspaceGitValidationStatus;

namespace TerminalAppLocalTests
{
    namespace
    {
        constexpr std::wstring_view TestRootPrefix{ L"workspace-git-metadata-" };

        std::filesystem::path FindGitExecutable()
        {
            const auto required = SearchPathW(nullptr, L"git.exe", nullptr, 0, nullptr, nullptr);
            if (required == 0)
            {
                return {};
            }
            std::wstring path(static_cast<size_t>(required), L'\0');
            const auto written = SearchPathW(nullptr, L"git.exe", nullptr, required, path.data(), nullptr);
            if (written == 0 || written >= required)
            {
                return {};
            }
            path.resize(written);
            return std::filesystem::path{ path };
        }

        std::wstring QuoteArgument(const std::wstring_view argument)
        {
            std::wstring quoted{ L'"' };
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
                    quoted.append(backslashes * 2 + 1, L'\\');
                    quoted.push_back(character);
                    backslashes = 0;
                    continue;
                }
                quoted.append(backslashes, L'\\');
                backslashes = 0;
                quoted.push_back(character);
            }
            quoted.append(backslashes * 2, L'\\');
            quoted.push_back(L'"');
            return quoted;
        }

        bool RunGit(const std::filesystem::path& gitExecutable,
                    const std::filesystem::path& workingDirectory,
                    const std::initializer_list<std::wstring_view> arguments)
        {
            std::wstring commandLine{ QuoteArgument(gitExecutable.native()) };
            for (const auto argument : arguments)
            {
                commandLine.push_back(L' ');
                commandLine.append(QuoteArgument(argument));
            }

            SECURITY_ATTRIBUTES securityAttributes{};
            securityAttributes.nLength = sizeof(securityAttributes);
            securityAttributes.bInheritHandle = TRUE;

            HANDLE rawRead{};
            HANDLE rawWrite{};
            if (!CreatePipe(&rawRead, &rawWrite, &securityAttributes, 0))
            {
                Log::Comment(fmt::format(FMT_COMPILE(L"RunGit CreatePipe failed error={} command={}"),
                                         GetLastError(),
                                         commandLine)
                                 .c_str());
                return false;
            }
            wil::unique_handle outputRead{ rawRead };
            wil::unique_handle outputWrite{ rawWrite };
            if (!SetHandleInformation(outputRead.get(), HANDLE_FLAG_INHERIT, 0))
            {
                Log::Comment(fmt::format(FMT_COMPILE(L"RunGit SetHandleInformation failed error={} command={}"),
                                         GetLastError(),
                                         commandLine)
                                 .c_str());
                return false;
            }

            HANDLE rawInputRead{};
            HANDLE rawInputWrite{};
            if (!CreatePipe(&rawInputRead, &rawInputWrite, &securityAttributes, 0))
            {
                Log::Comment(fmt::format(FMT_COMPILE(L"RunGit stdin CreatePipe failed error={} command={}"),
                                         GetLastError(),
                                         commandLine)
                                 .c_str());
                return false;
            }
            wil::unique_handle inputRead{ rawInputRead };
            wil::unique_handle inputWrite{ rawInputWrite };
            if (!SetHandleInformation(inputWrite.get(), HANDLE_FLAG_INHERIT, 0))
            {
                Log::Comment(fmt::format(FMT_COMPILE(L"RunGit stdin SetHandleInformation failed error={} command={}"),
                                         GetLastError(),
                                         commandLine)
                                 .c_str());
                return false;
            }

            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            startupInfo.dwFlags = STARTF_USESTDHANDLES;
            startupInfo.hStdInput = inputRead.get();
            startupInfo.hStdOutput = outputWrite.get();
            startupInfo.hStdError = outputWrite.get();
            PROCESS_INFORMATION processInformation{};
            if (!CreateProcessW(gitExecutable.c_str(),
                                commandLine.data(),
                                nullptr,
                                nullptr,
                                TRUE,
                                CREATE_NO_WINDOW,
                                nullptr,
                                workingDirectory.c_str(),
                                &startupInfo,
                                &processInformation))
            {
                Log::Comment(fmt::format(FMT_COMPILE(L"RunGit CreateProcess failed error={} cwd={} command={}"),
                                         GetLastError(),
                                         workingDirectory.native(),
                                         commandLine)
                                 .c_str());
                return false;
            }
            wil::unique_handle process{ processInformation.hProcess };
            wil::unique_handle thread{ processInformation.hThread };
            inputRead.reset();
            inputWrite.reset();
            outputWrite.reset();
            const auto wait = WaitForSingleObject(process.get(), 15000);
            if (wait != WAIT_OBJECT_0)
            {
                TerminateProcess(process.get(), ERROR_TIMEOUT);
                WaitForSingleObject(process.get(), 1000);
                Log::Comment(fmt::format(FMT_COMPILE(L"RunGit wait failed result={} error={} cwd={} command={}"),
                                         wait,
                                         GetLastError(),
                                         workingDirectory.native(),
                                         commandLine)
                                 .c_str());
                return false;
            }

            std::string output;
            std::array<char, 4096> buffer{};
            for (;;)
            {
                DWORD read{};
                if (!ReadFile(outputRead.get(), buffer.data(), gsl::narrow<DWORD>(buffer.size()), &read, nullptr) || read == 0)
                {
                    break;
                }
                output.append(buffer.data(), read);
            }
            DWORD exitCode{ ERROR_PROCESS_ABORTED };
            if (!GetExitCodeProcess(process.get(), &exitCode) || exitCode != 0)
            {
                winrt::hstring decodedOutput;
                try
                {
                    decodedOutput = winrt::hstring{ til::u8u16(output) };
                }
                catch (...)
                {
                    decodedOutput = L"<non-UTF-8 output>";
                }
                Log::Comment(fmt::format(FMT_COMPILE(L"RunGit failed exit={} cwd={} command={} output={}"),
                                         exitCode,
                                         workingDirectory.native(),
                                         commandLine,
                                         decodedOutput)
                                 .c_str());
                return false;
            }
            return true;
        }

        void WriteTextFile(const std::filesystem::path& path, const std::string_view contents)
        {
            std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
            VERIFY_IS_TRUE(static_cast<bool>(stream));
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            stream.close();
            VERIFY_IS_TRUE(static_cast<bool>(stream));
        }

        void VerifyStatus(const WorkspaceGitMetadataStatus expected, const WorkspaceGitMetadataStatus actual)
        {
            VERIFY_ARE_EQUAL(static_cast<int>(expected), static_cast<int>(actual));
        }

        void VerifyStatus(const WorkspaceGitValidationStatus expected, const WorkspaceGitValidationStatus actual)
        {
            VERIFY_ARE_EQUAL(static_cast<int>(expected), static_cast<int>(actual));
        }
    }

    class WorkspaceGitMetadataTests
    {
        TEST_CLASS(WorkspaceGitMetadataTests);

        TEST_METHOD(QueriesMainLinkedDetachedAndUnicodeWorktrees);
        TEST_METHOD(ValidatesExactDriftRepositoryWorktreeAndMissingStates);
        TEST_METHOD(RejectsInvalidBoundsAndClassifiesFakeTimeout);

        TEST_METHOD_SETUP(MethodSetup)
        {
            try
            {
                _temporaryFolderPath = std::filesystem::temp_directory_path().lexically_normal();
                if (!_temporaryFolderPath.has_filename())
                {
                    _temporaryFolderPath = _temporaryFolderPath.parent_path();
                }
                GUID id{};
                if (FAILED(CoCreateGuid(&id)))
                {
                    Log::Comment(L"WorkspaceGitMetadataTests setup: CoCreateGuid failed.");
                    return false;
                }
                _root = (_temporaryFolderPath /
                         std::filesystem::path{ fmt::format(FMT_COMPILE(L"{}{}-Unicode 空格"),
                                                            TestRootPrefix,
                                                            ::Microsoft::Console::Utils::GuidToPlainString(id)) })
                            .lexically_normal();
                _gitExecutable = FindGitExecutable();
                if (_gitExecutable.empty())
                {
                    Log::Comment(L"WorkspaceGitMetadataTests setup: git.exe was not found on PATH.");
                    return false;
                }
                if (!_Cleanup())
                {
                    Log::Comment(fmt::format(FMT_COMPILE(L"WorkspaceGitMetadataTests setup: cleanup failed root={}"), _root.native()).c_str());
                    return false;
                }
                std::error_code error;
                const auto created = std::filesystem::create_directories(_root, error);
                if (!created || error)
                {
                    Log::Comment(fmt::format(FMT_COMPILE(L"WorkspaceGitMetadataTests setup: create root failed value={} error={} root={}"),
                                             created,
                                             error.value(),
                                             _root.native())
                                     .c_str());
                    return false;
                }
                return true;
            }
            catch (...)
            {
                Log::Comment(L"WorkspaceGitMetadataTests setup: unexpected exception.");
                return false;
            }
        }

        TEST_METHOD_CLEANUP(MethodCleanup)
        {
            return _Cleanup();
        }

    private:
        bool _Cleanup() const noexcept
        {
            try
            {
                if (_root.empty() ||
                    _root.parent_path() != _temporaryFolderPath ||
                    _root.filename().native().rfind(TestRootPrefix, 0) != 0)
                {
                    return _root.empty();
                }
                std::error_code error;
                std::filesystem::remove_all(_root, error);
                return !error;
            }
            catch (...)
            {
                return false;
            }
        }

        bool _InitializeRepository(const std::filesystem::path& repository) const
        {
            std::error_code error;
            std::filesystem::create_directories(repository, error);
            if (error ||
                !RunGit(_gitExecutable, repository, { L"init", L"." }) ||
                !RunGit(_gitExecutable, repository, { L"symbolic-ref", L"HEAD", L"refs/heads/main" }))
            {
                return false;
            }
            WriteTextFile(repository / L"initial.txt", "initial\n");
            return RunGit(_gitExecutable, repository, { L"add", L"--", L"initial.txt" }) &&
                   RunGit(_gitExecutable,
                          repository,
                          { L"-c", L"user.name=Workspace Metadata Test",
                            L"-c", L"user.email=test@example.invalid",
                            L"-c", L"commit.gpgSign=false",
                            L"-c", L"core.hooksPath=NUL",
                            L"commit", L"-m", L"initial" });
        }

        bool _Commit(const std::filesystem::path& repository,
                     const std::wstring_view filename,
                     const std::string_view contents,
                     const std::wstring_view message) const
        {
            WriteTextFile(repository / std::filesystem::path{ filename }, contents);
            return RunGit(_gitExecutable, repository, { L"add", L"--", filename }) &&
                   RunGit(_gitExecutable,
                          repository,
                          { L"-c", L"user.name=Workspace Metadata Test",
                            L"-c", L"user.email=test@example.invalid",
                            L"-c", L"commit.gpgSign=false",
                            L"-c", L"core.hooksPath=NUL",
                            L"commit", L"-m", message });
        }

        std::filesystem::path _temporaryFolderPath;
        std::filesystem::path _root;
        std::filesystem::path _gitExecutable;
    };

    void WorkspaceGitMetadataTests::QueriesMainLinkedDetachedAndUnicodeWorktrees()
    {
        const auto repository = _root / L"主 repository";
        const auto linked = _root / L"linked 工作树";
        const auto detached = _root / L"detached 工作树";
        VERIFY_IS_TRUE(_InitializeRepository(repository));
        VERIFY_IS_TRUE(RunGit(_gitExecutable, repository, { L"worktree", L"add", L"-b", L"linked", linked.native(), L"HEAD" }));
        VERIFY_IS_TRUE(RunGit(_gitExecutable, repository, { L"worktree", L"add", L"--detach", detached.native(), L"HEAD" }));

        WorkspaceGitMetadata metadata;
        const auto mainResult = metadata.Query(repository);
        const auto linkedResult = metadata.Query(linked);
        const auto detachedResult = metadata.Query(detached);
        VerifyStatus(WorkspaceGitMetadataStatus::Succeeded, mainResult.Status);
        VerifyStatus(WorkspaceGitMetadataStatus::Succeeded, linkedResult.Status);
        VerifyStatus(WorkspaceGitMetadataStatus::Succeeded, detachedResult.Status);

        std::error_code error;
        const auto canonicalRepository = std::filesystem::canonical(repository, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));
        const auto canonicalLinked = std::filesystem::canonical(linked, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));
        const auto canonicalDetached = std::filesystem::canonical(detached, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));
        const auto canonicalCommon = std::filesystem::canonical(repository / L".git", error);
        VERIFY_IS_FALSE(static_cast<bool>(error));

        VERIFY_IS_TRUE(mainResult.Binding.RepositoryRoot == canonicalRepository);
        VERIFY_IS_TRUE(mainResult.Binding.WorktreePath == canonicalRepository);
        VERIFY_IS_TRUE(mainResult.Binding.CommonGitDirectory == canonicalCommon);
        VERIFY_IS_TRUE(mainResult.Binding.GitDirectory == canonicalCommon);
        VERIFY_ARE_EQUAL(L"refs/heads/main", mainResult.Binding.BranchRef);
        VERIFY_IS_GREATER_THAN(mainResult.Binding.CapturedAt, int64_t{ 0 });

        VERIFY_IS_TRUE(linkedResult.Binding.RepositoryRoot == canonicalRepository);
        VERIFY_IS_TRUE(linkedResult.Binding.WorktreePath == canonicalLinked);
        VERIFY_IS_TRUE(linkedResult.Binding.CommonGitDirectory == canonicalCommon);
        VERIFY_IS_FALSE(linkedResult.Binding.GitDirectory == linkedResult.Binding.CommonGitDirectory);
        VERIFY_ARE_EQUAL(L"refs/heads/linked", linkedResult.Binding.BranchRef);
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(linked / L".git"));

        VERIFY_IS_TRUE(detachedResult.Binding.RepositoryRoot == canonicalRepository);
        VERIFY_IS_TRUE(detachedResult.Binding.WorktreePath == canonicalDetached);
        VERIFY_IS_TRUE(detachedResult.Binding.BranchRef.empty());
        VERIFY_IS_TRUE(detachedResult.Binding.Head == mainResult.Binding.Head);
    }

    void WorkspaceGitMetadataTests::ValidatesExactDriftRepositoryWorktreeAndMissingStates()
    {
        const auto repository = _root / L"repository one";
        const auto linked = _root / L"linked one";
        const auto otherRepository = _root / L"repository two";
        VERIFY_IS_TRUE(_InitializeRepository(repository));
        VERIFY_IS_TRUE(_InitializeRepository(otherRepository));
        VERIFY_IS_TRUE(RunGit(_gitExecutable, repository, { L"worktree", L"add", L"-b", L"linked-one", linked.native(), L"HEAD" }));

        WorkspaceGitMetadata metadata;
        const auto original = metadata.Query(repository);
        const auto linkedBinding = metadata.Query(linked);
        VerifyStatus(WorkspaceGitMetadataStatus::Succeeded, original.Status);
        VerifyStatus(WorkspaceGitMetadataStatus::Succeeded, linkedBinding.Status);
        VerifyStatus(WorkspaceGitValidationStatus::ValidExact, metadata.Validate(original.Binding, repository).Status);
        VerifyStatus(WorkspaceGitValidationStatus::WorktreeMismatch, metadata.Validate(original.Binding, linked).Status);
        VerifyStatus(WorkspaceGitValidationStatus::DifferentRepository, metadata.Validate(original.Binding, otherRepository).Status);

        VERIFY_IS_TRUE(_Commit(repository, L"advance.txt", "advance\n", L"advance"));
        VerifyStatus(WorkspaceGitValidationStatus::SameRepositoryHeadAdvanced, metadata.Validate(original.Binding, repository).Status);

        const auto advanced = metadata.Query(repository);
        VerifyStatus(WorkspaceGitMetadataStatus::Succeeded, advanced.Status);
        VERIFY_IS_TRUE(RunGit(_gitExecutable, repository, { L"checkout", L"-b", L"branch-drift" }));
        VerifyStatus(WorkspaceGitValidationStatus::BranchMismatch, metadata.Validate(advanced.Binding, repository).Status);
        VERIFY_IS_TRUE(RunGit(_gitExecutable, repository, { L"checkout", L"-" }));
        VERIFY_IS_TRUE(RunGit(_gitExecutable, repository, { L"reset", L"--hard", L"HEAD~1" }));
        VerifyStatus(WorkspaceGitValidationStatus::HeadDiverged, metadata.Validate(advanced.Binding, repository).Status);

        VERIFY_IS_TRUE(RunGit(_gitExecutable, repository, { L"worktree", L"remove", L"--force", linked.native() }));
        VerifyStatus(WorkspaceGitValidationStatus::MissingWorktree, metadata.Validate(linkedBinding.Binding, linked).Status);
    }

    void WorkspaceGitMetadataTests::RejectsInvalidBoundsAndClassifiesFakeTimeout()
    {
        const auto repository = _root / L"bounded repository";
        VERIFY_IS_TRUE(_InitializeRepository(repository));

        WorkspaceGitMetadata metadata;
        const auto binding = metadata.Query(repository);
        VerifyStatus(WorkspaceGitMetadataStatus::Succeeded, binding.Status);
        VerifyStatus(WorkspaceGitMetadataStatus::Invalid, metadata.Query(L"relative\\path").Status);
        VerifyStatus(WorkspaceGitMetadataStatus::Invalid,
                     metadata.Query(std::filesystem::path{ L"C:\\" +
                                                           std::wstring(WorkspaceGitMetadata::MaximumPathCharacters, L'a') })
                         .Status);
        VerifyStatus(WorkspaceGitMetadataStatus::Invalid,
                     metadata.Query(repository, WorkspaceGitMetadata::MaximumTimeout + std::chrono::milliseconds{ 1 }).Status);
        VerifyStatus(WorkspaceGitValidationStatus::Invalid, metadata.Validate(WorkspaceGitBinding{}, repository).Status);
        auto invalidCapturedAt = binding.Binding;
        invalidCapturedAt.CapturedAt = WorkspaceGitMetadata::MaximumUnixMilliseconds + 1;
        VerifyStatus(WorkspaceGitValidationStatus::Invalid, metadata.Validate(invalidCapturedAt, repository).Status);

        std::vector<WorkspaceGitCommand> commands;
        std::vector<std::chrono::milliseconds> timeouts;
        WorkspaceGitMetadata timedOut{
            [&](const WorkspaceGitCommand command,
                const std::filesystem::path& workingDirectory,
                const std::chrono::milliseconds timeout,
                const std::wstring_view) {
                commands.push_back(command);
                timeouts.push_back(timeout);
                VERIFY_IS_TRUE(workingDirectory == std::filesystem::canonical(repository));
                return WorkspaceGitCommandResult{ .Status = WorkspaceGitCommandStatus::TimedOut };
            }
        };

        VerifyStatus(WorkspaceGitMetadataStatus::TimedOut, timedOut.Query(repository, std::chrono::milliseconds{ 250 }).Status);
        VERIFY_ARE_EQUAL(size_t{ 1 }, commands.size());
        VERIFY_ARE_EQUAL(static_cast<int>(WorkspaceGitCommand::ShowTopLevel), static_cast<int>(commands[0]));
        VERIFY_IS_GREATER_THAN(timeouts[0].count(), int64_t{ 0 });
        VERIFY_IS_LESS_THAN_OR_EQUAL(timeouts[0].count(), int64_t{ 250 });
        VerifyStatus(WorkspaceGitValidationStatus::Unverified,
                     timedOut.Validate(binding.Binding, repository, std::chrono::milliseconds{ 250 }).Status);

        WorkspaceGitMetadata invalidUtf8{
            [](const WorkspaceGitCommand, const std::filesystem::path&, const std::chrono::milliseconds, const std::wstring_view) {
                return WorkspaceGitCommandResult{
                    .Status = WorkspaceGitCommandStatus::Completed,
                    .ExitCode = 0,
                    .Stdout = std::string{ "\xff\n", 2 },
                };
            }
        };
        VerifyStatus(WorkspaceGitMetadataStatus::Unverified, invalidUtf8.Query(repository).Status);

        WorkspaceGitMetadata oversized{
            [](const WorkspaceGitCommand, const std::filesystem::path&, const std::chrono::milliseconds, const std::wstring_view) {
                return WorkspaceGitCommandResult{
                    .Status = WorkspaceGitCommandStatus::Completed,
                    .ExitCode = 0,
                    .Stdout = std::string(WorkspaceGitMetadata::MaximumStdoutBytes + 1, 'x'),
                };
            }
        };
        VerifyStatus(WorkspaceGitMetadataStatus::Unverified, oversized.Query(repository).Status);
    }
}
