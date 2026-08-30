// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeProcessWin32Support.h"

#include <mutex>
#include <system_error>

namespace TaskRuntimeLocalTests::ProcessFixtures::details
{
    namespace
    {
        constexpr size_t MaximumCapturedStandardErrorBytes{ 256 * 1024 };
    }

    struct StderrCaptureFile::Impl
    {
        explicit Impl(std::filesystem::path path) :
            Path{ std::move(path) }
        {
        }

        ~Impl()
        {
            std::error_code ignored;
            (void)std::filesystem::remove(Path, ignored);
        }

        [[nodiscard]] std::string Snapshot() const
        {
            std::scoped_lock lock{ Mutex };
            const auto raw = CreateFileW(
                Path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (raw == INVALID_HANDLE_VALUE)
            {
                return "[stderr capture unavailable: " + std::to_string(GetLastError()) + "]";
            }
            HandleOwner read{ raw };

            std::string result;
            result.reserve(4096);
            std::array<char, 4096> buffer{};
            while (result.size() < MaximumCapturedStandardErrorBytes)
            {
                DWORD copied{};
                const auto remaining = MaximumCapturedStandardErrorBytes - result.size();
                if (!ReadFile(
                        read.Get(),
                        buffer.data(),
                        static_cast<DWORD>((std::min)(remaining, buffer.size())),
                        &copied,
                        nullptr) ||
                    copied == 0)
                {
                    break;
                }
                result.append(buffer.data(), copied);
            }

            LARGE_INTEGER size{};
            if (GetFileSizeEx(read.Get(), &size) &&
                size.QuadPart > static_cast<LONGLONG>(result.size()))
            {
                result.append("\n[stderr truncated by test fixture]");
            }
            return result;
        }

        std::filesystem::path Path;
        mutable std::mutex Mutex;
    };

    StderrCaptureFile::StderrCaptureFile(std::filesystem::path path) :
        _impl{ std::make_unique<Impl>(std::move(path)) }
    {
    }

    StderrCaptureFile::~StderrCaptureFile() = default;

    std::string StderrCaptureFile::Snapshot() const
    {
        return _impl->Snapshot();
    }
}
