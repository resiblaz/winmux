// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentCatalogPage.h"
#include "AgentCatalogPage.g.cpp"
#include "RuntimeCrypto.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    using namespace agents::protocol;

    AgentCatalogPage::AgentCatalogPage()
    {
        _syncing = true;
        InitializeComponent();
        _syncing = false;
        _timer.Interval(std::chrono::seconds{ 5 });
        _timer.Tick([weak = get_weak()](auto&&, auto&&) {
            if (const auto self = weak.get(); self && self->_active && !self->_model.Saving && self->_profiles.size() <= 50) self->_refresh();
        });
        Unloaded([weak = get_weak()](auto&&, auto&&) { if (const auto self = weak.get()) self->Stop(); });
        _renderEditor();
    }

    void AgentCatalogPage::Open(std::shared_ptr<agents::IAgentCatalogSource> source, std::string workspace, std::wstring label,
                                winrt::TerminalApp::IDialogPresenter presenter, std::function<void()> closed)
    {
        ++_generation;
        _source = std::move(source); _workspace = std::move(workspace); _closed = std::move(closed); _presenter = presenter;
        _active = true; _listing = false; _loadingEditor = false;
        WorkspaceTarget().Text(std::wstring{ RS_(L"Agent_WorkspaceTarget") } + label);
        _default.reset(); _refresh(); _timer.Start(); _renderEditor();
    }
    void AgentCatalogPage::Stop()
    {
        _active = false; ++_generation; ++_editorGeneration; _listing = false; _timer.Stop();
        if (_model.Saving) { _model.Saving = false; _model.OutcomeUnknown = _model.Pending.has_value(); }
    }

    void AgentCatalogPage::_refresh(const bool more)
    {
        if (!_active || !_source || _listing) return;
        _listing = true;
        AgentCatalogQueryRequest query;
        if (!_workspace.empty()) query.WorkspaceId = _workspace;
        query.IncludeArchived = IncludeArchived().IsChecked().Value();
        if (more) query.AfterId = _after;
        const auto generation = _generation;
        _source->Query(query, [weak = get_weak(), generation, more](auto reply) {
            const auto self = weak.get();
            if (!self || generation != self->_generation) return;
            self->_listing = false;
            if (!reply.Succeeded) { self->StatusText().Text(til::u8u16(reply.Error)); self->_updateEnabled(); return; }
            if (reply.Value.Default && (!self->_default || reply.Value.Default->Version >= self->_default->Version)) self->_default = reply.Value.Default;
            if (!more) self->_profiles.clear();
            for (auto& profile : reply.Value.Profiles)
            {
                const auto found = std::ranges::find(self->_profiles, profile.Id, &AgentProfileProjection::Id);
                if (found == self->_profiles.end()) self->_profiles.emplace_back(std::move(profile));
                else if (profile.Version >= found->Version) *found = std::move(profile);
            }
            if (self->_model.Saved)
            {
                if (const auto found = std::ranges::find(self->_profiles, self->_model.Saved->Id, &AgentProfileProjection::Id); found != self->_profiles.end())
                {
                    if (found->Version < self->_model.Saved->Version) *found = *self->_model.Saved;
                    else if (found->Version > self->_model.Saved->Version) self->StatusText().Text(RS_(L"Agent_ChangedElsewhere"));
                }
            }
            self->_after = reply.Value.NextAfterId;
            self->_renderList(); self->_updateEnabled();
        });
    }

    void AgentCatalogPage::_renderList()
    {
        _syncing = true;
        AgentList().Items().Clear(); _visibleIds.clear();
        const auto search = std::wstring{ AgentSearch().Text() };
        int32_t selected = -1;
        for (const auto& profile : _profiles)
        {
            if (profile.Archived && !IncludeArchived().IsChecked().Value()) continue;
            const auto name = til::u8u16(profile.Configuration.Name);
            if (!search.empty() && name.find(search) == std::wstring::npos && til::u8u16(profile.Id).find(search) == std::wstring::npos) continue;
            auto text = name + L" · " + til::u8u16(profile.Id.substr(0, 8)) + L"\nCodex · v" + std::to_wstring(profile.Revision) + L" · ";
            text += profile.Archived ? std::wstring{ RS_(L"Agent_Archived") } : profile.ActiveRuns ? std::to_wstring(profile.ActiveRuns) + L" " + std::wstring{ RS_(L"Agent_ActiveRuns") } : std::wstring{ RS_(L"Agent_Idle") };
            if (_model.Saved && profile.Id == _model.Saved->Id) selected = static_cast<int32_t>(_visibleIds.size());
            _visibleIds.push_back(profile.Id); AgentList().Items().Append(box_value(hstring{ text }));
        }
        AgentList().SelectedIndex(selected);
        MoreAgentsButton().Visibility(_after.empty() ? Visibility::Collapsed : Visibility::Visible);
        _syncing = false;
    }

    void AgentCatalogPage::_load(std::string id)
    {
        if (!_source) return;
        _loadingEditor = true; _updateEnabled();
        const auto generation = _generation; const auto editor = ++_editorGeneration;
        AgentCatalogQueryRequest query; query.AgentId = id;
        if (!_workspace.empty()) query.WorkspaceId = _workspace;
        _source->Query(query, [weak = get_weak(), generation, editor](auto reply) {
            const auto self = weak.get();
            if (!self || generation != self->_generation || editor != self->_editorGeneration) return;
            self->_loadingEditor = false;
            if (!reply.Succeeded || reply.Value.Profiles.size() != 1)
            { self->StatusText().Text(til::u8u16(reply.Error)); self->_updateEnabled(); return; }
            self->_model.Load(std::move(reply.Value.Profiles.front()));
            self->StatusText().Text({});
            self->_default = std::move(reply.Value.Default);
            self->_renderEditor(); self->_refresh();
        });
    }

    void AgentCatalogPage::_renderEditor()
    {
        _syncing = true;
        NameInput().Text(til::u8u16(_model.Draft.Name)); DescriptionInput().Text(til::u8u16(_model.Draft.Description));
        ModelInput().Text(til::u8u16(_model.Draft.Model)); InstructionsInput().Text(til::u8u16(_model.Draft.Instructions));
        _syncing = false; _updateEnabled();
    }

    void AgentCatalogPage::_updateEnabled()
    {
        const auto busy = _model.Saving || _loadingEditor;
        const auto dirty = _model.Dirty();
        const auto saved = _model.Saved.has_value();
        const auto archived = saved && _model.Saved->Archived;
        const auto editable = !busy && !_model.OutcomeUnknown && !archived;
        NameInput().IsReadOnly(!editable); DescriptionInput().IsReadOnly(!editable);
        ModelInput().IsReadOnly(!editable); InstructionsInput().IsReadOnly(!editable);
        SaveAgentButton().IsEnabled(_source && !busy && (_model.OutcomeUnknown || (!archived && dirty)));
        SaveAgentButton().Content(box_value(RS_(_model.OutcomeUnknown ? L"Agent_RetryOperation" : L"Agent_Save")));
        CopyAgentButton().IsEnabled(!busy && !_model.OutcomeUnknown && !_model.Draft.Name.empty());
        ReloadAgentButton().IsEnabled(!busy); NewAgentButton().IsEnabled(!busy && !dirty && !_model.OutcomeUnknown);
        AgentList().IsEnabled(!busy && !dirty && !_model.OutcomeUnknown);
        ArchiveAgentButton().Visibility(saved && !archived ? Visibility::Visible : Visibility::Collapsed);
        RestoreAgentButton().Visibility(archived ? Visibility::Visible : Visibility::Collapsed);
        ArchiveAgentButton().IsEnabled(!busy && !dirty && !_model.OutcomeUnknown);
        RestoreAgentButton().IsEnabled(!busy && !_model.OutcomeUnknown);
        SetDefaultButton().IsEnabled(_default && saved && !archived && !busy && !dirty && !_model.OutcomeUnknown);
        ClearDefaultButton().IsEnabled(_default && _default->AgentId && !busy && !_model.OutcomeUnknown);
        BackButton().IsEnabled(!busy); RefreshButton().IsEnabled(!_listing);
        MoreAgentsButton().IsEnabled(!_listing); BusyProgress().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
        EditorTitle().Text(saved ? hstring{ til::u8u16(_model.Saved->Configuration.Name) + L" · v" + std::to_wstring(_model.Saved->Revision) } : RS_(L"Agent_NewTitle"));
        DraftStateText().Text(dirty ? RS_(L"Agent_UnsavedDraft") : L"");
        if (_model.OutcomeUnknown) StatusText().Text(RS_(L"Agent_UnknownOutcome"));
    }

    void AgentCatalogPage::_DraftChanged(const IInspectable&, const TextChangedEventArgs&)
    {
        if (_syncing) return;
        _model.Draft.Name = til::u16u8(NameInput().Text()); _model.Draft.Description = til::u16u8(DescriptionInput().Text());
        _model.Draft.Model = til::u16u8(ModelInput().Text()); _model.Draft.Instructions = til::u16u8(InstructionsInput().Text());
        _updateEnabled();
    }

    void AgentCatalogPage::_execute(AgentCatalogCommandRequest command)
    {
        if (!_source) return;
        _model.Pending = command; _model.Saving = true; _updateEnabled();
        const auto generation = _generation;
        _source->Execute(command, [weak = get_weak(), generation, command](auto reply) {
            const auto self = weak.get();
            if (!self || generation != self->_generation) return;
            self->_model.Complete(reply);
            if (!reply.Succeeded)
            {
                self->_updateEnabled();
                self->StatusText().Text(reply.OutcomeUnknown ? RS_(L"Agent_UnknownOutcome") : hstring{ til::u8u16(reply.Error) });
                return;
            }
            self->StatusText().Text(RS_(L"Agent_Saved"));
            if (command.Operation == AgentCatalogOperation::SetWorkspaceDefault) { self->_refresh(); self->_updateEnabled(); }
            else self->_load(command.AgentId);
        });
    }

    void AgentCatalogPage::_Save_Click(const IInspectable&, const RoutedEventArgs&)
    {
        const auto command = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid();
        const auto id = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid();
        if (!command || !id || _model.Saving || _loadingEditor) return;
        _execute(_model.Prepare(*command, *id));
    }
    void AgentCatalogPage::_operation(const AgentCatalogOperation operation)
    {
        const auto id = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid();
        if (!id || _model.Saving || _loadingEditor) return;
        AgentCatalogCommandRequest command;
        command.CommandId = *id; command.Operation = operation;
        if (operation == AgentCatalogOperation::SetWorkspaceDefault)
        {
            if (!_default) return;
            command.WorkspaceId = _workspace; command.ExpectedVersion = _default->Version;
            if (_model.Saved) command.AgentId = _model.Saved->Id;
        }
        else
        {
            if (!_model.Saved) return;
            command.AgentId = _model.Saved->Id; command.ExpectedVersion = _model.Saved->Version;
        }
        _execute(std::move(command));
    }

    void AgentCatalogPage::_Back_Click(const IInspectable&, const RoutedEventArgs&) { if (_model.Saving || _loadingEditor) return; Stop(); if (_closed) _closed(); }
    void AgentCatalogPage::_Refresh_Click(const IInspectable&, const RoutedEventArgs&) { _refresh(); }
    void AgentCatalogPage::_More_Click(const IInspectable&, const RoutedEventArgs&) { _refresh(true); }
    void AgentCatalogPage::_ArchivedFilter_Click(const IInspectable&, const RoutedEventArgs&) { _refresh(); }
    void AgentCatalogPage::_SearchChanged(const IInspectable&, const TextChangedEventArgs&) { if (!_syncing) _renderList(); }
    void AgentCatalogPage::_New_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (_model.Dirty() || _model.Saving || _model.OutcomeUnknown) return;
        ++_editorGeneration; _model.New(); StatusText().Text({}); _renderEditor(); _renderList(); NameInput().Focus(FocusState::Programmatic);
    }
    void AgentCatalogPage::_Copy_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (_model.Saving || _model.OutcomeUnknown) return;
        ++_editorGeneration; _model.Copy(_model.Draft.Name + til::u16u8(RS_(L"Agent_CopySuffix")));
        StatusText().Text({});
        _renderEditor(); _renderList(); NameInput().Focus(FocusState::Programmatic); NameInput().SelectAll();
    }
    void AgentCatalogPage::_Reload_Click(const IInspectable&, const RoutedEventArgs&)
    {
        if (_model.Saving) return;
        if (_model.OutcomeUnknown && _model.Pending && !_model.Pending->AgentId.empty()) _load(_model.Pending->AgentId);
        else if (_model.Saved) _load(_model.Saved->Id);
        else { ++_editorGeneration; _model.New(); StatusText().Text({}); _renderEditor(); _renderList(); }
    }
    void AgentCatalogPage::_AgentSelected(const IInspectable&, const SelectionChangedEventArgs&)
    {
        if (_syncing || _model.Dirty() || _model.Saving || _loadingEditor) return;
        const auto index = AgentList().SelectedIndex();
        if (index >= 0 && static_cast<size_t>(index) < _visibleIds.size()) _load(_visibleIds[index]);
    }
    void AgentCatalogPage::_Archive_Click(const IInspectable&, const RoutedEventArgs&) { _archive(); }
    void AgentCatalogPage::_Restore_Click(const IInspectable&, const RoutedEventArgs&) { _operation(AgentCatalogOperation::Restore); }
    void AgentCatalogPage::_SetDefault_Click(const IInspectable&, const RoutedEventArgs&) { _operation(AgentCatalogOperation::SetWorkspaceDefault); }
    void AgentCatalogPage::_ClearDefault_Click(const IInspectable&, const RoutedEventArgs&)
    {
        const auto id = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid();
        if (!id || !_default || _model.Saving) return;
        AgentCatalogCommandRequest command;
        command.CommandId = *id; command.Operation = AgentCatalogOperation::SetWorkspaceDefault;
        command.WorkspaceId = _workspace; command.ExpectedVersion = _default->Version;
        _execute(std::move(command));
    }
    void AgentCatalogPage::_Preview_Click(const IInspectable&, const RoutedEventArgs&) { _preview(); }

    safe_void_coroutine AgentCatalogPage::_archive()
    {
        const auto strong = get_strong();
        const auto presenter = _presenter.get();
        if (!presenter || !_model.Saved || _model.Dirty() || _model.Saving) co_return;
        const auto saved = *_model.Saved;
        auto body = std::wstring{ RS_(L"Agent_ArchiveBody") };
        for (const auto& workspace : saved.DefaultWorkspaces) body += L"\n" + til::u8u16(workspace);
        TextBlock text; text.Text(body); text.TextWrapping(TextWrapping::Wrap);
        ScrollViewer scroll; scroll.MaxHeight(320); scroll.Content(text);
        ContentDialog dialog; dialog.Title(box_value(RS_(L"Agent_ArchiveTitle"))); dialog.Content(scroll);
        dialog.PrimaryButtonText(RS_(L"Agent_Archive")); dialog.CloseButtonText(RS_(L"Agent_Cancel"));
        dialog.DefaultButton(ContentDialogButton::Close);
        if (co_await presenter.ShowDialog(dialog) == ContentDialogResult::Primary && _active && _model.Saved &&
            _model.Saved->Id == saved.Id && _model.Saved->Version == saved.Version)
            _operation(AgentCatalogOperation::Archive);
    }
    safe_void_coroutine AgentCatalogPage::_preview()
    {
        const auto strong = get_strong(); const auto presenter = _presenter.get();
        if (!presenter) co_return;
        TextBox text; text.IsReadOnly(true); text.AcceptsReturn(true); text.TextWrapping(TextWrapping::Wrap); text.MaxHeight(420);
        text.Text(_model.Draft.Instructions.empty() ? RS_(L"Agent_NoInstructions") : hstring{ L"Agent instructions:\n" + til::u8u16(_model.Draft.Instructions) });
        ContentDialog dialog; dialog.Title(box_value(RS_(L"Agent_PreviewTitle"))); dialog.Content(text); dialog.CloseButtonText(RS_(L"Agent_Close"));
        co_await presenter.ShowDialog(dialog);
    }
}
