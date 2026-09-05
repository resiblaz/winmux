// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CodexAppServerJsonRpc.h"

namespace Microsoft::Terminal::TaskProvider
{
    namespace
    {
        std::string _json(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            builder["indentation"] = "";
            return Json::writeString(builder, value);
        }

        bool _parse(const std::string_view value, Json::Value& root)
        {
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false;
            builder["allowTrailingCommas"] = false;
            builder["collectComments"] = false;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            builder["strictRoot"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            std::string error;
            return reader && reader->parse(
                                 value.data(), value.data() + value.size(),
                                 &root, &error) && root.isObject();
        }

        TaskProviderStatus _readStatus(const CodexTransportReadKind kind) noexcept
        {
            switch (kind)
            {
            case CodexTransportReadKind::Timeout: return TaskProviderStatus::Timeout;
            case CodexTransportReadKind::Cancelled: return TaskProviderStatus::Cancelled;
            case CodexTransportReadKind::EndOfStream: return TaskProviderStatus::TransportClosed;
            case CodexTransportReadKind::OutputLimitExceeded: return TaskProviderStatus::OutputLimitExceeded;
            default: return TaskProviderStatus::ResourceFailure;
            }
        }

        bool _method(const Json::Value& value)
        {
            return value.isMember("method") && value["method"].isString() &&
                   !value["method"].asString().empty();
        }
    }

    CodexAppServerJsonRpc::CodexAppServerJsonRpc(
        CodexAppServerTransportFactory transportFactory) :
        _factory{ std::move(transportFactory) }, _stopEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }
    {
    }

    CodexAppServerJsonRpc::~CodexAppServerJsonRpc()
    {
        Stop();
    }


    TaskProviderResult CodexAppServerJsonRpc::Start() noexcept
    {
        const std::scoped_lock lifecycle{ _lifecycleMutex };
        try
        {
            {
                const std::scoped_lock lock{ _mutex };
                if (_transport && !_failure && _transport->Running())
                    return { TaskProviderStatus::AlreadyApplied, {} };
            }
            _stopLocked();
            if (!_factory || !_stopEvent)
                return { TaskProviderStatus::ResourceFailure, "Codex transport configuration is unavailable" };
            auto transport = _factory();
            if (!transport)
                return { TaskProviderStatus::ResourceFailure, "Codex transport factory returned no transport" };
            const auto started = transport->Start();
            if (!started.Succeeded()) return started;
            const std::scoped_lock lock{ _mutex };
            _transport = std::move(transport);
            _failure.reset();
            _nextRequestId = 1;
            ResetEvent(_stopEvent.get());
            return started;
        }
        catch (...)
        {
            return { TaskProviderStatus::ResourceFailure, "Codex JSON-RPC start exhausted resources" };
        }
    }

    TaskProviderResult CodexAppServerJsonRpc::_write(
        Json::Value message,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent,
        const uint64_t epoch)
    {
        if (!timeoutMilliseconds)
            return { TaskProviderStatus::InvalidRequest, "Codex write timeout is invalid" };
        auto encoded = _json(message);
        encoded.push_back('\n');
        if (encoded.size() > MaximumCodexJsonLineBytes)
            return { TaskProviderStatus::OutputLimitExceeded, "Codex JSON-RPC request is oversized" };
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        std::unique_lock writer{ _writeMutex, std::defer_lock };
        for (;;)
        {
            if (cancellationEvent && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0)
                return { TaskProviderStatus::Cancelled, "Codex request was cancelled" };
            {
                const std::scoped_lock lock{ _mutex };
                if (epoch != _epoch || !_transport)
                    return { TaskProviderStatus::TransportClosed, "Codex transport generation changed" };
                if (_failure) return *_failure;
            }
            if (GetTickCount64() >= deadline)
                return { TaskProviderStatus::Timeout, "Codex write timed out" };
            if (writer.try_lock_for(std::chrono::milliseconds{ 5 })) break;
        }
        {
            const std::scoped_lock lock{ _mutex };
            if (epoch != _epoch || !_transport)
                return { TaskProviderStatus::TransportClosed, "Codex transport generation changed" };
            if (_failure) return *_failure;
        }
        // Stop signals IO first, then takes the direction locks before disposal.
        return _transport->Write(encoded, std::chrono::milliseconds{
            static_cast<int64_t>((std::max<uint64_t>)(1, deadline - (std::min)(deadline, GetTickCount64()))) }, cancellationEvent);
    }

