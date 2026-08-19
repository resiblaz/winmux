//----------------------------------------------------------------------------------------------------------------------
// <copyright file="TerminalApp.cs" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
// <summary>Helper and wrapper for generating the base test application and its UI Root node.</summary>
//----------------------------------------------------------------------------------------------------------------------
namespace WindowsTerminal.UIA.Tests.Elements
{
    using System;
    using System.Diagnostics;
    using System.IO;
    using System.Text;
    using System.Threading;

    using WindowsTerminal.UIA.Tests.Common;
    using WindowsTerminal.UIA.Tests.Common.NativeMethods;

    using OpenQA.Selenium.Remote;
    using OpenQA.Selenium.Appium;
    using OpenQA.Selenium.Appium.iOS;
    using OpenQA.Selenium.Interactions;

    using WEX.Logging.Interop;
    using WEX.TestExecution;
    using WEX.TestExecution.Markup;

    using System.Runtime.InteropServices;
    using System.Security.Principal;
    using OpenQA.Selenium;

    public class TerminalApp : IDisposable
    {
        protected const string AppDriverUrl = "http://127.0.0.1:4723";

        private IntPtr job;
        private string portableMarkerPath;
        private string portableSettingsDirectory;
        private bool portableMarkerCreated;
        private bool portableSettingsDirectoryCreated;
        private string packagedSettingsBackupPath;
        private string packagedStateBackupPath;
        private bool packagedSettingsDirectoryCreated;
        private string testArtifactsDirectory;
        private bool testArtifactsDirectoryCreated;
        private bool isPackaged;
        private bool packagedProcessLaunchStarted;
        private string launcherPath;

        public IOSDriver<IOSElement> Session { get; private set; }
        public Actions Actions { get; private set; }
        public AppiumWebElement UIRoot { get; private set; }
        public int ProcessId { get; private set; }
        public IntPtr WindowHandle { get; private set; }
        public string ExecutablePath { get; private set; }
        public string WindowName { get; private set; }
        public string TestArtifactsDirectory => this.testArtifactsDirectory;

        private bool isDisposed = false;

        private TestContext context;

        public string ContentPath { get; private set; }
        public string GetFullTestContentPath(string filename)
        {
            return Path.GetFullPath(Path.Combine(ContentPath, filename));
        }

        public TerminalApp(TestContext context, string shellToLaunch = "powershell.exe", string portableSettingsJson = null)
        {
            this.context = context;
            this.WindowName = $"WindowsTerminal.UIA.Tests.{Guid.NewGuid():N}";

            // If running locally, set WTPath to where we can find a loose
            // deployment of Windows Terminal. That means you'll need to build
            // the Terminal appx, then use
            // New-UnpackagedTerminalDistribution.ps1 to build an unpackaged
            // layout that can successfully launch. Then, point the tests at
            // that WindowsTerminal.exe like so:
            //
            //   te.exe WindowsTerminal.UIA.Tests.dll /p:WTPath=C:\the\path\to\the\unpackaged\layout\WindowsTerminal.exe
            //
            // On the build machines, the scripts lay it out at the terminal-0.0.1.0\ subfolder of the test deployment directory
            string path = Path.GetFullPath(Path.Combine(context.TestDeploymentDir, @"terminal-0.0.1.0\WindowsTerminal.exe"));
            if (context.Properties.Contains("WTPath"))
            {
                path = (string)context.Properties["WTPath"];
            }
            this.isPackaged = context.Properties.Contains("WTPackageFamilyName");
            this.launcherPath = context.Properties.Contains("WTLauncherPath")
                ? (string)context.Properties["WTLauncherPath"]
                : path;

            Log.Comment($"Windows Terminal will be launched from '{this.launcherPath}' (payload '{path}', packaged={this.isPackaged})");
            this.ExecutablePath = path;

            // Same goes for the content directory. Set WTTestContent for where the content files are
            // for running tests.
            // On the build machines, the scripts lay it out at the content\ subfolder.
            ContentPath = @"content";
            if (context.Properties.Contains("WTTestContent"))
            {
                ContentPath = (string)context.Properties["WTTestContent"];
            }
            Log.Comment($"Test Content will be loaded from '{Path.GetFullPath(ContentPath)}'");

            try
            {
                if (portableSettingsJson != null)
                {
                    if (this.isPackaged)
                    {
                        if (this.FindPackagedProcess(path, requireWindow: false).HasValue)
                        {
                            throw new InvalidOperationException(
                                $"The packaged UIA test requires no existing process from '{path}'. Close the Dev Terminal before running this test.");
                        }

                        this.PreparePackagedSettings(
                            (string)context.Properties["WTPackageFamilyName"],
                            portableSettingsJson);
                    }
                    else
                    {
                        this.PreparePortableSettings(path, portableSettingsJson);
                    }

                    this.PrepareTestArtifactsDirectory();
                }

                this.CreateProcess(path, shellToLaunch);
            }
            catch
            {
                try
                {
                    if (this.isPackaged && this.packagedProcessLaunchStarted && this.ProcessId == 0)
                    {
                        var process = this.FindPackagedProcess(path, requireWindow: false);
                        if (process.HasValue)
                        {
                            this.ProcessId = process.Value.ProcessId;
                        }
                    }

                    if (this.job != IntPtr.Zero || (this.isPackaged && this.packagedProcessLaunchStarted && this.ProcessId != 0))
                    {
                        this.ExitProcess();
                    }
                }
                catch (Exception cleanupException)
                {
                    Log.Comment($"Failed to stop a partially constructed UIA app safely: {cleanupException}");
                }
                finally
                {
                    this.CleanupTestSettings();
                }
                throw;
            }
        }

