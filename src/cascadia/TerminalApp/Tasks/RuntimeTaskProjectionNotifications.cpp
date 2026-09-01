// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskProjectionSource.h"

namespace winrt::TerminalApp::implementation::tasks
{
    using namespace ::Microsoft::Terminal::TaskRuntimeProtocol;

    void RuntimeTaskProjectionSource::_notify() noexcept
    {
        ChangedHandler handler;
        {
            const std::scoped_lock lock{ _mutex };
            if (_stopping)
                return;
            handler = _changed;
        }
        if (!handler)
            return;
        try
        {
            if (_dispatcher && !_dispatcher.HasThreadAccess())
            {
                static_cast<void>(_dispatcher.RunAsync(
                    Windows::UI::Core::CoreDispatcherPriority::Normal,
                    [handler = std::move(handler)] { handler(); }));
            }
            else
            {
                handler();
            }
        }
        catch (...)
        {
        }
    }

    void RuntimeTaskProjectionSource::_notifySurface(
        const TaskUiCommandRequest& command,
        const TaskUiCommandResult& result) noexcept
    try
    {
        if (!result.Surface && !result.SurfaceCloseAuthorized)
            return;
        SurfaceHandler handler;
        {
            const std::scoped_lock lock{ _mutex };
            if (_stopping)
                return;
            handler = _surfaceChanged;
        }
        if (!handler)
            return;
        const auto snapshot = _coordinator.Snapshot();
        const auto projected = std::ranges::find(
            snapshot, til::u8u16(command.TaskId), &TaskProjection::Id);
        if (projected == snapshot.end())
            return;

        TaskSurfaceDescriptor descriptor;
        descriptor.Action = result.Surface ?
                                TaskSurfaceAction::OpenOrActivate :
                                TaskSurfaceAction::CloseAuthorized;
        descriptor.TaskId = projected->Id;
        descriptor.WorkspaceId = projected->WorkspaceKey;
        descriptor.Title = projected->Title;
        descriptor.RuntimeIdentity = _identity.ProtocolIdentity;
        descriptor.RuntimeInstanceId = _client->RuntimeInstanceId();
        descriptor.ClientInstanceId = _clientInstanceId;
        descriptor.TerminalSessionId = result.Surface ? result.Surface->TerminalSessionId : std::string{};
        descriptor.PipeName = result.Surface ? til::u8u16(result.Surface->PipeName) : std::wstring{};
        descriptor.Generation = result.Surface ? result.Surface->Generation : projected->TerminalGeneration;
        descriptor.OwnedElsewhere = result.Surface && result.Surface->OwnedElsewhere;
        if (_dispatcher && !_dispatcher.HasThreadAccess())
        {
            static_cast<void>(_dispatcher.RunAsync(
                Windows::UI::Core::CoreDispatcherPriority::Normal,
                [handler = std::move(handler), descriptor = std::move(descriptor)] { handler(descriptor); }));
        }
        else
        {
            handler(descriptor);
        }
    }
    catch (...)
    {
    }
}
