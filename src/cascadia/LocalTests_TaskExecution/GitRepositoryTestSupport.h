// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitCommandRunner.h"
#include "GitExecutable.h"
#include "TaskExecutionTestSupport.h"

#include <fstream>

namespace TaskExecutionLocalTests::TestSupport
{
    class GitRepositoryFixture final
    {
    public:
        GitRepositoryFixture()
        {
            const auto git = Microsoft::Terminal::TaskExecution::FindGitExecutable();
            VERIFY_IS_TRUE(git.Succeeded());
            GitExecutable = *git.Executable;
            Cancellation = MakeManualResetEvent();
            VERIFY_IS_TRUE(static_cast<bool>(Cancellation));
        }

        [[nodiscard]] Microsoft::Terminal::TaskExecution::GitCommandResult Run(
            const std::filesystem::path& workingDirectory,
            std::vector<std::wstring> arguments,
            const uint32_t timeoutMilliseconds = 10000) const
        {
            using namespace Microsoft::Terminal::TaskExecution;
            const auto identity = ResolveCanonicalDirectoryIdentity(
                workingDirectory,
                DirectoryIdentityPolicy::Worktree);
            VERIFY_IS_TRUE(identity.Succeeded());
            GitCommandRequest request;
            request.GitExecutable = GitExecutable;
            request.WorkingDirectory = *identity.Identity;
            request.Arguments = std::move(arguments);
            request.CancellationEvent = Cancellation.Get();
            request.TimeoutMilliseconds = timeoutMilliseconds;
            request.MaximumStdoutBytes = 1024 * 1024;
            request.MaximumStderrBytes = 1024 * 1024;
            return Runner.Run(request);
        }

        bool Initialize(
            const std::filesystem::path& repository,
            const bool sha256 = false,
            const bool createCommit = true) const
        {
            std::error_code error;
            if (!std::filesystem::create_directories(repository, error) || error)
            {
                return false;
            }
            std::vector<std::wstring> init{ L"init" };
            if (sha256)
            {
                init.emplace_back(L"--object-format=sha256");
            }
            init.emplace_back(L".");
            if (!Run(repository, std::move(init)).Succeeded() ||
                !Run(repository, {
                    L"symbolic-ref",
                    L"HEAD",
                    L"refs/heads/main",
                }).Succeeded())
            {
                return false;
            }
            if (!createCommit)
            {
                return true;
            }
            WriteFile(repository / L"initial.txt", "initial\n");
            return Run(repository, { L"add", L"--", L"initial.txt" }).Succeeded() &&
                   Run(repository, {
                       L"-c",
                       L"user.name=Task Execution Test",
                       L"-c",
                       L"user.email=task-execution@example.invalid",
                       L"-c",
                       L"commit.gpgSign=false",
                       L"-c",
                       L"core.hooksPath=NUL",
                       L"commit",
                       L"--no-verify",
                       L"-m",
                       L"initial",
                   }).Succeeded();
        }

        bool InitializeBare(const std::filesystem::path& repository) const
        {
            std::error_code error;
            if (!std::filesystem::create_directories(repository, error) || error)
            {
                return false;
            }
            return Run(repository, { L"init", L"--bare", L"." }).Succeeded();
        }

        static void WriteFile(
            const std::filesystem::path& path,
            const std::string_view contents)
        {
            std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
            VERIFY_IS_TRUE(static_cast<bool>(stream));
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            stream.close();
            VERIFY_IS_TRUE(static_cast<bool>(stream));
        }

        TemporaryDirectory Temporary;
        std::filesystem::path GitExecutable;
        UniqueHandle Cancellation;
        Microsoft::Terminal::TaskExecution::GitCommandRunner Runner;
    };
}