        ~TerminalApp()
        {
            this.Dispose(false);
        }

        public void Dispose()
        {
            this.Dispose(true);
            GC.SuppressFinalize(this);
        }

        public AppiumWebElement GetRoot()
        {
            return this.UIRoot;
        }

        public void OpenNewTab(string title, string commandLine)
        {
            if (title == null)
            {
                throw new ArgumentNullException(nameof(title));
            }
            if (commandLine == null)
            {
                throw new ArgumentNullException(nameof(commandLine));
            }
            if (title.Contains("\""))
            {
                throw new ArgumentException("UIA tab titles cannot contain quotes.", nameof(title));
            }

            var arguments = $"-w \"{this.WindowName}\" new-tab --title \"{title}\" --suppressApplicationTitle -- {commandLine}";
            Log.Comment($"Opening a UIA test tab with: {this.launcherPath} {arguments}");

            using (var launcher = Process.Start(new ProcessStartInfo
            {
                FileName = this.launcherPath,
                Arguments = arguments,
                WorkingDirectory = Path.GetDirectoryName(this.ExecutablePath),
                CreateNoWindow = true,
                UseShellExecute = false,
                WindowStyle = ProcessWindowStyle.Hidden,
            }))
            {
                if (launcher == null)
                {
                    throw new InvalidOperationException("Failed to start the Windows Terminal command-line handoff process.");
                }

                if (!launcher.WaitForExit(10000))
                {
                    launcher.Kill();
                    throw new TimeoutException("Windows Terminal did not hand off the new-tab command within 10 seconds.");
                }

                if (launcher.ExitCode != 0)
                {
                    throw new InvalidOperationException($"Windows Terminal new-tab handoff exited with code {launcher.ExitCode}.");
                }
            }
        }
        protected virtual void Dispose(bool disposing)
        {
            if (!this.isDisposed)
            {
                try
                {
                    // ensure we're exited when this is destroyed or disposed of explicitly
                    this.ExitProcess();
                }
                finally
                {
                    this.CleanupTestSettings();
                    this.isDisposed = true;
                }
            }
        }
        private void CreateProcess(string path, string shellToLaunch)
        {
            string WindowTitleToFind = "WindowsTerminal.UIA.Tests";
            string args = $"-w \"{this.WindowName}\" new-tab --title \"{WindowTitleToFind}\" --suppressApplicationTitle -- \"{shellToLaunch}\"";

            if (this.isPackaged)
            {
                this.CreatePackagedProcess(path, args, WindowTitleToFind);
                return;
            }

            job = WinBase.CreateJobObject(IntPtr.Zero, IntPtr.Zero);
            NativeMethods.Win32NullHelper(job, "Creating job object to hold binaries under test.");

            Log.Comment("Attempting to launch command-line application at '{0}'", path);

            string binaryToRunPath = path;
            string launchArgs = $"{binaryToRunPath} {args}";

            WinBase.STARTUPINFO si = new WinBase.STARTUPINFO();
            si.cb = Marshal.SizeOf(si);

            WinBase.PROCESS_INFORMATION pi = new WinBase.PROCESS_INFORMATION();

            NativeMethods.Win32BoolHelper(WinBase.CreateProcess(null,
                launchArgs,
                IntPtr.Zero,
                IntPtr.Zero,
                false,
                WinBase.CP_CreationFlags.CREATE_SUSPENDED,
                IntPtr.Zero,
                null,
                ref si,
                out pi),
                "Attempting to create child host window process.");

            Log.Comment($"Host window PID: {pi.dwProcessId}");
            this.ProcessId = pi.dwProcessId;
            NativeMethods.Win32BoolHelper(WinBase.AssignProcessToJobObject(job, pi.hProcess), "Assigning new host window (suspended) to job object.");
            NativeMethods.Win32BoolHelper(-1 != WinBase.ResumeThread(pi.hThread), "Resume host window process now that it is attached and its launch of the child application will be caught in the job object.");

            this.CreateAutomationSession(pi.dwProcessId, WindowTitleToFind);
        }

