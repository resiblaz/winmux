// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ProtocolJson.h"

#include "ProtocolValidation.h"

#include <json/json.h>

#include <memory>
#include <set>

namespace Microsoft::Terminal::TaskRuntimeProtocol
{
    namespace
    {
        constexpr size_t MaximumCapabilityNameBytes{ 64 };
        constexpr size_t MaximumCapabilityEntries{ 64 };

        template<typename TValue>
        ProtocolJsonDecodeResult<TValue> _decodeFailure(
            const ProtocolJsonStatus status,
            std::string error)
        {
            ProtocolJsonDecodeResult<TValue> result;
            result.Status = status;
            result.Error = std::move(error);
            return result;
        }

        bool _parseJson(const std::string_view payload, Json::Value& root, std::string& error)
        {
            Json::CharReaderBuilder builder;
            builder["allowComments"] = false;
            builder["allowTrailingCommas"] = false;
            builder["collectComments"] = false;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            builder["strictRoot"] = true;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            return reader->parse(payload.data(), payload.data() + payload.size(), &root, &error);
        }

        bool _hasExactMembers(const Json::Value& value, const std::initializer_list<std::string_view> expected)
        {
            if (!value.isObject() || value.size() != expected.size())
            {
                return false;
            }
            return std::ranges::all_of(expected, [&](const auto name) {
                return value.isMember(std::string{ name });
            });
        }

        bool _readVersionRange(const Json::Value& value, ProtocolVersionRange& versions)
        {
            if (!_hasExactMembers(value, { "major", "minimumMinor", "preferredMinor" }) ||
                !value["major"].isUInt() || !value["minimumMinor"].isUInt() ||
                !value["preferredMinor"].isUInt())
            {
                return false;
            }
            const auto major = value["major"].asUInt();
            const auto minimum = value["minimumMinor"].asUInt();
            const auto preferred = value["preferredMinor"].asUInt();
            if (major == 0 || major > UINT16_MAX || minimum > UINT16_MAX ||
                preferred > UINT16_MAX || minimum > preferred)
            {
                return false;
            }
            versions = {
                static_cast<uint16_t>(major),
                static_cast<uint16_t>(minimum),
                static_cast<uint16_t>(preferred),
            };
            return true;
        }

        bool _readCapabilities(
            const Json::Value& value,
            const bool required,
            ProtocolCapabilitySet& capabilities,
            std::vector<std::string>& unknownRequired,
            std::set<std::string>& observedNames)
        {
            if (!value.isArray() || value.size() > MaximumCapabilityEntries)
            {
                return false;
            }
            for (const auto& entry : value)
            {
                if (!entry.isString())
                {
                    return false;
                }
                const auto name = entry.asString();
                if (name.empty() || name.size() > MaximumCapabilityNameBytes ||
                    !observedNames.emplace(name).second)
                {
                    return false;
                }
                if (const auto capability = CapabilityFromName(name))
                {
                    capabilities = AddCapability(capabilities, capability.value());
                }
                else if (required)
                {
                    unknownRequired.emplace_back(name);
                }
            }
            return true;
        }

        Json::Value _writeCapabilities(const ProtocolCapabilitySet capabilities)
        {
            Json::Value value{ Json::arrayValue };
            for (const auto& name : CapabilityNames(capabilities))
            {
                value.append(name);
            }
            return value;
        }

        std::string _writeJson(const Json::Value& value)
        {
            Json::StreamWriterBuilder builder;
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            builder["indentation"] = "";
            return Json::writeString(builder, value);
        }

        bool _validHelloRequest(const HelloRequest& request)
        {
            return IsCanonicalProtocolUuid(request.ClientInstanceId) &&
                   !ProtocolClientKindName(request.ClientKind).empty() &&
                   IsValidRuntimeIdentity(request.ExpectedRuntimeIdentity) &&
                   request.Versions.Major != 0 &&
                   request.Versions.MinimumMinor <= request.Versions.PreferredMinor &&
                   request.MaximumAcceptedControlFrameBytes >= MinimumControlFrameBytes &&
                   request.MaximumAcceptedControlFrameBytes <= MaximumControlFrameBytes &&
                   (request.RequiredCapabilities & request.OptionalCapabilities) == 0 &&
                   ((request.RequiredCapabilities | request.OptionalCapabilities) & ~AllProtocolCapabilities) == 0 &&
                   request.UnknownRequiredCapabilities.empty();
        }

