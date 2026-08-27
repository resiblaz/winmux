// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceSnapshotValidation.h"

#include "ActionAndArgs.h"
#include "ActionArgs.h"
#include "ApplicationState.h"

using namespace ::Microsoft::Terminal::Settings::Model;

namespace winrt::Microsoft::Terminal::Settings::Model::implementation
{
    namespace
    {
        bool _containsNul(const winrt::hstring& value) noexcept
        {
            return std::wstring_view{ value }.find(L'\0') != std::wstring_view::npos;
        }

        bool _isBoundedText(const winrt::hstring& value, const size_t maximum) noexcept
        {
            return value.size() <= maximum && !_containsNul(value);
        }

        bool _containsControlCharacter(const std::wstring_view value) noexcept
        {
            return std::ranges::any_of(value, [](const auto ch) { return ch < L' ' || ch == 0x7f; });
        }

        bool _isAbsoluteSafePath(const winrt::hstring& value) noexcept
        try
        {
            return _isBoundedText(value, WorkspaceSnapshotValidator::MaximumTextCharacters) &&
                   !_containsControlCharacter(value) &&
                   std::filesystem::path{ std::wstring_view{ value } }.is_absolute();
        }
        catch (...)
        {
            return false;
        }

        bool _sameWindowsPath(const std::filesystem::path& left, const std::filesystem::path& right) noexcept
        try
        {
            auto normalize = [](const std::filesystem::path& path) {
                auto text = path.lexically_normal().native();
                std::ranges::replace(text, L'/', L'\\');
                while (text.size() > 3 && text.back() == L'\\')
                {
                    text.pop_back();
                }
                return text;
            };
            const auto lhs = normalize(left);
            const auto rhs = normalize(right);
            if (lhs.empty() || rhs.empty() ||
                lhs.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                rhs.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }
            return CompareStringOrdinal(lhs.data(), gsl::narrow<int>(lhs.size()), rhs.data(), gsl::narrow<int>(rhs.size()), TRUE) == CSTR_EQUAL;
        }
        catch (...)
        {
            return false;
        }

