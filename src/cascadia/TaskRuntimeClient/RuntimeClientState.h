// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientEpoch.h"
#include "RuntimeClientInterfaces.h"
#include "RuntimeClientRequests.h"
#include "RuntimeClientSubscriptionStreamInternal.h"
#include "RuntimeClientSubscriptions.h"
#include "TaskRuntimeClient.h"

#include "RuntimeControlConnector.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Microsoft::Terminal::TaskRuntime::details
{
    class RuntimeClientEvent final
    {
    public:
        explicit RuntimeClientEvent(bool manualReset = true, bool signaled = false) noexcept;
        ~RuntimeClientEvent();

        RuntimeClientEvent(const RuntimeClientEvent&) = delete;
        RuntimeClientEvent& operator=(const RuntimeClientEvent&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] HANDLE Get() const noexcept;
        void Signal() noexcept;
        void Reset() noexcept;

    private:
        HANDLE _handle{};
    };

    struct RuntimeClientEpochContext final
    {
        RuntimeClientEpoch Epoch{};
        Microsoft::Terminal::TaskRuntimeProtocol::HelloResponse Hello;
        std::shared_ptr<IRuntimeControlConnection> Connection;
        RuntimeClientEvent Cancellation{ true, false };
        RuntimeClientEvent RetireRequested{ true, false };
        RuntimeClientEvent WorkersFinished{ true, true };
        RuntimeClientEvent CancelFinished{ true, true };
        RuntimeClientRetireCause RetireCause{ RuntimeClientRetireCause::None };
        RuntimeTransportStatus FailureStatus{ RuntimeTransportStatus::Succeeded };
        DWORD SystemError{};
        std::string Diagnostic;
        SRWLOCK LifecycleLock = SRWLOCK_INIT;
        SRWLOCK WorkerLock = SRWLOCK_INIT;
        uint32_t WorkerCount{};
        bool CancellationIssued{};
        bool CloseStarted{};

        RuntimeClientEpochContext(
            Microsoft::Terminal::TaskRuntimeProtocol::HelloResponse hello,
            std::unique_ptr<IRuntimeControlConnection> connection);
        ~RuntimeClientEpochContext();

        RuntimeClientEpochContext(const RuntimeClientEpochContext&) = delete;
        RuntimeClientEpochContext& operator=(const RuntimeClientEpochContext&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        void WorkerStarted() noexcept;
        void WorkerStopped() noexcept;
        void Cancel() noexcept;
        void Close() noexcept;
    };

    class RuntimeClientEpochWorkers final
    {
    public:
        explicit RuntimeClientEpochWorkers(
            std::shared_ptr<RuntimeClientEpochContext> epoch) noexcept;
        ~RuntimeClientEpochWorkers();

        RuntimeClientEpochWorkers(const RuntimeClientEpochWorkers&) = delete;
        RuntimeClientEpochWorkers& operator=(const RuntimeClientEpochWorkers&) = delete;

        void JoinAndClose() noexcept;

        std::unique_ptr<std::thread> Reader;
        std::unique_ptr<std::thread> Writer;

    private:
        std::shared_ptr<RuntimeClientEpochContext> _epoch;
        bool _finished{};
    };

    enum class RuntimeClientLogicalPhase
    {
        AttemptQueued,
        RetryPending,
    };

    enum class RuntimeClientLogicalOperationKind
    {
        UserRequest,
        SubscriptionSnapshot,
        SubscriptionUnsubscribe,
    };

    struct RuntimeClientLogicalOperation
    {
        RuntimeClientOperationId OperationId{};
        RuntimeClientLogicalOperationKind Kind{ RuntimeClientLogicalOperationKind::UserRequest };
        RuntimeClientSubscriptionTarget Subscription;
        Microsoft::Terminal::TaskRuntimeProtocol::ControlMethod Method{
            Microsoft::Terminal::TaskRuntimeProtocol::ControlMethod::Ping
        };
        std::string PayloadJson{ "{}" };
        size_t WireBytes{};
        RuntimeClientAttempt CurrentAttempt{ 1 };
        RuntimeClientAttempt MaximumAttempts{ 1 };
        std::string CurrentRequestId;
        RuntimeClientLogicalPhase Phase{ RuntimeClientLogicalPhase::AttemptQueued };
        std::shared_ptr<std::promise<RuntimeClientRequestResult>> Promise;
    };

    struct RuntimeClientSubscriptionDescriptor
    {
        RuntimeClientSubscriptionRevision Revision{};
        std::string ParamsJson{ "{}" };
        std::shared_ptr<RuntimeClientSubscriptionStreamProducer> Producer;
        uint32_t SnapshotAttempts{};
    };

    enum class RuntimeClientSubscriptionWriteResult
    {
        NoWork,
        Sent,
        RetireEpoch,
        Stop,
    };

    enum class RuntimeClientBackoffWaitResult
    {
        Elapsed,
        StopRequested,
        Failed,
    };

    struct RuntimeClientPendingSubscriptionDelivery
    {
        std::shared_ptr<RuntimeClientSubscriptionStreamProducer> Producer;
        RuntimeClientSubscriptionEvent Event;
        bool Close{};
    };

    struct RuntimeClientWriterItem
    {
        RuntimeClientOperationId OperationId{};
        RuntimeClientAttempt Attempt{};
        std::string RequestId;
    };

    struct RuntimeClientPendingCompletion
    {
        std::shared_ptr<std::promise<RuntimeClientRequestResult>> Promise;
        RuntimeClientRequestResult Result;
    };

    struct RuntimeClientRetryWork
    {
        RuntimeClientOperationId OperationId{};
        RuntimeClientAttempt NextAttempt{};
    };

    static_assert(std::is_nothrow_move_constructible_v<RuntimeClientPendingCompletion>);
    static_assert(std::is_nothrow_move_constructible_v<RuntimeClientRetryWork>);
    static_assert(std::is_nothrow_move_constructible_v<RuntimeClientRetiredRequest>);
    static_assert(std::is_nothrow_move_constructible_v<RuntimeClientPendingSubscriptionDelivery>);

    struct RuntimeClientSharedState final : std::enable_shared_from_this<RuntimeClientSharedState>
    {
        TaskRuntimeClientConfiguration Configuration;
        TaskRuntimeClientDependencies Dependencies;
        bool IsValid{};

        mutable std::mutex Mutex;
        std::condition_variable WriterWake;
        TaskRuntimeClientState ClientState{ TaskRuntimeClientState::Stopped };
        bool AdmissionOpen{};
        bool StartedOnce{};
        RuntimeClientEvent StopEvent{ true, false };
        std::mutex JoinMutex;
        std::unique_ptr<std::thread> Supervisor;
        std::thread::id SupervisorThreadId;
        std::thread::id ReaderThreadId;
        std::thread::id WriterThreadId;

        RuntimeClientEpochState Epochs;
        std::shared_ptr<RuntimeClientEpochContext> Active;
        RuntimeClientRequestRegistry Requests;
        RuntimeClientSubscriptionRegistry Subscriptions;
        std::shared_ptr<RuntimeClientSubscriptionAggregateBudget> SubscriptionBudget;
        std::unordered_map<RuntimeClientOperationId, RuntimeClientLogicalOperation> Operations;
        std::unordered_map<RuntimeClientLogicalSubscriptionId, RuntimeClientSubscriptionDescriptor>
            SubscriptionDescriptors;
        std::unordered_set<std::string> IssuedRequestIds;
        std::deque<RuntimeClientWriterItem> WriterQueue;
        std::vector<RuntimeClientPendingCompletion> CompletionBuffer;
        std::vector<RuntimeClientRetryWork> RetryBuffer;
        std::vector<RuntimeClientRetiredRequest> RetiredBuffer;
        std::vector<RuntimeClientPendingSubscriptionDelivery> SubscriptionDeliveryBuffer;
        size_t QueuedBytes{};
        size_t UserQueuedBytes{};
        size_t UserOperationCount{};
        size_t SubscriptionOperationCount{};
        RuntimeClientOperationId NextOperationId{ 1 };
        RuntimeClientLogicalSubscriptionId NextSubscriptionId{ 1 };
        uint32_t ConsecutiveFailures{};
        std::string CurrentRuntimeInstanceId;

        explicit RuntimeClientSharedState(
            TaskRuntimeClientConfiguration configuration,
            TaskRuntimeClientDependencies dependencies);
        ~RuntimeClientSharedState();

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] bool Start();
        [[nodiscard]] RuntimeClientRequestHandle Submit(
            Microsoft::Terminal::TaskRuntimeProtocol::ControlMethod method,
            std::string payloadJson,
            uint32_t maximumAttempts);
        [[nodiscard]] RuntimeClientSubscriptionHandle Subscribe(RuntimeClientSubscribeOptions options);
        void RequestSubscriptionRemove(
            RuntimeClientLogicalSubscriptionId logicalId,
            RuntimeClientSubscriptionRevision revision) noexcept;
        void RequestStop() noexcept;
        [[nodiscard]] bool WaitForStopped() noexcept;
        [[nodiscard]] TaskRuntimeClientState State() const noexcept;
        [[nodiscard]] RuntimeClientEpoch ActiveEpoch() const noexcept;
        [[nodiscard]] std::string RuntimeInstanceId() const;

        void RunSupervisor() noexcept;
        void RunReader(std::shared_ptr<RuntimeClientEpochContext> epoch) noexcept;
        void RunWriter(std::shared_ptr<RuntimeClientEpochContext> epoch) noexcept;
        void RequestEpochRetire(
            const std::shared_ptr<RuntimeClientEpochContext>& epoch,
            RuntimeClientRetireCause cause,
            RuntimeTransportStatus transportStatus,
            DWORD systemError,
            std::string_view diagnostic) noexcept;
        [[nodiscard]] RuntimeClientBackoffWaitResult WaitForReconnectBackoff(
            uint32_t failureCount) noexcept;
        [[nodiscard]] bool HasSubscriptionWriterWorkLocked(RuntimeClientEpoch epoch) const noexcept;
        [[nodiscard]] RuntimeClientSubscriptionWriteResult RunSubscriptionWriter(
            const std::shared_ptr<RuntimeClientEpochContext>& epoch) noexcept;
        [[nodiscard]] RuntimeClientSubscriptionWriteResult RejectOversizedSubscriptionRequest(
            const std::shared_ptr<RuntimeClientEpochContext>& epoch,
            RuntimeClientSubscriptionTarget target) noexcept;
        [[nodiscard]] bool HandleSubscriptionResponse(
            const std::shared_ptr<RuntimeClientEpochContext>& epoch,
            Microsoft::Terminal::TaskRuntimeProtocol::ControlResponse& response) noexcept;
        [[nodiscard]] bool HandleSubscriptionNotification(
            const std::shared_ptr<RuntimeClientEpochContext>& epoch,
            Microsoft::Terminal::TaskRuntimeProtocol::ControlNotification notification) noexcept;

        [[nodiscard]] std::optional<RuntimeClientPendingCompletion> CompleteOperationLocked(
            RuntimeClientOperationId operationId,
            RuntimeClientRequestResult result);
        void EraseSubscriptionOperationLocked(RuntimeClientOperationId operationId) noexcept;
        void RetireSubscriptionsLocked(
            RuntimeClientEpoch epoch,
            bool stopping,
            std::vector<RuntimeClientPendingSubscriptionDelivery>& deliveries) noexcept;
        void FailSubscriptionsLocked(
            RuntimeClientSubscriptionEventKind kind,
            RuntimeTransportStatus transportStatus,
            DWORD systemError,
            std::string_view diagnostic,
            std::vector<RuntimeClientPendingSubscriptionDelivery>& deliveries) noexcept;
        void ProcessRetiredLocked(
            std::vector<RuntimeClientRetiredRequest>& retired,
            bool stopping,
            bool protocolFault,
            RuntimeTransportStatus transportStatus,
            DWORD systemError,
            std::string_view diagnostic,
            std::vector<RuntimeClientRetryWork>& retries,
            std::vector<RuntimeClientPendingCompletion>& completions);
        void QueueRetries(
            std::vector<RuntimeClientRetryWork>& retries,
            std::vector<RuntimeClientPendingCompletion>& completions) noexcept;
        void FinishStopped(std::vector<RuntimeClientPendingCompletion>& completions) noexcept;
        void FailAllLocked(
            RuntimeClientRequestOutcome outcome,
            RuntimeTransportStatus transportStatus,
            DWORD systemError,
            std::string_view diagnostic,
            std::vector<RuntimeClientPendingCompletion>& completions);

        static void Deliver(std::vector<RuntimeClientPendingCompletion>& completions) noexcept;
        static void DeliverSubscriptions(
            std::vector<RuntimeClientPendingSubscriptionDelivery>& deliveries) noexcept;
    };
}
