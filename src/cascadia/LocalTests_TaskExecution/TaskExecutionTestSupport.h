// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <thread>
#include <utility>

namespace TaskExecutionLocalTests::TestSupport
{
    class UniqueHandle final
    {
    public:
        UniqueHandle() noexcept = default;
        explicit UniqueHandle(const HANDLE value) noexcept :
            _value{ value }
        {
        }

        ~UniqueHandle()
        {
            Reset();
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
                Reset();
                _value = std::exchange(other._value, nullptr);
            }
            return *this;
        }

        [[nodiscard]] HANDLE Get() const noexcept
        {
            return _value;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return _value && _value != INVALID_HANDLE_VALUE;
        }

        void Reset() noexcept
        {
            if (_value && _value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(_value);
            }
            _value = nullptr;
        }

    private:
        HANDLE _value{};
    };

    inline UniqueHandle MakeManualResetEvent(const bool signaled = false)
    {
        return UniqueHandle{ CreateEventW(nullptr, TRUE, signaled, nullptr) };
    }

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            std::array<wchar_t, MAX_PATH> temporaryDirectory{};
            VERIFY_IS_GREATER_THAN(
                GetTempPathW(
                    static_cast<DWORD>(temporaryDirectory.size()),
                    temporaryDirectory.data()),
                DWORD{ 0 });
            std::array<wchar_t, MAX_PATH> temporaryFile{};
            VERIFY_IS_TRUE(GetTempFileNameW(
                               temporaryDirectory.data(),
                               L"tex",
                               0,
                               temporaryFile.data()) != 0);
            _path = temporaryFile.data();
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

    template<typename TPredicate>
    bool WaitUntil(TPredicate&& predicate, const DWORD timeoutMilliseconds = 5000)
    {
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        while (!predicate())
        {
            if (GetTickCount64() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }
        return true;
    }
}