    std::optional<Json::Value> CodexAppServerJsonRpc::_readMessage(
        const uint64_t deadline,
        const HANDLE cancellationEvent,
        TaskProviderResult& failure)
    {
        for (;;)
        {
            const auto newline = _buffer.find('\n');
            if (newline != std::string::npos)
            {
                auto line = _buffer.substr(0, newline);
                _buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                Json::Value value;
                if (line.empty() || line.size() > MaximumCodexJsonLineBytes ||
                    !_parse(line, value))
                {
                    failure = { TaskProviderStatus::MalformedResponse, "Codex JSON-RPC message is malformed" };
                    return std::nullopt;
                }
                return value;
            }
            if (_buffer.size() > MaximumCodexJsonLineBytes)
            {
                failure = { TaskProviderStatus::OutputLimitExceeded, "Codex JSON-RPC line is oversized" };
                return std::nullopt;
            }
            const auto now = GetTickCount64();
            if (now >= deadline)
            {
                failure = { TaskProviderStatus::Timeout, "Codex JSON-RPC read timed out" };
                return std::nullopt;
            }
            const auto read = _transport->Read(
                std::chrono::milliseconds{ static_cast<int64_t>(
                    (std::min<uint64_t>)(deadline - now, UINT32_MAX)) },
                cancellationEvent);
            if (read.Kind != CodexTransportReadKind::Data)
            {
                failure = {
                    _readStatus(read.Kind),
                    read.Kind == CodexTransportReadKind::Timeout ?
                        "Codex JSON-RPC read timed out" :
                        "Codex JSON-RPC transport closed",
                };
                return std::nullopt;
            }
            if (read.Bytes.empty() ||
                read.Bytes.size() > MaximumCodexJsonLineBytes -
                                        (std::min)(_buffer.size(), MaximumCodexJsonLineBytes))
            {
                failure = { TaskProviderStatus::OutputLimitExceeded, "Codex JSON-RPC input is oversized" };
                return std::nullopt;
            }
            _buffer.append(read.Bytes);
        }
    }


    bool CodexAppServerJsonRpc::_pumpOnce(const uint64_t epoch) noexcept
    {
        std::unique_lock reader{ _readMutex, std::try_to_lock };
        if (!reader.owns_lock()) return false;
        try
        {
            {
                const std::scoped_lock lock{ _mutex };
                if (epoch != _epoch || !_transport || _failure) return false;
            }
            TaskProviderResult failure;
            const auto message = _readMessage(GetTickCount64() + 10, _stopEvent.get(), failure);
            std::unique_lock lock{ _mutex };
            if (epoch != _epoch) return false;
            if (!message)
            {
                if (failure.Status == TaskProviderStatus::Timeout) return false;
                _failure = failure;
            }
            else if (_method(*message))
            {
                if (_notifications.size() >= MaximumCodexQueuedNotifications)
                    _failure = TaskProviderResult{ TaskProviderStatus::OutputLimitExceeded, "Codex notification queue is full" };
                else
                    _notifications.push_back(*message);
            }
            else if (!message->isMember("id") || !(*message)["id"].isUInt64() ||
                     (*message)["id"].asUInt64() == 0 || (*message)["id"].asUInt64() >= _nextRequestId)
            {
                _failure = TaskProviderResult{ TaskProviderStatus::MalformedResponse, "Codex response ID was never issued" };
            }
            else
            {
                const auto found = _pending.find((*message)["id"].asUInt64());
                // Timed-out/cancelled requests may still receive a late response.
                if (found != _pending.end() && !found->second)
                {
                    CodexJsonRpcResult result;
                    if (message->isMember("error"))
                    {
                        const auto& error = (*message)["error"];
                        if (!error.isObject() || !error["code"].isInt64() || !error["message"].isString())
                        {
                            result.Status = TaskProviderStatus::MalformedResponse;
                            result.Error = "Codex RPC error is malformed";
                        }
                        else
                        {
                            result.RpcCode = error["code"].asInt64();
                            result.Status = result.RpcCode == -32601 ? TaskProviderStatus::UnsupportedCapability : TaskProviderStatus::Rejected;
                            result.Error = error["message"].asString().substr(0, MaximumProviderDiagnosticBytes);
                        }
                    }
                    else if (message->isMember("result"))
                    {
                        result.Status = TaskProviderStatus::Succeeded;
                        result.Value = (*message)["result"];
                    }
                    else
                    {
                        result.Status = TaskProviderStatus::MalformedResponse;
                        result.Error = "Codex response has no result";
                    }
                    found->second = std::move(result);
                }
            }
            const auto failed = _failure.has_value();
            lock.unlock();
            _changed.notify_all();
            if (failed && _transport) _transport->RequestStop();
            return true;
        }
        catch (...)
        {
            const std::scoped_lock lock{ _mutex };
            if (epoch == _epoch)
                _failure = TaskProviderResult{ TaskProviderStatus::ResourceFailure, "Codex response dispatch exhausted resources" };
            _changed.notify_all();
            return false;
        }
    }