        bool _validHelloResponse(const HelloResponse& response)
        {
            return IsValidRuntimeIdentity(response.RuntimeIdentity) &&
                   IsCanonicalProtocolUuid(response.RuntimeInstanceId) &&
                   response.ProtocolMajor != 0 &&
                   (response.Capabilities & ~AllProtocolCapabilities) == 0 &&
                   response.MaximumControlFrameBytes >= MinimumControlFrameBytes &&
                   response.MaximumControlFrameBytes <= MaximumControlFrameBytes &&
                   response.MaximumTerminalFrameBytes != 0 &&
                   response.MaximumTerminalFrameBytes <= MaximumTerminalFrameBytes &&
                   response.HeartbeatMilliseconds >= 250 && response.HeartbeatMilliseconds <= 60000;
        }
    }

    ProtocolJsonDecodeResult<HelloRequest> DecodeHelloRequest(const std::string_view payload)
    {
        if (payload.size() > MaximumControlFrameBytes)
        {
            return _decodeFailure<HelloRequest>(ProtocolJsonStatus::Oversized, "Handshake JSON exceeds the control payload limit");
        }
        if (!IsValidUtf8(payload))
        {
            return _decodeFailure<HelloRequest>(ProtocolJsonStatus::InvalidUtf8, "Handshake JSON is not valid UTF-8");
        }

        Json::Value root;
        std::string error;
        if (!_parseJson(payload, root, error))
        {
            return _decodeFailure<HelloRequest>(ProtocolJsonStatus::MalformedJson, std::move(error));
        }
        if (!_hasExactMembers(
                root,
                { "type", "clientInstanceId", "clientKind", "expectedRuntimeIdentity", "protocol",
                  "requiredCapabilities", "optionalCapabilities", "maxControlFrameBytes" }) ||
            !root["type"].isString() || root["type"].asString() != "hello" ||
            !root["clientInstanceId"].isString() || !root["clientKind"].isString() ||
            !root["expectedRuntimeIdentity"].isString() || !root["maxControlFrameBytes"].isUInt())
        {
            return _decodeFailure<HelloRequest>(ProtocolJsonStatus::InvalidSchema, "Hello request fields are invalid");
        }

        HelloRequest request;
        request.ClientInstanceId = root["clientInstanceId"].asString();
        request.ExpectedRuntimeIdentity = root["expectedRuntimeIdentity"].asString();
        const auto clientKind = ProtocolClientKindFromName(root["clientKind"].asString());
        const auto maximumFrameBytes = root["maxControlFrameBytes"].asUInt();
        std::set<std::string> observedCapabilities;
        if (!clientKind || !_readVersionRange(root["protocol"], request.Versions) ||
            !_readCapabilities(
                root["requiredCapabilities"],
                true,
                request.RequiredCapabilities,
                request.UnknownRequiredCapabilities,
                observedCapabilities) ||
            !_readCapabilities(
                root["optionalCapabilities"],
                false,
                request.OptionalCapabilities,
                request.UnknownRequiredCapabilities,
                observedCapabilities) ||
            !IsCanonicalProtocolUuid(request.ClientInstanceId) ||
            !IsValidRuntimeIdentity(request.ExpectedRuntimeIdentity) ||
            maximumFrameBytes < MinimumControlFrameBytes || maximumFrameBytes > MaximumControlFrameBytes)
        {
            return _decodeFailure<HelloRequest>(ProtocolJsonStatus::InvalidSchema, "Hello request values are invalid");
        }
        request.ClientKind = clientKind.value();
        request.MaximumAcceptedControlFrameBytes = maximumFrameBytes;

        ProtocolJsonDecodeResult<HelloRequest> result;
        result.Status = ProtocolJsonStatus::Succeeded;
        result.Value = std::move(request);
        return result;
    }