        private void CreatePackagedProcess(string executablePath, string arguments, string fallbackWindowTitle)
        {
            if (this.FindPackagedProcess(executablePath, requireWindow: false).HasValue)
            {
                throw new InvalidOperationException(
                    $"The packaged UIA test requires no existing process from '{executablePath}'. Close the Dev Terminal before running this test.");
            }

            this.packagedProcessLaunchStarted = true;
            Log.Comment($"Activating packaged Windows Terminal with: {this.launcherPath} {arguments}");
            using (var launcher = Process.Start(new ProcessStartInfo
            {
                FileName = this.launcherPath,
                Arguments = arguments,
                WorkingDirectory = Path.GetDirectoryName(executablePath),
                CreateNoWindow = true,
                UseShellExecute = false,
                WindowStyle = ProcessWindowStyle.Hidden,
            }))
            {
                if (launcher == null)
                {
                    throw new InvalidOperationException("Failed to activate the packaged Windows Terminal.");
                }

                if (!launcher.WaitForExit(10000))
                {
                    launcher.Kill();
                    throw new TimeoutException("The packaged Windows Terminal launcher did not exit within 10 seconds.");
                }

                if (launcher.ExitCode != 0)
                {
                    throw new InvalidOperationException($"The packaged Windows Terminal launcher exited with code {launcher.ExitCode}.");
                }
            }

            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
            (int ProcessId, IntPtr WindowHandle)? process = null;
            while (DateTime.UtcNow < deadline && !process.HasValue)
            {
                process = this.FindPackagedProcess(executablePath, requireWindow: true);
                if (!process.HasValue)
                {
                    Thread.Sleep(100);
                }
            }

            if (!process.HasValue)
            {
                throw new TimeoutException($"The packaged Windows Terminal did not create a hosting window from '{executablePath}'.");
            }

            this.ProcessId = process.Value.ProcessId;
            this.WindowHandle = process.Value.WindowHandle;
            Log.Comment($"Packaged host window PID: {this.ProcessId}");
            this.CreateAutomationSession(this.ProcessId, fallbackWindowTitle);
        }

        private (int ProcessId, IntPtr WindowHandle)? FindPackagedProcess(string executablePath, bool requireWindow)
        {
            var processName = Path.GetFileNameWithoutExtension(executablePath);
            foreach (var process in Process.GetProcessesByName(processName))
            {
                using (process)
                {
                    try
                    {
                        var runningPath = GetProcessExecutablePath(process);
                        if (!string.Equals(runningPath, executablePath, StringComparison.OrdinalIgnoreCase))
                        {
                            continue;
                        }

                        var windowHandle = process.MainWindowHandle;
                        if (requireWindow && windowHandle == IntPtr.Zero)
                        {
                            continue;
                        }

                        return (process.Id, windowHandle);
                    }
                    catch (InvalidOperationException)
                    {
                    }
                    catch (System.ComponentModel.Win32Exception)
                    {
                    }
                    catch (NullReferenceException)
                    {
                        // System.Diagnostics may expose a partially populated
                        // ProcessModule while a packaged process is starting.
                    }
                }
            }

            return null;
        }

        private static string GetProcessExecutablePath(Process process)
        {
            for (var attempt = 0; attempt < 5; ++attempt)
            {
                try
                {
                    process.Refresh();
                    var mainModule = process.MainModule;
                    var path = mainModule == null ? null : mainModule.FileName;
                    if (!string.IsNullOrEmpty(path))
                    {
                        return path;
                    }
                }
                catch (InvalidOperationException)
                {
                }
                catch (System.ComponentModel.Win32Exception)
                {
                }
                catch (NullReferenceException)
                {
                }

                Thread.Sleep(50);
            }

            return null;
        }