    CodexJsonRpcResult CodexAppServerJsonRpc::Request(
        const std::string_view method,
        const Json::Value& params,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent) noexcept
    {
        CodexJsonRpcResult result;
        uint64_t requestId{};
        uint64_t epoch{};
        try
        {
            if (method.empty() || method.size() > 128 || !params.isObject() || !timeoutMilliseconds)
            {
                result.Status = TaskProviderStatus::InvalidRequest;
                result.Error = "Codex JSON-RPC request is invalid";
                return result;
            }
            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            {
                const std::scoped_lock lock{ _mutex };
                if (!_transport || _failure || !_nextRequestId)
                {
                    result.Status = _failure ? _failure->Status : TaskProviderStatus::Unavailable;
                    result.Error = _failure ? _failure->Error : "Codex transport is unavailable";
                    return result;
                }
                if (_pending.size() >= MaximumCodexPendingRequests)
                {
                    result.Status = TaskProviderStatus::ResourceFailure;
                    result.Error = "Codex pending request capacity was reached";
                    return result;
                }
                epoch = _epoch;
                requestId = _nextRequestId++;
                _pending.emplace(requestId, std::nullopt);
            }
            Json::Value message{ Json::objectValue };
            message["id"] = Json::UInt64{ requestId };
            message["method"] = std::string{ method };
            message["params"] = params;
            const auto written = _write(std::move(message), timeoutMilliseconds, cancellationEvent, epoch);
            if (!written.Succeeded())
            {
                const std::scoped_lock lock{ _mutex };
                if (epoch == _epoch) _pending.erase(requestId);
                result.Status = written.Status;
                result.Error = written.Error;
                return result;
            }
            for (;;)
            {
                {
                    std::unique_lock lock{ _mutex };
                    if (epoch != _epoch)
                    {
                        result.Status = TaskProviderStatus::TransportClosed;
                        result.Error = "Codex transport generation changed";
                        return result;
                    }
                    const auto found = _pending.find(requestId);
                    if (found != _pending.end() && found->second)
                    {
                        result = std::move(*found->second);
                        _pending.erase(found);
                        return result;
                    }
                    const auto cancelled = cancellationEvent && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0;
                    if (_failure || cancelled || GetTickCount64() >= deadline)
                    {
                        _pending.erase(requestId);
                        result.Status = _failure ? _failure->Status : cancelled ? TaskProviderStatus::Cancelled : TaskProviderStatus::Timeout;
                        result.Error = _failure ? _failure->Error : cancelled ? "Codex request was cancelled" : "Codex response timed out";
                        return result;
                    }
                }
                if (!_pumpOnce(epoch))
                {
                    std::unique_lock lock{ _mutex };
                    _changed.wait_for(lock, std::chrono::milliseconds{ 5 });
                }
            }
        }
        catch (...)
        {
            const std::scoped_lock lock{ _mutex };
            if (epoch == _epoch) _pending.erase(requestId);
            result.Status = TaskProviderStatus::ResourceFailure;
            result.Error = "Codex JSON-RPC request exhausted resources";
            return result;
        }
    }

