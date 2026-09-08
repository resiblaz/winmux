// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "ProtocolRpcJson.h"
#include "ProtocolValidation.h"
#include "ProtocolVersion.h"
#include <json/json.h>
#include <array>
#include <algorithm>
#include <limits>
#include <memory>

namespace Microsoft::Terminal::TaskRuntimeProtocol::workspace_json
{
    inline constexpr uint64_t MaxVersion = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    inline bool Exact(const Json::Value& value, std::initializer_list<std::string_view> keys)
    {
        return value.isObject() && value.size() == keys.size() &&
               std::ranges::all_of(keys, [&](auto key) { return value.isMember(std::string{ key }); });
    }
    inline bool Text(const Json::Value& value, std::string& result, size_t limit, bool required = false)
    {
        if (!value.isString()) return false;
        result = value.asString();
        return result.size() <= limit && (!required || !result.empty()) && result.find('\0') == std::string::npos && IsValidUtf8(result);
    }
    inline bool Id(const Json::Value& value, std::string& result, bool allowEmpty = false)
    { return Text(value, result, 36) && ((allowEmpty && result.empty()) || IsCanonicalProtocolUuid(result)); }
    inline bool Id(const Json::Value& value, std::optional<std::string>& result)
    {
        if (value.isNull()) { result.reset(); return true; }
        std::string id; if (!Id(value, id)) return false; result = std::move(id); return true;
    }
    inline bool UInt(const Json::Value& value, uint64_t& result, uint64_t maximum = MaxVersion)
    { if (!value.isUInt64() || value.asUInt64() > maximum) return false; result = value.asUInt64(); return true; }
    inline bool Int(const Json::Value& value, int64_t& result)
    { if (!value.isInt64()) return false; result = value.asInt64(); return true; }
    inline bool Bool(const Json::Value& value, bool& result)
    { if (!value.isBool()) return false; result = value.asBool(); return true; }
    template<typename T> bool Enum(const Json::Value& value, T& result, uint64_t maximum)
    { uint64_t number{}; if (!UInt(value, number, maximum)) return false; result = static_cast<T>(number); return true; }
    template<typename T> bool Enum(const Json::Value& value, std::optional<T>& result, uint64_t maximum)
    { if (value.isNull()) { result.reset(); return true; } T parsed{}; if (!Enum(value, parsed, maximum)) return false; result = parsed; return true; }
    inline Json::Value Optional(const std::optional<std::string>& value) { return value ? Json::Value{ *value } : Json::Value{}; }
    template<typename T> Json::Value OptionalEnum(const std::optional<T>& value) { return value ? Json::Value{ static_cast<uint32_t>(*value) } : Json::Value{}; }
    inline Json::Value Numbers(const std::array<uint64_t, 7>& values)
    { Json::Value result{ Json::arrayValue }; for (auto value : values) result.append(Json::UInt64{ value }); return result; }
    inline bool Numbers(const Json::Value& value, std::array<uint64_t, 7>& result)
    {
        if (!value.isArray() || value.size() != result.size()) return false;
        for (Json::ArrayIndex i = 0; i < result.size(); ++i) if (!UInt(value[i], result[i])) return false;
        return true;
    }
    template<typename T, typename Reader> ProtocolRpcJsonDecodeResult<T> Decode(std::string_view payload, Reader read)
    {
        ProtocolRpcJsonDecodeResult<T> result;
        result.Status = ProtocolRpcJsonStatus::InvalidSchema; result.Error = "Project or board payload schema is invalid";
        if (payload.size() > MaximumControlFrameBytes || !IsValidUtf8(payload))
        { result.Status = payload.size() > MaximumControlFrameBytes ? ProtocolRpcJsonStatus::Oversized : ProtocolRpcJsonStatus::InvalidUtf8; return result; }
        Json::CharReaderBuilder builder;
        builder["allowComments"] = false; builder["allowTrailingCommas"] = false;
        builder["failIfExtra"] = true; builder["rejectDupKeys"] = true;
        const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
        Json::Value root; std::string error;
        if (!reader || !reader->parse(payload.data(), payload.data() + payload.size(), &root, &error))
        { result.Status = ProtocolRpcJsonStatus::MalformedJson; return result; }
        if (read(root, result.Value)) { result.Status = ProtocolRpcJsonStatus::Succeeded; result.Error.clear(); }
        return result;
    }
    template<typename Decoder> ProtocolRpcJsonEncodeResult Encode(const Json::Value& root, Decoder decode)
    {
        Json::StreamWriterBuilder writer; writer["emitUTF8"] = true; writer["indentation"] = ""; writer["commentStyle"] = "None";
        auto payload = Json::writeString(writer, root); const auto checked = decode(payload);
        if (!checked.Succeeded()) return { false, {}, checked.Error };
        return { true, std::move(payload), {} };
    }
}
