// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientHeartbeat.h"

#include "ProtocolValidation.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        constexpr char HexAlphabet[]{ "0123456789abcdef" };

        void _writeHex(char*& destination, const uint64_t value, const size_t digits) noexcept
        {
            for (size_t index = 0; index < digits; ++index)
            {
                const auto shift = (digits - index - 1) * 4;
                *destination++ = HexAlphabet[(value >> shift) & 0xf];
            }
        }

        std::optional<uint64_t> _parseHex(const std::string_view value) noexcept
        {
            uint64_t result{};
            for (const auto character : value)
            {
                const auto digit = character >= '0' && character <= '9' ?
                                       static_cast<uint8_t>(character - '0') :
                                   character >= 'a' && character <= 'f' ?
                                       static_cast<uint8_t>(character - 'a' + 10) :
                                       uint8_t{ 0xff };
                if (digit == 0xff)
                {
                    return std::nullopt;
                }
                result = (result << 4) | digit;
            }
            return result;
        }

    }

    bool RuntimeClientHeartbeatState::Initialize(
        const RuntimeClientEpoch epoch,
        const RuntimeClientMonotonicMilliseconds now,
        const uint32_t heartbeatMilliseconds) noexcept
    {
        if (_phase != RuntimeClientHeartbeatPhase::Inactive || epoch == 0 ||
            heartbeatMilliseconds < MinimumRuntimeClientHeartbeatMilliseconds ||
            heartbeatMilliseconds > MaximumRuntimeClientHeartbeatMilliseconds)
        {
            _fail();
            return false;
        }

        _epoch = epoch;
        _intervalMilliseconds = heartbeatMilliseconds;
        _phase = RuntimeClientHeartbeatPhase::Monitoring;
        if (!_setWindow(now))
        {
            _fail();
            return false;
        }
        return true;
    }

    RuntimeClientHeartbeatDecision RuntimeClientHeartbeatState::Decide(
        const RuntimeClientMonotonicMilliseconds now) noexcept
    {
        if (_phase == RuntimeClientHeartbeatPhase::Inactive ||
            _phase == RuntimeClientHeartbeatPhase::Failed)
        {
            return { RuntimeClientHeartbeatAction::Failed, 0 };
        }
        if (_phase == RuntimeClientHeartbeatPhase::TimedOut)
        {
            return { RuntimeClientHeartbeatAction::Timeout, _silenceDeadline };
        }
        if (now < _lastInbound)
        {
            _fail();
            return { RuntimeClientHeartbeatAction::Failed, 0 };
        }

        // The full silence deadline always wins over the half-interval due time.
        if (now >= _silenceDeadline)
        {
            _phase = RuntimeClientHeartbeatPhase::TimedOut;
            return { RuntimeClientHeartbeatAction::Timeout, _silenceDeadline };
        }
        if (_phase == RuntimeClientHeartbeatPhase::Monitoring && now >= _pingDue)
        {
            _phase = RuntimeClientHeartbeatPhase::PingDue;
            return { RuntimeClientHeartbeatAction::QueuePing, _silenceDeadline };
        }
        if (_phase == RuntimeClientHeartbeatPhase::Monitoring)
        {
            return { RuntimeClientHeartbeatAction::Wait, _pingDue };
        }
        return { RuntimeClientHeartbeatAction::Wait, _silenceDeadline };
    }

    bool RuntimeClientHeartbeatState::BeginPing(const RuntimeClientOperationId operationId) noexcept
    {
        if (_phase != RuntimeClientHeartbeatPhase::PingDue || operationId == 0 ||
            _pingOperationId != 0)
        {
            return false;
        }
        _pingOperationId = operationId;
        _phase = RuntimeClientHeartbeatPhase::PingInFlight;
        return true;
    }

    RuntimeClientHeartbeatInboundResult RuntimeClientHeartbeatState::ObserveInbound(
        const RuntimeClientMonotonicMilliseconds now) noexcept
    {
        RuntimeClientHeartbeatInboundResult result;
        if (_phase == RuntimeClientHeartbeatPhase::Inactive)
        {
            return result;
        }
        if (_phase == RuntimeClientHeartbeatPhase::Failed ||
            _phase == RuntimeClientHeartbeatPhase::TimedOut)
        {
            result.Status = RuntimeClientHeartbeatStatus::Failed;
            return result;
        }
        if (now >= _silenceDeadline)
        {
            _phase = RuntimeClientHeartbeatPhase::TimedOut;
            result.Status = RuntimeClientHeartbeatStatus::Failed;
            return result;
        }
        if (_pingOperationId != 0)
        {
            result.SupersededOperation = _pingOperationId;
            _pingOperationId = 0;
        }
        if (now < _lastInbound || !_setWindow(now))
        {
            _fail();
            result.Status = RuntimeClientHeartbeatStatus::Failed;
            return result;
        }
        _phase = RuntimeClientHeartbeatPhase::Monitoring;
        result.Status = RuntimeClientHeartbeatStatus::Succeeded;
        return result;
    }

    RuntimeClientHeartbeatStatus RuntimeClientHeartbeatState::AcknowledgePing(
        const RuntimeClientOperationId operationId,
        const RuntimeClientMonotonicMilliseconds now) noexcept
    {
        if (_phase == RuntimeClientHeartbeatPhase::Inactive)
        {
            return RuntimeClientHeartbeatStatus::Inactive;
        }
        if (_phase != RuntimeClientHeartbeatPhase::PingInFlight || operationId == 0 ||
            operationId != _pingOperationId)
        {
            return _phase == RuntimeClientHeartbeatPhase::Failed ||
                           _phase == RuntimeClientHeartbeatPhase::TimedOut ?
                       RuntimeClientHeartbeatStatus::Failed :
                       RuntimeClientHeartbeatStatus::Stale;
        }
        return ObserveInbound(now).Status;
    }

    void RuntimeClientHeartbeatState::Retire() noexcept
    {
        _phase = RuntimeClientHeartbeatPhase::Inactive;
        _epoch = 0;
        _intervalMilliseconds = 0;
        _lastInbound = 0;
        _pingDue = 0;
        _silenceDeadline = 0;
        _pingOperationId = 0;
    }

    bool RuntimeClientHeartbeatState::HasWriterWork() const noexcept
    {
        return _phase == RuntimeClientHeartbeatPhase::PingDue;
    }

    std::optional<RuntimeClientHeartbeatView> RuntimeClientHeartbeatState::ActiveView() const noexcept
    {
        if (_phase == RuntimeClientHeartbeatPhase::Inactive || _epoch == 0)
        {
            return std::nullopt;
        }
        return RuntimeClientHeartbeatView{
            _phase,
            _epoch,
            _intervalMilliseconds,
            _lastInbound,
            _pingDue,
            _silenceDeadline,
            _pingOperationId,
            HasWriterWork(),
        };
    }

    bool RuntimeClientHeartbeatState::_setWindow(
        const RuntimeClientMonotonicMilliseconds now) noexcept
    {
        const auto maximum = (std::numeric_limits<RuntimeClientMonotonicMilliseconds>::max)();
        if (_intervalMilliseconds == 0 || _intervalMilliseconds > maximum - now)
        {
            return false;
        }
        const auto half = (std::max)(uint32_t{ 1 }, _intervalMilliseconds / 2);
        if (half > maximum - now)
        {
            return false;
        }
        _lastInbound = now;
        _pingDue = now + half;
        _silenceDeadline = now + _intervalMilliseconds;
        return _pingDue > now && _silenceDeadline > _pingDue;
    }

    void RuntimeClientHeartbeatState::_fail() noexcept
    {
        _phase = RuntimeClientHeartbeatPhase::Failed;
    }

    std::optional<uint32_t> RuntimeClientHeartbeatNamespace(
        const std::string_view clientInstanceId) noexcept
    {
        if (!IsCanonicalProtocolUuid(clientInstanceId))
        {
            return std::nullopt;
        }
        uint32_t value{ 2166136261u };
        for (const auto character : clientInstanceId)
        {
            value ^= static_cast<uint8_t>(character);
            value *= 16777619u;
        }
        return value;
    }

    std::optional<std::string> MakeRuntimeClientHeartbeatRequestId(
        const std::string_view clientInstanceId,
        const uint64_t sequence) noexcept
    {
        const auto requestNamespace = RuntimeClientHeartbeatNamespace(clientInstanceId);
        if (!requestNamespace || sequence == 0)
        {
            return std::nullopt;
        }
        try
        {
            char text[36]{};
            auto output = text;
            _writeHex(output, 0xf17e0001u, 8);
            *output++ = '-';
            _writeHex(output, *requestNamespace >> 16, 4);
            *output++ = '-';
            _writeHex(output, *requestNamespace & 0xffffu, 4);
            *output++ = '-';
            _writeHex(output, sequence >> 48, 4);
            *output++ = '-';
            _writeHex(output, sequence & 0xffffffffffffull, 12);
            if (output != text + std::size(text))
            {
                return std::nullopt;
            }
            std::string result{ text, std::size(text) };
            return IsCanonicalProtocolUuid(result) ?
                       std::optional<std::string>{ std::move(result) } :
                       std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    bool IsReservedHeartbeatRequestId(const std::string_view requestId) noexcept
    {
        return requestId.starts_with(RuntimeClientHeartbeatRequestIdPrefix) &&
               IsCanonicalProtocolUuid(requestId);
    }

    std::optional<RuntimeClientHeartbeatRequestIdentity>
        ParseRuntimeClientHeartbeatRequestId(const std::string_view requestId) noexcept
    {
        if (!requestId.starts_with(RuntimeClientHeartbeatRequestIdPrefix) ||
            !IsCanonicalProtocolUuid(requestId))
        {
            return std::nullopt;
        }
        const auto namespaceHigh = _parseHex(requestId.substr(9, 4));
        const auto namespaceLow = _parseHex(requestId.substr(14, 4));
        const auto sequenceHigh = _parseHex(requestId.substr(19, 4));
        const auto sequenceLow = _parseHex(requestId.substr(24, 12));
        if (!namespaceHigh || !namespaceLow || !sequenceHigh || !sequenceLow)
        {
            return std::nullopt;
        }
        const auto requestNamespace = static_cast<uint32_t>(
            (*namespaceHigh << 16) | *namespaceLow);
        const auto sequence = (*sequenceHigh << 48) | *sequenceLow;
        return sequence == 0 ?
                   std::nullopt :
                   std::optional<RuntimeClientHeartbeatRequestIdentity>{
                       RuntimeClientHeartbeatRequestIdentity{ requestNamespace, sequence }
                   };
    }
}
