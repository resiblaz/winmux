// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace winrt::TerminalApp::implementation
{
    enum class SidebarFilterMode
    {
        All = 0,
        Attention = 1,
        Running = 2,
    };

    enum class SidebarNodeKind
    {
        Workspace = 0,
        Surface = 1,
        Pane = 2,
    };

    enum class SidebarStatusKind
    {
        Idle = 0,
        Running = 1,
        Attention = 2,
    };

    enum class SidebarIconKind
    {
        Terminal = 0,
        Claude = 1,
        Codex = 2,
        Agy = 3,
    };

    struct SidebarPaneInput
    {
        uint32_t PaneId{};
        std::wstring Title;
        bool IsActive{};
        bool HasAttention{};
        bool IsRunning{};
        SidebarIconKind Icon{ SidebarIconKind::Terminal };
    };

    struct SidebarSurfaceInput
    {
        winrt::guid SurfaceId{};
        std::wstring Title;
        std::wstring Subtitle;
        uint32_t UnreadCount{};
        bool IsActive{};
        bool IsRunning{};
        bool HasAttention{};
        bool IsVimActive{};
        SidebarIconKind Icon{ SidebarIconKind::Terminal };
        std::vector<SidebarPaneInput> Panes;
    };

    struct SidebarWorkspaceInput
    {
        winrt::guid WorkspaceId{};
        std::wstring Title;
        std::wstring Directory;
        std::wstring GitBranch;
        std::wstring ListeningPorts;
        uint32_t UnreadCount{};
        bool IsPinned{};
        bool IsSelected{};
        bool IsRunning{};
        bool HasAttention{};
        std::vector<SidebarSurfaceInput> Surfaces;
    };

    struct SidebarProjectedNode
    {
        SidebarNodeKind Kind{ SidebarNodeKind::Workspace };
        size_t WorkspaceInputIndex{};
        size_t SurfaceInputIndex{};
        size_t PaneInputIndex{};
        winrt::guid WorkspaceId{};
        winrt::guid SurfaceId{};
        uint32_t PaneId{};
        uint32_t WorkspaceOrdinal{}; // 1-based ordinal in workspace list
        uint32_t SurfaceOrdinal{}; // 1-based ordinal within workspace
        bool IsCollapsed{};
        bool IsLastChild{};
        bool ShowCollapsedUnread{};
        SidebarStatusKind Status{ SidebarStatusKind::Idle };
        SidebarIconKind Icon{ SidebarIconKind::Terminal };
    };

    struct SidebarProjection
    {
        uint32_t TotalSurfaceCount{};
        uint32_t AttentionSurfaceCount{};
        uint32_t RunningSurfaceCount{};
        bool AnyWorkspaceExpanded{};
        bool IsFiltered{};
        std::vector<SidebarProjectedNode> Nodes;
    };

    class WorkspaceSidebarTreeModel final
    {
    public:
        static SidebarIconKind DetectIconKind(const std::wstring_view text)
        {
            if (text.empty())
            {
                return SidebarIconKind::Terminal;
            }
            const auto lowered = _normalize(text);
            if (_containsWord(lowered, L"claude"))
            {
                return SidebarIconKind::Claude;
            }
            if (_containsWord(lowered, L"codex"))
            {
                return SidebarIconKind::Codex;
            }
            if (_containsWord(lowered, L"agy") || _containsWord(lowered, L"antigravity"))
            {
                return SidebarIconKind::Agy;
            }
            return SidebarIconKind::Terminal;
        }

        SidebarFilterMode FilterMode() const noexcept
        {
            return _filterMode;
        }

        void SetFilterMode(const SidebarFilterMode mode) noexcept
        {
            _filterMode = mode;
        }

        const std::wstring& SearchQuery() const noexcept
        {
            return _searchQuery;
        }

        void SetSearchQuery(std::wstring query)
        {
            _searchQuery = std::move(query);
            _normalizedQuery = _normalize(_searchQuery);
        }

        // Auto-accordion: when the active workspace changes, reset manual
        // expand/collapse overrides so the newly selected workspace expands
        // and all unselected workspaces stay collapsed.
        void SetSelectedWorkspace(const winrt::guid& workspaceId)
        {
            if (workspaceId != winrt::guid{} && workspaceId != _selectedWorkspaceId)
            {
                _selectedWorkspaceId = workspaceId;
                _manuallyCollapsedWorkspaces.clear();
                _manuallyExpandedWorkspaces.clear();
            }
        }

        bool IsWorkspaceCollapsed(const winrt::guid& workspaceId, const bool isSelected) const noexcept
        {
            if (isSelected)
            {
                return _manuallyCollapsedWorkspaces.contains(workspaceId);
            }
            return !_manuallyExpandedWorkspaces.contains(workspaceId);
        }

        bool IsWorkspaceCollapsed(const winrt::guid& workspaceId) const noexcept
        {
            return IsWorkspaceCollapsed(workspaceId, workspaceId != winrt::guid{} && workspaceId == _selectedWorkspaceId);
        }

        void SetWorkspaceCollapsed(const winrt::guid& workspaceId, const bool collapsed, const bool isSelected)
        {
            if (workspaceId == winrt::guid{})
            {
                return;
            }
            if (isSelected)
            {
                _manuallyExpandedWorkspaces.erase(workspaceId);
                if (collapsed)
                {
                    _manuallyCollapsedWorkspaces.insert(workspaceId);
                }
                else
                {
                    _manuallyCollapsedWorkspaces.erase(workspaceId);
                }
            }
            else
            {
                _manuallyCollapsedWorkspaces.erase(workspaceId);
                if (collapsed)
                {
                    _manuallyExpandedWorkspaces.erase(workspaceId);
                }
                else
                {
                    _manuallyExpandedWorkspaces.insert(workspaceId);
                }
            }
        }

        void SetWorkspaceCollapsed(const winrt::guid& workspaceId, const bool collapsed)
        {
            SetWorkspaceCollapsed(workspaceId, collapsed, workspaceId != winrt::guid{} && workspaceId == _selectedWorkspaceId);
        }

        bool ToggleWorkspaceCollapsed(const winrt::guid& workspaceId, const bool isSelected)
        {
            const auto nextCollapsed = !IsWorkspaceCollapsed(workspaceId, isSelected);
            SetWorkspaceCollapsed(workspaceId, nextCollapsed, isSelected);
            return nextCollapsed;
        }

        bool ToggleWorkspaceCollapsed(const winrt::guid& workspaceId)
        {
            return ToggleWorkspaceCollapsed(workspaceId, workspaceId != winrt::guid{} && workspaceId == _selectedWorkspaceId);
        }

        // Returns true if workspaces were collapsed, false if expanded.
        bool ToggleCollapseAll(const std::vector<winrt::guid>& workspaceIds)
        {
            bool anyExpanded = false;
            for (const auto& id : workspaceIds)
            {
                if (id != winrt::guid{} && !IsWorkspaceCollapsed(id))
                {
                    anyExpanded = true;
                    break;
                }
            }

            _manuallyCollapsedWorkspaces.clear();
            _manuallyExpandedWorkspaces.clear();

            if (anyExpanded)
            {
                if (_selectedWorkspaceId != winrt::guid{})
                {
                    _manuallyCollapsedWorkspaces.insert(_selectedWorkspaceId);
                }
                return true;
            }

            for (const auto& id : workspaceIds)
            {
                if (id != winrt::guid{} && id != _selectedWorkspaceId)
                {
                    _manuallyExpandedWorkspaces.insert(id);
                }
            }
            return false;
        }

        void PruneCollapsed(const std::vector<winrt::guid>& activeWorkspaceIds)
        {
            std::unordered_set<winrt::guid> activeSet(activeWorkspaceIds.begin(), activeWorkspaceIds.end());
            if (!_manuallyCollapsedWorkspaces.empty())
            {
                std::erase_if(_manuallyCollapsedWorkspaces, [&](const auto& id) {
                    return !activeSet.contains(id);
                });
            }
            if (!_manuallyExpandedWorkspaces.empty())
            {
                std::erase_if(_manuallyExpandedWorkspaces, [&](const auto& id) {
                    return !activeSet.contains(id);
                });
            }
            if (_selectedWorkspaceId != winrt::guid{} && !activeSet.contains(_selectedWorkspaceId))
            {
                _selectedWorkspaceId = {};
            }
        }

        SidebarProjection Project(const std::vector<SidebarWorkspaceInput>& workspaces) const
        {
            SidebarProjection result{};
            const auto hasSearch = !_normalizedQuery.empty();
            const auto hasStatusFilter = _filterMode != SidebarFilterMode::All;
            result.IsFiltered = hasSearch || hasStatusFilter;

            for (size_t wsIndex = 0; wsIndex < workspaces.size(); ++wsIndex)
            {
                const auto& ws = workspaces[wsIndex];
                const auto wsMatchesSearch = !hasSearch || _workspaceMatchesSearch(ws);
                const auto wsDefaultCollapsed = IsWorkspaceCollapsed(ws.WorkspaceId, ws.IsSelected);
                if (!wsDefaultCollapsed)
                {
                    result.AnyWorkspaceExpanded = true;
                }

                std::vector<size_t> matchedSurfaceIndices;
                matchedSurfaceIndices.reserve(ws.Surfaces.size());

                for (size_t sfIndex = 0; sfIndex < ws.Surfaces.size(); ++sfIndex)
                {
                    const auto& sf = ws.Surfaces[sfIndex];
                    ++result.TotalSurfaceCount;

                    const auto sfAttention = _surfaceNeedsAttention(sf);
                    const auto sfRunning = _surfaceIsRunning(sf);
                    if (sfAttention)
                    {
                        ++result.AttentionSurfaceCount;
                    }
                    if (sfRunning)
                    {
                        ++result.RunningSurfaceCount;
                    }

                    if (!_matchesFilterMode(sfAttention, sfRunning, ws.HasAttention || ws.UnreadCount > 0, ws.IsRunning))
                    {
                        continue;
                    }

                    if (!wsMatchesSearch && !_surfaceOrPaneMatchesSearch(sf))
                    {
                        continue;
                    }

                    matchedSurfaceIndices.push_back(sfIndex);
                }

                // When filtering/searching, hide workspaces that have no matching surfaces.
                if (result.IsFiltered && matchedSurfaceIndices.empty())
                {
                    continue;
                }

                // Temporarily expand matching workspaces while searching or filtering
                // without mutating the user's accordion/manual collapse state.
                const auto effectivelyCollapsed = wsDefaultCollapsed && !result.IsFiltered;
                const auto wsAttention = ws.HasAttention || ws.UnreadCount > 0 ||
                                         std::ranges::any_of(ws.Surfaces, [](const auto& sf) { return _surfaceNeedsAttention(sf); });
                const auto wsRunning = ws.IsRunning ||
                                       std::ranges::any_of(ws.Surfaces, [](const auto& sf) { return _surfaceIsRunning(sf); });

                SidebarProjectedNode wsNode{};
                wsNode.Kind = SidebarNodeKind::Workspace;
                wsNode.WorkspaceInputIndex = wsIndex;
                wsNode.WorkspaceId = ws.WorkspaceId;
                wsNode.WorkspaceOrdinal = static_cast<uint32_t>(wsIndex + 1);
                wsNode.IsCollapsed = effectivelyCollapsed;
                wsNode.ShowCollapsedUnread = effectivelyCollapsed && (ws.UnreadCount > 0 || wsAttention);
                wsNode.Status = wsAttention ? SidebarStatusKind::Attention :
                                wsRunning   ? SidebarStatusKind::Running :
                                              SidebarStatusKind::Idle;
                result.Nodes.push_back(wsNode);

                if (effectivelyCollapsed)
                {
                    continue;
                }

                for (const auto sfIndex : matchedSurfaceIndices)
                {
                    const auto& sf = ws.Surfaces[sfIndex];
                    const auto sfAttention = _surfaceNeedsAttention(sf);
                    const auto sfRunning = _surfaceIsRunning(sf);

                    auto sfIcon = sf.Icon;
                    if (sfIcon == SidebarIconKind::Terminal)
                    {
                        sfIcon = DetectIconKind(sf.Title);
                        if (sfIcon == SidebarIconKind::Terminal)
                        {
                            for (const auto& pane : sf.Panes)
                            {
                                const auto paneIcon = pane.Icon != SidebarIconKind::Terminal ? pane.Icon : DetectIconKind(pane.Title);
                                if (paneIcon != SidebarIconKind::Terminal)
                                {
                                    sfIcon = paneIcon;
                                    break;
                                }
                            }
                        }
                    }

                    SidebarProjectedNode sfNode{};
                    sfNode.Kind = SidebarNodeKind::Surface;
                    sfNode.WorkspaceInputIndex = wsIndex;
                    sfNode.SurfaceInputIndex = sfIndex;
                    sfNode.WorkspaceId = ws.WorkspaceId;
                    sfNode.SurfaceId = sf.SurfaceId;
                    sfNode.WorkspaceOrdinal = static_cast<uint32_t>(wsIndex + 1);
                    sfNode.SurfaceOrdinal = static_cast<uint32_t>(sfIndex + 1);
                    sfNode.Status = sfAttention ? SidebarStatusKind::Attention :
                                    sfRunning   ? SidebarStatusKind::Running :
                                                  SidebarStatusKind::Idle;
                    sfNode.Icon = sfIcon;
                    result.Nodes.push_back(sfNode);

                    // 3rd level: expand split pane leaf rows when a surface has >1 leaf panes.
                    if (sf.Panes.size() > 1)
                    {
                        for (size_t paneIndex = 0; paneIndex < sf.Panes.size(); ++paneIndex)
                        {
                            const auto& pane = sf.Panes[paneIndex];
                            const auto paneIcon = pane.Icon != SidebarIconKind::Terminal ? pane.Icon : DetectIconKind(pane.Title);

                            SidebarProjectedNode paneNode{};
                            paneNode.Kind = SidebarNodeKind::Pane;
                            paneNode.WorkspaceInputIndex = wsIndex;
                            paneNode.SurfaceInputIndex = sfIndex;
                            paneNode.PaneInputIndex = paneIndex;
                            paneNode.WorkspaceId = ws.WorkspaceId;
                            paneNode.SurfaceId = sf.SurfaceId;
                            paneNode.PaneId = pane.PaneId;
                            paneNode.WorkspaceOrdinal = static_cast<uint32_t>(wsIndex + 1);
                            paneNode.SurfaceOrdinal = static_cast<uint32_t>(sfIndex + 1);
                            paneNode.IsLastChild = (paneIndex + 1 == sf.Panes.size());
                            paneNode.Status = pane.HasAttention ? SidebarStatusKind::Attention :
                                              pane.IsRunning    ? SidebarStatusKind::Running :
                                                                  SidebarStatusKind::Idle;
                            paneNode.Icon = paneIcon;
                            result.Nodes.push_back(paneNode);
                        }
                    }
                }
            }

            return result;
        }

    private:
        SidebarFilterMode _filterMode{ SidebarFilterMode::All };
        std::wstring _searchQuery;
        std::wstring _normalizedQuery;
        winrt::guid _selectedWorkspaceId{};
        std::unordered_set<winrt::guid> _manuallyCollapsedWorkspaces;
        std::unordered_set<winrt::guid> _manuallyExpandedWorkspaces;

        static bool _isAsciiAlphaNum(const wchar_t ch) noexcept
        {
            return (ch >= L'a' && ch <= L'z') ||
                   (ch >= L'A' && ch <= L'Z') ||
                   (ch >= L'0' && ch <= L'9');
        }

        static bool _containsWord(const std::wstring_view lowered, const std::wstring_view needle) noexcept
        {
            if (needle.empty() || lowered.size() < needle.size())
            {
                return false;
            }
            size_t pos = 0;
            while ((pos = lowered.find(needle, pos)) != std::wstring_view::npos)
            {
                const auto leftOk = pos == 0 || !_isAsciiAlphaNum(lowered[pos - 1]);
                const auto rightPos = pos + needle.size();
                const auto rightOk = rightPos == lowered.size() || !_isAsciiAlphaNum(lowered[rightPos]);
                if (leftOk && rightOk)
                {
                    return true;
                }
                ++pos;
            }
            return false;
        }

        static std::wstring _normalize(const std::wstring_view input)
        {
            size_t start = 0;
            while (start < input.size() && (input[start] == L' ' || input[start] == L'\t' || input[start] == L'\r' || input[start] == L'\n'))
            {
                ++start;
            }
            size_t end = input.size();
            while (end > start && (input[end - 1] == L' ' || input[end - 1] == L'\t' || input[end - 1] == L'\r' || input[end - 1] == L'\n'))
            {
                --end;
            }
            std::wstring value{ input.substr(start, end - start) };
            for (auto& ch : value)
            {
                if (ch >= L'A' && ch <= L'Z')
                {
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
            }
            return value;
        }

        bool _containsQuery(const std::wstring_view haystack) const
        {
            if (_normalizedQuery.empty())
            {
                return true;
            }
            if (haystack.empty())
            {
                return false;
            }
            const auto lowered = _normalize(haystack);
            return lowered.find(_normalizedQuery) != std::wstring::npos;
        }

        bool _workspaceMatchesSearch(const SidebarWorkspaceInput& ws) const
        {
            return _containsQuery(ws.Title) ||
                   _containsQuery(ws.Directory) ||
                   _containsQuery(ws.GitBranch) ||
                   _containsQuery(ws.ListeningPorts);
        }

        bool _surfaceOrPaneMatchesSearch(const SidebarSurfaceInput& sf) const
        {
            if (_containsQuery(sf.Title) || _containsQuery(sf.Subtitle))
            {
                return true;
            }
            for (const auto& pane : sf.Panes)
            {
                if (_containsQuery(pane.Title))
                {
                    return true;
                }
            }
            return false;
        }

        static bool _surfaceNeedsAttention(const SidebarSurfaceInput& sf) noexcept
        {
            if (sf.UnreadCount > 0 || sf.HasAttention)
            {
                return true;
            }
            return std::ranges::any_of(sf.Panes, [](const auto& pane) { return pane.HasAttention; });
        }

        static bool _surfaceIsRunning(const SidebarSurfaceInput& sf) noexcept
        {
            if (sf.IsRunning)
            {
                return true;
            }
            return std::ranges::any_of(sf.Panes, [](const auto& pane) { return pane.IsRunning; });
        }

        bool _matchesFilterMode(const bool sfAttention,
                                const bool sfRunning,
                                const bool wsAttention,
                                const bool wsRunning) const noexcept
        {
            switch (_filterMode)
            {
            case SidebarFilterMode::Attention:
                return sfAttention || wsAttention;
            case SidebarFilterMode::Running:
                return sfRunning || wsRunning;
            case SidebarFilterMode::All:
            default:
                return true;
            }
        }
    };
}
