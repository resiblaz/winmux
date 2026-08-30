// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "DirectDirectoryLease.h"

#include "DirectoryMutexInternal.h"

namespace Microsoft::Terminal::TaskExecution
{
    namespace details
    {
        struct DirectDirectoryWaiter final
        {
            DirectDirectoryWaiter()
            {
                TurnEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            }

            ~DirectDirectoryWaiter()
            {
                if (TurnEvent)
                {
                    CloseHandle(TurnEvent);
                }
            }

            HANDLE TurnEvent{};
        };

        struct DirectDirectoryQueueState final
        {
            std::mutex Mutex;
            std::deque<std::shared_ptr<DirectDirectoryWaiter>> Waiters;
            DWORD OwnerThreadId{};
        };

        struct DirectDirectoryLeaseData final
        {
            DirectoryMutexLease Mutex;
            std::shared_ptr<DirectDirectoryQueueState> Queue;
            std::shared_ptr<DirectDirectoryWaiter> Waiter;
        };
    }

    namespace
    {
        DirectDirectoryLeaseResult _failure(
            const TaskDirectoryLockStatus status,
            const DWORD systemError,
            std::string error)
        {
            DirectDirectoryLeaseResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _retireWaiter(
            const std::shared_ptr<details::DirectDirectoryQueueState>& queue,
            const std::shared_ptr<details::DirectDirectoryWaiter>& waiter,
            const bool clearOwner) noexcept
        {
            std::scoped_lock lock{ queue->Mutex };
            const auto found = std::ranges::find(queue->Waiters, waiter);
            if (found == queue->Waiters.end())
            {
                return false;
            }
            const auto wasFront = found == queue->Waiters.begin();
            queue->Waiters.erase(found);
            if (clearOwner)
            {
                queue->OwnerThreadId = 0;
            }
            return !wasFront || queue->Waiters.empty() ||
                   SetEvent(queue->Waiters.front()->TurnEvent) != FALSE;
        }

        TaskDirectoryLockStatus _waitForTurn(
            const std::shared_ptr<details::DirectDirectoryWaiter>& waiter,
            const HANDLE cancellationEvent,
            DWORD& systemError) noexcept
        {
            std::array<HANDLE, 2> handles{ cancellationEvent, waiter->TurnEvent };
            const auto waited = WaitForMultipleObjects(
                static_cast<DWORD>(handles.size()),
                handles.data(),
                FALSE,
                INFINITE);
            if (waited == WAIT_OBJECT_0)
            {
                systemError = ERROR_OPERATION_ABORTED;
                return TaskDirectoryLockStatus::Cancelled;
            }
            if (waited == WAIT_OBJECT_0 + 1)
            {
                return TaskDirectoryLockStatus::Acquired;
            }
            systemError = waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
            return TaskDirectoryLockStatus::SystemError;
        }
    }

    struct DirectDirectoryLeaseManager::Impl final
    {
        mutable std::mutex Mutex;
        std::unordered_map<
            std::string,
            std::weak_ptr<details::DirectDirectoryQueueState>> Queues;
    };

    DirectDirectoryLease::DirectDirectoryLease() noexcept = default;

    DirectDirectoryLease::~DirectDirectoryLease()
    {
        if (_data)
        {
            const auto released = Release();
            if (released != TaskDirectoryLockReleaseStatus::Released &&
                released != TaskDirectoryLockReleaseStatus::NotOwned)
            {
                std::terminate();
            }
        }
    }

    DirectDirectoryLease::DirectDirectoryLease(DirectDirectoryLease&& other) noexcept
    {
        if (other._data &&
            other._data->Mutex.OwnerThreadId() != GetCurrentThreadId())
        {
            std::terminate();
        }
        _data = std::move(other._data);
    }

    DirectDirectoryLease& DirectDirectoryLease::operator=(DirectDirectoryLease&& other) noexcept
    {
        if (this != &other)
        {
            if ((_data && _data->Mutex.OwnerThreadId() != GetCurrentThreadId()) ||
                (other._data &&
                 other._data->Mutex.OwnerThreadId() != GetCurrentThreadId()))
            {
                std::terminate();
            }
            if (_data)
            {
                const auto released = Release();
                if (released != TaskDirectoryLockReleaseStatus::Released &&
                    released != TaskDirectoryLockReleaseStatus::NotOwned)
                {
                    std::terminate();
                }
            }
            _data = std::move(other._data);
        }
        return *this;
    }

