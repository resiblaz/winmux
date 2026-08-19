// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "../TerminalApp/DesktopNotification.h"

using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace TerminalAppUnitTests
{
    class DesktopNotificationTests
    {
        TEST_CLASS(DesktopNotificationTests);

        TEST_METHOD(TracksOnlyTheLatestBoundedToastActivations);
    };

    void DesktopNotificationTests::TracksOnlyTheLatestBoundedToastActivations()
    {
        using winrt::TerminalApp::implementation::DesktopNotification;

        DesktopNotification::ClearTrackedActivationsForTests();
        const auto clear = wil::scope_exit([]() {
            DesktopNotification::ClearTrackedActivationsForTests();
        });

        for (size_t replacement = 0; replacement < 32; ++replacement)
        {
            VERIFY_IS_TRUE(DesktopNotification::TrackActivationForTests(L"same-workspace"));
        }
        VERIFY_ARE_EQUAL(size_t{ 1 }, DesktopNotification::TrackedActivationCountForTests());

        const auto maximum = DesktopNotification::MaximumTrackedActivationsForTests();
        for (size_t index = 0; index < maximum + 16; ++index)
        {
            VERIFY_IS_TRUE(DesktopNotification::TrackActivationForTests(winrt::hstring{ fmt::format(FMT_COMPILE(L"workspace-{}"), index) }));
        }
        VERIFY_ARE_EQUAL(maximum, DesktopNotification::TrackedActivationCountForTests());

        DesktopNotification::ClearTrackedActivationsForTests();
        VERIFY_ARE_EQUAL(size_t{ 0 }, DesktopNotification::TrackedActivationCountForTests());
    }
}
