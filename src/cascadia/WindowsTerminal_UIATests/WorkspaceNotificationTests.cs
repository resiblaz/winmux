//----------------------------------------------------------------------------------------------------------------------
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//----------------------------------------------------------------------------------------------------------------------

namespace WindowsTerminal.UIA.Tests
{
    using System;
    using System.IO;
    using System.Text;
    using System.Threading;

    using OpenQA.Selenium;
    using OpenQA.Selenium.Appium;
    using WEX.Logging.Interop;
    using WEX.TestExecution;
    using WEX.TestExecution.Markup;

    using WindowsTerminal.UIA.Tests.Common.NativeMethods;
    using WindowsTerminal.UIA.Tests.Elements;

    [TestClass]
    public class WorkspaceNotificationTests
    {
        private const string PortableSettings = @"
{
    ""tabLayout"": ""workspaceSidebar"",
    ""defaultProfile"": ""{4f8b421e-5de8-4a4f-aac6-2b0ea6733153}"",
    ""profiles"": {
        ""defaults"": {
            ""compatibility.allowOSC777"": true
        },
        ""list"": [
            {
                ""guid"": ""{4f8b421e-5de8-4a4f-aac6-2b0ea6733153}"",
                ""name"": ""Workspace UIA PowerShell"",
                ""commandline"": ""powershell.exe"",
                ""compatibility.allowOSC777"": true
            }
        ]
    }
}";

