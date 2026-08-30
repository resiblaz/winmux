// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "ActivationTestSupport.h"
#include "FakeRuntimeWindowsProcessApi.h"
#include "RuntimeProcessLauncher.h"
#include "RuntimeProcessObservation.h"

#include <algorithm>

namespace TaskRuntimeActivationLocalTests
{
    using namespace Microsoft::Terminal::TaskRuntime;

    inline RuntimeProcessLaunchRequest ValidLaunchRequest(
        const UniqueActivationIdentity& identity,
        const HANDLE cancellationEvent = nullptr)
    {
        RuntimeProcessLaunchRequest request;
        request.ExecutablePath = LR"(C:\Program Files\Richer Terminal\TaskRuntime.exe)";
        request.TrustedRoot = request.ExecutablePath.parent_path();
        request.Identity = identity.Identity();
        request.CancellationEvent = cancellationEvent;
        return request;
    }

    inline std::shared_ptr<IRuntimeProcessLauncher> MakeTestProcessLauncher(
        std::shared_ptr<FakeRuntimeWindowsProcessApi> api)
    {
        return details::MakeRuntimeProcessLauncherForTesting(std::move(api));
    }

    inline size_t HandleCloseCount(
        const FakeRuntimeWindowsProcessApi& api,
        const HANDLE handle)
    {
        const auto closed = api.ClosedHandleSnapshot();
        return static_cast<size_t>(std::ranges::count(closed, handle));
    }

    template<typename TPredicate>
    inline bool WaitUntil(
        TPredicate&& predicate,
        const DWORD timeoutMilliseconds = 3000)
    {
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        while (!predicate())
        {
            const auto now = GetTickCount64();
            if (now >= deadline)
            {
                return predicate();
            }
            Sleep(static_cast<DWORD>((std::min)(uint64_t{ 5 }, deadline - now)));
        }
        return true;
    }
}
