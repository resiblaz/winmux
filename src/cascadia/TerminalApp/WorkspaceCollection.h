// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceItemViewModel.h"
#include <functional>

namespace winrt::TerminalApp::implementation
{
    // Canonical owner of ordered Workspaces and their Surface Tabs. Flattening
    // Items().Surfaces must match TerminalPage's Tab collection; all grouping
    // mutations stay behind this type so that invariant has one enforcement
    // point.
    class WorkspaceCollection final
    {
    public:
        struct FlatSpan
        {
            uint32_t First{};
            uint32_t Count{};
        };

        WorkspaceCollection();

        class UpdateScope final
        {
        public:
            explicit UpdateScope(WorkspaceCollection& owner) noexcept;
            UpdateScope(UpdateScope&& other) noexcept;
            UpdateScope(const UpdateScope&) = delete;
            ~UpdateScope() noexcept;
            void Commit();
        private:
            WorkspaceCollection* _owner;
        };
        [[nodiscard]] UpdateScope BeginUpdate() noexcept;
        std::function<void()> SetChangedHandler(std::function<void()> handler);
        uint64_t Generation() const noexcept;
        bool IsUpdating() const noexcept { return _updateDepth != 0; }
        void NotifyNativeReorder() { _markChanged(); }

        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel> Items() const noexcept;
        std::optional<uint32_t> IndexOf(const winrt::guid& workspaceId) const noexcept;
        std::optional<uint32_t> IndexOfTab(const TerminalApp::Tab& tab) const noexcept;
        TerminalApp::WorkspaceItemViewModel ItemForTab(const TerminalApp::Tab& tab) const noexcept;
        std::optional<uint32_t> FlatTabIndex(const TerminalApp::Tab& tab) const noexcept;
        std::optional<uint32_t> ActiveFlatTabIndex(const winrt::guid& workspaceId) const noexcept;
        std::optional<FlatSpan> Span(const winrt::guid& workspaceId) const noexcept;
        uint32_t FlatTabCountBefore(uint32_t workspaceIndex) const noexcept;
        std::optional<uint32_t> FlatInsertIndexAfter(const winrt::guid& workspaceId) const noexcept;
        uint32_t WorkspaceIndexForFlatInsertion(uint32_t flatTabIndex) const noexcept;

        void Insert(uint32_t index, const TerminalApp::WorkspaceItemViewModel& item);
        void Remove(uint32_t index);
        void Move(uint32_t from, uint32_t to);
        winrt::guid AddSurface(const winrt::guid& workspaceId,
                              const TerminalApp::Tab& tab,
                              std::optional<winrt::guid> surfaceId = std::nullopt);
        bool RemoveSurface(const TerminalApp::Tab& tab);
        bool MoveSurface(const TerminalApp::Tab& tab, uint32_t targetIndex);
        void SetActiveTab(const TerminalApp::Tab& tab);
        void AssertInvariants(const Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab>& tabs) const;

    private:
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel> _items;
        std::function<void()> _changed;
        uint32_t _updateDepth{};
        uint64_t _generation{};
        bool _dirty{};
        bool _publishing{};
        void _markChanged();
        void _endUpdate();
        void _publish();
    };
}
