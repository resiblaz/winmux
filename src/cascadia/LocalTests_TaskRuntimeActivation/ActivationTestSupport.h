// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeActivationDeadline.h"
#include "RuntimeActivationServices.h"
#include "RuntimeIdentity.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <stdexcept>

namespace TaskRuntimeActivationLocalTests
{
    using namespace Microsoft::Terminal::TaskRuntime;

    class UniqueActivationIdentity final
    {
    public:
        UniqueActivationIdentity()
        {
            std::array<wchar_t, MAX_PATH> temporaryDirectory{};
            if (GetTempPathW(
                    static_cast<DWORD>(temporaryDirectory.size()),
                    temporaryDirectory.data()) == 0)
            {
                throw std::runtime_error{ "GetTempPath failed" };
            }
            std::array<wchar_t, MAX_PATH> temporaryFile{};
            if (!GetTempFileNameW(
                    temporaryDirectory.data(),
                    L"rta",
                    0,
                    temporaryFile.data()))
            {
                throw std::runtime_error{ "GetTempFileName failed" };
            }
            _root = temporaryFile.data();
            std::filesystem::remove(_root);

            static std::atomic_uint64_t sequence{};
            const auto value = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
            std::array<wchar_t, 32> channel{};
            swprintf_s(
                channel.data(),
                channel.size(),
                L"act-%08lx-%08llx",
                GetCurrentProcessId(),
                value);
            const auto identity = BuildCurrentUserRuntimeIdentity(channel.data(), _root);
            if (!identity.Succeeded())
            {
                throw std::runtime_error{ identity.Error };
            }
            _identity = identity.Identity;
        }

        [[nodiscard]] const RuntimeIdentity& Identity() const noexcept
        {
            return _identity;
        }

    private:
        std::filesystem::path _root;
        RuntimeIdentity _identity;
    };

    inline RuntimeActivationDeadline DeadlineAfter(
        IRuntimeActivationServices& services,
        const uint32_t timeoutMilliseconds = 2000)
    {
        const auto deadline = MakeRuntimeActivationDeadline(
            services.Now(),
            timeoutMilliseconds);
        if (!deadline)
        {
            throw std::runtime_error{ "Activation test deadline could not be created" };
        }
        return *deadline;
    }

    inline wil::unique_handle MakeTestEvent(
        const bool manualReset = true,
        const bool signaled = false)
    {
        wil::unique_handle event{ CreateEventW(
            nullptr,
            manualReset ? TRUE : FALSE,
            signaled ? TRUE : FALSE,
            nullptr) };
        if (!event)
        {
            throw std::runtime_error{ "Activation test event creation failed" };
        }
        return event;
    }
}
