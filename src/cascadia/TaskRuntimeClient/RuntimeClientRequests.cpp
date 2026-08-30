// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientRequests.h"

#include "ProtocolRpc.h"
#include "ProtocolValidation.h"

#include <algorithm>
#include <deque>
#include <type_traits>
#include <unordered_map>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    static_assert(std::is_nothrow_move_constructible_v<RuntimeClientRetiredRequest>);

    namespace
    {
        constexpr size_t AbsoluteMaximumActiveRequests{ 4096 };
        constexpr size_t AbsoluteMaximumRequestTombstones{ 65536 };

        struct Tombstone
        {
            std::string RequestId;
            RuntimeClientEpoch Epoch{};
            RuntimeClientRequestTombstoneKind Kind{
                RuntimeClientRequestTombstoneKind::CompletedDuplicateGuard
            };
        };

        bool _validConfiguration(const RuntimeClientRequestRegistryConfiguration& configuration) noexcept
        {
            return configuration.MaximumActiveRequests != 0 &&
                   configuration.MaximumActiveRequests <= AbsoluteMaximumActiveRequests &&
                   configuration.MaximumTombstones != 0 &&
                   configuration.MaximumTombstones <= AbsoluteMaximumRequestTombstones;
        }

        bool _validAttempt(const RuntimeClientRequestAttemptInfo& attempt) noexcept
        {
            return attempt.OperationId != 0 && attempt.Attempt != 0 && attempt.Epoch == 0 &&
                   attempt.Transmission == ClientRequestTransmission::NotSent &&
                   IsCanonicalProtocolUuid(attempt.RequestId) &&
                   !ControlMethodName(attempt.Method).empty();
        }
    }

    struct RuntimeClientRequestRegistry::Impl
    {
        RuntimeClientRequestRegistryConfiguration Configuration;
        bool IsValid{};
        std::unordered_map<std::string, RuntimeClientRequestAttemptInfo> ActiveByRequestId;
        std::unordered_map<RuntimeClientOperationId, std::string> RequestIdByOperation;
        std::unordered_map<std::string, Tombstone> TombstonesByRequestId;
        std::deque<Tombstone> TombstoneOrder;

        explicit Impl(const RuntimeClientRequestRegistryConfiguration configuration) :
            Configuration{ configuration },
            IsValid{ _validConfiguration(configuration) }
        {
            if (IsValid)
            {
                ActiveByRequestId.reserve(Configuration.MaximumActiveRequests);
                RequestIdByOperation.reserve(Configuration.MaximumActiveRequests);
                TombstonesByRequestId.reserve(Configuration.MaximumTombstones);
            }
        }

        bool AddTombstone(
            const RuntimeClientRequestAttemptInfo& attempt,
            const RuntimeClientRequestTombstoneKind kind) noexcept
        {
            try
            {
                Tombstone tombstone{ attempt.RequestId, attempt.Epoch, kind };
                TombstoneOrder.emplace_back(std::move(tombstone));
                try
                {
                    const auto& insertedTombstone = TombstoneOrder.back();
                    const auto inserted = TombstonesByRequestId.emplace(
                                              insertedTombstone.RequestId,
                                              insertedTombstone)
                                              .second;
                    if (!inserted)
                    {
                        TombstoneOrder.pop_back();
                        return false;
                    }
                }
                catch (...)
                {
                    TombstoneOrder.pop_back();
                    return false;
                }

                while (TombstoneOrder.size() > Configuration.MaximumTombstones)
                {
                    TombstonesByRequestId.erase(TombstoneOrder.front().RequestId);
                    TombstoneOrder.pop_front();
                }
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] RuntimeClientResponseMatch MissingResponseMatch(
            const std::string& requestId,
            const RuntimeClientEpoch receivedEpoch) const
        {
            const auto retired = TombstonesByRequestId.find(requestId);
            return retired != TombstonesByRequestId.end() &&
                           retired->second.Epoch == receivedEpoch ?
                       RuntimeClientResponseMatch::DuplicateOrLate :
                       RuntimeClientResponseMatch::UnknownRequest;
        }
    };

    RuntimeClientRequestRegistry::RuntimeClientRequestRegistry(
        const RuntimeClientRequestRegistryConfiguration configuration) :
        _impl{ std::make_unique<Impl>(configuration) }
    {
    }

    RuntimeClientRequestRegistry::~RuntimeClientRequestRegistry() = default;

    bool RuntimeClientRequestRegistry::Valid() const noexcept
    {
        return _impl && _impl->IsValid;
    }

    RuntimeClientRequestAddStatus RuntimeClientRequestRegistry::Add(
        RuntimeClientRequestAttemptInfo attempt)
    {
        if (!Valid() || !_validAttempt(attempt))
        {
            return RuntimeClientRequestAddStatus::Invalid;
        }
        if (_impl->ActiveByRequestId.size() >= _impl->Configuration.MaximumActiveRequests)
        {
            return RuntimeClientRequestAddStatus::CapacityExceeded;
        }
        if (_impl->ActiveByRequestId.contains(attempt.RequestId) ||
            _impl->TombstonesByRequestId.contains(attempt.RequestId))
        {
            return RuntimeClientRequestAddStatus::DuplicateRequestId;
        }
        if (_impl->RequestIdByOperation.contains(attempt.OperationId))
        {
            return RuntimeClientRequestAddStatus::OperationAlreadyActive;
        }

        const auto operationId = attempt.OperationId;
        const auto requestId = attempt.RequestId;
        const auto [operation, operationInserted] =
            _impl->RequestIdByOperation.emplace(operationId, requestId);
        if (!operationInserted)
        {
            return RuntimeClientRequestAddStatus::OperationAlreadyActive;
        }
        try
        {
            const auto [request, requestInserted] =
                _impl->ActiveByRequestId.emplace(requestId, std::move(attempt));
            if (!requestInserted)
            {
                _impl->RequestIdByOperation.erase(operation);
                return RuntimeClientRequestAddStatus::DuplicateRequestId;
            }
        }
        catch (...)
        {
            _impl->RequestIdByOperation.erase(operation);
            throw;
        }
        return RuntimeClientRequestAddStatus::Added;
    }

    bool RuntimeClientRequestRegistry::BeginWrite(
        const std::string_view requestId,
        const RuntimeClientEpoch epoch)
    {
        if (!Valid() || epoch == 0)
        {
            return false;
        }
        const auto found = _impl->ActiveByRequestId.find(std::string{ requestId });
        if (found == _impl->ActiveByRequestId.end() || found->second.Epoch != 0 ||
            found->second.Transmission != ClientRequestTransmission::NotSent)
        {
            return false;
        }
        found->second.Epoch = epoch;
        found->second.Transmission = ClientRequestTransmission::WriteStarted;
        return true;
    }

    bool RuntimeClientRequestRegistry::MarkWriteCompleted(
        const std::string_view requestId,
        const RuntimeClientEpoch epoch)
    {
        if (!Valid() || epoch == 0)
        {
            return false;
        }
        const auto found = _impl->ActiveByRequestId.find(std::string{ requestId });
        if (found == _impl->ActiveByRequestId.end() || found->second.Epoch != epoch ||
            found->second.Transmission != ClientRequestTransmission::WriteStarted)
        {
            return false;
        }
        found->second.Transmission = ClientRequestTransmission::WriteCompleted;
        return true;
    }

    RuntimeClientCorrelatedResponse RuntimeClientRequestRegistry::CompleteResponse(
        const RuntimeClientEpoch receivedEpoch,
        const RuntimeClientEpoch activeEpoch,
        const std::string_view requestId)
    {
        RuntimeClientCorrelatedResponse result;
        if (!Valid() || receivedEpoch == 0 || activeEpoch == 0 || requestId.empty())
        {
            result.Status = RuntimeClientResponseMatch::Invalid;
            return result;
        }
        if (receivedEpoch != activeEpoch)
        {
            result.Status = RuntimeClientResponseMatch::StaleEpoch;
            return result;
        }

        const std::string key{ requestId };
        const auto found = _impl->ActiveByRequestId.find(key);
        if (found == _impl->ActiveByRequestId.end())
        {
            result.Status = _impl->MissingResponseMatch(key, receivedEpoch);
            return result;
        }
        if (found->second.Epoch != receivedEpoch ||
            found->second.Transmission == ClientRequestTransmission::NotSent)
        {
            result.Status = RuntimeClientResponseMatch::UnknownRequest;
            return result;
        }

        result.Attempt = found->second;
        _impl->AddTombstone(
            found->second,
            RuntimeClientRequestTombstoneKind::CompletedDuplicateGuard);
        _impl->RequestIdByOperation.erase(found->second.OperationId);
        _impl->ActiveByRequestId.erase(found);
        result.Status = RuntimeClientResponseMatch::Matched;
        return result;
    }

    std::vector<RuntimeClientRetiredRequest> RuntimeClientRequestRegistry::RetireEpoch(
        const RuntimeClientEpoch epoch)
    {
        std::vector<RuntimeClientRetiredRequest> retired;
        if (!Valid() || epoch == 0)
        {
            return retired;
        }
        try
        {
            retired.reserve(_impl->ActiveByRequestId.size());
            if (!RetireEpochInto(epoch, retired))
            {
                retired.clear();
            }
        }
        catch (...)
        {
            retired.clear();
        }
        return retired;
    }

    bool RuntimeClientRequestRegistry::RetireEpochInto(
        const RuntimeClientEpoch epoch,
        std::vector<RuntimeClientRetiredRequest>& retired) noexcept
    {
        retired.clear();
        if (!Valid() || epoch == 0)
        {
            return false;
        }

        const auto matching = static_cast<size_t>(std::ranges::count_if(
            _impl->ActiveByRequestId,
            [epoch](const auto& value) { return value.second.Epoch == epoch; }));
        if (retired.capacity() < matching)
        {
            return false;
        }

        auto current = _impl->ActiveByRequestId.begin();
        while (current != _impl->ActiveByRequestId.end())
        {
            auto& attempt = current->second;
            if (attempt.Epoch != epoch)
            {
                ++current;
                continue;
            }

            const auto operationId = attempt.OperationId;
            const auto action = EvaluateClientDisconnect(attempt.Method, attempt.Transmission);
            _impl->AddTombstone(
                attempt,
                RuntimeClientRequestTombstoneKind::RetiredEpoch);
            retired.emplace_back(RuntimeClientRetiredRequest{
                std::move(attempt),
                action,
            });
            _impl->RequestIdByOperation.erase(operationId);
            current = _impl->ActiveByRequestId.erase(current);
        }

        std::ranges::sort(retired, {}, [](const auto& value) {
            return std::pair{ value.Attempt.OperationId, value.Attempt.Attempt };
        });
        return true;
    }

    std::optional<RuntimeClientRequestAttemptInfo> RuntimeClientRequestRegistry::CancelQueued(
        const RuntimeClientOperationId operationId)
    {
        if (!Valid() || operationId == 0)
        {
            return std::nullopt;
        }
        const auto operation = _impl->RequestIdByOperation.find(operationId);
        if (operation == _impl->RequestIdByOperation.end())
        {
            return std::nullopt;
        }
        const auto request = _impl->ActiveByRequestId.find(operation->second);
        if (request == _impl->ActiveByRequestId.end() || request->second.Epoch != 0 ||
            request->second.Transmission != ClientRequestTransmission::NotSent)
        {
            return std::nullopt;
        }

        auto cancelled = request->second;
        _impl->AddTombstone(
            cancelled,
            RuntimeClientRequestTombstoneKind::CancelledBeforeWrite);
        _impl->ActiveByRequestId.erase(request);
        _impl->RequestIdByOperation.erase(operation);
        return cancelled;
    }

    std::vector<RuntimeClientRequestAttemptInfo> RuntimeClientRequestRegistry::DrainQueuedForStop()
    {
        std::vector<RuntimeClientRequestAttemptInfo> queued;
        if (!Valid())
        {
            return queued;
        }
        for (const auto& [requestId, attempt] : _impl->ActiveByRequestId)
        {
            if (attempt.Epoch == 0 &&
                attempt.Transmission == ClientRequestTransmission::NotSent)
            {
                queued.emplace_back(attempt);
            }
        }
        std::ranges::sort(queued, {}, [](const auto& value) {
            return std::pair{ value.OperationId, value.Attempt };
        });
        for (const auto& attempt : queued)
        {
            _impl->AddTombstone(
                attempt,
                RuntimeClientRequestTombstoneKind::CancelledBeforeWrite);
            _impl->RequestIdByOperation.erase(attempt.OperationId);
            _impl->ActiveByRequestId.erase(attempt.RequestId);
        }
        return queued;
    }

    void RuntimeClientRequestRegistry::ClearRetiredEpochTombstones(
        const RuntimeClientEpoch epoch) noexcept
    {
        if (!Valid() || epoch == 0)
        {
            return;
        }
        auto current = _impl->TombstoneOrder.begin();
        while (current != _impl->TombstoneOrder.end())
        {
            if (current->Epoch == epoch)
            {
                _impl->TombstonesByRequestId.erase(current->RequestId);
                current = _impl->TombstoneOrder.erase(current);
            }
            else
            {
                ++current;
            }
        }
    }

    size_t RuntimeClientRequestRegistry::ActiveCount() const noexcept
    {
        return Valid() ? _impl->ActiveByRequestId.size() : 0;
    }

    size_t RuntimeClientRequestRegistry::QueuedCount() const noexcept
    {
        if (!Valid())
        {
            return 0;
        }
        return std::ranges::count_if(_impl->ActiveByRequestId, [](const auto& value) {
            return value.second.Epoch == 0 &&
                   value.second.Transmission == ClientRequestTransmission::NotSent;
        });
    }

    size_t RuntimeClientRequestRegistry::TombstoneCount() const noexcept
    {
        return Valid() ? _impl->TombstonesByRequestId.size() : 0;
    }
}