        private const string FirstNotificationTitle = "Workspace UIA notification";
        private const string FirstNotificationBody = "sidebar source jump";
        private const string ShortcutNotificationTitle = "Workspace shortcut notification";
        private const string ShortcutNotificationBody = "shortcut source jump";
        private const string FirstSourceWorkspaceTitle = "Workspace UIA source";
        private const string ShortcutSourceWorkspaceTitle = "Workspace UIA shortcut source";
        private static readonly string PowerShellExecutablePath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.Windows),
            "System32",
            "WindowsPowerShell",
            "v1.0",
            "powershell.exe");

        public TestContext TestContext { get; set; }

        [TestMethod]
        public void WorkspaceSidebarStartsShell()
        {
            const string sidebarSettings = @"
{
    ""tabLayout"": ""workspaceSidebar"",
    ""profiles"": { ""defaults"": {}, ""list"": [] }
}";

            using (var app = new TerminalApp(
                TestContext,
                shellToLaunch: PowerShellExecutablePath,
                portableSettingsJson: sidebarSettings))
            {
                var markerPath = Path.Combine(app.TestArtifactsDirectory, "packaged-shell.marker");
                NativeKeyboard.FocusTerminalSurface(app.WindowHandle);
                NativeKeyboard.SendText($"[IO.File]::WriteAllText('{markerPath.Replace("'", "''")}', 'ok')");
                NativeKeyboard.SendKey(NativeKeyboard.VirtualKeyReturn);
                WaitFor(() => File.Exists(markerPath), "The workspace Sidebar did not execute shell input.");
            }
        }

        [TestMethod]
        public void NotificationFlowsThroughSidebarToastAndShortcut()
        {
            using (var app = new TerminalApp(
                TestContext,
                shellToLaunch: PowerShellExecutablePath,
                portableSettingsJson: PortableSettings))
            {
                Verify.AreEqual(1, GetWorkspaceItems(app).Count, "The test must start with one workspace.");
                Verify.IsFalse(HasUnreadNotification(app), "The test must start without an unread Sidebar badge.");

                var firstNotification = CreateNotificationCommand(app.TestArtifactsDirectory, "first", FirstNotificationTitle, FirstNotificationBody);
                app.OpenNewTab(
                    FirstSourceWorkspaceTitle,
                    firstNotification.CommandLine);
                WaitFor(() => GetWorkspaceItems(app).Count == 2, "A second workspace was not created.");
                WaitFor(
                    () => File.Exists(firstNotification.ReadyPath),
                    "The first notification command did not become ready.",
                    TimeSpan.FromSeconds(30));

                // The command-line handoff selects the new Workspace. The
                // notification process waits on a release file so the test can
                // deterministically switch away before OSC 777 is emitted.
                SelectWorkspace(app, 0);
                File.WriteAllText(firstNotification.ReleasePath, string.Empty, new UTF8Encoding(false));
                WaitForNotificationEmission(firstNotification);

                WaitFor(
                    () => HasUnreadNotification(app),
                    "The Sidebar did not expose the background OSC 777 as unread.");

                WaitFor(
                    () => app.Session.FindElementsByName(FirstNotificationTitle).Count > 0,
                    "The desktop Toast did not expose the OSC 777 title through UI Automation.",
                    TimeSpan.FromSeconds(8));

                GetNotificationCenter(app).Click();
                WaitFor(
                    () => app.GetRoot().FindElementsByAccessibilityId("NotificationHistoryList").Count == 1,
                    "The notification center did not open.");

                AppiumWebElement historyItem = null;
                WaitFor(
                    () => (historyItem = FindNotificationHistoryItem(app, FirstNotificationBody)) != null,
                    "The notification center did not contain the OSC 777 body.");
                historyItem.Click();

                WaitFor(
                    () => IsWorkspaceSelected(app, 1),
                    "Clicking notification history did not focus the source workspace.");
                WaitFor(
                    () => !HasUnreadNotification(app),
                    "Source navigation did not mark the notification as read.");

                // Repeat the background notification flow in a new source
                // Workspace and use Ctrl+Shift+U instead of notification history.
                var shortcutNotification = CreateNotificationCommand(app.TestArtifactsDirectory, "shortcut", ShortcutNotificationTitle, ShortcutNotificationBody);
                app.OpenNewTab(
                    ShortcutSourceWorkspaceTitle,
                    shortcutNotification.CommandLine);
                WaitFor(() => GetWorkspaceItems(app).Count == 3, "A third workspace was not created.");
                WaitFor(
                    () => File.Exists(shortcutNotification.ReadyPath),
                    "The shortcut notification command did not become ready.",
                    TimeSpan.FromSeconds(30));
                SelectWorkspace(app, 0);
                File.WriteAllText(shortcutNotification.ReleasePath, string.Empty, new UTF8Encoding(false));
                WaitForNotificationEmission(shortcutNotification);
                WaitFor(
                    () => HasUnreadNotification(app),
                    "The second background OSC 777 was not marked unread.");

                NativeKeyboard.FocusTerminalSurface(app.WindowHandle);
                NativeKeyboard.SendShortcut(
                    NativeKeyboard.VirtualKeyControl,
                    NativeKeyboard.VirtualKeyShift,
                    NativeKeyboard.VirtualKeyU);
                WaitFor(
                    () => IsWorkspaceSelected(app, 2),
                    "Ctrl+Shift+U did not focus the latest unread workspace.");
                WaitFor(
                    () => !HasUnreadNotification(app),
                    "Ctrl+Shift+U did not mark the source notification as read.");
            }
        }

        private static NotificationCommand CreateNotificationCommand(string directory, string markerName, string title, string body)
        {
            var readyPath = Path.Combine(directory, $"{markerName}-notification.ready");
            var releasePath = Path.Combine(directory, $"{markerName}-notification.release");
            var emittedPath = Path.Combine(directory, $"{markerName}-notification.emitted");
            var errorPath = Path.Combine(directory, $"{markerName}-notification.error.txt");

            var script = $@"
$ErrorActionPreference = 'Stop'
$readyPath = {PowerShellLiteral(readyPath)}
$releasePath = {PowerShellLiteral(releasePath)}
$emittedPath = {PowerShellLiteral(emittedPath)}
$errorPath = {PowerShellLiteral(errorPath)}
try {{
    [System.IO.File]::WriteAllText($readyPath, 'ready')
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while (-not [System.IO.File]::Exists($releasePath)) {{
        if ([DateTime]::UtcNow -ge $deadline) {{
            throw 'Timed out waiting for the UIA background-workspace release signal.'
        }}
        Start-Sleep -Milliseconds 50
    }}
    $sequence = [char]27 + ']777;notify;' + {PowerShellLiteral(title)} + ';' + {PowerShellLiteral(body)} + [char]7
    [Console]::Write($sequence)
    [Console]::Out.Flush()
    [System.IO.File]::WriteAllText($emittedPath, 'emitted')
    Start-Sleep -Seconds 30
}}
catch {{
    [System.IO.File]::WriteAllText($errorPath, ($_ | Out-String))
    throw
}}
";

            var encodedScript = Convert.ToBase64String(Encoding.Unicode.GetBytes(script));
            return new NotificationCommand
            {
                CommandLine = $"{PowerShellExecutablePath} -NoLogo -NoProfile -EncodedCommand {encodedScript}",
                ReadyPath = readyPath,
                ReleasePath = releasePath,
                EmittedPath = emittedPath,
                ErrorPath = errorPath,
            };
        }

        private static string PowerShellLiteral(string value)
        {
            return $"'{value.Replace("'", "''")}'";
        }

        private static void WaitForNotificationEmission(NotificationCommand command)
        {
            WaitFor(
                () => File.Exists(command.EmittedPath) || File.Exists(command.ErrorPath),
                "The background notification command did not finish emitting OSC 777.");

            if (File.Exists(command.ErrorPath))
            {
                Verify.Fail($"The background notification command failed: {File.ReadAllText(command.ErrorPath)}");
            }

            Verify.IsTrue(File.Exists(command.EmittedPath), "The background notification command did not emit OSC 777.");
        }

        private static void SelectWorkspace(TerminalApp app, int index)
        {
            var items = GetWorkspaceItems(app);
            Verify.IsTrue(items.Count > index, $"Workspace index {index} does not exist.");
            WaitFor(
                () =>
                {
                    var currentItems = GetWorkspaceItems(app);
                    if (currentItems.Count <= index)
                    {
                        return false;
                    }
                    if (currentItems[index].Selected)
                    {
                        return true;
                    }

                    currentItems[index].Click();
                    return IsWorkspaceSelected(app, index);
                },
                $"Workspace index {index} was not selected.");
        }

        private static AppiumWebElement GetNotificationCenter(TerminalApp app)
        {
            return app.GetRoot().FindElementByAccessibilityId("NotificationCenterButton");
        }

        private static bool HasUnreadNotification(TerminalApp app)
        {
            var badges = app.GetRoot().FindElementsByAccessibilityId("NotificationCenterBadgeText");
            if (badges.Count == 0)
            {
                return false;
            }

            var badge = badges[0];
            return badge.GetAttribute("Name") == "1" || badge.Text == "1";
        }

        private static bool IsWorkspaceSelected(TerminalApp app, int index)
        {
            var items = GetWorkspaceItems(app);
            return items.Count > index && items[index].Selected;
        }

        private static System.Collections.ObjectModel.ReadOnlyCollection<AppiumWebElement> GetWorkspaceItems(TerminalApp app)
        {
            return app.GetRoot().FindElementsByXPath(".//List[@AutomationId='WorkspaceList']/ListItem");
        }

        private static AppiumWebElement FindNotificationHistoryItem(TerminalApp app, string body)
        {
            var elements = app.GetRoot().FindElementsByName(body);
            return elements.Count > 0 ? elements[0] : null;
        }

        private static void WaitFor(Func<bool> condition, string failureMessage, TimeSpan? timeout = null)
        {
            var deadline = DateTime.UtcNow + (timeout ?? TimeSpan.FromSeconds(10));
            Exception lastException = null;

            while (DateTime.UtcNow < deadline)
            {
                try
                {
                    if (condition())
                    {
                        return;
                    }
                }
                catch (WebDriverException exception)
                {
                    lastException = exception;
                }

                Thread.Sleep(100);
            }

            if (lastException != null)
            {
                Log.Comment($"Last UI Automation exception while waiting: {lastException}");
            }

            Verify.IsTrue(false, failureMessage);
        }

        private sealed class NotificationCommand
        {
            public string CommandLine { get; set; }
            public string ReadyPath { get; set; }
            public string ReleasePath { get; set; }
            public string EmittedPath { get; set; }
            public string ErrorPath { get; set; }
        }
    }
}
