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
        _factory{ std::move(transportFactory) }
    {
    }

    CodexAppServerJsonRpc::~CodexAppServerJsonRpc()
    {
        Stop();
    }

    TaskProviderResult CodexAppServerJsonRpc::Start() noexcept
    {
        const std::scoped_lock lock{ _mutex };
        try
        {
            if (!_factory)
            {
                return { TaskProviderStatus::InvalidRequest, "Codex transport factory is unavailable" };
            }
            if (_transport && _transport->Running())
            {
                return { TaskProviderStatus::AlreadyApplied, {} };
            }
            _transport = _factory();
            if (!_transport)
            {
                return { TaskProviderStatus::ResourceFailure, "Codex transport factory returned no transport" };
            }
            _buffer.clear();
            _notifications.clear();
            _nextRequestId = 1;
            return _transport->Start();
        }
        catch (...)
        {
            return { TaskProviderStatus::ResourceFailure, "Codex JSON-RPC start exhausted resources" };
        }
    }

    TaskProviderResult CodexAppServerJsonRpc::_write(
        Json::Value message,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent)
    {
        if (!_transport || !_transport->Running() || timeoutMilliseconds == 0)
        {
            return { TaskProviderStatus::Unavailable, "Codex JSON-RPC transport is not running" };
        }
        auto encoded = _json(message);
        encoded.push_back('\n');
        if (encoded.size() > MaximumCodexJsonLineBytes)
        {
            return { TaskProviderStatus::OutputLimitExceeded, "Codex JSON-RPC request is oversized" };
        }
        return _transport->Write(
            encoded,
            std::chrono::milliseconds{ timeoutMilliseconds },
            cancellationEvent);
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

    CodexJsonRpcResult CodexAppServerJsonRpc::Request(
        const std::string_view method,
        const Json::Value& params,
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent) noexcept
    {
        const std::scoped_lock lock{ _mutex };
        CodexJsonRpcResult result;
        try
        {
            if (method.empty() || method.size() > 128 || !params.isObject() ||
                _nextRequestId == 0 || timeoutMilliseconds == 0)
            {
                result.Status = TaskProviderStatus::InvalidRequest;
                result.Error = "Codex JSON-RPC request is invalid";
                return result;
            }
            const auto requestId = _nextRequestId++;
            Json::Value message{ Json::objectValue };
            message["id"] = Json::UInt64{ requestId };
            message["method"] = std::string{ method };
            message["params"] = params;
            const auto written = _write(
                std::move(message), timeoutMilliseconds, cancellationEvent);
            if (!written.Succeeded())
            {
                result.Status = written.Status;
                result.Error = written.Error;
                return result;
            }
            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            for (;;)
            {
                TaskProviderResult failure;
                const auto received = _readMessage(deadline, cancellationEvent, failure);
                if (!received)
                {
                    result.Status = failure.Status;
                    result.Error = failure.Error;
                    return result;
                }
                if (_method(*received))
                {
                    if (_notifications.size() >= MaximumCodexQueuedNotifications)
                    {
                        result.Status = TaskProviderStatus::OutputLimitExceeded;
                        result.Error = "Codex notification queue is full";
                        return result;
                    }
                    _notifications.emplace_back(*received);
                    continue;
                }
                if (!received->isMember("id") ||
                    !(*received)["id"].isUInt64() ||
                    (*received)["id"].asUInt64() != requestId)
                {
                    result.Status = TaskProviderStatus::MalformedResponse;
                    result.Error = "Codex response ID is missing or unexpected";
                    return result;
                }
                if (received->isMember("error"))
                {
                    const auto& error = (*received)["error"];
                    if (!error.isObject() || !error.isMember("code") ||
                        !error["code"].isInt64() || !error.isMember("message") ||
                        !error["message"].isString())
                    {
                        result.Status = TaskProviderStatus::MalformedResponse;
                        result.Error = "Codex RPC error is malformed";
                        return result;
                    }
                    result.RpcCode = error["code"].asInt64();
                    result.Status = result.RpcCode == -32601 ?
                                        TaskProviderStatus::UnsupportedCapability :
                                        TaskProviderStatus::Rejected;
                    result.Error = error["message"].asString().substr(
                        0, MaximumProviderDiagnosticBytes);
                    return result;
                }
                if (!received->isMember("result"))
                {
                    result.Status = TaskProviderStatus::MalformedResponse;
                    result.Error = "Codex response has no result";
                    return result;
                }
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = (*received)["result"];
                return result;
            }
        }
        catch (...)
        {
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
        const std::scoped_lock lock{ _mutex };
        try
        {
            if (method.empty() || method.size() > 128 || !params.isObject())
            {
                return { TaskProviderStatus::InvalidRequest, "Codex notification is invalid" };
            }
            Json::Value message{ Json::objectValue };
            message["method"] = std::string{ method };
            message["params"] = params;
            return _write(std::move(message), timeoutMilliseconds, cancellationEvent);
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
        const std::scoped_lock lock{ _mutex };
        try
        {
            if ((!requestId.isUInt64() && !requestId.isString()) ||
                requestId.empty() || !response.isObject())
            {
                return { TaskProviderStatus::InvalidRequest, "Codex JSON-RPC response is invalid" };
            }
            Json::Value message{ Json::objectValue };
            message["id"] = requestId;
            message["result"] = response;
            return _write(std::move(message), timeoutMilliseconds, cancellationEvent);
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
        const std::scoped_lock lock{ _mutex };
        CodexJsonRpcResult result;
        try
        {
            if (!_transport || !_transport->Running() || timeoutMilliseconds == 0)
            {
                result.Status = TaskProviderStatus::Unavailable;
                result.Error = "Codex notification transport is unavailable";
                return result;
            }
            if (!_notifications.empty())
            {
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = std::move(_notifications.front());
                _notifications.pop_front();
                return result;
            }
            TaskProviderResult failure;
            const auto received = _readMessage(
                GetTickCount64() + timeoutMilliseconds,
                cancellationEvent,
                failure);
            if (!received)
            {
                result.Status = failure.Status;
                result.Error = failure.Error;
                return result;
            }
            if (!_method(*received))
            {
                result.Status = TaskProviderStatus::MalformedResponse;
                result.Error = "Unexpected Codex response arrived without an active request";
                return result;
            }
            result.Status = TaskProviderStatus::Succeeded;
            result.Value = *received;
            return result;
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
        return _transport && _transport->Running();
    }

    void CodexAppServerJsonRpc::Stop() noexcept
    {
        const std::scoped_lock lock{ _mutex };
        if (_transport)
        {
            _transport->Stop();
        }
        _transport.reset();
        _buffer.clear();
        _notifications.clear();
    }
}
