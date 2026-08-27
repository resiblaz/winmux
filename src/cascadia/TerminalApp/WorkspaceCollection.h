// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceItemViewModel.h"

namespace winrt::TerminalApp::implementation
{
    // Canonical owner of the ordered Workspace projection. The order mirrors
    // TerminalPage's Tab collection and all mutations are kept behind this
    // type so the parallel-collection invariant has one enforcement point.
    class WorkspaceCollection final
    {
    public:
        WorkspaceCollection();

        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel> Items() const noexcept;
        std::optional<uint32_t> IndexOf(const winrt::guid& workspaceId) const noexcept;

        void Insert(uint32_t index, const TerminalApp::WorkspaceItemViewModel& item);
        void Remove(uint32_t index);
        void Move(uint32_t from, uint32_t to);

    private:
        Windows::Foundation::Collections::IObservableVector<TerminalApp::WorkspaceItemViewModel> _items;
    };
}
