// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "FakeCodexAppServerTransport.h"

using namespace Microsoft::Terminal::TaskProvider;

namespace TaskProviderLocalTests
{
    namespace
    {
        std::string _json(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            auto result = Json::writeString(builder, value);
            result.push_back('\n');
            return result;
        }

        bool _parse(const std::string_view value, Json::Value& root)
        {
            Json::CharReaderBuilder builder;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            std::string error;
            return reader && reader->parse(value.data(), value.data() + value.size(), &root, &error);
        }

        Json::Value _result(const std::string& method, const Json::Value& request)
        {
            Json::Value value{ Json::objectValue };
            if (method == "initialize")
            {
                value["codexHome"] = "C:/fake/codex";
                value["platformFamily"] = "windows";
                value["platformOs"] = "windows";
                value["userAgent"] = "codex_cli_rs/0.151.0";
            }
            else if (method == "thread/start")
            {
                value["thread"]["id"] = "provider-thread-1";
            }
            else if (method == "thread/resume")
            {
                value["thread"]["id"] = request["params"]["threadId"];
            }
            else if (method == "turn/start")
            {
                value["turn"]["id"] = "provider-turn-1";
            }
            return value;
        }

        class FakeCodexAppServerTransport final : public ICodexAppServerTransport
        {
        public:
            explicit FakeCodexAppServerTransport(std::shared_ptr<FakeCodexAppServerState> state) :
                _state{ std::move(state) }
            {
            }

            TaskProviderResult Start() noexcept override
            {
                const std::scoped_lock lock{ _state->Mutex };
                _state->Started = true;
                _state->Stopped = false;
                return { TaskProviderStatus::Succeeded, {} };
            }

            TaskProviderResult Write(
                const std::string_view bytes,
                std::chrono::milliseconds,
                HANDLE) noexcept override
            {
                try
                {
                    auto line = bytes;
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
                    Json::Value request;
                    if (!_parse(line, request) || !request.isObject())
                    {
                        return { TaskProviderStatus::MalformedResponse, "Fake received malformed JSON" };
                    }
                    const std::scoped_lock lock{ _state->Mutex };
                    _state->Writes.emplace_back(request);
                    if (!request.isMember("id") || !request.isMember("method") || !request["method"].isString())
                    {
                        return { TaskProviderStatus::Succeeded, {} };
                    }
                    const auto method = request["method"].asString();
                    if (_state->SuppressedResponses.contains(method))
                    {
                        return { TaskProviderStatus::Succeeded, {} };
                    }
                    Json::Value response{ Json::objectValue };
                    response["id"] = request["id"];
                    if (const auto error = _state->RpcErrors.find(method); error != _state->RpcErrors.end())
                    {
                        response["error"]["code"] = Json::Int64{ error->second };
                        response["error"]["message"] = "scripted rejection";
                    }
                    else
                    {
                        response["result"] = _result(method, request);
                    }
                    _state->Reads.emplace_back(_json(response));
                    return { TaskProviderStatus::Succeeded, {} };
                }
                catch (...)
                {
                    return { TaskProviderStatus::ResourceFailure, "Fake write failed" };
                }
            }

            CodexTransportReadResult Read(std::chrono::milliseconds, HANDLE cancellationEvent) noexcept override
            {
                if (cancellationEvent && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0)
                {
                    return { CodexTransportReadKind::Cancelled };
                }
                const std::scoped_lock lock{ _state->Mutex };
                if (!_state->Reads.empty())
                {
                    auto bytes = std::move(_state->Reads.front());
                    _state->Reads.pop_front();
                    return { CodexTransportReadKind::Data, std::move(bytes) };
                }
                return { _state->EmptyReadKind };
            }

            bool Running() const noexcept override
            {
                const std::scoped_lock lock{ _state->Mutex };
                return _state->Started && !_state->Stopped;
            }

            void Stop() noexcept override
            {
                const std::scoped_lock lock{ _state->Mutex };
                _state->Stopped = true;
            }

        private:
            std::shared_ptr<FakeCodexAppServerState> _state;
        };
    }

    void FakeCodexAppServerState::Queue(const Json::Value& value)
    {
        const std::scoped_lock lock{ Mutex };
        Reads.emplace_back(_json(value));
    }

    std::vector<Json::Value> FakeCodexAppServerState::SnapshotWrites() const
    {
        const std::scoped_lock lock{ Mutex };
        return Writes;
    }

    CodexAppServerTransportFactory MakeFakeCodexAppServerTransportFactory(
        std::shared_ptr<FakeCodexAppServerState> state)
    {
        return [state = std::move(state)] {
            return std::make_unique<FakeCodexAppServerTransport>(state);
        };
    }
}
