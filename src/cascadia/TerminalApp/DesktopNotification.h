/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- DesktopNotification.h

Module Description:
- Helper for sending Windows desktop toast notifications. Used to surface
  terminal activity events to the user via the Windows notification center.
--*/

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include <winrt/Windows.UI.Notifications.h>

namespace winrt::TerminalApp::implementation
{
    struct DesktopNotificationArgs
    {
        winrt::hstring Title;
        winrt::hstring Message;
        winrt::hstring Tag;
    };

    class DesktopNotification
    {
    public:
        static bool ShouldSendNotification();
        static void SendNotification(const DesktopNotificationArgs& args, std::function<void()> activatedFunc);

        static bool TrackActivationForTests(const winrt::hstring& tag);
        static size_t TrackedActivationCountForTests();
        static size_t MaximumTrackedActivationsForTests() noexcept;
        static void ClearTrackedActivationsForTests();

    private:
        struct TrackedActivation
        {
            winrt::Windows::UI::Notifications::ToastNotification Notification{ nullptr };
            winrt::event_token Token{};
            uint64_t Sequence{};
        };

        static bool _TrackActivation(const winrt::hstring& tag,
                                     const winrt::Windows::UI::Notifications::ToastNotification& notification,
                                     winrt::event_token token);
        static void _RevokeActivation(TrackedActivation& activation) noexcept;

        static std::atomic<uint64_t> _lastNotificationTime;
        static std::mutex _trackedActivationsMutex;
        static std::unordered_map<std::wstring, TrackedActivation> _trackedActivations;
        static uint64_t _nextActivationSequence;

        // Minimum interval between notifications, in milliseconds (GetTickCount64 units).
        static constexpr uint64_t MinNotificationIntervalMs = 5'000;
        static constexpr size_t MaximumTrackedActivations = 128;
    };
}
