// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitCommandRunner.h"
#include "GitExecutable.h"

namespace TaskGitLocalTests::TestSupport
{
    class UniqueHandle final
    {
    public:
        explicit UniqueHandle(const HANDLE value = nullptr) noexcept :
            _value{ value }
        {
        }
        ~UniqueHandle()
        {
            if (_value && _value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(_value);
            }
        }
        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;
        UniqueHandle(UniqueHandle&& other) noexcept :
            _value{ std::exchange(other._value, nullptr) }
        {
        }
        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
            {
                Reset(std::exchange(other._value, nullptr));
            }
            return *this;
        }
        void Reset(const HANDLE value = nullptr) noexcept
        {
            if (_value && _value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(_value);
            }
            _value = value;
        }
        [[nodiscard]] HANDLE Get() const noexcept
        {
            return _value;
        }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return _value && _value != INVALID_HANDLE_VALUE;
        }

    private:
        HANDLE _value{};
    };

    inline UniqueHandle MakeEvent(const bool signaled = false)
    {
        return UniqueHandle{ CreateEventW(nullptr, TRUE, signaled, nullptr) };
    }

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            std::array<wchar_t, MAX_PATH> directory{};
            VERIFY_IS_GREATER_THAN(
                GetTempPathW(static_cast<DWORD>(directory.size()), directory.data()),
                DWORD{ 0 });
            std::array<wchar_t, MAX_PATH> file{};
            VERIFY_IS_TRUE(GetTempFileNameW(
                               directory.data(),
                               L"tgt",
                               0,
                               file.data()) != 0);
            _path = file.data();
            std::filesystem::remove(_path);
            VERIFY_IS_TRUE(std::filesystem::create_directory(_path));
        }
        ~TemporaryDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(_path, ignored);
        }
        [[nodiscard]] const std::filesystem::path& Path() const noexcept
        {
            return _path;
        }

    private:
        std::filesystem::path _path;
    };

    inline std::string Utf8Path(const std::filesystem::path& path)
    {
        const auto value = path.generic_u8string();
        return {
            reinterpret_cast<const char*>(value.data()),
            value.size(),
        };
    }

    class GitFixture final
    {
    public:
        GitFixture()
        {
            const auto git = Microsoft::Terminal::TaskExecution::FindGitExecutable();
            VERIFY_IS_TRUE(git.Succeeded());
            GitExecutable = *git.Executable;
            Cancellation = MakeEvent();
            VERIFY_IS_TRUE(static_cast<bool>(Cancellation));
        }

        Microsoft::Terminal::TaskExecution::GitCommandResult Run(
            const std::filesystem::path& directory,
            std::vector<std::wstring> arguments,
            const uint32_t timeout = 30000) const
        {
            using namespace Microsoft::Terminal::TaskExecution;
            const auto identity = ResolveCanonicalDirectoryIdentity(
                directory,
                DirectoryIdentityPolicy::Worktree);
            VERIFY_IS_TRUE(identity.Succeeded());
            GitCommandRequest request;
            request.GitExecutable = GitExecutable;
            request.WorkingDirectory = *identity.Identity;
            request.Arguments = std::move(arguments);
            request.CancellationEvent = Cancellation.Get();
            request.TimeoutMilliseconds = timeout;
            return Runner.Run(request);
        }

        bool Initialize(
            const std::filesystem::path& repository,
            const bool sha256 = false) const
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
                }).Succeeded() ||
                !Run(repository, {
                    L"config",
                    L"core.autocrlf",
                    L"false",
                }).Succeeded() ||
                !Run(repository, {
                    L"config",
                    L"core.safecrlf",
                    L"false",
                }).Succeeded())
            {
                return false;
            }
            Write(repository / L"tracked.txt", "base\n");
            return Run(repository, { L"add", L"--", L"tracked.txt" }).Succeeded() &&
                   Run(repository, {
                       L"-c",
                       L"user.name=Task Git Test",
                       L"-c",
                       L"user.email=task-git@example.invalid",
                       L"-c",
                       L"commit.gpgSign=false",
                       L"-c",
                       L"core.hooksPath=NUL",
                       L"commit",
                       L"--no-verify",
                       L"-m",
                       L"base",
                   }).Succeeded();
        }

        static void Write(
            const std::filesystem::path& path,
            const std::string_view contents)
        {
            std::ofstream output{ path, std::ios::binary | std::ios::trunc };
            VERIFY_IS_TRUE(static_cast<bool>(output));
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            output.close();
            VERIFY_IS_TRUE(static_cast<bool>(output));
        }

        TemporaryDirectory Temporary;
        std::filesystem::path GitExecutable;
        UniqueHandle Cancellation;
        Microsoft::Terminal::TaskExecution::GitCommandRunner Runner;
    };
}
