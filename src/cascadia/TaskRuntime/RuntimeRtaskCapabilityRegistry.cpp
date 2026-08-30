// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeRtaskCapabilityRegistry.h"

#include "ProtocolValidation.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    struct RuntimeRtaskCapabilityRegistry::Entry
    {
        std::string Capability;
        std::string TaskId;
        std::string RunId;
        RtaskMethodSet Methods{};
        RuntimeRtaskMilliseconds IssuedAt{};
        RuntimeRtaskMilliseconds ExpiresAt{};
    };

    RuntimeRtaskCapabilityRegistry::~RuntimeRtaskCapabilityRegistry() = default;

    RuntimeRtaskCapabilityRegistry::RuntimeRtaskCapabilityRegistry(
        RuntimeRtaskCapabilityRegistryConfiguration configuration,
        std::shared_ptr<IRuntimeRtaskCapabilityTokenSource> tokenSource) noexcept :
        _configuration{ std::move(configuration) },
        _tokenSource{ std::move(tokenSource) }
    {
        _valid = _tokenSource && _configuration.MaximumCapabilities != 0 &&
                 _configuration.MaximumCapabilities <= MaximumRuntimeRtaskCapabilities &&
                 _configuration.MaximumLifetimeMilliseconds != 0 &&
                 _configuration.MaximumLifetimeMilliseconds <=
                     MaximumRuntimeRtaskCapabilityLifetimeMilliseconds;
        if (_valid)
        {
            try
            {
                _entries.reserve(_configuration.MaximumCapabilities);
            }
            catch (...)
            {
                _valid = false;
            }
        }
    }

    bool RuntimeRtaskCapabilityRegistry::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeRtaskCapabilityResult RuntimeRtaskCapabilityRegistry::IssueOrRenew(
        const std::string_view taskId,
        const std::string_view runId,
        const RtaskMethodSet methods,
        const uint32_t lifetimeMilliseconds,
        const RuntimeRtaskMilliseconds now)
    {
        RuntimeRtaskCapabilityResult result;
        if (!_valid)
        {
            return result;
        }
        if (!IsCanonicalProtocolUuid(taskId))
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidTask;
            return result;
        }
        if (!IsCanonicalProtocolUuid(runId))
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidRun;
            return result;
        }
        if (!IsValidRtaskMethodSet(methods))
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidMethodSet;
            return result;
        }
        if (lifetimeMilliseconds == 0 ||
            lifetimeMilliseconds > _configuration.MaximumLifetimeMilliseconds)
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidLifetime;
            return result;
        }
        if (const auto time = _observeTime(now);
            time != RuntimeRtaskCapabilityStatus::Authorized)
        {
            result.Status = time;
            return result;
        }
        const auto expiresAt = _deadline(now, lifetimeMilliseconds);
        if (!expiresAt)
        {
            result.Status = RuntimeRtaskCapabilityStatus::ClockExhausted;
            return result;
        }
        _purgeExpired(now);

        const auto existing = std::ranges::find_if(_entries, [&](const auto& entry) {
            return entry.RunId == runId;
        });
        if (existing != _entries.end())
        {
            if (existing->TaskId != taskId || existing->Methods != methods)
            {
                result.Status = RuntimeRtaskCapabilityStatus::ScopeConflict;
                return result;
            }
            try
            {
                auto grant = _grant(*existing);
                if (*expiresAt > grant.ExpiresAt)
                {
                    grant.ExpiresAt = *expiresAt;
                    result.Status = RuntimeRtaskCapabilityStatus::Renewed;
                }
                else
                {
                    result.Status = RuntimeRtaskCapabilityStatus::Replayed;
                }
                result.Grant = std::move(grant);
                existing->ExpiresAt = result.Grant->ExpiresAt;
                return result;
            }
            catch (...)
            {
                result.Status = RuntimeRtaskCapabilityStatus::ResourceFailure;
                return result;
            }
        }
        if (_entries.size() >= _configuration.MaximumCapabilities)
        {
            result.Status = RuntimeRtaskCapabilityStatus::CapacityExceeded;
            return result;
        }

        constexpr size_t maximumTokenAttempts{ 8 };
        for (size_t attempt = 0; attempt < maximumTokenAttempts; ++attempt)
        {
            std::optional<std::string> token;
            try
            {
                token = _tokenSource->Generate();
            }
            catch (...)
            {
                result.Status = RuntimeRtaskCapabilityStatus::TokenSourceFailure;
                return result;
            }
            if (!token || !IsValidRtaskCapability(*token))
            {
                result.Status = RuntimeRtaskCapabilityStatus::TokenSourceFailure;
                return result;
            }
            const auto collision = std::ranges::any_of(_entries, [&](const auto& entry) {
                return _sameCapability(entry.Capability, *token);
            });
            if (collision)
            {
                continue;
            }

            try
            {
                Entry entry;
                entry.Capability = std::move(*token);
                entry.TaskId = taskId;
                entry.RunId = runId;
                entry.Methods = methods;
                entry.IssuedAt = now;
                entry.ExpiresAt = *expiresAt;
                result.Grant = _grant(entry);
                _entries.emplace_back(std::move(entry));
                result.Status = RuntimeRtaskCapabilityStatus::Issued;
                return result;
            }
            catch (...)
            {
                result.Grant.reset();
                result.Status = RuntimeRtaskCapabilityStatus::ResourceFailure;
                return result;
            }
        }
        result.Status = RuntimeRtaskCapabilityStatus::TokenCollision;
        return result;
    }

    RuntimeRtaskCapabilityResult RuntimeRtaskCapabilityRegistry::Authorize(
        const std::string_view capability,
        const std::string_view runId,
        const RtaskMethod method,
        const RuntimeRtaskMilliseconds now)
    {
        RuntimeRtaskCapabilityResult result;
        if (!_valid)
        {
            return result;
        }
        if (!IsValidRtaskCapability(capability))
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidToken;
            return result;
        }
        if (!IsCanonicalProtocolUuid(runId))
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidRun;
            return result;
        }
        if (RtaskMethodBit(method) == 0)
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidMethod;
            return result;
        }
        if (const auto time = _observeTime(now);
            time != RuntimeRtaskCapabilityStatus::Authorized)
        {
            result.Status = time;
            return result;
        }

        const auto entry = std::ranges::find_if(_entries, [&](const auto& candidate) {
            return _sameCapability(candidate.Capability, capability);
        });
        if (entry == _entries.end())
        {
            result.Status = RuntimeRtaskCapabilityStatus::Missing;
            return result;
        }
        if (entry->ExpiresAt <= now)
        {
            _entries.erase(entry);
            result.Status = RuntimeRtaskCapabilityStatus::Expired;
            return result;
        }
        if (entry->RunId != runId)
        {
            result.Status = RuntimeRtaskCapabilityStatus::WrongRun;
            return result;
        }
        if (!HasRtaskMethod(entry->Methods, method))
        {
            result.Status = RuntimeRtaskCapabilityStatus::WrongMethod;
            return result;
        }
        try
        {
            result.Binding = RuntimeRtaskCapabilityBinding{
                entry->TaskId,
                entry->RunId,
                entry->Methods,
                entry->ExpiresAt,
            };
            result.Status = RuntimeRtaskCapabilityStatus::Authorized;
        }
        catch (...)
        {
            result.Status = RuntimeRtaskCapabilityStatus::ResourceFailure;
        }
        return result;
    }

    RuntimeRtaskCapabilityResult RuntimeRtaskCapabilityRegistry::InvalidateRun(
        const std::string_view runId) noexcept
    {
        RuntimeRtaskCapabilityResult result;
        if (!_valid)
        {
            return result;
        }
        if (!IsCanonicalProtocolUuid(runId))
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidRun;
            return result;
        }
        const auto before = _entries.size();
        std::erase_if(_entries, [&](const auto& entry) {
            return entry.RunId == runId;
        });
        result.InvalidatedCapabilities = before - _entries.size();
        result.Status = result.InvalidatedCapabilities == 0 ?
                            RuntimeRtaskCapabilityStatus::Missing :
                            RuntimeRtaskCapabilityStatus::Invalidated;
        return result;
    }

    RuntimeRtaskCapabilityResult RuntimeRtaskCapabilityRegistry::InvalidateCapability(
        const std::string_view capability) noexcept
    {
        RuntimeRtaskCapabilityResult result;
        if (!_valid)
        {
            return result;
        }
        if (!IsValidRtaskCapability(capability))
        {
            result.Status = RuntimeRtaskCapabilityStatus::InvalidToken;
            return result;
        }
        const auto found = std::ranges::find_if(_entries, [&](const auto& entry) {
            return _sameCapability(entry.Capability, capability);
        });
        if (found == _entries.end())
        {
            result.Status = RuntimeRtaskCapabilityStatus::Missing;
            return result;
        }
        _entries.erase(found);
        result.InvalidatedCapabilities = 1;
        result.Status = RuntimeRtaskCapabilityStatus::Invalidated;
        return result;
    }

    size_t RuntimeRtaskCapabilityRegistry::Size() const noexcept
    {
        return _entries.size();
    }

    RuntimeRtaskCapabilityStatus RuntimeRtaskCapabilityRegistry::_observeTime(
        const RuntimeRtaskMilliseconds now) noexcept
    {
        if (_observedTime && now < _lastObservedTime)
        {
            return RuntimeRtaskCapabilityStatus::ClockRegression;
        }
        _lastObservedTime = now;
        _observedTime = true;
        return RuntimeRtaskCapabilityStatus::Authorized;
    }

    std::optional<RuntimeRtaskMilliseconds> RuntimeRtaskCapabilityRegistry::_deadline(
        const RuntimeRtaskMilliseconds now,
        const uint32_t lifetimeMilliseconds) const noexcept
    {
        const auto maximum = (std::numeric_limits<RuntimeRtaskMilliseconds>::max)();
        return lifetimeMilliseconds > maximum - now ?
                   std::nullopt :
                   std::optional<RuntimeRtaskMilliseconds>{
                       now + lifetimeMilliseconds
                   };
    }

    void RuntimeRtaskCapabilityRegistry::_purgeExpired(
        const RuntimeRtaskMilliseconds now) noexcept
    {
        std::erase_if(_entries, [&](const auto& entry) {
            return entry.ExpiresAt <= now;
        });
    }

    bool RuntimeRtaskCapabilityRegistry::_sameCapability(
        const std::string_view left,
        const std::string_view right) noexcept
    {
        if (left.size() != right.size())
        {
            return false;
        }
        uint8_t difference{};
        for (size_t index = 0; index < left.size(); ++index)
        {
            difference |= static_cast<uint8_t>(left[index]) ^
                          static_cast<uint8_t>(right[index]);
        }
        return difference == 0;
    }

    RuntimeRtaskCapabilityGrant RuntimeRtaskCapabilityRegistry::_grant(
        const Entry& entry)
    {
        return {
            entry.Capability,
            entry.TaskId,
            entry.RunId,
            entry.Methods,
            entry.IssuedAt,
            entry.ExpiresAt,
        };
    }
}
