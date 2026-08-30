// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "DirectDirectoryLease.h"
#include "DirectoryIdentity.h"
#include "GitRepositoryLock.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using namespace Microsoft::Terminal::TaskExecution;

namespace
{
    constexpr DWORD ChildWaitMilliseconds{ 15000 };

    struct Options
    {
        std::wstring Mode;
        std::filesystem::path Path;
        std::wstring RuntimeIdentity;
        std::wstring AcquiredEvent;
        std::wstring ReleaseEvent;
    };

    class HandleOwner final
    {
    public:
        explicit HandleOwner(const HANDLE value = nullptr) noexcept :
            _value{ value }
        {
        }

        ~HandleOwner()
        {
            if (_value && _value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(_value);
            }
        }

        HandleOwner(const HandleOwner&) = delete;
        HandleOwner& operator=(const HandleOwner&) = delete;

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

    std::optional<Options> _parse(const int argc, wchar_t* const* const argv)
    {
        if (argc != 11)
        {
            return std::nullopt;
        }
        Options options;
        for (int index = 1; index < argc; index += 2)
        {
            const std::wstring_view name{ argv[index] };
            const std::wstring value{ argv[index + 1] };
            if (name == L"--mode")
            {
                options.Mode = value;
            }
            else if (name == L"--path")
            {
                options.Path = value;
            }
            else if (name == L"--runtime-id")
            {
                options.RuntimeIdentity = value;
            }
            else if (name == L"--acquired-event")
            {
                options.AcquiredEvent = value;
            }
            else if (name == L"--release-event")
            {
                options.ReleaseEvent = value;
            }
            else
            {
                return std::nullopt;
            }
        }
        if ((options.Mode != L"direct" && options.Mode != L"git") ||
            options.Path.empty() || options.RuntimeIdentity.empty() ||
            options.RuntimeIdentity.size() > 128 || options.AcquiredEvent.empty() ||
            options.ReleaseEvent.empty())
        {
            return std::nullopt;
        }
        return options;
    }

    int _run(const Options& options)
    {
        HandleOwner acquiredEvent{ OpenEventW(
            EVENT_MODIFY_STATE,
            FALSE,
            options.AcquiredEvent.c_str()) };
        HandleOwner releaseEvent{ OpenEventW(
            SYNCHRONIZE,
            FALSE,
            options.ReleaseEvent.c_str()) };
        HandleOwner cancellationEvent{ CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr) };
        if (!acquiredEvent || !releaseEvent || !cancellationEvent)
        {
            return 3;
        }

        const auto policy = options.Mode == L"direct" ?
                                DirectoryIdentityPolicy::Direct :
                                DirectoryIdentityPolicy::Worktree;
        const auto identity = ResolveCanonicalDirectoryIdentity(
            options.Path,
            policy);
        if (!identity.Succeeded())
        {
            return 4;
        }

        if (options.Mode == L"direct")
        {
            DirectDirectoryLeaseManager manager;
            auto lease = manager.Acquire(
                *identity.Identity,
                cancellationEvent.Get());
            if (!lease.Acquired())
            {
                return 5;
            }
            if (!SetEvent(acquiredEvent.Get()) ||
                WaitForSingleObject(releaseEvent.Get(), ChildWaitMilliseconds) !=
                    WAIT_OBJECT_0)
            {
                return 6;
            }
            return lease.Lease.Release() == TaskDirectoryLockReleaseStatus::Released ?
                       0 :
                       7;
        }

        auto lock = AcquireGitRepositoryLock(
            *identity.Identity,
            cancellationEvent.Get(),
            GitRepositoryLockTimeoutMilliseconds);
        if (!lock.Acquired())
        {
            return 8;
        }
        if (!SetEvent(acquiredEvent.Get()) ||
            WaitForSingleObject(releaseEvent.Get(), ChildWaitMilliseconds) !=
                WAIT_OBJECT_0)
        {
            return 9;
        }
        return lock.Lock.Release() == TaskDirectoryLockReleaseStatus::Released ?
                   0 :
                   10;
    }
}

int wmain(const int argc, wchar_t* const* const argv)
{
    const auto options = _parse(argc, argv);
    if (!options)
    {
        return 2;
    }
    try
    {
        return _run(*options);
    }
    catch (...)
    {
        return 11;
    }
}
