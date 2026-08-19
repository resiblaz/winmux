// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "DesktopNotification.h"

#include <WtExeUtils.h>

using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::Data::Xml::Dom;

namespace winrt::TerminalApp::implementation
{
    std::atomic<uint64_t> DesktopNotification::_lastNotificationTime{ 0 };
    std::mutex DesktopNotification::_trackedActivationsMutex;
    std::unordered_map<std::wstring, DesktopNotification::TrackedActivation> DesktopNotification::_trackedActivations;
    uint64_t DesktopNotification::_nextActivationSequence{ 0 };

    void DesktopNotification::_RevokeActivation(TrackedActivation& activation) noexcept
    {
        try
        {
            if (activation.Notification)
            {
                activation.Notification.Activated(activation.Token);
                activation.Notification = nullptr;
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
        }
    }

    bool DesktopNotification::_TrackActivation(const winrt::hstring& tag,
                                               const ToastNotification& notification,
                                               const winrt::event_token token)
    {
        std::optional<TrackedActivation> retired;
        try
        {
            const std::wstring key{ tag };
            const std::scoped_lock lock{ _trackedActivationsMutex };
            TrackedActivation replacement{ notification, token, ++_nextActivationSequence };

            if (const auto existing = _trackedActivations.find(key); existing != _trackedActivations.end())
            {
                retired.emplace(std::move(existing->second));
                existing->second = std::move(replacement);
            }
            else
            {
                _trackedActivations.emplace(key, std::move(replacement));
                if (_trackedActivations.size() > MaximumTrackedActivations)
                {
                    const auto oldest = std::min_element(_trackedActivations.begin(), _trackedActivations.end(), [](const auto& lhs, const auto& rhs) {
                        return lhs.second.Sequence < rhs.second.Sequence;
                    });
                    retired.emplace(std::move(oldest->second));
                    _trackedActivations.erase(oldest);
                }
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            return false;
        }

        if (retired)
        {
            _RevokeActivation(retired.value());
        }
        return true;
    }

    bool DesktopNotification::TrackActivationForTests(const winrt::hstring& tag)
    {
        auto xml = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastText02);
        auto notification = ToastNotification{ xml };
        const auto token = notification.Activated([](const auto&, const auto&) {});
        if (_TrackActivation(tag, notification, token))
        {
            return true;
        }

        notification.Activated(token);
        return false;
    }

    size_t DesktopNotification::TrackedActivationCountForTests()
    {
        const std::scoped_lock lock{ _trackedActivationsMutex };
        return _trackedActivations.size();
    }

    size_t DesktopNotification::MaximumTrackedActivationsForTests() noexcept
    {
        return MaximumTrackedActivations;
    }

    void DesktopNotification::ClearTrackedActivationsForTests()
    {
        std::vector<TrackedActivation> activations;
        {
            const std::scoped_lock lock{ _trackedActivationsMutex };
            activations.reserve(_trackedActivations.size());
            for (auto& [tag, activation] : _trackedActivations)
            {
                activations.emplace_back(std::move(activation));
            }
            _trackedActivations.clear();
        }

        for (auto& activation : activations)
        {
            _RevokeActivation(activation);
        }
    }

    // Method Description:
    // - Rate-limits toast notifications so we don't spam the user.
    // Return Value:
    // - Returns true if a notification is allowed, false if too recent.
    bool DesktopNotification::ShouldSendNotification()
    {
        const auto now = GetTickCount64();
        auto last = _lastNotificationTime.load(std::memory_order_relaxed);

        // Subtraction wraps cleanly modulo 2^64, so the delta is correct even
        // across the (~584 million year) GetTickCount64 rollover.
        if (now - last < MinNotificationIntervalMs)
        {
            return false;
        }

        // Attempt to update; if another thread beat us, that's fine — we'll skip this one.
        return _lastNotificationTime.compare_exchange_strong(last, now, std::memory_order_relaxed);
    }

    // Method Description:
    // - Sends a toast notification with the given title and message.
    // - When the user clicks the toast, the `Activated` callback fires
    //   with the tabIndex that was passed in, so the caller can switch
    //   to the correct tab and summon the window.
    // Arguments:
    // - args: The title, message, and tab index to include in the notification.
    // - activated: A callback invoked on the background thread when the
    //              toast is clicked. The uint32_t parameter is the tab index.
    void DesktopNotification::SendNotification(const DesktopNotificationArgs& args, std::function<void()> activatedFunc)
    {
        try
        {
            if (!ShouldSendNotification())
            {
                return;
            }

            // Build the toast XML. We use a simple template with a title and body text.
            //
            // <toast launch="--from-toast">
            //   <visual>
            //     <binding template="ToastGeneric">
            //       <text>Title</text>
            //       <text>Message</text>
            //     </binding>
            //   </visual>
            // </toast>
            auto toastXml = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastText02);
            auto textNodes = toastXml.GetElementsByTagName(L"text");

            // First <text> is the title
            textNodes.Item(0).InnerText(args.Title);
            // Second <text> is the body
            textNodes.Item(1).InnerText(args.Message);

            auto toastElement = toastXml.DocumentElement();

            // When a toast is clicked, Windows launches a new instance of the app
            // with the "launch" attribute as command-line arguments. We handle
            // toast activation in-process via the Activated event below, so the
            // new instance should do nothing. "--from-toast" is recognized by
            // AppCommandlineArgs::ParseArgs as a no-op sentinel.
            toastElement.SetAttribute(L"launch", L"--from-toast");

            toastElement.SetAttribute(L"scenario", L"default");

            auto toast = ToastNotification{ toastXml };

            // Set the tag and group to enable notification replacement.
            // Repeated notifications with the same tag replace the previous one
            // rather than stacking in the notification center.
            toast.Tag(args.Tag);
            toast.Group(L"WindowsTerminal");

            // When the user activates (clicks) the toast, fire the callback.
            std::optional<winrt::event_token> activationToken;
            if (activatedFunc)
            {
                activationToken.emplace(toast.Activated([activatedFunc = std::move(activatedFunc)](const auto& /*sender*/, const auto& /*eventArgs*/) {
                    activatedFunc();
                }));
            }
            bool activationTracked = false;
            const auto revokeUntrackedActivation = wil::scope_exit([&]() noexcept {
                if (activationToken && !activationTracked)
                {
                    try
                    {
                        toast.Activated(activationToken.value());
                    }
                    catch (...)
                    {
                        LOG_CAUGHT_EXCEPTION();
                    }
                }
            });

            // For packaged apps, CreateToastNotifier() uses the package identity automatically.
            // For unpackaged apps, we must pass the explicit AUMID that was registered
            // at startup via SetCurrentProcessExplicitAppUserModelID.
            winrt::Windows::UI::Notifications::ToastNotifier notifier{ nullptr };
            if (IsPackaged())
            {
                notifier = ToastNotificationManager::CreateToastNotifier();
            }
            else
            {
                // Retrieve the AUMID that was set by WindowEmperor at startup.
                wil::unique_cotaskmem_string aumid;
                if (SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&aumid)))
                {
                    notifier = ToastNotificationManager::CreateToastNotifier(aumid.get());
                }
            }
            if (notifier)
            {
                notifier.Show(toast);
                if (activationToken)
                {
                    activationTracked = _TrackActivation(args.Tag, toast, activationToken.value());
                }
            }
        }
        catch (...)
        {
            // Toast notification is a best-effort feature. If it fails (e.g., notifications
            // are disabled, or the app is unpackaged without proper AUMID setup), we silently
            // ignore the error.
            LOG_CAUGHT_EXCEPTION();
        }
    }
}
