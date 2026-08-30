// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalInputLease.h"

#include "ProtocolValidation.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        bool _validClient(const std::string_view clientInstanceId) noexcept
        {
            return Microsoft::Terminal::TaskRuntimeProtocol::IsCanonicalProtocolUuid(
                clientInstanceId);
        }
    }

    RuntimeTerminalInputLease::RuntimeTerminalInputLease(
        RuntimeTerminalInputLeaseConfiguration configuration) noexcept :
        _configuration{ std::move(configuration) },
        _lastIssuedRevision{ _configuration.InitialLeaseRevision }
    {
        _valid = _configuration.Generation != 0 &&
                 _configuration.LeaseDurationMilliseconds != 0 &&
                 _configuration.LeaseDurationMilliseconds <=
                     MaximumRuntimeTerminalLeaseDurationMilliseconds;
    }

    bool RuntimeTerminalInputLease::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeTerminalInputLeaseResult RuntimeTerminalInputLease::Acquire(
        const std::string_view clientInstanceId,
        const uint64_t generation,
        const RuntimeTerminalLeaseMilliseconds now)
    {
        if (!_valid)
        {
            return _result(RuntimeTerminalInputLeaseStatus::InvalidConfiguration);
        }
        if (!_validClient(clientInstanceId))
        {
            return _result(RuntimeTerminalInputLeaseStatus::InvalidClient);
        }
        if (generation != _configuration.Generation)
        {
            return _result(RuntimeTerminalInputLeaseStatus::StaleGeneration);
        }
        if (const auto time = _observeTime(now);
            time != RuntimeTerminalInputLeaseStatus::Authorized)
        {
            return _result(time);
        }
        (void)_expireIfDue(now);
        const auto expiresAt = _deadline(now);
        if (!expiresAt)
        {
            return _result(RuntimeTerminalInputLeaseStatus::ClockExhausted);
        }

        if (_current)
        {
            if (_current->OwnerClientInstanceId != clientInstanceId)
            {
                return _result(RuntimeTerminalInputLeaseStatus::Busy);
            }
            _current->ExpiresAt = *expiresAt;
            return _result(RuntimeTerminalInputLeaseStatus::Renewed);
        }
        if (_lastIssuedRevision ==
            (std::numeric_limits<RuntimeTerminalLeaseRevision>::max)())
        {
            return _result(RuntimeTerminalInputLeaseStatus::RevisionExhausted);
        }

        RuntimeTerminalInputLeaseSnapshot lease;
        try
        {
            lease.OwnerClientInstanceId = clientInstanceId;
        }
        catch (...)
        {
            return _result(RuntimeTerminalInputLeaseStatus::ResourceFailure);
        }
        lease.Generation = _configuration.Generation;
        lease.LeaseRevision = ++_lastIssuedRevision;
        lease.ExpiresAt = *expiresAt;
        _current = std::move(lease);
        return _result(RuntimeTerminalInputLeaseStatus::Acquired);
    }

    RuntimeTerminalInputLeaseResult RuntimeTerminalInputLease::Release(
        const std::string_view clientInstanceId,
        const uint64_t generation,
        const RuntimeTerminalLeaseRevision leaseRevision,
        const RuntimeTerminalLeaseMilliseconds now) noexcept
    {
        if (!_valid)
        {
            return _result(RuntimeTerminalInputLeaseStatus::InvalidConfiguration);
        }
        if (!_validClient(clientInstanceId))
        {
            return _result(RuntimeTerminalInputLeaseStatus::InvalidClient);
        }
        if (generation != _configuration.Generation)
        {
            return _result(RuntimeTerminalInputLeaseStatus::StaleGeneration);
        }
        if (const auto time = _observeTime(now);
            time != RuntimeTerminalInputLeaseStatus::Authorized)
        {
            return _result(time);
        }
        if (_expireIfDue(now) == RuntimeTerminalInputLeaseStatus::Expired)
        {
            return _result(RuntimeTerminalInputLeaseStatus::Expired);
        }
        if (!_current)
        {
            return _result(RuntimeTerminalInputLeaseStatus::AlreadyReleased);
        }
        if (_current->OwnerClientInstanceId != clientInstanceId)
        {
            return _result(RuntimeTerminalInputLeaseStatus::NotOwner);
        }
        if (_current->LeaseRevision != leaseRevision)
        {
            return _result(RuntimeTerminalInputLeaseStatus::StaleLease);
        }
        _current.reset();
        return _result(RuntimeTerminalInputLeaseStatus::Released);
    }

    RuntimeTerminalInputLeaseResult RuntimeTerminalInputLease::AuthorizeInput(
        const std::string_view clientInstanceId,
        const uint64_t generation,
        const RuntimeTerminalLeaseRevision leaseRevision,
        const RuntimeTerminalLeaseMilliseconds now) noexcept
    {
        if (!_valid)
        {
            return _result(RuntimeTerminalInputLeaseStatus::InvalidConfiguration);
        }
        if (!_validClient(clientInstanceId))
        {
            return _result(RuntimeTerminalInputLeaseStatus::InvalidClient);
        }
        if (generation != _configuration.Generation)
        {
            return _result(RuntimeTerminalInputLeaseStatus::StaleGeneration);
        }
        if (const auto time = _observeTime(now);
            time != RuntimeTerminalInputLeaseStatus::Authorized)
        {
            return _result(time);
        }
        if (_expireIfDue(now) == RuntimeTerminalInputLeaseStatus::Expired)
        {
            return _result(RuntimeTerminalInputLeaseStatus::Expired);
        }
        if (!_current)
        {
            return _result(RuntimeTerminalInputLeaseStatus::NoLease);
        }
        if (_current->OwnerClientInstanceId != clientInstanceId)
        {
            return _result(RuntimeTerminalInputLeaseStatus::NotOwner);
        }
        if (_current->LeaseRevision != leaseRevision)
        {
            return _result(RuntimeTerminalInputLeaseStatus::StaleLease);
        }
        return _result(RuntimeTerminalInputLeaseStatus::Authorized);
    }

    RuntimeTerminalInputLeaseResult RuntimeTerminalInputLease::Disconnect(
        const std::string_view clientInstanceId,
        const RuntimeTerminalLeaseMilliseconds now) noexcept
    {
        if (!_valid)
        {
            return _result(RuntimeTerminalInputLeaseStatus::InvalidConfiguration);
        }
        if (!_validClient(clientInstanceId))
        {
            return _result(RuntimeTerminalInputLeaseStatus::InvalidClient);
        }
        if (const auto time = _observeTime(now);
            time != RuntimeTerminalInputLeaseStatus::Authorized)
        {
            return _result(time);
        }
        if (_expireIfDue(now) == RuntimeTerminalInputLeaseStatus::Expired)
        {
            return _result(RuntimeTerminalInputLeaseStatus::Expired);
        }
        if (!_current)
        {
            return _result(RuntimeTerminalInputLeaseStatus::AlreadyReleased);
        }
        if (_current->OwnerClientInstanceId != clientInstanceId)
        {
            return _result(RuntimeTerminalInputLeaseStatus::NotOwner);
        }
        _current.reset();
        return _result(RuntimeTerminalInputLeaseStatus::Released);
    }

    RuntimeTerminalInputLeaseResult RuntimeTerminalInputLease::Expire(
        const RuntimeTerminalLeaseMilliseconds now) noexcept
    {
        if (!_valid)
        {
            return _result(RuntimeTerminalInputLeaseStatus::InvalidConfiguration);
        }
        if (const auto time = _observeTime(now);
            time != RuntimeTerminalInputLeaseStatus::Authorized)
        {
            return _result(time);
        }
        const auto expired = _expireIfDue(now);
        return _result(expired == RuntimeTerminalInputLeaseStatus::Expired ?
                           expired :
                           RuntimeTerminalInputLeaseStatus::NoLease);
    }

    std::optional<RuntimeTerminalInputLeaseSnapshot> RuntimeTerminalInputLease::Current() const
    {
        return _current;
    }

    RuntimeTerminalLeaseRevision RuntimeTerminalInputLease::LastIssuedRevision() const noexcept
    {
        return _lastIssuedRevision;
    }

    RuntimeTerminalInputLeaseStatus RuntimeTerminalInputLease::_observeTime(
        const RuntimeTerminalLeaseMilliseconds now) noexcept
    {
        if (_observedTime && now < _lastObservedNow)
        {
            return RuntimeTerminalInputLeaseStatus::ClockRegression;
        }
        _observedTime = true;
        _lastObservedNow = now;
        return RuntimeTerminalInputLeaseStatus::Authorized;
    }

    RuntimeTerminalInputLeaseStatus RuntimeTerminalInputLease::_expireIfDue(
        const RuntimeTerminalLeaseMilliseconds now) noexcept
    {
        if (_current && now >= _current->ExpiresAt)
        {
            _current.reset();
            return RuntimeTerminalInputLeaseStatus::Expired;
        }
        return RuntimeTerminalInputLeaseStatus::NoLease;
    }

    std::optional<RuntimeTerminalLeaseMilliseconds> RuntimeTerminalInputLease::_deadline(
        const RuntimeTerminalLeaseMilliseconds now) const noexcept
    {
        const auto maximum =
            (std::numeric_limits<RuntimeTerminalLeaseMilliseconds>::max)();
        if (now == maximum)
        {
            return std::nullopt;
        }
        return _configuration.LeaseDurationMilliseconds > maximum - now ?
                   std::optional<RuntimeTerminalLeaseMilliseconds>{ maximum } :
                   std::optional<RuntimeTerminalLeaseMilliseconds>{
                       now + _configuration.LeaseDurationMilliseconds
                   };
    }

    RuntimeTerminalInputLeaseResult RuntimeTerminalInputLease::_result(
        const RuntimeTerminalInputLeaseStatus status) const noexcept
    {
        RuntimeTerminalInputLeaseResult result;
        result.Status = status;
        try
        {
            result.Lease = _current;
        }
        catch (...)
        {
            result.Status = RuntimeTerminalInputLeaseStatus::ResourceFailure;
            result.Lease.reset();
        }
        return result;
    }
}