        private void CreateAutomationSession(int processId, string fallbackWindowTitle)
        {
            Globals.WaitForTimeout();

            DesiredCapabilities appCapabilities = new DesiredCapabilities();
            appCapabilities.SetCapability("app", @"Root");
            Session = new IOSDriver<IOSElement>(new Uri(AppDriverUrl), appCapabilities);

            Verify.IsNotNull(Session);
            Actions = new Actions(Session);
            Verify.IsNotNull(Actions);

            // Apply the timeout before locating the window. Startup time varies
            // significantly between packaged and unpackaged Debug builds.
            Session.Manage().Timeouts().ImplicitWait = TimeSpan.FromSeconds(15);

            Globals.WaitForLongTimeout();

            // The workspace-sidebar layout deliberately uses the application
            // display name for the hosting window title. Locate it by the PID
            // and stable Win32 class instead of localized or layout-dependent
            // text, then retain the legacy title lookup as a compatibility
            // fallback for older layouts.
            try
            {
                UIRoot = Session.FindElementByXPath($"//Window[@ProcessId='{processId}' and @ClassName='CASCADIA_HOSTING_WINDOW_CLASS']");
            }
            catch (NoSuchElementException)
            {
                UIRoot = Session.FindElementByName(fallbackWindowTitle);
            }

            using (var terminalProcess = Process.GetProcessById(processId))
            {
                terminalProcess.Refresh();
                this.WindowHandle = terminalProcess.MainWindowHandle;
            }
            NativeMethods.Win32NullHelper(this.WindowHandle, "Locating the Terminal hosting window handle.");
        }

        private void PreparePackagedSettings(string packageFamilyName, string settingsJson)
        {
            if (string.IsNullOrWhiteSpace(packageFamilyName) ||
                !string.Equals(Path.GetFileName(packageFamilyName), packageFamilyName, StringComparison.Ordinal))
            {
                throw new ArgumentException("A package family name without path components is required.", nameof(packageFamilyName));
            }

            var packagesDirectory = Path.GetFullPath(Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Packages"));
            var settingsDirectory = Path.GetFullPath(Path.Combine(packagesDirectory, packageFamilyName, "LocalState"));
            if (!settingsDirectory.StartsWith(packagesDirectory + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException($"The packaged UIA settings path escaped the current user's package data: '{settingsDirectory}'.");
            }

            this.portableSettingsDirectory = settingsDirectory;
            this.packagedSettingsDirectoryCreated = !Directory.Exists(settingsDirectory);
            Directory.CreateDirectory(settingsDirectory);

            var settingsPath = Path.Combine(settingsDirectory, "settings.json");
            var statePath = Path.Combine(settingsDirectory, "state.json");
            this.packagedSettingsBackupPath = settingsPath + ".WindowsTerminal.UIA.Tests.backup";
            this.packagedStateBackupPath = statePath + ".WindowsTerminal.UIA.Tests.backup";

            this.RecoverInterruptedPackagedFile(settingsPath, this.packagedSettingsBackupPath);
            this.RecoverInterruptedPackagedFile(statePath, this.packagedStateBackupPath);
            this.BackupPackagedFile(settingsPath, this.packagedSettingsBackupPath);
            this.BackupPackagedFile(statePath, this.packagedStateBackupPath);

            File.WriteAllText(settingsPath, settingsJson, new UTF8Encoding(false));
        }

        private void RecoverInterruptedPackagedFile(string path, string backupPath)
        {
            if (!File.Exists(backupPath))
            {
                return;
            }

            Log.Comment($"Recovering packaged settings left by an interrupted UIA run: '{path}'.");
            if (File.Exists(path))
            {
                File.Delete(path);
            }
            File.Move(backupPath, path);
        }

        private void BackupPackagedFile(string path, string backupPath)
        {
            if (File.Exists(path))
            {
                File.Move(path, backupPath);
            }
        }

        private void PreparePortableSettings(string executablePath, string settingsJson)
        {
            var executableDirectory = Path.GetDirectoryName(executablePath);
            var markerPath = Path.Combine(executableDirectory, ".portable");
            var settingsDirectory = Path.Combine(executableDirectory, "settings");

            if (File.Exists(markerPath) || Directory.Exists(settingsDirectory))
            {
                throw new InvalidOperationException(
                    $"The UIA test requires an unpackaged layout without existing portable state: '{executableDirectory}'.");
            }

            this.portableMarkerPath = markerPath;
            this.portableSettingsDirectory = settingsDirectory;

            File.WriteAllText(markerPath, string.Empty, new UTF8Encoding(false));
            this.portableMarkerCreated = true;

            Directory.CreateDirectory(settingsDirectory);
            this.portableSettingsDirectoryCreated = true;
            File.WriteAllText(Path.Combine(settingsDirectory, "settings.json"), settingsJson, new UTF8Encoding(false));
        }

        private void PrepareTestArtifactsDirectory()
        {
            if (string.IsNullOrEmpty(this.portableSettingsDirectory))
            {
                throw new InvalidOperationException("UIA test artifacts require an isolated settings directory.");
            }

            var settingsDirectory = Path.GetFullPath(this.portableSettingsDirectory);
            var artifactsDirectory = Path.GetFullPath(Path.Combine(
                settingsDirectory,
                $"WindowsTerminal.UIA.Tests.{Guid.NewGuid():N}"));
            if (!artifactsDirectory.StartsWith(settingsDirectory + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException($"The UIA test artifacts path escaped the isolated settings directory: '{artifactsDirectory}'.");
            }

            Directory.CreateDirectory(artifactsDirectory);
            this.testArtifactsDirectory = artifactsDirectory;
            this.testArtifactsDirectoryCreated = true;
        }

        private void CleanupTestSettings()
        {
            try
            {
                if (this.testArtifactsDirectoryCreated && Directory.Exists(this.testArtifactsDirectory))
                {
                    Directory.Delete(this.testArtifactsDirectory, true);
                }
            }
            catch (Exception exception)
            {
                Log.Comment($"Failed to clean isolated UIA artifacts: {exception}");
            }

            try
            {
                if (this.isPackaged)
                {
                    this.CleanupPackagedSettings();
                }
                else
                {
                    if (this.portableSettingsDirectoryCreated && Directory.Exists(this.portableSettingsDirectory))
                    {
                        Directory.Delete(this.portableSettingsDirectory, true);
                    }

                    if (this.portableMarkerCreated && File.Exists(this.portableMarkerPath))
                    {
                        File.Delete(this.portableMarkerPath);
                    }
                }
            }
            catch (Exception exception)
            {
                Log.Comment($"Failed to clean isolated UIA settings: {exception}");
            }
            finally
            {
                this.portableSettingsDirectoryCreated = false;
                this.portableMarkerCreated = false;
                this.packagedSettingsDirectoryCreated = false;
                this.packagedSettingsBackupPath = null;
                this.packagedStateBackupPath = null;
                this.testArtifactsDirectoryCreated = false;
                this.testArtifactsDirectory = null;
                this.portableSettingsDirectory = null;
                this.portableMarkerPath = null;
            }
        }

        private void CleanupPackagedSettings()
        {
            if (string.IsNullOrEmpty(this.portableSettingsDirectory))
            {
                return;
            }

            var settingsPath = Path.Combine(this.portableSettingsDirectory, "settings.json");
            var statePath = Path.Combine(this.portableSettingsDirectory, "state.json");
            this.RestorePackagedFile(settingsPath, this.packagedSettingsBackupPath);
            this.RestorePackagedFile(statePath, this.packagedStateBackupPath);

            if (this.packagedSettingsDirectoryCreated &&
                Directory.Exists(this.portableSettingsDirectory) &&
                Directory.GetFileSystemEntries(this.portableSettingsDirectory).Length == 0)
            {
                Directory.Delete(this.portableSettingsDirectory, false);
            }
        }

        private void RestorePackagedFile(string path, string backupPath)
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }

            if (!string.IsNullOrEmpty(backupPath) && File.Exists(backupPath))
            {
                File.Move(backupPath, path);
            }
        }

        private bool IsRunningAsAdmin()
        {
            return new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator);
        }

