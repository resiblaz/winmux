// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "ProjectNavigationControls.h"

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Markup;

namespace winrt::TerminalApp::implementation::projects
{
    ProjectNavigationControls::ProjectNavigationControls()
    {
        Toolbar = XamlReader::Load(LR"XAML(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Padding="20,12" Background="{ThemeResource WorkspaceChromeSurfaceRaisedBrush}">
  <ScrollViewer HorizontalScrollBarVisibility="Auto" HorizontalScrollMode="Enabled" VerticalScrollBarVisibility="Disabled" VerticalScrollMode="Disabled">
    <StackPanel Orientation="Horizontal" Spacing="8">
      <ComboBox x:Name="Projects" Width="220" VerticalAlignment="Center" HorizontalContentAlignment="Stretch"/>
      <Button x:Name="Tasks"/><Button x:Name="Overview"/><Button x:Name="Library"/><Button x:Name="New"/><Button x:Name="Agents"/>
    </StackPanel>
  </ScrollViewer>
</Grid>)XAML").as<Grid>();
        const auto side = [&](const wchar_t* name) { return Toolbar.FindName(name).as<Button>(); };
        _Wire(side(L"New"), L"Project_New", ProjectNavigationAction::NewProject);
        _Wire(side(L"Library"), L"Project_Library", ProjectNavigationAction::Library);
        _Wire(side(L"Agents"), L"Project_Agents", ProjectNavigationAction::Agents);
        _Wire(side(L"Overview"), L"Project_Manage", ProjectNavigationAction::Overview);
        _Wire(side(L"Tasks"), L"Project_Tasks", ProjectNavigationAction::Tasks);
        _projects = Toolbar.FindName(L"Projects").as<ComboBox>();
        _projects.PlaceholderText(RS_(L"Project_Label"));
        Automation::AutomationProperties::SetName(_projects, RS_(L"Project_Label"));
        _projectClick = _projects.SelectionChanged([this](const auto&, const SelectionChangedEventArgs&) {
            if (_rendering || !_handler) return;
            if (const auto selected = _projects.SelectedItem().try_as<ComboBoxItem>())
                _handler(ProjectNavigationAction::SelectProject, til::u16u8(winrt::unbox_value<winrt::hstring>(selected.Tag())));
        });
        Content = XamlReader::Load(LR"XAML(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Padding="24" RowSpacing="12" Background="{ThemeResource WorkspaceChromeSurfaceBrush}">
  <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="*"/></Grid.RowDefinitions>
  <TextBlock x:Name="Heading" FontSize="28" TextWrapping="Wrap"/>
  <StackPanel Grid.Row="1" Spacing="8"><TextBlock x:Name="Notice" TextWrapping="Wrap" Foreground="{ThemeResource WorkspaceChromeAccentBrush}"/><Button x:Name="Retry" Visibility="Collapsed"/></StackPanel>
  <Grid Grid.Row="2">
    <ListView x:Name="Library" IsItemClickEnabled="True" SelectionMode="Single" HorizontalContentAlignment="Stretch"/>
    <ScrollViewer x:Name="OverviewScroll" VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
      <StackPanel x:Name="Overview" MaxWidth="760" HorizontalAlignment="Left" Spacing="16">
        <TextBlock x:Name="Root" FontSize="16" TextWrapping="Wrap"/>
        <TextBlock x:Name="Counts" FontSize="16" TextWrapping="Wrap"/>
        <StackPanel Orientation="Horizontal" Spacing="8"><Button x:Name="OpenTasks"/><Button x:Name="OpenTerminal"/><Button x:Name="Defaults"/></StackPanel>
        <TextBox x:Name="Name" FontSize="16" MaxLength="128"/>
        <TextBox x:Name="Description" FontSize="16" AcceptsReturn="True" MinHeight="140" TextWrapping="Wrap" MaxLength="4096"/>
        <StackPanel Orientation="Horizontal" Spacing="8"><Button x:Name="Save"/><Button x:Name="Bind"/></StackPanel>
        <StackPanel Orientation="Horizontal" Spacing="8"><Button x:Name="Archive"/><Button x:Name="Restore"/><Button x:Name="CloseView"/></StackPanel>
      </StackPanel>
    </ScrollViewer>
  </Grid>
</Grid>)XAML").as<Grid>();
        const auto button = [&](const wchar_t* name) { return Content.FindName(name).as<Button>(); };
        _Wire(button(L"OpenTasks"), L"Project_Tasks", ProjectNavigationAction::Tasks);
        _Wire(button(L"OpenTerminal"), L"Project_NewTerminal", ProjectNavigationAction::Terminal);
        _Wire(button(L"Defaults"), L"Project_DefaultAgent", ProjectNavigationAction::Agents);
        _Wire(button(L"CloseView"), L"Project_CloseView", ProjectNavigationAction::CloseProject);
        _save = button(L"Save"); _Wire(_save, L"Project_Save", ProjectNavigationAction::Save);
        _bind = button(L"Bind"); _Wire(_bind, L"Project_Bind", ProjectNavigationAction::Bind);
        _archive = button(L"Archive"); _Wire(_archive, L"Project_Archive", ProjectNavigationAction::Archive);
        _restore = button(L"Restore"); _Wire(_restore, L"Project_Restore", ProjectNavigationAction::Restore);
        _retry = button(L"Retry"); _Wire(_retry, L"Project_Retry", ProjectNavigationAction::Retry);
        _name = Content.FindName(L"Name").as<TextBox>(); _description = Content.FindName(L"Description").as<TextBox>();
        _name.Header(winrt::box_value(RS_(L"Project_Name"))); _description.Header(winrt::box_value(RS_(L"Project_Description")));
        _root = Content.FindName(L"Root").as<TextBlock>(); _counts = Content.FindName(L"Counts").as<TextBlock>();
        _heading = Content.FindName(L"Heading").as<TextBlock>(); _notice = Content.FindName(L"Notice").as<TextBlock>();
        _overview = Content.FindName(L"Overview").as<StackPanel>(); _library = Content.FindName(L"Library").as<ListView>();
        _nameChanged = _name.TextChanged([this](const auto&, const auto&) { _CaptureDraft(); });
        _descriptionChanged = _description.TextChanged([this](const auto&, const auto&) { _CaptureDraft(); });
        _libraryClick = _library.ItemClick([this](const auto&, const ItemClickEventArgs& args) {
            _ActivateClickedItem(_library, args.ClickedItem());
        });
    }
    ProjectNavigationControls::~ProjectNavigationControls()
    {
        for (const auto& [button, token] : _buttons) button.Click(token);
        _name.TextChanged(_nameChanged); _description.TextChanged(_descriptionChanged);
        _projects.SelectionChanged(_projectClick); _library.ItemClick(_libraryClick);
    }
    void ProjectNavigationControls::_Wire(const Button& button, const wchar_t* label, ProjectNavigationAction action)
    {
        button.Content(winrt::box_value(RS_(label)));
        Automation::AutomationProperties::SetName(button, RS_(label));
        _buttons.emplace_back(button, button.Click([this, action](const auto&, const auto&) { if (_handler) _handler(action, _active); }));
    }
    void ProjectNavigationControls::_ActivateClickedItem(const ListView& list, const Windows::Foundation::IInspectable& item)
    {
        if (!_handler) return;
        for (const auto& candidate : list.Items())
        {
            if (const auto row = candidate.try_as<ListViewItem>(); row && (candidate == item || row.Content() == item))
            {
                const auto id = winrt::unbox_value_or<winrt::hstring>(row.Tag(), {});
                if (!id.empty()) _handler(ProjectNavigationAction::SelectProject, til::u16u8(id));
                return;
            }
        }
    }
    void ProjectNavigationControls::_CaptureDraft()
    {
        if (_rendering || _active.empty()) return;
        auto& draft = _drafts[_active];
        const auto name = std::wstring{ _name.Text() }, description = std::wstring{ _description.Text() };
        if (draft.Name != name || draft.Description != description)
        { draft.Name = name; draft.Description = description; draft.Dirty = true; }
    }
    ProjectMetadataDraft ProjectNavigationControls::Draft() const
    {
        const auto found = _drafts.find(_active);
        if (found == _drafts.end()) return {};
        auto draft = found->second; draft.Name = _name.Text(); draft.Description = _description.Text(); return draft;
    }
}