    ProtocolJsonDecodeResult<HelloResponse> DecodeHelloResponse(const std::string_view payload)
    {
        if (payload.size() > MaximumControlFrameBytes)
        {
            return _decodeFailure<HelloResponse>(ProtocolJsonStatus::Oversized, "Handshake JSON exceeds the control payload limit");
        }
        if (!IsValidUtf8(payload))
        {
            return _decodeFailure<HelloResponse>(ProtocolJsonStatus::InvalidUtf8, "Handshake JSON is not valid UTF-8");
        }

        Json::Value root;
        std::string error;
        if (!_parseJson(payload, root, error))
        {
            return _decodeFailure<HelloResponse>(ProtocolJsonStatus::MalformedJson, std::move(error));
        }
        if (!_hasExactMembers(
                root,
                { "type", "runtimeIdentity", "runtimeInstanceId", "protocol", "capabilities",
                  "maxControlFrameBytes", "maxTerminalFrameBytes", "heartbeatMilliseconds" }) ||
            !root["type"].isString() || root["type"].asString() != "hello_ack" ||
            !root["runtimeIdentity"].isString() || !root["runtimeInstanceId"].isString() ||
            !root["maxControlFrameBytes"].isUInt() || !root["maxTerminalFrameBytes"].isUInt() ||
            !root["heartbeatMilliseconds"].isUInt() ||
            !_hasExactMembers(root["protocol"], { "major", "minor" }) ||
            !root["protocol"]["major"].isUInt() || !root["protocol"]["minor"].isUInt())
        {
            return _decodeFailure<HelloResponse>(ProtocolJsonStatus::InvalidSchema, "Hello response fields are invalid");
        }

        HelloResponse response;
        response.RuntimeIdentity = root["runtimeIdentity"].asString();
        response.RuntimeInstanceId = root["runtimeInstanceId"].asString();
        const auto major = root["protocol"]["major"].asUInt();
        const auto minor = root["protocol"]["minor"].asUInt();
        response.MaximumControlFrameBytes = root["maxControlFrameBytes"].asUInt();
        response.MaximumTerminalFrameBytes = root["maxTerminalFrameBytes"].asUInt();
        response.HeartbeatMilliseconds = root["heartbeatMilliseconds"].asUInt();
        std::set<std::string> observedCapabilities;
        std::vector<std::string> ignoredUnknown;
        if (major == 0 || major > UINT16_MAX || minor > UINT16_MAX ||
            !_readCapabilities(
                root["capabilities"],
                false,
                response.Capabilities,
                ignoredUnknown,
                observedCapabilities))
        {
            return _decodeFailure<HelloResponse>(ProtocolJsonStatus::InvalidSchema, "Hello response values are invalid");
        }
        response.ProtocolMajor = static_cast<uint16_t>(major);
        response.ProtocolMinor = static_cast<uint16_t>(minor);
        if (!_validHelloResponse(response))
        {
            return _decodeFailure<HelloResponse>(ProtocolJsonStatus::InvalidSchema, "Hello response values are invalid");
        }

        ProtocolJsonDecodeResult<HelloResponse> result;
        result.Status = ProtocolJsonStatus::Succeeded;
        result.Value = std::move(response);
        return result;
    }

    ProtocolJsonEncodeResult EncodeHelloRequest(const HelloRequest& request)
    {
        ProtocolJsonEncodeResult result;
        if (!_validHelloRequest(request))
        {
            result.Error = "Hello request is invalid";
            return result;
        }
        Json::Value root{ Json::objectValue };
        root["type"] = "hello";
        root["clientInstanceId"] = request.ClientInstanceId;
        root["clientKind"] = std::string{ ProtocolClientKindName(request.ClientKind) };
        root["expectedRuntimeIdentity"] = request.ExpectedRuntimeIdentity;
        root["protocol"]["major"] = request.Versions.Major;
        root["protocol"]["minimumMinor"] = request.Versions.MinimumMinor;
        root["protocol"]["preferredMinor"] = request.Versions.PreferredMinor;
        root["requiredCapabilities"] = _writeCapabilities(request.RequiredCapabilities);
        root["optionalCapabilities"] = _writeCapabilities(request.OptionalCapabilities);
        root["maxControlFrameBytes"] = request.MaximumAcceptedControlFrameBytes;
        result.Payload = _writeJson(root);
        result.Succeeded = result.Payload.size() <= MaximumControlFrameBytes && IsValidUtf8(result.Payload);
        if (!result.Succeeded)
        {
            result.Payload.clear();
            result.Error = "Encoded Hello request exceeds protocol bounds";
        }
        return result;
    }

    ProtocolJsonEncodeResult EncodeHelloResponse(const HelloResponse& response)
    {
        ProtocolJsonEncodeResult result;
        if (!_validHelloResponse(response))
        {
            result.Error = "Hello response is invalid";
            return result;
        }
        Json::Value root{ Json::objectValue };
        root["type"] = "hello_ack";
        root["runtimeIdentity"] = response.RuntimeIdentity;
        root["runtimeInstanceId"] = response.RuntimeInstanceId;
        root["protocol"]["major"] = response.ProtocolMajor;
        root["protocol"]["minor"] = response.ProtocolMinor;
        root["capabilities"] = _writeCapabilities(response.Capabilities);
        root["maxControlFrameBytes"] = response.MaximumControlFrameBytes;
        root["maxTerminalFrameBytes"] = response.MaximumTerminalFrameBytes;
        root["heartbeatMilliseconds"] = response.HeartbeatMilliseconds;
        result.Payload = _writeJson(root);
        result.Succeeded = result.Payload.size() <= MaximumControlFrameBytes && IsValidUtf8(result.Payload);
        if (!result.Succeeded)
        {
            result.Payload.clear();
            result.Error = "Encoded Hello response exceeds protocol bounds";
        }
        return result;
    }
}
