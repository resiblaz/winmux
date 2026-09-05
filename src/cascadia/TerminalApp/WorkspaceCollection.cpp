// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "WorkspaceCollection.h"

namespace winrt::TerminalApp::implementation
{
    WorkspaceCollection::WorkspaceCollection() :
        _items{ winrt::single_threaded_observable_vector<TerminalApp::WorkspaceItemViewModel>() }
    {
    }

    Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel> WorkspaceCollection::Items() const noexcept
    {
        return _items;
    }

    std::optional<uint32_t> WorkspaceCollection::IndexOf(const winrt::guid& workspaceId) const noexcept
    try
    {
        if (workspaceId != winrt::guid{})
        {
            for (uint32_t index = 0; index < _items.Size(); ++index)
            {
                if (_items.GetAt(index).WorkspaceId() == workspaceId)
                {
                    return index;
                }
            }
        }
        return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }

    std::optional<uint32_t> WorkspaceCollection::IndexOfTab(const TerminalApp::Tab& tab) const noexcept
    try
    {
        if (tab)
        {
            for (uint32_t index = 0; index < _items.Size(); ++index)
            {
                if (winrt::get_self<WorkspaceItemViewModel>(_items.GetAt(index))->ContainsTab(tab))
                {
                    return index;
                }
            }
        }
        return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }

    TerminalApp::WorkspaceItemViewModel WorkspaceCollection::ItemForTab(const TerminalApp::Tab& tab) const noexcept
    try
    {
        if (const auto index = IndexOfTab(tab))
        {
            return _items.GetAt(index.value());
        }
        return nullptr;
    }
    catch (...)
    {
        return nullptr;
    }

    std::optional<uint32_t> WorkspaceCollection::FlatTabIndex(const TerminalApp::Tab& tab) const noexcept
    try
    {
        uint32_t flatIndex{};
        for (const auto& item : _items)
        {
            for (const auto& surface : item.Surfaces())
            {
                if (surface == tab)
                {
                    return flatIndex;
                }
                ++flatIndex;
            }
        }
        return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }

    std::optional<uint32_t> WorkspaceCollection::ActiveFlatTabIndex(const winrt::guid& workspaceId) const noexcept
    try
    {
        if (const auto index = IndexOf(workspaceId))
        {
            const auto item = _items.GetAt(index.value());
            const auto first = FlatTabCountBefore(index.value());
            uint32_t surfaceIndex{};
            if (item.Surfaces().IndexOf(item.SourceTab(), surfaceIndex))
            {
                return first + surfaceIndex;
            }
        }
        return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }

    std::optional<WorkspaceCollection::FlatSpan> WorkspaceCollection::Span(const winrt::guid& workspaceId) const noexcept
    try
    {
        if (const auto index = IndexOf(workspaceId))
        {
            return FlatSpan{ FlatTabCountBefore(index.value()), _items.GetAt(index.value()).SurfaceCount() };
        }
        return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }

    uint32_t WorkspaceCollection::FlatTabCountBefore(const uint32_t workspaceIndex) const noexcept
    try
    {
        uint32_t count{};
        const auto end = std::min(workspaceIndex, _items.Size());
        for (uint32_t index = 0; index < end; ++index)
        {
            count += _items.GetAt(index).SurfaceCount();
        }
        return count;
    }
    catch (...)
    {
        return 0;
    }

    std::optional<uint32_t> WorkspaceCollection::FlatInsertIndexAfter(const winrt::guid& workspaceId) const noexcept
    {
        if (const auto index = IndexOf(workspaceId))
        {
            return FlatTabCountBefore(index.value()) + _items.GetAt(index.value()).SurfaceCount();
        }
        return std::nullopt;
    }

    uint32_t WorkspaceCollection::WorkspaceIndexForFlatInsertion(const uint32_t flatTabIndex) const noexcept
    try
    {
        uint32_t offset{};
        for (uint32_t index = 0; index < _items.Size(); ++index)
        {
            const auto next = offset + _items.GetAt(index).SurfaceCount();
            if (flatTabIndex <= offset)
            {
                return index;
            }
            if (flatTabIndex < next)
            {
                return index + 1;
            }
            offset = next;
        }
        return _items.Size();
    }
    catch (...)
    {
        return _items.Size();
    }

    WorkspaceCollection::UpdateScope::UpdateScope(WorkspaceCollection& owner) noexcept : _owner{ &owner }
    {
        ++owner._updateDepth;
    }

    WorkspaceCollection::UpdateScope::UpdateScope(UpdateScope&& other) noexcept : _owner{ std::exchange(other._owner, nullptr) } {}

    WorkspaceCollection::UpdateScope::~UpdateScope() noexcept
    {
        try { Commit(); } CATCH_LOG();
    }

    void WorkspaceCollection::UpdateScope::Commit()
    {
        if (const auto owner = std::exchange(_owner, nullptr)) owner->_endUpdate();
    }

    WorkspaceCollection::UpdateScope WorkspaceCollection::BeginUpdate() noexcept { return UpdateScope{ *this }; }

