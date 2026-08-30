// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientState.h"
#include "RuntimeClientThreads.h"

namespace Microsoft::Terminal::TaskRuntime::details
{
    RuntimeClientEvent::RuntimeClientEvent(
        const bool manualReset,
        const bool signaled) noexcept :
        _handle{ CreateEventW(nullptr, manualReset ? TRUE : FALSE, signaled ? TRUE : FALSE, nullptr) }
    {
    }

    RuntimeClientEvent::~RuntimeClientEvent()
    {
        if (_handle)
        {
            CloseHandle(_handle);
        }
    }

    RuntimeClientEvent::operator bool() const noexcept
    {
        return _handle != nullptr;
    }

    HANDLE RuntimeClientEvent::Get() const noexcept
    {
        return _handle;
    }

    void RuntimeClientEvent::Signal() noexcept
    {
        if (_handle)
        {
            SetEvent(_handle);
        }
    }

    void RuntimeClientEvent::Reset() noexcept
    {
        if (_handle)
        {
            ResetEvent(_handle);
        }
    }

    RuntimeClientEpochContext::RuntimeClientEpochContext(
        Microsoft::Terminal::TaskRuntimeProtocol::HelloResponse hello,
        std::unique_ptr<IRuntimeControlConnection> connection) :
        Hello{ std::move(hello) },
        Connection{ std::move(connection) }
    {
    }

    RuntimeClientEpochContext::~RuntimeClientEpochContext()
    {
        Close();
    }

    bool RuntimeClientEpochContext::Valid() const noexcept
    {
        return Connection && Connection->Connected() && Cancellation && RetireRequested &&
               WorkersFinished && CancelFinished;
    }

    void RuntimeClientEpochContext::WorkerStarted() noexcept
    {
        AcquireSRWLockExclusive(&WorkerLock);
        if (WorkerCount++ == 0)
        {
            WorkersFinished.Reset();
        }
        ReleaseSRWLockExclusive(&WorkerLock);
    }

    void RuntimeClientEpochContext::WorkerStopped() noexcept
    {
        AcquireSRWLockExclusive(&WorkerLock);
        if (WorkerCount != 0 && --WorkerCount == 0)
        {
            WorkersFinished.Signal();
        }
        ReleaseSRWLockExclusive(&WorkerLock);
    }

    void RuntimeClientEpochContext::Cancel() noexcept
    {
        Cancellation.Signal();
        RetireRequested.Signal();

        std::shared_ptr<IRuntimeControlConnection> connection;
        AcquireSRWLockExclusive(&LifecycleLock);
        if (!CloseStarted && !CancellationIssued && Connection)
        {
            CancellationIssued = true;
            CancelFinished.Reset();
            connection = Connection;
        }
        ReleaseSRWLockExclusive(&LifecycleLock);

        if (connection)
        {
            connection->CancelPendingIo();
            CancelFinished.Signal();
        }
    }

    void RuntimeClientEpochContext::Close() noexcept
    {
        WaitForSingleObject(WorkersFinished.Get(), INFINITE);

        std::shared_ptr<IRuntimeControlConnection> connection;
        AcquireSRWLockExclusive(&LifecycleLock);
        if (!CloseStarted)
        {
            CloseStarted = true;
            connection = std::move(Connection);
        }
        ReleaseSRWLockExclusive(&LifecycleLock);

        if (connection)
        {
            WaitForSingleObject(CancelFinished.Get(), INFINITE);
            connection->Close();
        }
    }

    RuntimeClientEpochWorkers::RuntimeClientEpochWorkers(
        std::shared_ptr<RuntimeClientEpochContext> epoch) noexcept :
        _epoch{ std::move(epoch) }
    {
    }

    RuntimeClientEpochWorkers::~RuntimeClientEpochWorkers()
    {
        JoinAndClose();
    }

    void RuntimeClientEpochWorkers::JoinAndClose() noexcept
    {
        if (_finished)
        {
            return;
        }
        _finished = true;
        if (_epoch)
        {
            _epoch->Cancel();
        }
        (void)WaitAndReleaseRuntimeClientThread(Reader);
        (void)WaitAndReleaseRuntimeClientThread(Writer);
        if (_epoch)
        {
            // Close also waits for the epoch worker-count barrier. If native
            // thread waiting failed and a wrapper had to detach, request state
            // still cannot advance while that worker can touch the connection.
            _epoch->Close();
        }
    }
}
