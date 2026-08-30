// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientThreads.h"

#include "RuntimeClientState.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    void DetachOrRetainRuntimeClientThread(
        std::unique_ptr<std::thread>& thread) noexcept
    {
        if (!thread)
        {
            return;
        }
        try
        {
            if (thread->joinable())
            {
                thread->detach();
            }
            thread.reset();
        }
        catch (...)
        {
            // Avoid std::thread's terminating destructor if the standard
            // wrapper cannot release an otherwise valid native thread.
            (void)thread.release();
        }
    }

    bool WaitAndReleaseRuntimeClientThread(
        std::unique_ptr<std::thread>& thread) noexcept
    {
        if (!thread)
        {
            return true;
        }
        if (!thread->joinable())
        {
            thread.reset();
            return true;
        }
        if (WaitForSingleObject(thread->native_handle(), INFINITE) != WAIT_OBJECT_0)
        {
            DetachOrRetainRuntimeClientThread(thread);
            return false;
        }
        try
        {
            thread->join();
            thread.reset();
        }
        catch (...)
        {
            // The native handle proved completion, so releasing the wrapper is
            // safe even if the standard-library join bookkeeping failed.
            DetachOrRetainRuntimeClientThread(thread);
        }
        return true;
    }

    std::unique_ptr<std::thread> StartRuntimeClientEpochWorker(
        const std::shared_ptr<RuntimeClientEpochContext>& epoch,
        std::function<void()> callback)
    {
        epoch->WorkerStarted();
        try
        {
            return std::make_unique<std::thread>(
                [epoch, callback = std::move(callback)]() mutable {
                    try
                    {
                        callback();
                    }
                    catch (...)
                    {
                    }
                    epoch->WorkerStopped();
                });
        }
        catch (...)
        {
            epoch->WorkerStopped();
            throw;
        }
    }
}
