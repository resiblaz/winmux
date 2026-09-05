// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskTerminalPipeServer.h"

#include "RuntimePipe.h"
#include "RuntimeSecurity.h"
#include "RuntimeTerminalChannel.h"
#include "RuntimeTerminalHandshake.h"
#include "RuntimeTerminalSession.h"

#include "ProtocolCapabilities.h"
#include "TerminalFrame.h"

#include <atomic>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <wil/resource.h>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        constexpr uint32_t AcceptPollMilliseconds{ 250 };
        constexpr uint32_t HandshakeMilliseconds{ 5000 };
        constexpr uint32_t IoPollMilliseconds{ 50 };
        constexpr uint32_t GracefulCloseMilliseconds{ 750 };
        constexpr uint64_t SurfaceReservationMilliseconds{ 10'000 };

        RuntimeHandshakeConfiguration _handshake(
            const RuntimeIdentity& identity,
            const std::string& runtimeInstanceId)
        {
            RuntimeHandshakeConfiguration result;
            result.RuntimeIdentity = identity.ProtocolIdentity;
            result.RuntimeInstanceId = runtimeInstanceId;
            result.Capabilities =
                static_cast<uint64_t>(ProtocolCapability::TerminalStream) |
                static_cast<uint64_t>(ProtocolCapability::InputControllerLease) |
                static_cast<uint64_t>(ProtocolCapability::TaskTerminalSurfaces);
            return result;
        }

        std::optional<uint64_t> _acknowledged(const TerminalFrame& frame) noexcept
        {
            if (frame.Kind != TerminalFrameKind::Attach || frame.Generation == 0)
                return std::nullopt;
            if (frame.Payload.empty())
                return uint64_t{};
            uint64_t value{};
            const auto text = std::string_view{
                reinterpret_cast<const char*>(frame.Payload.data()), frame.Payload.size()
            };
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ?
                       std::optional<uint64_t>{ value } :
                       std::nullopt;
        }
    }

    struct RuntimeTaskTerminalPipeServer::Impl
    {
        struct Worker
        {
            std::thread Thread;
            std::shared_ptr<std::atomic_bool> Finished;
        };

        RuntimeIdentity Identity;
        std::string RuntimeInstanceId;
        std::string TaskId;
        RuntimeTerminalEndpoint TerminalEndpoint;
        std::shared_ptr<RuntimeTaskTerminalBackend> Backend;
        RuntimeTerminalSession Session;
        wil::unique_handle StopEvent;
        std::thread Acceptor;
        mutable std::mutex Mutex;
        std::condition_variable Changed;
        std::vector<Worker> Workers;
        std::unordered_set<std::string> AttachedClients;
        std::string SurfaceOwner;
        uint64_t SurfaceClaimExpiresAt{};
        bool Stopping{};

        Impl(
            RuntimeIdentity identity,
            std::string runtimeInstanceId,
            std::string taskId,
            RuntimeTerminalEndpoint endpoint,
            std::shared_ptr<RuntimeTaskTerminalBackend> backend) :
            Identity{ std::move(identity) },
            RuntimeInstanceId{ std::move(runtimeInstanceId) },
            TaskId{ std::move(taskId) },
            TerminalEndpoint{ std::move(endpoint) },
            Backend{ std::move(backend) },
            Session{ [&] {
                        RuntimeTerminalSessionConfiguration configuration;
                        configuration.Generation = TerminalEndpoint.Generation;
                        configuration.InitialServerSequence = 1;
                        return configuration;
                    }(),
                     Backend },
            StopEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }
        {
            Workers.reserve(DefaultRuntimeTerminalMaximumClients);
        }

        [[nodiscard]] bool Valid() const noexcept
        {
            return ValidateRuntimeTerminalEndpoint(TerminalEndpoint) && Backend &&
                   Session.Valid() && static_cast<bool>(StopEvent);
        }

        [[nodiscard]] bool ClaimSurface(const std::string_view clientId)
        {
            const std::scoped_lock lock{ Mutex };
            const auto now = GetTickCount64();
            const auto ownerAttached = AttachedClients.contains(SurfaceOwner);
            if (SurfaceOwner.empty() || SurfaceOwner == clientId ||
                (!ownerAttached && now >= SurfaceClaimExpiresAt))
            {
                SurfaceOwner.assign(clientId);
                SurfaceClaimExpiresAt = now + SurfaceReservationMilliseconds;
                return true;
            }
            return false;
        }

        void MarkAttached(const std::string_view clientId) noexcept
        try
        {
            const std::scoped_lock lock{ Mutex };
            AttachedClients.emplace(clientId);
            if (SurfaceOwner == clientId)
            {
                SurfaceClaimExpiresAt = (std::numeric_limits<uint64_t>::max)();
            }
            Changed.notify_all();
        }
        catch (...)
        {
        }

        void MarkDetached(const std::string_view clientId) noexcept
        try
        {
            const std::scoped_lock lock{ Mutex };
            AttachedClients.erase(std::string{ clientId });
            if (SurfaceOwner == clientId)
            {
                SurfaceOwner.clear();
                SurfaceClaimExpiresAt = 0;
            }
            Changed.notify_all();
        }
        catch (...)
        {
        }

        void PruneWorkers()
        {
            std::vector<std::thread> completed;
            {
                const std::scoped_lock lock{ Mutex };
                for (auto iterator = Workers.begin(); iterator != Workers.end();)
                {
                    if (iterator->Finished->load(std::memory_order_acquire))
                    {
                        completed.emplace_back(std::move(iterator->Thread));
                        iterator = Workers.erase(iterator);
                    }
                    else
                    {
                        ++iterator;
                    }
                }
            }
            for (auto& thread : completed)
                if (thread.joinable())
                    thread.join();
        }

        void RunAcceptor() noexcept
        {
            auto security = CreateRuntimeSecurityAttributes(Identity.UserSid);
            if (!security.Succeeded())
                return;
            bool firstInstance{ true };
            while (WaitForSingleObject(StopEvent.get(), 0) == WAIT_TIMEOUT)
            {
                PruneWorkers();
                auto listener = CreateRuntimeTerminalPipe(
                    TerminalEndpoint, security.Security.Attributes(), firstInstance);
                firstInstance = false;
                if (!listener.Created())
                {
                    if (WaitForSingleObject(StopEvent.get(), AcceptPollMilliseconds) == WAIT_OBJECT_0)
                        break;
                    continue;
                }
                auto accepted = AcceptRuntimeTerminalChannel(
                    listener.Pipe, TerminalEndpoint, MaximumTerminalFrameBytes, AcceptPollMilliseconds, StopEvent.get());
                if (!accepted.Succeeded())
                {
                    if (accepted.Status == RuntimeTransportStatus::Cancelled)
                        break;
                    continue;
                }
                auto handshake = PerformRuntimeTerminalServerHandshake(
                    accepted.Channel, _handshake(Identity, RuntimeInstanceId), HandshakeMilliseconds, StopEvent.get());
                if (!handshake.Succeeded() || handshake.Request.ClientKind != ProtocolClientKind::TerminalUi)
                {
                    accepted.Channel.Close();
                    continue;
                }
                std::thread thread;
                try
                {
                    auto finished = std::make_shared<std::atomic_bool>(false);
                    thread = std::thread{
                        [this,
                         clientId = std::move(handshake.Request.ClientInstanceId),
                         channel = std::move(accepted.Channel),
                         finished]() mutable {
                            RunClient(std::move(clientId), std::move(channel));
                            finished->store(true, std::memory_order_release);
                            Changed.notify_all();
                        }
                    };
                    const std::scoped_lock lock{ Mutex };
                    Workers.push_back({ std::move(thread), std::move(finished) });
                }
                catch (...)
                {
                    SetEvent(StopEvent.get());
                    if (thread.joinable())
                        thread.join();
                    accepted.Channel.Close();
                    break;
                }
            }
            PruneWorkers();
        }

        void RunClient(std::string clientId, RuntimeTerminalChannel channel) noexcept
        {
            bool attached{};
            const auto cleanup = wil::scope_exit([&]() noexcept {
                if (attached)
                {
                    const std::scoped_lock lock{ Mutex };
                    static_cast<void>(Session.Detach(clientId, GetTickCount64()));
                }
                MarkDetached(clientId);
                channel.Close();
            });
            const auto attachFrame = channel.Receive(HandshakeMilliseconds, StopEvent.get());
            const auto acknowledged = attachFrame.Succeeded() ? _acknowledged(attachFrame.Frame) : std::nullopt;
            if (!acknowledged || attachFrame.Frame.Generation != TerminalEndpoint.Generation)
                return;
            {
                const std::scoped_lock lock{ Mutex };
                const auto result = Session.Attach(
                    clientId, TerminalEndpoint.Generation, *acknowledged, GetTickCount64());
                if (result.Status != RuntimeTerminalSessionStatus::Attached &&
                    result.Status != RuntimeTerminalSessionStatus::ReplayQueued &&
                    result.Status != RuntimeTerminalSessionStatus::ResetQueued)
                    return;
                attached = true;
                static_cast<void>(Session.AcquireInputLease(clientId, GetTickCount64()));
            }
            MarkAttached(clientId);

            while (WaitForSingleObject(StopEvent.get(), 0) == WAIT_TIMEOUT)
            {
                std::optional<TerminalFrame> outbound;
                {
                    const std::scoped_lock lock{ Mutex };
                    auto pulled = Session.Pull(clientId);
                    if (pulled.Status == RuntimeTerminalSessionStatus::FramePulled)
                        outbound = std::move(pulled.Frame);
                    else if (pulled.Status == RuntimeTerminalSessionStatus::NotAttached)
                        break;
                }
                if (outbound)
                {
                    const auto end = (outbound->Flags & static_cast<uint32_t>(TerminalFrameFlag::EndOfStream)) != 0;
                    if (!channel.Send(*outbound, HandshakeMilliseconds, StopEvent.get()).Succeeded() || end)
                        break;
                    continue;
                }
                auto received = channel.Receive(IoPollMilliseconds, StopEvent.get());
                if (received.Status == RuntimeTransportStatus::Timeout)
                    continue;
                if (!received.Succeeded())
                    break;
                RuntimeTerminalSessionResult prepared;
                {
                    const std::scoped_lock lock{ Mutex };
                    std::optional<RuntimeTerminalLeaseRevision> leaseRevision;
                    if (received.Frame.Kind == TerminalFrameKind::Input || received.Frame.Kind == TerminalFrameKind::Resize)
                    {
                        const auto acquired = Session.AcquireInputLease(clientId, GetTickCount64());
                        if (acquired.Lease && acquired.Lease->OwnerClientInstanceId == clientId)
                        {
                            leaseRevision = acquired.Lease->LeaseRevision;
                        }
                    }
                    prepared = Session.PrepareFrame(
                        clientId, received.Frame, GetTickCount64(), leaseRevision);
                }
                // Provider callbacks can publish output back to this session.
                // Never invoke them while holding Mutex.
                const auto handled = Session.DispatchFrame(received.Frame, std::move(prepared));
                if (handled.Status == RuntimeTerminalSessionStatus::InvalidFrame ||
                    handled.Status == RuntimeTerminalSessionStatus::StaleGeneration ||
                    handled.Status == RuntimeTerminalSessionStatus::ResourceFailure)
                    break;
            }
        }

        RuntimeTaskTerminalPipeResult Publish(std::span<const uint8_t> output, const uint32_t flags)
        {
            RuntimeTaskTerminalPipeResult result;
            std::vector<uint8_t> payload{ output.begin(), output.end() };
            const std::scoped_lock lock{ Mutex };
            if (Stopping)
            {
                result.Error = "Task terminal is stopping";
                return result;
            }
            if (!payload.empty())
                Backend->AppendOutput(payload);
            const auto published = Session.PublishOutput(std::move(payload), flags, GetTickCount64());
            if (published.Status != RuntimeTerminalSessionStatus::OutputPublished &&
                published.Status != RuntimeTerminalSessionStatus::OutputPublishedWithSlowPeerDisconnect)
            {
                result.Error = "Task terminal output could not be published";
                return result;
            }
            result.Succeeded = true;
            result.ClientCount = Session.ClientCount();
            Changed.notify_all();
            return result;
        }

        void Stop(const bool graceful) noexcept
        {
            {
                std::unique_lock lock{ Mutex };
                if (Stopping)
                    return;
                if (graceful)
                {
                    const auto closed = Session.PublishOutput(
                        {}, static_cast<uint32_t>(TerminalFrameFlag::EndOfStream), GetTickCount64());
                    if (closed.Status == RuntimeTerminalSessionStatus::OutputPublished ||
                        closed.Status == RuntimeTerminalSessionStatus::OutputPublishedWithSlowPeerDisconnect)
                    {
                        Changed.notify_all();
                        Changed.wait_for(lock, std::chrono::milliseconds{ GracefulCloseMilliseconds }, [&] {
                            return Session.ClientCount() == 0;
                        });
                    }
                }
                Stopping = true;
            }
            if (StopEvent)
                SetEvent(StopEvent.get());
            Changed.notify_all();
            if (Acceptor.joinable() && Acceptor.get_id() != std::this_thread::get_id())
                Acceptor.join();
            std::vector<std::thread> workers;
            {
                const std::scoped_lock lock{ Mutex };
                for (auto& worker : Workers)
                    workers.emplace_back(std::move(worker.Thread));
                Workers.clear();
            }
            for (auto& worker : workers)
            {
                if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
                    worker.join();
            }
        }
    };

    RuntimeTaskTerminalPipeServer::RuntimeTaskTerminalPipeServer(
        RuntimeIdentity identity,
        std::string runtimeInstanceId,
        std::string taskId,
        RuntimeTerminalEndpoint endpoint,
        std::shared_ptr<RuntimeTaskTerminalBackend> backend) :
        _impl{ std::make_unique<Impl>(
            std::move(identity),
            std::move(runtimeInstanceId),
            std::move(taskId),
            std::move(endpoint),
            std::move(backend)) }
    {
    }

    RuntimeTaskTerminalPipeServer::~RuntimeTaskTerminalPipeServer()
    {
        Stop(false);
    }

    bool RuntimeTaskTerminalPipeServer::Valid() const noexcept { return _impl && _impl->Valid(); }
    bool RuntimeTaskTerminalPipeServer::ClaimSurface(const std::string_view clientId) noexcept
    try
    {
        return _impl && _impl->ClaimSurface(clientId);
    }
    catch (...)
    {
        return false;
    }
    bool RuntimeTaskTerminalPipeServer::Start() noexcept
    try
    {
        if (!Valid())
            return false;
        _impl->Acceptor = std::thread{ [impl = _impl.get()] { impl->RunAcceptor(); } };
        return true;
    }
    catch (...)
    {
        return false;
    }
    RuntimeTaskTerminalPipeResult RuntimeTaskTerminalPipeServer::Publish(
        const std::span<const uint8_t> output,
        const uint32_t flags)
    {
        return _impl ? _impl->Publish(output, flags) : RuntimeTaskTerminalPipeResult{};
    }
    size_t RuntimeTaskTerminalPipeServer::ClientCount() const noexcept
    {
        if (!_impl)
            return 0;
        const std::scoped_lock lock{ _impl->Mutex };
        return _impl->Session.ClientCount();
    }
    const RuntimeTerminalEndpoint& RuntimeTaskTerminalPipeServer::Endpoint() const noexcept
    {
        static const RuntimeTerminalEndpoint empty;
        return _impl ? _impl->TerminalEndpoint : empty;
    }
    void RuntimeTaskTerminalPipeServer::Stop(const bool graceful) noexcept
    {
        if (_impl)
            _impl->Stop(graceful);
    }
}