        private void ExitProcess()
        {
            Globals.SweepAllModules(this.context);

            // Release attachment to the child process console.
            WinCon.FreeConsole();

            this.UIRoot = null;

            if (this.job != IntPtr.Zero)
            {
                WinBase.TerminateJobObject(this.job, 0);
                this.job = IntPtr.Zero;
            }
            else if (this.isPackaged && this.ProcessId != 0)
            {
                try
                {
                    using (var process = Process.GetProcessById(this.ProcessId))
                    {
                        var runningPath = GetProcessExecutablePath(process);
                        if (!string.Equals(runningPath, this.ExecutablePath, StringComparison.OrdinalIgnoreCase))
                        {
                            throw new InvalidOperationException(
                                $"Refusing to stop unexpected process {this.ProcessId} from '{runningPath ?? "<unknown>"}'.");
                        }

                        process.Kill();
                        process.WaitForExit(5000);
                    }
                }
                catch (ArgumentException)
                {
                    // The packaged process already exited.
                }
                catch (InvalidOperationException exception)
                {
                    Log.Comment($"Failed to stop the packaged UIA process safely: {exception}");
                }
                catch (System.ComponentModel.Win32Exception exception)
                {
                    Log.Comment($"Failed to stop the packaged UIA process safely: {exception}");
                }
            }
        }
    }
}
