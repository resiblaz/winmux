// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "CodexAppServerTransport.h"

#include <json/json.h>

namespace TaskProviderLocalTests
{
    struct FakeCodexAppServerState
    {
        mutable std::mutex Mutex;
        std::deque<std::string> Reads;
        std::vector<Json::Value> Writes;
        std::unordered_map<std::string, int64_t> RpcErrors;
        std::unordered_set<std::string> SuppressedResponses;
        Microsoft::Terminal::TaskProvider::CodexTransportReadKind EmptyReadKind{
            Microsoft::Terminal::TaskProvider::CodexTransportReadKind::Timeout
        };
        bool Started{};
        bool Stopped{};

        void Queue(const Json::Value& value);
        [[nodiscard]] std::vector<Json::Value> SnapshotWrites() const;
    };

    [[nodiscard]] Microsoft::Terminal::TaskProvider::CodexAppServerTransportFactory
    MakeFakeCodexAppServerTransportFactory(
        std::shared_ptr<FakeCodexAppServerState> state);
}