    std::function<void()> WorkspaceCollection::SetChangedHandler(std::function<void()> handler)
    {
        return std::exchange(_changed, std::move(handler));
    }

    uint64_t WorkspaceCollection::Generation() const noexcept { return _generation; }

    void WorkspaceCollection::_markChanged() { _dirty = true; _publish(); }

    void WorkspaceCollection::_endUpdate() { if (--_updateDepth == 0) _publish(); }

    void WorkspaceCollection::_publish()
    {
        if (_updateDepth || _publishing) return;
        _publishing = true;
        const auto reset = wil::scope_exit([&] { _publishing = false; });
        while (std::exchange(_dirty, false))
        {
            ++_generation;
            if (_changed) _changed();
        }
    }

    void WorkspaceCollection::Insert(const uint32_t index, const TerminalApp::WorkspaceItemViewModel& item)
    {
        THROW_HR_IF(E_INVALIDARG, !item || index > _items.Size());
        THROW_HR_IF(E_UNEXPECTED, IndexOf(item.WorkspaceId()).has_value());
        _items.InsertAt(index, item);
        _markChanged();
    }

    void WorkspaceCollection::Remove(const uint32_t index)
    {
        THROW_HR_IF(E_BOUNDS, index >= _items.Size());
        _items.RemoveAt(index);
        _markChanged();
    }

    void WorkspaceCollection::Move(const uint32_t from, const uint32_t to)
    {
        THROW_HR_IF(E_BOUNDS, from >= _items.Size() || to >= _items.Size());
        if (from != to)
        {
            const auto item = _items.GetAt(from);
            _items.RemoveAt(from);
            _items.InsertAt(to, item);
            _markChanged();
        }
    }

    winrt::guid WorkspaceCollection::AddSurface(const winrt::guid& workspaceId,
                                                const TerminalApp::Tab& tab,
                                                const std::optional<winrt::guid> surfaceId)
    {
        const auto index = IndexOf(workspaceId);
        THROW_HR_IF(E_INVALIDARG, !index);
        const auto id = winrt::get_self<WorkspaceItemViewModel>(_items.GetAt(index.value()))->AddSurface(tab, surfaceId);
        _markChanged();
        return id;
    }

    bool WorkspaceCollection::RemoveSurface(const TerminalApp::Tab& tab)
    {
        const auto index = IndexOfTab(tab);
        if (!index)
        {
            return false;
        }

        const auto item = _items.GetAt(index.value());
        const auto itemImpl = winrt::get_self<WorkspaceItemViewModel>(item);
        THROW_HR_IF(E_UNEXPECTED, !itemImpl->RemoveSurface(tab));
        if (item.SurfaceCount() == 0)
        {
            _items.RemoveAt(index.value());
            _markChanged();
            return true;
        }
        _markChanged();
        return false;
    }

    bool WorkspaceCollection::MoveSurface(const TerminalApp::Tab& tab, const uint32_t targetIndex)
    {
        const auto item = ItemForTab(tab);
        const auto moved = item && winrt::get_self<WorkspaceItemViewModel>(item)->MoveSurface(tab, targetIndex);
        if (moved) _markChanged();
        return moved;
    }

    void WorkspaceCollection::SetActiveTab(const TerminalApp::Tab& tab)
    {
        const auto item = ItemForTab(tab);
        THROW_HR_IF(E_INVALIDARG, !item);
        if (item.SourceTab() != tab)
        {
            winrt::get_self<WorkspaceItemViewModel>(item)->SetActiveTab(tab);
            _markChanged();
        }
    }

    void WorkspaceCollection::AssertInvariants(const Windows::Foundation::Collections::IObservableVector<TerminalApp::Tab>& tabs) const
    {
        uint32_t flatIndex{};
        std::unordered_set<winrt::guid> workspaceIds;
        std::unordered_set<winrt::guid> surfaceIds;
        for (const auto& item : _items)
        {
            THROW_HR_IF(E_UNEXPECTED, !item || item.WorkspaceId() == winrt::guid{} || item.SurfaceCount() == 0);
            THROW_HR_IF(E_UNEXPECTED, !workspaceIds.emplace(item.WorkspaceId()).second);
            const auto itemImpl = winrt::get_self<WorkspaceItemViewModel>(item);
            THROW_HR_IF(E_UNEXPECTED, !itemImpl->ContainsTab(item.SourceTab()));
            for (const auto& surface : item.Surfaces())
            {
                THROW_HR_IF(E_UNEXPECTED, flatIndex >= tabs.Size() || tabs.GetAt(flatIndex) != surface);
                const auto surfaceId = itemImpl->SurfaceIdForTab(surface);
                THROW_HR_IF(E_UNEXPECTED, surfaceId == winrt::guid{} || !surfaceIds.emplace(surfaceId).second);
                ++flatIndex;
            }
        }
        THROW_HR_IF(E_UNEXPECTED, flatIndex != tabs.Size());
    }
}
