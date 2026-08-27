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

    void WorkspaceCollection::Insert(const uint32_t index, const TerminalApp::WorkspaceItemViewModel& item)
    {
        THROW_HR_IF(E_INVALIDARG, !item || index > _items.Size());
        THROW_HR_IF(E_UNEXPECTED, IndexOf(item.WorkspaceId()).has_value());
        _items.InsertAt(index, item);
    }

    void WorkspaceCollection::Remove(const uint32_t index)
    {
        THROW_HR_IF(E_BOUNDS, index >= _items.Size());
        _items.RemoveAt(index);
    }

    void WorkspaceCollection::Move(const uint32_t from, const uint32_t to)
    {
        THROW_HR_IF(E_BOUNDS, from >= _items.Size() || to >= _items.Size());
        if (from != to)
        {
            const auto item = _items.GetAt(from);
            _items.RemoveAt(from);
            _items.InsertAt(to, item);
        }
    }
}
