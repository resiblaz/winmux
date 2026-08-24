// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceCommandline.h"

#include "WorkspaceCommandline.g.cpp"
#include "WorkspaceCommandlineResult.g.cpp"

#include "ApplicationState.h"
#include "CascadiaSettings.h"
#include "WorkspaceSnapshotExchange.h"
#include "WorkspaceSnapshotStore.h"
#include "../../types/inc/utils.hpp"

using namespace ::Microsoft::Console;
using namespace winrt::Microsoft::Terminal::Settings::Model;
namespace Model = winrt::Microsoft::Terminal::Settings::Model;
using WorkspaceSnapshotExchange = Model::implementation::WorkspaceSnapshotExchange;
using WorkspaceSnapshotExchangeResult = Model::implementation::WorkspaceSnapshotExchangeResult;
using WorkspaceSnapshotExchangeStatus = Model::implementation::WorkspaceSnapshotExchangeStatus;
using WorkspaceSnapshotImportMode = Model::implementation::WorkspaceSnapshotImportMode;
using WorkspaceSnapshotStore = Model::implementation::WorkspaceSnapshotStore;
using WorkspaceSnapshotStoreScope = Model::implementation::WorkspaceSnapshotStoreScope;
using WorkspaceSnapshotStoreStatus = Model::implementation::WorkspaceSnapshotStoreStatus;

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    namespace
    {
        constexpr std::wstring_view Usage{
            L"Usage:\n"
            L"  wt workspace list [--format table|json]\n"
            L"  wt workspace open <snapshot-id> [-w target]\n"
            L"  wt workspace export <snapshot-id> <path> [--force]\n"
            L"  wt workspace import <path> [--replace|--copy]\n"
        };

        WorkspaceCommandlineParseResult _parseError(const std::wstring_view message)
        {
            std::wstring error{ message };
            error.append(L"\n");
            error.append(Usage);
            return { false, {}, winrt::hstring{ error } };
        }

        bool _equalsInsensitive(const std::wstring_view left, const std::wstring_view right) noexcept
        {
            if (left.size() != right.size() || left.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }
            return CompareStringOrdinal(left.data(), gsl::narrow<int>(left.size()), right.data(), gsl::narrow<int>(right.size()), TRUE) == CSTR_EQUAL;
        }

        std::optional<winrt::guid> _parseSnapshotId(const winrt::hstring& value) noexcept
        try
        {
            if (value.size() != 36)
            {
                return std::nullopt;
            }
            const winrt::guid id{ Utils::GuidFromPlainString(value.c_str()) };
            const auto canonical = Utils::GuidToPlainString(id);
            if (id == winrt::guid{} || !_equalsInsensitive(value, canonical))
            {
                return std::nullopt;
            }
            return id;
        }
        catch (...)
        {
            return std::nullopt;
        }

        bool _validPath(const winrt::hstring& value) noexcept
        {
            return !value.empty() &&
                   value.size() <= 32767 &&
                   std::ranges::none_of(value, [](const wchar_t ch) { return ch < L' '; });
        }

        bool _validWindowTarget(const winrt::hstring& value) noexcept
        {
            if (value.empty() ||
                value.size() > 1024 ||
                std::ranges::any_of(value, [](const wchar_t ch) { return ch < L' '; }))
            {
                return false;
            }
            return value[0] != L'-' || til::parse_signed<int64_t>(value, 10).has_value();
        }

        std::wstring _safeDisplayText(const winrt::hstring& value)
        {
            std::wstring output;
            output.reserve(value.size());
            for (const auto ch : value)
            {
                switch (ch)
                {
                case L'\\':
                    output.append(L"\\\\");
                    break;
                case L'\r':
                    output.append(L"\\r");
                    break;
                case L'\n':
                    output.append(L"\\n");
                    break;
                case L'\t':
                    output.append(L"\\t");
                    break;
                default:
                    output.push_back(ch < L' ' ||
                                             (ch >= 0x7f && ch <= 0x9f) ||
                                             ch == 0x2028 || ch == 0x2029 ||
                                             (ch >= 0x202a && ch <= 0x202e) ||
                                             (ch >= 0x2066 && ch <= 0x2069) ?
                                         L'?' :
                                         ch);
                    break;
                }
            }
            return output;
        }

        int32_t _exitCode(const WorkspaceCommandlineExitCode value) noexcept
        {
            return static_cast<int32_t>(value);
        }

        Model::WorkspaceCommandlineResult _result(
            const WorkspaceCommandlineExitCode code,
            winrt::hstring standardOutput = {},
            winrt::hstring standardError = {},
            const bool openRequested = false,
            const winrt::guid& snapshotId = {},
            winrt::hstring windowTarget = {})
        {
            return *winrt::make_self<implementation::WorkspaceCommandlineResult>(
                _exitCode(code),
                std::move(standardOutput),
                std::move(standardError),
                openRequested,
                snapshotId,
                std::move(windowTarget));
        }

        WorkspaceSnapshotStoreScope _scope() noexcept
        {
            return Utils::IsRunningElevated() ? WorkspaceSnapshotStoreScope::Elevated : WorkspaceSnapshotStoreScope::Standard;
        }

        bool _profileExists(const Model::CascadiaSettings& settings, const winrt::hstring& profile)
        {
            return settings &&
                   winrt::get_self<implementation::CascadiaSettings>(settings)->GetProfileByName(profile) != nullptr;
        }

        struct SnapshotProfileValidation
        {
            bool Valid{};
            winrt::hstring MissingProfile;
        };

        SnapshotProfileValidation _validateSnapshotProfiles(const Model::WorkspaceSnapshot& snapshot,
                                                            const Model::CascadiaSettings& settings) noexcept
        try
        {
            if (!snapshot || !settings || !snapshot.Actions())
            {
                return {};
            }
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
                if (!_profileExists(settings, profile))
                {
                    return { true, profile };
                }
            }
            return { true, {} };
        }
        catch (...)
        {
            return {};
        }

        Model::WorkspaceCommandlineResult _missingProfileError(const winrt::hstring& profile)
        {
            std::wstring message{ L"Workspace references a profile that does not exist: " };
            message.append(_safeDisplayText(profile));
            message.push_back(L'\n');
            return _result(WorkspaceCommandlineExitCode::ProfileNotFound, {}, winrt::hstring{ message });
        }

        struct WorkspaceSnapshotListResult
        {
            WorkspaceSnapshotStoreStatus Status{ WorkspaceSnapshotStoreStatus::Succeeded };
            std::vector<Model::WorkspaceSnapshot> Snapshots;
        };

        WorkspaceSnapshotListResult _loadSnapshotList(const WorkspaceSnapshotStore& store) noexcept
        try
        {
            constexpr size_t MaximumEntriesToInspect{ 4096 };
            constexpr size_t MaximumSnapshots{ 128 };
            const auto& root = store.StoreRoot();
            const auto rootAttributes = GetFileAttributesW(root.c_str());
            if (rootAttributes == INVALID_FILE_ATTRIBUTES)
            {
                const auto error = GetLastError();
                return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ?
                           WorkspaceSnapshotListResult{} :
                           WorkspaceSnapshotListResult{ WorkspaceSnapshotStoreStatus::IoError };
            }
            if (WI_IsAnyFlagSet(rootAttributes, FILE_ATTRIBUTE_REPARSE_POINT) ||
                WI_IsFlagClear(rootAttributes, FILE_ATTRIBUTE_DIRECTORY))
            {
                return { WorkspaceSnapshotStoreStatus::Invalid };
            }

            WorkspaceSnapshotListResult result;
            std::error_code error;
            size_t inspected{};
            std::filesystem::directory_iterator iterator{ root, std::filesystem::directory_options::none, error };
            const std::filesystem::directory_iterator end;
            for (; !error && iterator != end && inspected < MaximumEntriesToInspect;
                 iterator.increment(error), ++inspected)
            {
                const auto attributes = GetFileAttributesW(iterator->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    const auto lastError = GetLastError();
                    if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND)
                    {
                        continue;
                    }
                    return { WorkspaceSnapshotStoreStatus::IoError };
                }
                if (WI_IsAnyFlagSet(attributes, FILE_ATTRIBUTE_REPARSE_POINT) ||
                    WI_IsFlagClear(attributes, FILE_ATTRIBUTE_DIRECTORY))
                {
                    continue;
                }

                const auto filename = winrt::hstring{ iterator->path().filename().native() };
                const auto id = _parseSnapshotId(filename);
                const auto canonical = id ? Utils::GuidToPlainString(id.value()) : std::wstring{};
                if (!id || std::wstring_view{ filename } != std::wstring_view{ canonical })
                {
                    continue;
                }

                const auto loaded = store.Load(id.value());
                if (loaded.Status == WorkspaceSnapshotStoreStatus::Succeeded && loaded.Snapshot)
                {
                    result.Snapshots.emplace_back(loaded.Snapshot);
                    if (result.Snapshots.size() > MaximumSnapshots)
                    {
                        return { WorkspaceSnapshotStoreStatus::CapacityExceeded };
                    }
                }
                else if (loaded.Status != WorkspaceSnapshotStoreStatus::NotFound)
                {
                    return { loaded.Status };
                }
            }
            if (error || iterator != end)
            {
                return { WorkspaceSnapshotStoreStatus::IoError };
            }
            return result;
        }
        catch (...)
        {
            return { WorkspaceSnapshotStoreStatus::IoError };
        }

        winrt::hstring _tableText(std::vector<Model::WorkspaceSnapshot> snapshots)
        {
            const auto idText = [](const winrt::guid& id) { return Utils::GuidToPlainString(id); };
            std::ranges::sort(snapshots, [&](const auto& left, const auto& right) {
                if (left.UpdatedAt() != right.UpdatedAt())
                {
                    return left.UpdatedAt() > right.UpdatedAt();
                }
                return idText(left.Id()) < idText(right.Id());
            });

            std::wstring output{ L"ID                                   REVISION PINNED UPDATED_AT     NAME  ROOT\n" };
            for (const auto& snapshot : snapshots)
            {
                fmt::format_to(std::back_inserter(output),
                               FMT_COMPILE(L"{:<36} {:>8} {:<6} {:>13} {}  {}\n"),
                               idText(snapshot.Id()),
                               snapshot.Revision(),
                               snapshot.Pinned() ? L"yes" : L"no",
                               snapshot.UpdatedAt(),
                               _safeDisplayText(snapshot.Name()),
                               _safeDisplayText(snapshot.RootDirectory()));
            }
            return winrt::hstring{ output };
        }

        winrt::hstring _jsonText(std::vector<Model::WorkspaceSnapshot> snapshots)
        {
            const auto idText = [](const winrt::guid& id) { return Utils::GuidToPlainString(id); };
            std::ranges::sort(snapshots, [&](const auto& left, const auto& right) {
                if (left.UpdatedAt() != right.UpdatedAt())
                {
                    return left.UpdatedAt() > right.UpdatedAt();
                }
                return idText(left.Id()) < idText(right.Id());
            });

            Json::Value root{ Json::arrayValue };
            for (const auto& snapshot : snapshots)
            {
                Json::Value entry{ Json::objectValue };
                entry["id"] = til::u16u8(idText(snapshot.Id()));
                entry["workspaceId"] = til::u16u8(idText(snapshot.WorkspaceId()));
                entry["revision"] = Json::Value{ static_cast<Json::UInt64>(snapshot.Revision()) };
                entry["name"] = til::u16u8(snapshot.Name());
                entry["rootDirectory"] = til::u16u8(snapshot.RootDirectory());
                entry["createdAt"] = Json::Value{ static_cast<Json::Int64>(snapshot.CreatedAt()) };
                entry["updatedAt"] = Json::Value{ static_cast<Json::Int64>(snapshot.UpdatedAt()) };
                entry["lastOpenedAt"] = Json::Value{ static_cast<Json::Int64>(snapshot.LastOpenedAt()) };
                entry["pinned"] = snapshot.Pinned();
                entry["actionCount"] = snapshot.Actions() ? snapshot.Actions().Size() : 0;
                entry["agentResumeCount"] = snapshot.AgentResumeEntries() ? snapshot.AgentResumeEntries().Size() : 0;
                entry["hasGitBinding"] = snapshot.GitBinding() != nullptr;
                root.append(std::move(entry));
            }

            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            builder["commentStyle"] = "None";
            builder["emitUTF8"] = true;
            auto output = Json::writeString(builder, root);
            output.push_back('\n');
            return winrt::hstring{ til::u8u16(output) };
        }

        Model::WorkspaceCommandlineResult _storeError(const WorkspaceSnapshotStoreStatus status)
        {
            switch (status)
            {
            case WorkspaceSnapshotStoreStatus::NotFound:
                return _result(WorkspaceCommandlineExitCode::NotFound, {}, L"Workspace snapshot was not found.\n");
            case WorkspaceSnapshotStoreStatus::Conflict:
                return _result(WorkspaceCommandlineExitCode::Conflict, {}, L"Workspace snapshot changed concurrently.\n");
            case WorkspaceSnapshotStoreStatus::Invalid:
            case WorkspaceSnapshotStoreStatus::Corrupt:
            case WorkspaceSnapshotStoreStatus::Oversized:
                return _result(WorkspaceCommandlineExitCode::InvalidData, {}, L"Workspace snapshot is invalid or corrupt.\n");
            case WorkspaceSnapshotStoreStatus::CapacityExceeded:
                return _result(WorkspaceCommandlineExitCode::Conflict, {}, L"Workspace snapshot capacity was exceeded.\n");
            case WorkspaceSnapshotStoreStatus::ScopeUnavailable:
                return _result(WorkspaceCommandlineExitCode::Unavailable, {}, L"Workspace storage is unavailable for this process scope.\n");
            default:
                return _result(WorkspaceCommandlineExitCode::IoError, {}, L"Workspace storage could not be accessed.\n");
            }
        }

        Model::WorkspaceCommandlineResult _exchangeError(const WorkspaceSnapshotExchangeResult& exchange)
        {
            switch (exchange.Status)
            {
            case WorkspaceSnapshotExchangeStatus::NotFound:
                return _result(WorkspaceCommandlineExitCode::NotFound, {}, L"Workspace snapshot or file was not found.\n");
            case WorkspaceSnapshotExchangeStatus::DestinationExists:
                return _result(WorkspaceCommandlineExitCode::Conflict, {}, L"Destination already exists; use --force to replace it.\n");
            case WorkspaceSnapshotExchangeStatus::Conflict:
            case WorkspaceSnapshotExchangeStatus::CapacityExceeded:
                return _result(WorkspaceCommandlineExitCode::Conflict, {}, L"Workspace import conflicts with existing state.\n");
            case WorkspaceSnapshotExchangeStatus::ProfileNotFound:
            {
                std::wstring message{ L"Imported workspace references a profile that does not exist: " };
                message.append(_safeDisplayText(exchange.MissingProfile));
                message.push_back(L'\n');
                return _result(WorkspaceCommandlineExitCode::ProfileNotFound, {}, winrt::hstring{ message });
            }
            case WorkspaceSnapshotExchangeStatus::ProfileLookupUnavailable:
            case WorkspaceSnapshotExchangeStatus::ScopeUnavailable:
                return _result(WorkspaceCommandlineExitCode::Unavailable, {}, L"Workspace import validation is unavailable.\n");
            case WorkspaceSnapshotExchangeStatus::MalformedJson:
            case WorkspaceSnapshotExchangeStatus::UnsupportedFormat:
            case WorkspaceSnapshotExchangeStatus::UnsupportedFormatVersion:
            case WorkspaceSnapshotExchangeStatus::UnknownField:
            case WorkspaceSnapshotExchangeStatus::InvalidEnvelope:
            case WorkspaceSnapshotExchangeStatus::InvalidSnapshot:
            case WorkspaceSnapshotExchangeStatus::UnsafeAction:
            case WorkspaceSnapshotExchangeStatus::Oversized:
                return _result(WorkspaceCommandlineExitCode::InvalidData, {}, L"Workspace exchange file is invalid or unsafe.\n");
            default:
                return _result(WorkspaceCommandlineExitCode::IoError, {}, L"Workspace exchange operation failed.\n");
            }
        }
    }

    WorkspaceCommandlineParseResult ParseWorkspaceCommandline(const std::span<const winrt::hstring> arguments) noexcept
    try
    {
        if (arguments.size() < 2 || arguments[0] != L"workspace")
        {
            return _parseError(L"Expected the workspace command group.");
        }

        WorkspaceCommandlineRequest request;
        const auto command = std::wstring_view{ arguments[1] };
        if (command == L"list")
        {
            request.Kind = WorkspaceCommandKind::List;
            if (arguments.size() == 2)
            {
                return { true, std::move(request), {} };
            }
            if (arguments.size() != 4 || arguments[2] != L"--format")
            {
                return _parseError(L"Invalid workspace list arguments.");
            }
            if (arguments[3] == L"table")
            {
                request.ListFormat = WorkspaceListFormat::Table;
            }
            else if (arguments[3] == L"json")
            {
                request.ListFormat = WorkspaceListFormat::Json;
            }
            else
            {
                return _parseError(L"Workspace list format must be table or json.");
            }
            return { true, std::move(request), {} };
        }

        if (command == L"open")
        {
            if (arguments.size() != 3 && arguments.size() != 5)
            {
                return _parseError(L"Invalid workspace open arguments.");
            }
            const auto id = _parseSnapshotId(arguments[2]);
            if (!id)
            {
                return _parseError(L"Snapshot ID must be a canonical UUID.");
            }
            request.Kind = WorkspaceCommandKind::Open;
            request.SnapshotId = id.value();
            if (arguments.size() == 5)
            {
                if (arguments[3] != L"-w" || !_validWindowTarget(arguments[4]))
                {
                    return _parseError(L"Workspace open requires a non-empty -w target.");
                }
                request.WindowTarget = arguments[4];
            }
            return { true, std::move(request), {} };
        }

        if (command == L"export")
        {
            if (arguments.size() != 4 && arguments.size() != 5)
            {
                return _parseError(L"Invalid workspace export arguments.");
            }
            const auto id = _parseSnapshotId(arguments[2]);
            if (!id)
            {
                return _parseError(L"Snapshot ID must be a canonical UUID.");
            }
            if (!_validPath(arguments[3]) || arguments[3] == L"--force")
            {
                return _parseError(L"Workspace export requires a destination path.");
            }
            if (arguments.size() == 5 && arguments[4] != L"--force")
            {
                return _parseError(L"Unknown workspace export option.");
            }
            request.Kind = WorkspaceCommandKind::Export;
            request.SnapshotId = id.value();
            request.Path = arguments[3];
            request.Force = arguments.size() == 5;
            return { true, std::move(request), {} };
        }

        if (command == L"import")
        {
            if (arguments.size() != 3 && arguments.size() != 4)
            {
                return _parseError(L"Invalid workspace import arguments.");
            }
            if (!_validPath(arguments[2]) || arguments[2] == L"--replace" || arguments[2] == L"--copy")
            {
                return _parseError(L"Workspace import requires a source path.");
            }
            request.Kind = WorkspaceCommandKind::Import;
            request.Path = arguments[2];
            if (arguments.size() == 4)
            {
                if (arguments[3] == L"--replace")
                {
                    request.ImportMode = WorkspaceImportMode::Replace;
                }
                else if (arguments[3] == L"--copy")
                {
                    request.ImportMode = WorkspaceImportMode::Copy;
                }
                else
                {
                    return _parseError(L"Workspace import accepts only --replace or --copy.");
                }
            }
            return { true, std::move(request), {} };
        }

        return _parseError(L"Unknown workspace command.");
    }
    catch (...)
    {
        return _parseError(L"Workspace command arguments could not be parsed.");
    }

    WorkspaceCommandlineResult::WorkspaceCommandlineResult(const int32_t exitCode,
                                                           winrt::hstring standardOutput,
                                                           winrt::hstring standardError,
                                                           const bool openRequested,
                                                           const winrt::guid& snapshotId,
                                                           winrt::hstring windowTarget) :
        _exitCode{ exitCode },
        _standardOutput{ std::move(standardOutput) },
        _standardError{ std::move(standardError) },
        _openRequested{ openRequested },
        _snapshotId{ snapshotId },
        _windowTarget{ std::move(windowTarget) }
    {
    }

    int32_t WorkspaceCommandlineResult::ExitCode() const noexcept
    {
        return _exitCode;
    }

    winrt::hstring WorkspaceCommandlineResult::StandardOutput() const
    {
        return _standardOutput;
    }

    winrt::hstring WorkspaceCommandlineResult::StandardError() const
    {
        return _standardError;
    }

    bool WorkspaceCommandlineResult::OpenRequested() const noexcept
    {
        return _openRequested;
    }

    winrt::guid WorkspaceCommandlineResult::SnapshotId() const noexcept
    {
        return _snapshotId;
    }

    winrt::hstring WorkspaceCommandlineResult::WindowTarget() const
    {
        return _windowTarget;
    }

    Model::WorkspaceCommandlineResult WorkspaceCommandline::Execute(
        const Windows::Foundation::Collections::IVectorView<winrt::hstring>& arguments) noexcept
    try
    {
        const auto argumentValues = arguments ? wil::to_vector(arguments) : std::vector<winrt::hstring>{};
        const auto parsed = ParseWorkspaceCommandline(argumentValues);
        if (!parsed.Succeeded)
        {
            return _result(WorkspaceCommandlineExitCode::InvalidArguments, {}, parsed.Error);
        }

        if (_scope() == WorkspaceSnapshotStoreScope::Elevated)
        {
            return _result(WorkspaceCommandlineExitCode::Unavailable, {}, L"Workspace storage is unavailable in elevated processes.\n");
        }

        // ApplicationState owns the one-time v1 state.json migration. Make
        // sure a first-run CLI read observes the migrated independent store;
        // list/export do not otherwise load Settings, and open loads the Store
        // before it needs profile validation.
        (void)ApplicationState::SharedInstance();

        const std::filesystem::path stateRoot{ std::wstring_view{ CascadiaSettings::SettingsDirectory() } };
        WorkspaceSnapshotStore store{ stateRoot, WorkspaceSnapshotStoreScope::Standard };
        const auto& request = parsed.Request;
        switch (request.Kind)
        {
        case WorkspaceCommandKind::List:
        {
            auto snapshots = _loadSnapshotList(store);
            if (snapshots.Status != WorkspaceSnapshotStoreStatus::Succeeded)
            {
                return _storeError(snapshots.Status);
            }
            return _result(WorkspaceCommandlineExitCode::Succeeded,
                           request.ListFormat == WorkspaceListFormat::Json ? _jsonText(std::move(snapshots.Snapshots)) : _tableText(std::move(snapshots.Snapshots)));
        }
        case WorkspaceCommandKind::Open:
        {
            const auto loaded = store.Load(request.SnapshotId);
            if (loaded.Status != WorkspaceSnapshotStoreStatus::Succeeded || !loaded.Snapshot)
            {
                return _storeError(loaded.Status);
            }
            const auto settings = CascadiaSettings::LoadAll();
            const auto profiles = _validateSnapshotProfiles(loaded.Snapshot, settings);
            if (!profiles.Valid)
            {
                return _result(WorkspaceCommandlineExitCode::InvalidData, {}, L"Workspace snapshot profiles could not be validated.\n");
            }
            if (!profiles.MissingProfile.empty())
            {
                return _missingProfileError(profiles.MissingProfile);
            }
            return _result(WorkspaceCommandlineExitCode::Succeeded,
                           {},
                           {},
                           true,
                           request.SnapshotId,
                           request.WindowTarget);
        }
        case WorkspaceCommandKind::Export:
        {
            const auto exchange = WorkspaceSnapshotExchange::ExportToFile(
                store,
                request.SnapshotId,
                std::filesystem::path{ std::wstring_view{ request.Path } },
                request.Force);
            return exchange ? _result(WorkspaceCommandlineExitCode::Succeeded) : _exchangeError(exchange);
        }
        case WorkspaceCommandKind::Import:
        {
            const auto settings = CascadiaSettings::LoadAll();
            const auto mode = request.ImportMode == WorkspaceImportMode::Replace ? WorkspaceSnapshotImportMode::Replace :
                              request.ImportMode == WorkspaceImportMode::Copy ? WorkspaceSnapshotImportMode::Copy :
                                                                               WorkspaceSnapshotImportMode::Fail;
            const auto exchange = WorkspaceSnapshotExchange::ImportFromFile(
                store,
                std::filesystem::path{ std::wstring_view{ request.Path } },
                mode,
                [&settings](const winrt::hstring& profile) {
                    return _profileExists(settings, profile);
                });
            if (!exchange)
            {
                return _exchangeError(exchange);
            }
            if (!exchange.Snapshot)
            {
                return _result(WorkspaceCommandlineExitCode::IoError, {}, L"Workspace import completed without an imported snapshot identity.\n");
            }
            auto importedId = Utils::GuidToPlainString(exchange.Snapshot.Id());
            importedId.push_back(L'\n');
            return _result(WorkspaceCommandlineExitCode::Succeeded, winrt::hstring{ importedId });
        }
        }
        return _result(WorkspaceCommandlineExitCode::InvalidArguments, {}, L"Unknown workspace command.\n");
    }
    catch (...)
    {
        return _result(WorkspaceCommandlineExitCode::IoError, {}, L"Workspace command failed unexpectedly.\n");
    }
}