    DirectDirectoryLease::operator bool() const noexcept
    {
        return _data && static_cast<bool>(_data->Mutex);
    }

    bool DirectDirectoryLease::Abandoned() const noexcept
    {
        return _data && _data->Mutex.Abandoned();
    }

    DWORD DirectDirectoryLease::OwnerThreadId() const noexcept
    {
        return _data ? _data->Mutex.OwnerThreadId() : 0;
    }

    HANDLE DirectDirectoryLease::Handle() const noexcept
    {
        return _data ? _data->Mutex.Handle() : nullptr;
    }

    const std::wstring& DirectDirectoryLease::MutexName() const noexcept
    {
        static const std::wstring empty;
        return _data ? _data->Mutex.MutexName() : empty;
    }

    TaskDirectoryLockReleaseStatus DirectDirectoryLease::Release() noexcept
    {
        if (!_data)
        {
            return TaskDirectoryLockReleaseStatus::NotOwned;
        }
        const auto released = _data->Mutex.Release();
        if (released != TaskDirectoryLockReleaseStatus::Released &&
            released != TaskDirectoryLockReleaseStatus::NotOwned)
        {
            return released;
        }
        const auto retired = _retireWaiter(
            _data->Queue,
            _data->Waiter,
            true);
        _data.reset();
        return retired ?
                   TaskDirectoryLockReleaseStatus::Released :
                   TaskDirectoryLockReleaseStatus::SystemError;
    }

    std::shared_ptr<DirectDirectoryLeaseManager::Impl>
        DirectDirectoryLeaseManager::_sharedImpl()
    {
        static const auto shared = std::make_shared<Impl>();
        return shared;
    }

    DirectDirectoryLeaseManager::DirectDirectoryLeaseManager() :
        _impl{ _sharedImpl() }
    {
    }

    DirectDirectoryLeaseManager::~DirectDirectoryLeaseManager() = default;