        bool _isSafeContent(const Model::INewContentArgs& content) noexcept
        {
            try
            {
                const auto terminalArgs = content.try_as<Model::NewTerminalArgs>();
                if (!terminalArgs ||
                    terminalArgs.Profile().empty() ||
                    !_isBoundedText(terminalArgs.Profile(), WorkspaceSnapshotValidator::MaximumTextCharacters) ||
                    !terminalArgs.Commandline().empty() ||
                    !_isBoundedText(terminalArgs.StartingDirectory(), WorkspaceSnapshotValidator::MaximumTextCharacters) ||
                    !_isBoundedText(terminalArgs.TabTitle(), WorkspaceSnapshotValidator::MaximumTextCharacters) ||
                    !_isBoundedText(terminalArgs.ColorScheme(), WorkspaceSnapshotValidator::MaximumTextCharacters) ||
                    terminalArgs.ProfileIndex() ||
                    terminalArgs.SessionId() != winrt::guid{} ||
                    terminalArgs.ContentId() != 0 ||
                    terminalArgs.AppendCommandLine())
                {
                    return false;
                }

                if (const auto elevate = terminalArgs.Elevate(); elevate && elevate.Value())
                {
                    return false;
                }
                if (const auto reload = terminalArgs.ReloadEnvironmentVariables(); reload && reload.Value())
                {
                    return false;
                }
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool _isHexObjectId(const std::wstring_view value) noexcept
        {
            if (value.size() != 40 && value.size() != 64)
            {
                return false;
            }
            return std::ranges::all_of(value, [](const auto ch) {
                return (ch >= L'0' && ch <= L'9') ||
                       (ch >= L'a' && ch <= L'f') ||
                       (ch >= L'A' && ch <= L'F');
            });
        }

        bool _validTimestamp(const int64_t value) noexcept
        {
            return value >= 0 && value <= WorkspaceSnapshotValidator::MaximumUnixMilliseconds;
        }
    }

    bool WorkspaceSnapshotValidator::IsSafeActions(const Windows::Foundation::Collections::IVector<Model::ActionAndArgs>& actions) noexcept
    {
        try
        {
            if (!actions || actions.Size() == 0 || actions.Size() > MaximumActions)
            {
                return false;
            }

            bool createdTab = false;
            uint32_t tabCount{};
            for (uint32_t index = 0; index < actions.Size(); ++index)
            {
                const auto action = actions.GetAt(index);
                if (!action)
                {
                    return false;
                }

                switch (action.Action())
                {
                case Model::ShortcutAction::NewTab:
                {
                    const auto args = action.Args().try_as<Model::NewTabArgs>();
                    if (!args || !_isSafeContent(args.ContentArgs()) || ++tabCount > MaximumSurfaces)
                    {
                        return false;
                    }
                    createdTab = true;
                    break;
                }
                case Model::ShortcutAction::SplitPane:
                {
                    const auto args = action.Args().try_as<Model::SplitPaneArgs>();
                    if (!createdTab || !args || !std::isfinite(args.SplitSize()) ||
                        args.SplitSize() <= 0.0f || args.SplitSize() >= 1.0f ||
                        !_isSafeContent(args.ContentArgs()))
                    {
                        return false;
                    }
                    break;
                }
                case Model::ShortcutAction::RenameTab:
                {
                    const auto args = action.Args().try_as<Model::RenameTabArgs>();
                    if (!createdTab || !args || !_isBoundedText(args.Title(), MaximumTextCharacters))
                    {
                        return false;
                    }
                    break;
                }
                case Model::ShortcutAction::SetTabColor:
                    if (!createdTab || !action.Args().try_as<Model::SetTabColorArgs>())
                    {
                        return false;
                    }
                    break;
                case Model::ShortcutAction::MoveFocus:
                    if (!createdTab || !action.Args().try_as<Model::MoveFocusArgs>())
                    {
                        return false;
                    }
                    break;
                case Model::ShortcutAction::FocusPane:
                    if (!createdTab || !action.Args().try_as<Model::FocusPaneArgs>())
                    {
                        return false;
                    }
                    break;
                case Model::ShortcutAction::TogglePaneZoom:
                    if (!createdTab || action.Args())
                    {
                        return false;
                    }
                    break;
                default:
                    return false;
                }
            }

            if (!createdTab)
            {
                return false;
            }

            const auto serialized = Model::ActionAndArgs::Serialize(actions);
            return til::u16u8(serialized).size() <= MaximumSerializedBytes;
        }
        catch (...)
        {
            return false;
        }
    }

    WorkspaceSnapshotValidationResult WorkspaceSnapshotValidator::Validate(const Model::WorkspaceSnapshot& snapshot,
                                                                           const bool allowZeroRevision) noexcept
    {
        try
        {
            if (!snapshot)
            {
                return { WorkspaceSnapshotValidationError::NullSnapshot };
            }
            if (snapshot.Version() != CurrentVersion)
            {
                return { WorkspaceSnapshotValidationError::UnsupportedVersion };
            }
            if (!allowZeroRevision && snapshot.Revision() == 0)
            {
                return { WorkspaceSnapshotValidationError::InvalidRevision };
            }
            if (snapshot.Id() == winrt::guid{} || snapshot.WorkspaceId() == winrt::guid{})
            {
                return { WorkspaceSnapshotValidationError::InvalidIdentity };
            }
            if (snapshot.Name().empty() || !_isBoundedText(snapshot.Name(), MaximumNameCharacters))
            {
                return { WorkspaceSnapshotValidationError::InvalidName };
            }
            if (!_isBoundedText(snapshot.RootDirectory(), MaximumTextCharacters))
            {
                return { WorkspaceSnapshotValidationError::InvalidRootDirectory };
            }
            if (!_validTimestamp(snapshot.CreatedAt()) || !_validTimestamp(snapshot.UpdatedAt()) ||
                !_validTimestamp(snapshot.LastOpenedAt()) ||
                (snapshot.CreatedAt() > 0 && snapshot.UpdatedAt() > 0 && snapshot.UpdatedAt() < snapshot.CreatedAt()))
            {
                return { WorkspaceSnapshotValidationError::InvalidTimestamp };
            }
            if (!IsSafeActions(snapshot.Actions()))
            {
                return { WorkspaceSnapshotValidationError::InvalidActions };
            }

            uint32_t surfaceCount{};
            for (const auto& action : snapshot.Actions())
            {
                surfaceCount += action.Action() == Model::ShortcutAction::NewTab ? 1u : 0u;
            }
            if (const auto surfaceIds = snapshot.SurfaceIds(); surfaceIds && surfaceIds.Size() > 0)
            {
                if (surfaceIds.Size() != surfaceCount || surfaceIds.Size() > MaximumSurfaces || snapshot.ActiveSurfaceId() == winrt::guid{})
                {
                    return { WorkspaceSnapshotValidationError::InvalidIdentity };
                }
                std::unordered_set<winrt::guid> uniqueSurfaceIds;
                bool activeSurfaceFound{};
                for (const auto& surfaceId : surfaceIds)
                {
                    if (surfaceId == winrt::guid{} || !uniqueSurfaceIds.emplace(surfaceId).second)
                    {
                        return { WorkspaceSnapshotValidationError::InvalidIdentity };
                    }
                    activeSurfaceFound = activeSurfaceFound || surfaceId == snapshot.ActiveSurfaceId();
                }
                if (!activeSurfaceFound)
                {
                    return { WorkspaceSnapshotValidationError::InvalidIdentity };
                }
            }
            else if (surfaceCount != 1 || snapshot.ActiveSurfaceId() != winrt::guid{})
            {
                // Existing v2 snapshots predate Surface metadata and are
                // interpreted as one implicit Surface.
                return { WorkspaceSnapshotValidationError::InvalidIdentity };
            }

            if (const auto entries = snapshot.AgentResumeEntries())
            {
                if (entries.Size() > MaximumAgentResumeEntries)
                {
                    return { WorkspaceSnapshotValidationError::InvalidAgentResume };
                }
                for (const auto& entry : entries)
                {
                    if (entry.Version == 0 || entry.WorkspaceId != snapshot.WorkspaceId() ||
                        entry.Provider.empty() || !_isBoundedText(entry.Provider, MaximumIdentifierCharacters) ||
                        entry.SessionId.empty() || !_isBoundedText(entry.SessionId, MaximumIdentifierCharacters) ||
                        !_isBoundedText(entry.WorkingDirectory, MaximumTextCharacters) ||
                        !entry.RequiresConfirmation)
                    {
                        return { WorkspaceSnapshotValidationError::InvalidAgentResume };
                    }
                }
            }

            if (const auto binding = snapshot.GitBinding())
            {
                if (binding.RepositoryRoot().empty() || binding.WorktreePath().empty() ||
                    binding.CommonGitDirectory().empty() || binding.GitDirectory().empty() || binding.Head().empty() ||
                    !_isAbsoluteSafePath(binding.RepositoryRoot()) ||
                    !_isAbsoluteSafePath(binding.WorktreePath()) ||
                    !_isAbsoluteSafePath(binding.CommonGitDirectory()) ||
                    !_isAbsoluteSafePath(binding.GitDirectory()) ||
                    !_isBoundedText(binding.BranchRef(), MaximumBranchCharacters) ||
                    _containsControlCharacter(binding.BranchRef()) ||
                    (!binding.BranchRef().empty() && !std::wstring_view{ binding.BranchRef() }.starts_with(L"refs/")) ||
                    !_isBoundedText(binding.Head(), 64) ||
                    !_isHexObjectId(binding.Head()) || !_validTimestamp(binding.CapturedAt()) ||
                    !_sameWindowsPath(std::filesystem::path{ std::wstring_view{ snapshot.RootDirectory() } },
                                      std::filesystem::path{ std::wstring_view{ binding.WorktreePath() } }) ||
                    !_sameWindowsPath(std::filesystem::path{ std::wstring_view{ binding.RepositoryRoot() } },
                                      std::filesystem::path{ std::wstring_view{ binding.CommonGitDirectory() } }.parent_path()))
                {
                    return { WorkspaceSnapshotValidationError::InvalidGitBinding };
                }
            }

            const auto serialized = Model::WorkspaceSnapshot::ToJson(snapshot);
            if (til::u16u8(serialized).size() > MaximumSerializedBytes)
            {
                return { WorkspaceSnapshotValidationError::SerializedSizeExceeded };
            }
            return {};
        }
        catch (...)
        {
            return { WorkspaceSnapshotValidationError::SerializedSizeExceeded };
        }
    }

    Model::WorkspaceSnapshot WorkspaceSnapshotValidator::DeepCopy(const Model::WorkspaceSnapshot& snapshot) noexcept
    try
    {
        return snapshot ? Model::WorkspaceSnapshot::FromJson(Model::WorkspaceSnapshot::ToJson(snapshot)) : nullptr;
    }
    catch (...)
    {
        return nullptr;
    }

    Model::WorkspaceSnapshot WorkspaceSnapshotValidator::UpgradeLegacy(const Model::WorkspaceSnapshot& snapshot) noexcept
    {
        if (!snapshot || snapshot.Version() != LegacyVersion)
        {
            return nullptr;
        }

        auto upgraded = DeepCopy(snapshot);
        if (!upgraded)
        {
            return nullptr;
        }
        upgraded.Version(CurrentVersion);
        upgraded.GitBinding(nullptr);
        return upgraded;
    }

    bool WorkspaceSnapshotValidator::EquivalentPortableContent(const Model::WorkspaceSnapshot& left,
                                                               const Model::WorkspaceSnapshot& right) noexcept
    try
    {
        if (!left || !right)
        {
            return false;
        }
        auto leftJson = WorkspaceSnapshot::ToJsonValue(left);
        auto rightJson = WorkspaceSnapshot::ToJsonValue(right);
        leftJson.removeMember("revision");
        rightJson.removeMember("revision");
        leftJson.removeMember("lastOpenedAt");
        rightJson.removeMember("lastOpenedAt");
        return leftJson == rightJson;
    }
    catch (...)
    {
        return false;
    }
}
