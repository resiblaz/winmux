// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TaskPage.h"
#include "RuntimeCrypto.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace winrt::TerminalApp::implementation
{
    safe_void_coroutine TaskPage::_EditAcceptance()
    {
        const auto lifetime = get_strong(); const auto selected = _viewModel.SelectedTask(); const auto presenter = _dialogPresenter.get();
        if (!selected || !presenter || !_source || !_source->UsesSummaries() || !_actions) co_return;
        const auto task = winrt::get_self<implementation::TaskItemViewModel>(selected)->NativeProjection();
        if (task.IsSummary || task.HasActiveRun || task.Status != tasks::TaskStatus::InReview || task.Acceptance.empty() || _actions->Pending(task.Id)) co_return;
        ContentDialog dialog; dialog.Title(box_value(RS_(L"TaskBoard_EditAcceptanceTitle")));
        dialog.PrimaryButtonText(RS_(L"TaskBoard_Confirm")); dialog.CloseButtonText(RS_(L"TaskBoard_Cancel")); dialog.DefaultButton(ContentDialogButton::Close);
        StackPanel content; content.Spacing(12); std::vector<CheckBox> criteria;
        for (const auto& criterion : task.Acceptance)
        {
            CheckBox checkbox; TextBlock label; label.Text(criterion.Text); label.TextWrapping(TextWrapping::Wrap);
            checkbox.Content(label); checkbox.IsChecked(criterion.Done); content.Children().Append(checkbox); criteria.push_back(checkbox);
        }
        ScrollViewer scroll; scroll.MaxHeight(420); scroll.Content(content); dialog.Content(scroll);
        if (co_await presenter.ShowDialog(dialog) != ContentDialogResult::Primary) co_return;
        const auto command = ::Microsoft::Terminal::TaskRuntime::NewRuntimeUuid(); if (!command) co_return;
        projects::protocol::TaskBoardCommandRequest request;
        request.CommandId = *command; request.ProjectId = til::u16u8(task.WorkspaceKey); request.TaskId = til::u16u8(task.Id);
        request.ExpectedTaskVersion = task.Version; request.Operation = projects::protocol::TaskBoardOperation::UpdateAcceptance;
        request.Target = projects::protocol::TaskProjectionStatus::InReview;
        for (uint32_t index = 0; index < criteria.size(); ++index) request.Acceptance.emplace_back(index, winrt::unbox_value_or<bool>(criteria[index].IsChecked(), false));
        _actions->Execute(std::move(request));
    }
}