    DirectDirectoryLeaseResult DirectDirectoryLeaseManager::Acquire(
        const CanonicalDirectoryIdentity& directory,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            if (!_impl || !directory.Valid() ||
                directory.Policy() != DirectoryIdentityPolicy::Direct)
            {
                return _failure(
                    TaskDirectoryLockStatus::InvalidIdentity,
                    ERROR_INVALID_PARAMETER,
                    "Direct directory lease identity is invalid");
            }
            if (!cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE)
            {
                return _failure(
                    TaskDirectoryLockStatus::InvalidCancellation,
                    ERROR_INVALID_HANDLE,
                    "Direct directory lease cancellation handle is invalid");
            }
            const auto cancelled = WaitForSingleObject(cancellationEvent, 0);
            if (cancelled == WAIT_OBJECT_0)
            {
                return _failure(
                    TaskDirectoryLockStatus::Cancelled,
                    ERROR_OPERATION_ABORTED,
                    "Direct directory lease was cancelled before queueing");
            }
            if (cancelled != WAIT_TIMEOUT)
            {
                return _failure(
                    TaskDirectoryLockStatus::InvalidCancellation,
                    cancelled == WAIT_FAILED ? GetLastError() : ERROR_INVALID_HANDLE,
                    "Direct directory lease cancellation handle is not waitable");
            }

            std::shared_ptr<details::DirectDirectoryQueueState> queue;
            {
                std::scoped_lock lock{ _impl->Mutex };
                const auto found = _impl->Queues.find(directory.Digest());
                if (found != _impl->Queues.end())
                {
                    queue = found->second.lock();
                }
                if (!queue)
                {
                    for (auto iterator = _impl->Queues.begin();
                         iterator != _impl->Queues.end();)
                    {
                        if (iterator->second.expired())
                        {
                            iterator = _impl->Queues.erase(iterator);
                        }
                        else
                        {
                            ++iterator;
                        }
                    }
                    queue = std::make_shared<details::DirectDirectoryQueueState>();
                    _impl->Queues[directory.Digest()] = queue;
                }
            }

            auto waiter = std::make_shared<details::DirectDirectoryWaiter>();
            if (!waiter->TurnEvent)
            {
                return _failure(
                    TaskDirectoryLockStatus::SystemError,
                    GetLastError(),
                    "Direct directory FIFO event creation failed");
            }
            std::unique_ptr<details::DirectDirectoryLeaseData> leaseData;
            try
            {
                leaseData = std::make_unique<details::DirectDirectoryLeaseData>();
            }
            catch (...)
            {
                return _failure(
                    TaskDirectoryLockStatus::ResourceFailure,
                    ERROR_NOT_ENOUGH_MEMORY,
                    "Direct directory lease state allocation failed");
            }

            {
                std::scoped_lock lock{ queue->Mutex };
                if (queue->OwnerThreadId == GetCurrentThreadId())
                {
                    return _failure(
                        TaskDirectoryLockStatus::ReentrantAcquire,
                        ERROR_POSSIBLE_DEADLOCK,
                        "Direct directory lease cannot be reacquired by its owner thread");
                }
                queue->Waiters.emplace_back(waiter);
                if (queue->Waiters.front() == waiter &&
                    !SetEvent(waiter->TurnEvent))
                {
                    queue->Waiters.pop_front();
                    return _failure(
                        TaskDirectoryLockStatus::SystemError,
                        GetLastError(),
                        "Direct directory FIFO head could not be signaled");
                }
            }

            DWORD systemError{};
            const auto turn = _waitForTurn(waiter, cancellationEvent, systemError);
            if (turn != TaskDirectoryLockStatus::Acquired)
            {
                const auto retired = _retireWaiter(queue, waiter, false);
                return _failure(
                    retired ? turn : TaskDirectoryLockStatus::SystemError,
                    retired ? systemError : ERROR_INVALID_STATE,
                    retired ?
                        "Direct directory FIFO wait did not acquire the head" :
                        "Direct directory FIFO cancellation could not retire its waiter");
            }

            auto acquired = details::AcquireDirectoryMutex(
                directory,
                details::DirectoryMutexScope::Direct,
                INFINITE,
                cancellationEvent);
            if (!acquired.Acquired())
            {
                const auto retired = _retireWaiter(queue, waiter, false);
                return _failure(
                    retired ? acquired.Status : TaskDirectoryLockStatus::SystemError,
                    retired ? acquired.SystemError : ERROR_INVALID_STATE,
                    retired ?
                        std::move(acquired.Error) :
                        "Direct directory FIFO failed to advance after lock failure");
            }

            bool ownershipValid{};
            {
                std::scoped_lock lock{ queue->Mutex };
                ownershipValid = !queue->Waiters.empty() &&
                                 queue->Waiters.front() == waiter &&
                                 queue->OwnerThreadId == 0;
                if (ownershipValid)
                {
                    queue->OwnerThreadId = GetCurrentThreadId();
                }
            }
            if (!ownershipValid)
            {
                (void)acquired.Lease.Release();
                (void)_retireWaiter(queue, waiter, true);
                return _failure(
                    TaskDirectoryLockStatus::SystemError,
                    ERROR_INVALID_STATE,
                    "Direct directory FIFO ownership changed unexpectedly");
            }

            DirectDirectoryLeaseResult result;
            result.Status = TaskDirectoryLockStatus::Acquired;
            leaseData->Mutex = std::move(acquired.Lease);
            leaseData->Queue = std::move(queue);
            leaseData->Waiter = std::move(waiter);
            result.Lease._data = std::move(leaseData);
            return result;
        }
        catch (const std::exception&)
        {
            return _failure(
                TaskDirectoryLockStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Direct directory lease exhausted local resources");
        }
        catch (...)
        {
            return _failure(
                TaskDirectoryLockStatus::ResourceFailure,
                ERROR_UNHANDLED_EXCEPTION,
                "Unexpected Direct directory lease failure");
        }
    }

    size_t DirectDirectoryLeaseManager::ParticipantCount(
        const CanonicalDirectoryIdentity& directory) const noexcept
    {
        try
        {
            if (!_impl || !directory.Valid())
            {
                return 0;
            }
            std::shared_ptr<details::DirectDirectoryQueueState> queue;
            {
                std::scoped_lock lock{ _impl->Mutex };
                const auto found = _impl->Queues.find(directory.Digest());
                if (found == _impl->Queues.end())
                {
                    return 0;
                }
                queue = found->second.lock();
            }
            if (!queue)
            {
                return 0;
            }
            std::scoped_lock lock{ queue->Mutex };
            return queue->Waiters.size();
        }
        catch (...)
        {
            return 0;
        }
    }
}
