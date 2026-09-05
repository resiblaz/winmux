// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "CodexAppServerTransport.h"

#include <json/json.h>

#include <deque>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <wil/resource.h>

namespace Microsoft::Terminal::TaskProvider
{
    inline constexpr size_t MaximumCodexJsonLineBytes{ 1024 * 1024 };
    inline constexpr size_t MaximumCodexQueuedNotifications{ 4096 };
    inline constexpr size_t MaximumCodexPendingRequests{ 128 };

    struct CodexJsonRpcResult : TaskProviderResult
    {
        Json::Value Value;
        int64_t RpcCode{};
    };

    class CodexAppServerJsonRpc final
    {
    public:
        explicit CodexAppServerJsonRpc(
            CodexAppServerTransportFactory transportFactory);
        ~CodexAppServerJsonRpc();

        CodexAppServerJsonRpc(const CodexAppServerJsonRpc&) = delete;
        CodexAppServerJsonRpc& operator=(const CodexAppServerJsonRpc&) = delete;

        [[nodiscard]] TaskProviderResult Start() noexcept;
        [[nodiscard]] CodexJsonRpcResult Request(
            std::string_view method,
            const Json::Value& params,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent) noexcept;
        [[nodiscard]] TaskProviderResult Notify(
            std::string_view method,
            const Json::Value& params,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent) noexcept;
        [[nodiscard]] TaskProviderResult Respond(
            const Json::Value& requestId,
            const Json::Value& result,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent) noexcept;
        [[nodiscard]] CodexJsonRpcResult NextNotification(
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent) noexcept;
        [[nodiscard]] bool Running() const noexcept;
        void Stop() noexcept;

    private:
        CodexAppServerTransportFactory _factory;
        std::unique_ptr<ICodexAppServerTransport> _transport;
        mutable std::mutex _mutex;
        std::mutex _lifecycleMutex;
        std::mutex _readMutex;
        std::timed_mutex _writeMutex;
        std::condition_variable _changed;
        wil::unique_handle _stopEvent;
        std::string _buffer;
        std::deque<Json::Value> _notifications;
        std::unordered_map<uint64_t, std::optional<CodexJsonRpcResult>> _pending;
        std::optional<TaskProviderResult> _failure;
        uint64_t _nextRequestId{ 1 };
        uint64_t _epoch{ 1 };

        [[nodiscard]] std::optional<Json::Value> _readMessage(
            uint64_t deadline,
            HANDLE cancellationEvent,
            TaskProviderResult& failure);
        [[nodiscard]] TaskProviderResult _write(
            Json::Value message,
            uint32_t timeoutMilliseconds,
            HANDLE cancellationEvent,
            uint64_t epoch);
        bool _pumpOnce(uint64_t epoch) noexcept;
        void _stopLocked() noexcept;
    };
}
