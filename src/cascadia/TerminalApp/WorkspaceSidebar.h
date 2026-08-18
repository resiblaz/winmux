// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "WorkspaceSidebar.g.h"

namespace winrt::TerminalApp::implementation
{
    struct WorkspaceSidebar : WorkspaceSidebarT<WorkspaceSidebar>
    {
        WorkspaceSidebar();

        bool IsExpanded() const noexcept;
        void IsExpanded(bool value);
        double ExpandedWidth() const noexcept;
        void ExpandedWidth(double value);

        til::property_changed_event PropertyChanged;
        til::typed_event<TerminalApp::WorkspaceSidebar, Windows::Foundation::IInspectable> NewRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> PinRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> CloseRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> CloseOthersRequested;
        til::typed_event<TerminalApp::WorkspaceSidebar, TerminalApp::WorkspaceItemViewModel> RenameRequested;

        void _Toggle_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Resize_DragDelta(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::Controls::Primitives::DragDeltaEventArgs& args);
        void _New_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Pin_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Close_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _CloseOthers_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);
        void _Rename_Click(const Windows::Foundation::IInspectable& sender, const Windows::UI::Xaml::RoutedEventArgs& args);

    private:
        static constexpr double MinimumExpandedWidth{ 220.0 };
        static constexpr double MaximumExpandedWidth{ 480.0 };
        static constexpr double CollapsedWidth{ 48.0 };

        bool _isExpanded{ true };
        double _expandedWidth{ 280.0 };

        void _ApplyLayout();
        TerminalApp::WorkspaceItemViewModel _ItemFromSender(const Windows::Foundation::IInspectable& sender) const;
    };
}

namespace winrt::TerminalApp::factory_implementation
{
    BASIC_FACTORY(WorkspaceSidebar);
}
