// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "ProjectNavigationState.h"

namespace winrt::TerminalApp::implementation::projects
{
    enum class ProjectNavigationAction : uint8_t { NewProject, Library, Agents, SelectProject, CloseProject, Overview, Tasks, Terminal, Summary, Bind, Save, Archive, Restore, Retry };
    struct ProjectMetadataDraft
    {
        std::wstring Name;
        std::wstring Description;
        uint64_t Version{};
        bool Dirty{};
    };
    class ProjectNavigationControls final
    {
    public:
        ProjectNavigationControls();
        ~ProjectNavigationControls();
        Windows::UI::Xaml::Controls::Grid Toolbar{ nullptr };
        Windows::UI::Xaml::Controls::Grid Content{ nullptr };
        void SetHandler(std::function<void(ProjectNavigationAction, std::string)> handler) { _handler = std::move(handler); }
        void Render(const ProjectNavigationState& state, bool library);
        ProjectMetadataDraft Draft() const;
    private:
        std::function<void(ProjectNavigationAction, std::string)> _handler;
        std::map<std::string, ProjectMetadataDraft> _drafts;
        std::map<std::string, Windows::UI::Xaml::Controls::ComboBoxItem> _rows;
        std::vector<std::pair<Windows::UI::Xaml::Controls::Button, winrt::event_token>> _buttons;
        Windows::UI::Xaml::Controls::ComboBox _projects{ nullptr };
        Windows::UI::Xaml::Controls::ListView _library{ nullptr };
        Windows::UI::Xaml::Controls::StackPanel _overview{ nullptr };
        Windows::UI::Xaml::Controls::TextBox _name{ nullptr };
        Windows::UI::Xaml::Controls::TextBox _description{ nullptr };
        Windows::UI::Xaml::Controls::TextBlock _root{ nullptr };
        Windows::UI::Xaml::Controls::TextBlock _heading{ nullptr };
        Windows::UI::Xaml::Controls::TextBlock _notice{ nullptr };
        Windows::UI::Xaml::Controls::TextBlock _counts{ nullptr };
        Windows::UI::Xaml::Controls::Button _save{ nullptr };
        Windows::UI::Xaml::Controls::Button _archive{ nullptr };
        Windows::UI::Xaml::Controls::Button _restore{ nullptr };
        Windows::UI::Xaml::Controls::Button _bind{ nullptr };
        Windows::UI::Xaml::Controls::Button _retry{ nullptr };
        std::string _active;
        bool _rendering{};
        winrt::event_token _nameChanged{}, _descriptionChanged{}, _projectClick{}, _libraryClick{};
        void _Wire(const Windows::UI::Xaml::Controls::Button& button, const wchar_t* label, ProjectNavigationAction action);
        void _CaptureDraft();
        void _ActivateClickedItem(const Windows::UI::Xaml::Controls::ListView& list, const Windows::Foundation::IInspectable& item);
        void _RenderSelector(const ProjectNavigationState& state);
        void _RenderLibrary(const ProjectNavigationState& state);
    };
}