    TaskProviderResult CodexAppServerJsonRpc::Notify(
        const std::string_view method,
        const Json::Value& params,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            if (method.empty() || method.size() > 128 || !params.isObject())
                return { TaskProviderStatus::InvalidRequest, "Codex notification is invalid" };
            Json::Value message{ Json::objectValue };
            message["method"] = std::string{ method };
            message["params"] = params;
            uint64_t epoch;
            { const std::scoped_lock lock{ _mutex }; epoch = _epoch; }
            return _write(std::move(message), timeoutMilliseconds, cancellationEvent, epoch);
        }
        catch (...)
        {
            return { TaskProviderStatus::ResourceFailure, "Codex notification exhausted resources" };
        }
    }

    TaskProviderResult CodexAppServerJsonRpc::Respond(
        const Json::Value& requestId,
        const Json::Value& response,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent) noexcept
    {
        try
        {
            if ((!requestId.isUInt64() && !requestId.isString()) || requestId.empty() || !response.isObject())
                return { TaskProviderStatus::InvalidRequest, "Codex JSON-RPC response is invalid" };
            Json::Value message{ Json::objectValue };
            message["id"] = requestId;
            message["result"] = response;
            uint64_t epoch;
            { const std::scoped_lock lock{ _mutex }; epoch = _epoch; }
            return _write(std::move(message), timeoutMilliseconds, cancellationEvent, epoch);
        }
        catch (...)
        {
            return { TaskProviderStatus::ResourceFailure, "Codex JSON-RPC response exhausted resources" };
        }
    }

    CodexJsonRpcResult CodexAppServerJsonRpc::NextNotification(
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent) noexcept
    {
        CodexJsonRpcResult result;
        try
        {
            uint64_t epoch;
            { const std::scoped_lock lock{ _mutex }; epoch = _epoch; }
            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            for (;;)
            {
                {
                    std::unique_lock lock{ _mutex };
                    if (epoch != _epoch || !_transport || _failure || !timeoutMilliseconds)
                    {
                        result.Status = _failure ? _failure->Status : TaskProviderStatus::Unavailable;
                        result.Error = _failure ? _failure->Error : "Codex notification transport is unavailable";
                        return result;
                    }
                    if (!_notifications.empty())
                    {
                        result.Status = TaskProviderStatus::Succeeded;
                        result.Value = std::move(_notifications.front());
                        _notifications.pop_front();
                        return result;
                    }
                    const auto cancelled = cancellationEvent && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0;
                    if (cancelled || GetTickCount64() >= deadline)
                    {
                        result.Status = cancelled ? TaskProviderStatus::Cancelled : TaskProviderStatus::Timeout;
                        return result;
                    }
                }
                if (!_pumpOnce(epoch))
                {
                    std::unique_lock lock{ _mutex };
                    _changed.wait_for(lock, std::chrono::milliseconds{ 5 });
                }
            }
        }
        catch (...)
        {
            result.Status = TaskProviderStatus::ResourceFailure;
            result.Error = "Codex notification read exhausted resources";
            return result;
        }
    }

    bool CodexAppServerJsonRpc::Running() const noexcept
    {
        const std::scoped_lock lock{ _mutex };
        return _transport && !_failure && _transport->Running();
    }

    void CodexAppServerJsonRpc::_stopLocked() noexcept
    {
        ICodexAppServerTransport* transport{};
        {
            const std::scoped_lock lock{ _mutex };
            ++_epoch;
            if (_stopEvent) SetEvent(_stopEvent.get());
            transport = _transport.get();
        }
        _changed.notify_all();
        if (transport) transport->RequestStop();
        const std::scoped_lock io{ _writeMutex, _readMutex };
        if (transport) transport->Stop();
        const std::scoped_lock lock{ _mutex };
        _transport.reset();
        _buffer.clear();
        _notifications.clear();
        _pending.clear();
        _failure.reset();
    }

    void CodexAppServerJsonRpc::Stop() noexcept
    {
        const std::scoped_lock lifecycle{ _lifecycleMutex };
        _stopLocked();
    }
}
