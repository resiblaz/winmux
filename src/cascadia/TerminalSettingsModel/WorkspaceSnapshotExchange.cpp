// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceSnapshotExchange.h"

#include "ActionArgs.h"
#include "ApplicationState.h"
#include "../../types/inc/utils.hpp"

using namespace ::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    namespace
    {
        constexpr std::array<std::string_view, 4> EnvelopeFields{
            "format",
            "formatVersion",
            "exportedAt",
            "snapshot",
        };

        constexpr std::array<std::string_view, 13> SnapshotFields{
            "version",
            "revision",
            "id",
            "workspaceId",
            "name",
            "rootDirectory",
            "createdAt",
            "updatedAt",
            "lastOpenedAt",
            "actions",
            "pinned",
            "agentResumeEntries",
            "gitBinding",
        };

        int64_t _unixMillisecondsNow() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        bool _validTimestamp(const int64_t value) noexcept
        {
            return value >= 0 && value <= WorkspaceSnapshotValidator::MaximumUnixMilliseconds;
        }

        template<size_t Size>
        bool _hasOnlyFields(const Json::Value& value,
                            const std::array<std::string_view, Size>& allowed) noexcept
        try
        {
            if (!value.isObject())
            {
                return false;
            }
            for (const auto& member : value.getMemberNames())
            {
                if (std::ranges::find(allowed, std::string_view{ member }) == allowed.end())
                {
                    return false;
                }
            }
            return true;
        }
        catch (...)
        {
            return false;
        }

        bool _hasAllEnvelopeFields(const Json::Value& value) noexcept
        {
            return std::ranges::all_of(EnvelopeFields, [&](const auto field) {
                return value.isMember(field.data());
            });
        }

        bool _parseStrictJson(const std::string& content, Json::Value& root) noexcept
        try
        {
            Json::CharReaderBuilder builder;
            builder["collectComments"] = false;
            builder["allowComments"] = false;
            builder["strictRoot"] = true;
            builder["failIfExtra"] = true;
            builder["rejectDupKeys"] = true;
            builder["allowSpecialFloats"] = false;
            builder["allowTrailingCommas"] = false;
            std::string errors;
            const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
            return reader->parse(content.data(), content.data() + content.size(), &root, &errors) && root.isObject();
        }
        catch (...)
        {
            return false;
        }

        WorkspaceSnapshotExchangeStatus _readBounded(const std::filesystem::path& path,
                                                      std::string& content) noexcept
        {
            content.clear();
            if (path.empty())
            {
                return WorkspaceSnapshotExchangeStatus::InvalidEnvelope;
            }

            wil::unique_hfile file{ CreateFileW(path.c_str(),
                                                GENERIC_READ,
                                                FILE_SHARE_READ,
                                                nullptr,
                                                OPEN_EXISTING,
                                                FILE_FLAG_SEQUENTIAL_SCAN,
                                                nullptr) };
            if (!file)
            {
                return GetLastError() == ERROR_FILE_NOT_FOUND ?
                           WorkspaceSnapshotExchangeStatus::NotFound :
                           WorkspaceSnapshotExchangeStatus::IoError;
            }

            FILE_ATTRIBUTE_TAG_INFO tagInfo{};
            if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tagInfo, sizeof(tagInfo)) ||
                WI_IsAnyFlagSet(tagInfo.FileAttributes, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            {
                return WorkspaceSnapshotExchangeStatus::IoError;
            }

            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0)
            {
                return WorkspaceSnapshotExchangeStatus::IoError;
            }
            if (static_cast<uint64_t>(size.QuadPart) > WorkspaceSnapshotExchange::MaximumEnvelopeBytes)
            {
                return WorkspaceSnapshotExchangeStatus::Oversized;
            }

            content.resize(static_cast<size_t>(size.QuadPart));
            size_t offset{};
            while (offset < content.size())
            {
                DWORD read{};
                const auto remaining = std::min<size_t>(content.size() - offset, std::numeric_limits<DWORD>::max());
                if (!ReadFile(file.get(), content.data() + offset, gsl::narrow<DWORD>(remaining), &read, nullptr) || read == 0)
                {
                    content.clear();
                    return WorkspaceSnapshotExchangeStatus::IoError;
                }
                offset += read;
            }
            return WorkspaceSnapshotExchangeStatus::Succeeded;
        }

        bool _writeAll(const HANDLE file, const std::string_view content) noexcept
        {
            size_t offset{};
            while (offset < content.size())
            {
                DWORD written{};
                const auto remaining = std::min<size_t>(content.size() - offset, std::numeric_limits<DWORD>::max());
                if (!WriteFile(file, content.data() + offset, gsl::narrow<DWORD>(remaining), &written, nullptr) || written == 0)
                {
                    return false;
                }
                offset += written;
            }
            return FlushFileBuffers(file) != FALSE;
        }

        bool _isDirectoryWithoutReparsePoint(const std::filesystem::path& path) noexcept
        {
            const auto attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   WI_IsFlagSet(attributes, FILE_ATTRIBUTE_DIRECTORY) &&
                   WI_IsFlagClear(attributes, FILE_ATTRIBUTE_REPARSE_POINT);
        }

        WorkspaceSnapshotExchangeStatus _writeAtomic(const std::filesystem::path& destination,
                                                      const std::string_view content,
                                                      const bool force) noexcept
        try
        {
            if (destination.empty() || destination.filename().empty())
            {
                return WorkspaceSnapshotExchangeStatus::IoError;
            }

            auto parent = destination.parent_path();
            if (parent.empty())
            {
                parent = std::filesystem::current_path();
            }
            if (!_isDirectoryWithoutReparsePoint(parent))
            {
                return WorkspaceSnapshotExchangeStatus::IoError;
            }

            const auto existingAttributes = GetFileAttributesW(destination.c_str());
            if (existingAttributes != INVALID_FILE_ATTRIBUTES)
            {
                if (!force)
                {
                    return WorkspaceSnapshotExchangeStatus::DestinationExists;
                }
                if (WI_IsAnyFlagSet(existingAttributes, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                {
                    return WorkspaceSnapshotExchangeStatus::IoError;
                }
            }

            GUID temporaryId{};
            if (FAILED(CoCreateGuid(&temporaryId)))
            {
                return WorkspaceSnapshotExchangeStatus::IoError;
            }
            const auto temporaryPath = parent /
                                       fmt::format(FMT_COMPILE(L"{}.{}.tmp"),
                                                   destination.filename().native(),
                                                   ::Microsoft::Console::Utils::GuidToPlainString(temporaryId));

            wil::unique_hfile temporary{ CreateFileW(temporaryPath.c_str(),
                                                     GENERIC_WRITE,
                                                     0,
                                                     nullptr,
                                                     CREATE_NEW,
                                                     FILE_ATTRIBUTE_TEMPORARY,
                                                     nullptr) };
            if (!temporary)
            {
                return WorkspaceSnapshotExchangeStatus::IoError;
            }
            auto cleanup = wil::scope_exit([&]() { DeleteFileW(temporaryPath.c_str()); });
            if (!_writeAll(temporary.get(), content))
            {
                return WorkspaceSnapshotExchangeStatus::IoError;
            }
            temporary.reset();

            const auto targetAttributes = GetFileAttributesW(destination.c_str());
            if (targetAttributes != INVALID_FILE_ATTRIBUTES)
            {
                if (!force)
                {
                    return WorkspaceSnapshotExchangeStatus::DestinationExists;
                }
                if (WI_IsAnyFlagSet(targetAttributes, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
                    !ReplaceFileW(destination.c_str(), temporaryPath.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
                {
                    return WorkspaceSnapshotExchangeStatus::IoError;
                }
            }
            else if (!MoveFileExW(temporaryPath.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
            {
                if (!force && GetLastError() == ERROR_ALREADY_EXISTS)
                {
                    return WorkspaceSnapshotExchangeStatus::DestinationExists;
                }
                return WorkspaceSnapshotExchangeStatus::IoError;
            }

            cleanup.release();
            return WorkspaceSnapshotExchangeStatus::Succeeded;
        }
        catch (...)
        {
            return WorkspaceSnapshotExchangeStatus::IoError;
        }

        WorkspaceSnapshotExchangeResult _fromStoreResult(const WorkspaceSnapshotStoreRecord& result) noexcept
        {
            WorkspaceSnapshotExchangeResult exchangeResult;
            exchangeResult.UnderlyingStoreStatus = result.Status;
            exchangeResult.Snapshot = result.Snapshot;
            switch (result.Status)
            {
            case WorkspaceSnapshotStoreStatus::Succeeded:
                exchangeResult.Status = WorkspaceSnapshotExchangeStatus::Succeeded;
                break;
            case WorkspaceSnapshotStoreStatus::Unchanged:
                exchangeResult.Status = WorkspaceSnapshotExchangeStatus::Unchanged;
                break;
            case WorkspaceSnapshotStoreStatus::NotFound:
                exchangeResult.Status = WorkspaceSnapshotExchangeStatus::NotFound;
                break;
            case WorkspaceSnapshotStoreStatus::Conflict:
                exchangeResult.Status = WorkspaceSnapshotExchangeStatus::Conflict;
                break;
            case WorkspaceSnapshotStoreStatus::Oversized:
                exchangeResult.Status = WorkspaceSnapshotExchangeStatus::Oversized;
                break;
            case WorkspaceSnapshotStoreStatus::CapacityExceeded:
                exchangeResult.Status = WorkspaceSnapshotExchangeStatus::CapacityExceeded;
                break;
            case WorkspaceSnapshotStoreStatus::ScopeUnavailable:
                exchangeResult.Status = WorkspaceSnapshotExchangeStatus::ScopeUnavailable;
                break;
            case WorkspaceSnapshotStoreStatus::Invalid:
            case WorkspaceSnapshotStoreStatus::Corrupt:
                exchangeResult.Status = WorkspaceSnapshotExchangeStatus::InvalidSnapshot;
                break;
            default:
                exchangeResult.Status = WorkspaceSnapshotExchangeStatus::IoError;
                break;
            }
            return exchangeResult;
        }

        WorkspaceSnapshotExchangeResult _validateProfiles(
            const Model::WorkspaceSnapshot& snapshot,
            const WorkspaceSnapshotExchange::ProfileExistsCallback& profileExists) noexcept
        {
            if (!profileExists)
            {
                return { WorkspaceSnapshotExchangeStatus::ProfileLookupUnavailable };
            }

            try
            {
                for (const auto& action : snapshot.Actions())
                {
                    Model::INewContentArgs content{ nullptr };
                    switch (action.Action())
                    {
                    case Model::ShortcutAction::NewTab:
                        content = action.Args().as<Model::NewTabArgs>().ContentArgs();
                        break;
                    case Model::ShortcutAction::SplitPane:
                        content = action.Args().as<Model::SplitPaneArgs>().ContentArgs();
                        break;
                    default:
                        continue;
                    }

                    const auto profile = content.as<Model::NewTerminalArgs>().Profile();
                    if (!profileExists(profile))
                    {
                        WorkspaceSnapshotExchangeResult result{ WorkspaceSnapshotExchangeStatus::ProfileNotFound };
                        result.MissingProfile = profile;
                        return result;
                    }
                }
                return { WorkspaceSnapshotExchangeStatus::Succeeded };
            }
            catch (...)
            {
                return { WorkspaceSnapshotExchangeStatus::ProfileLookupUnavailable };
            }
        }

        bool _newGuid(winrt::guid& id) noexcept
        {
            GUID value{};
            if (FAILED(CoCreateGuid(&value)))
            {
                return false;
            }
            id = value;
            return true;
        }

        Model::WorkspaceSnapshot _makeCopy(const Model::WorkspaceSnapshot& source,
                                           const int64_t now) noexcept
        try
        {
            auto copy = WorkspaceSnapshotValidator::DeepCopy(source);
            if (!copy)
            {
                return nullptr;
            }

            winrt::guid snapshotId{};
            winrt::guid workspaceId{};
            if (!_newGuid(snapshotId) || !_newGuid(workspaceId))
            {
                return nullptr;
            }
            copy.Id(snapshotId);
            copy.WorkspaceId(workspaceId);
            copy.Revision(0);
            copy.CreatedAt(now);
            copy.UpdatedAt(now);
            copy.LastOpenedAt(0);

            if (const auto entries = copy.AgentResumeEntries())
            {
                auto rewritten = winrt::single_threaded_vector<Model::AgentResumeInfo>();
                for (auto entry : entries)
                {
                    entry.WorkspaceId = workspaceId;
                    rewritten.Append(std::move(entry));
                }
                copy.AgentResumeEntries(std::move(rewritten));
            }
            return copy;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    WorkspaceSnapshotExchangeResult WorkspaceSnapshotExchange::ExportToFile(
        const WorkspaceSnapshotStore& store,
        const winrt::guid& snapshotId,
        const std::filesystem::path& destination,
        const bool force,
        const std::optional<int64_t> exportedAt) noexcept
    try
    {
        const auto loaded = store.Load(snapshotId);
        if (loaded.Status != WorkspaceSnapshotStoreStatus::Succeeded || !loaded.Snapshot)
        {
            return _fromStoreResult(loaded);
        }

        const auto validation = WorkspaceSnapshotValidator::Validate(loaded.Snapshot);
        if (!validation)
        {
            WorkspaceSnapshotExchangeResult result{ WorkspaceSnapshotExchangeStatus::InvalidSnapshot };
            result.ValidationError = validation.Error;
            return result;
        }

        const auto timestamp = exportedAt.value_or(_unixMillisecondsNow());
        if (!_validTimestamp(timestamp))
        {
            return { WorkspaceSnapshotExchangeStatus::InvalidEnvelope };
        }

        Json::Value envelope{ Json::objectValue };
        envelope["format"] = Format.data();
        envelope["formatVersion"] = FormatVersion;
        envelope["exportedAt"] = Json::Value{ static_cast<Json::Int64>(timestamp) };
        envelope["snapshot"] = WorkspaceSnapshot::ToJsonValue(loaded.Snapshot);

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        builder["commentStyle"] = "None";
        builder["emitUTF8"] = true;
        const auto content = Json::writeString(builder, envelope);
        if (content.size() > MaximumEnvelopeBytes)
        {
            return { WorkspaceSnapshotExchangeStatus::Oversized };
        }

        const auto status = _writeAtomic(destination, content, force);
        WorkspaceSnapshotExchangeResult result{ status };
        if (status == WorkspaceSnapshotExchangeStatus::Succeeded)
        {
            result.Snapshot = WorkspaceSnapshotValidator::DeepCopy(loaded.Snapshot);
            result.UnderlyingStoreStatus = WorkspaceSnapshotStoreStatus::Succeeded;
        }
        return result;
    }
    catch (...)
    {
        return { WorkspaceSnapshotExchangeStatus::IoError };
    }

    WorkspaceSnapshotExchangeResult WorkspaceSnapshotExchange::ImportFromFile(
        WorkspaceSnapshotStore& store,
        const std::filesystem::path& source,
        const WorkspaceSnapshotImportMode mode,
        const ProfileExistsCallback& profileExists,
        const std::optional<int64_t> importedAt) noexcept
    try
    {
        std::string content;
        const auto readStatus = _readBounded(source, content);
        if (readStatus != WorkspaceSnapshotExchangeStatus::Succeeded)
        {
            return { readStatus };
        }

        Json::Value envelope;
        if (!_parseStrictJson(content, envelope))
        {
            return { WorkspaceSnapshotExchangeStatus::MalformedJson };
        }
        if (!_hasOnlyFields(envelope, EnvelopeFields))
        {
            return { WorkspaceSnapshotExchangeStatus::UnknownField };
        }
        if (!_hasAllEnvelopeFields(envelope) ||
            !envelope["format"].isString() ||
            !envelope["formatVersion"].isUInt() ||
            !envelope["exportedAt"].isInt64() ||
            !envelope["snapshot"].isObject())
        {
            return { WorkspaceSnapshotExchangeStatus::InvalidEnvelope };
        }
        if (envelope["format"].asString() != Format)
        {
            return { WorkspaceSnapshotExchangeStatus::UnsupportedFormat };
        }
        if (envelope["formatVersion"].asUInt() != FormatVersion)
        {
            return { WorkspaceSnapshotExchangeStatus::UnsupportedFormatVersion };
        }
        if (!_validTimestamp(envelope["exportedAt"].asInt64()))
        {
            return { WorkspaceSnapshotExchangeStatus::InvalidEnvelope };
        }

        const auto& snapshotJson = envelope["snapshot"];
        if (!_hasOnlyFields(snapshotJson, SnapshotFields))
        {
            return { WorkspaceSnapshotExchangeStatus::UnknownField };
        }

        Model::WorkspaceSnapshot snapshot{ nullptr };
        try
        {
            snapshot = WorkspaceSnapshot::FromJsonValue(snapshotJson);
        }
        catch (...)
        {
            return { WorkspaceSnapshotExchangeStatus::InvalidSnapshot };
        }

        if (!snapshot || !WorkspaceSnapshotValidator::IsSafeActions(snapshot.Actions()))
        {
            return { WorkspaceSnapshotExchangeStatus::UnsafeAction };
        }
        const auto validation = WorkspaceSnapshotValidator::Validate(snapshot);
        if (!validation)
        {
            WorkspaceSnapshotExchangeResult result{
                validation.Error == WorkspaceSnapshotValidationError::InvalidActions ?
                    WorkspaceSnapshotExchangeStatus::UnsafeAction :
                    WorkspaceSnapshotExchangeStatus::InvalidSnapshot
            };
            result.ValidationError = validation.Error;
            return result;
        }

        const auto profiles = _validateProfiles(snapshot, profileExists);
        if (!profiles)
        {
            return profiles;
        }

        switch (mode)
        {
        case WorkspaceSnapshotImportMode::Fail:
            return _fromStoreResult(store.Commit(snapshot, std::nullopt));
        case WorkspaceSnapshotImportMode::Replace:
        {
            const auto existing = store.Load(snapshot.Id());
            if (existing.Status != WorkspaceSnapshotStoreStatus::Succeeded || !existing.Snapshot)
            {
                return _fromStoreResult(existing);
            }
            if (existing.Snapshot.Id() != snapshot.Id() || existing.Snapshot.Revision() != snapshot.Revision())
            {
                WorkspaceSnapshotExchangeResult result{ WorkspaceSnapshotExchangeStatus::Conflict };
                result.UnderlyingStoreStatus = WorkspaceSnapshotStoreStatus::Conflict;
                result.Snapshot = existing.Snapshot;
                return result;
            }
            return _fromStoreResult(store.Commit(snapshot, snapshot.Revision()));
        }
        case WorkspaceSnapshotImportMode::Copy:
        {
            const auto timestamp = importedAt.value_or(_unixMillisecondsNow());
            if (!_validTimestamp(timestamp))
            {
                return { WorkspaceSnapshotExchangeStatus::InvalidEnvelope };
            }
            for (uint32_t attempt = 0; attempt < 4; ++attempt)
            {
                auto copy = _makeCopy(snapshot, timestamp);
                if (!copy)
                {
                    return { WorkspaceSnapshotExchangeStatus::IoError };
                }
                const auto copiedValidation = WorkspaceSnapshotValidator::Validate(copy, true);
                if (!copiedValidation)
                {
                    WorkspaceSnapshotExchangeResult result{ WorkspaceSnapshotExchangeStatus::InvalidSnapshot };
                    result.ValidationError = copiedValidation.Error;
                    return result;
                }
                auto result = _fromStoreResult(store.Commit(copy, std::nullopt));
                if (result.Status != WorkspaceSnapshotExchangeStatus::Conflict)
                {
                    return result;
                }
            }
            return { WorkspaceSnapshotExchangeStatus::Conflict };
        }
        default:
            return { WorkspaceSnapshotExchangeStatus::InvalidEnvelope };
        }
    }
    catch (...)
    {
        return { WorkspaceSnapshotExchangeStatus::IoError };
    }
}
