//----------------------------------------------------------------------------------------------------------------------
// <copyright file="Init.cs" company="Microsoft">
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// </copyright>
//----------------------------------------------------------------------------------------------------------------------

using System;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Threading;
using WEX.Logging.Interop;
using WEX.TestExecution.Markup;

namespace WindowsTerminal.UIA.Tests
{
    [TestClass]
    public class Init
    {
        private static Process appDriver;

        [AssemblyInitialize]
        public static void SetupAll(TestContext context)
        {
            if (IsDriverReady())
            {
                Log.Comment("WinAppDriver is already listening on 127.0.0.1:4723; reusing it.");
                return;
            }

            Log.Comment("Searching for WinAppDriver in the same directory where this test was launched from...");
            string winAppDriver = Path.Combine(context.TestDeploymentDir, "WinAppDriver", "WinAppDriver.exe");

            if (!File.Exists(winAppDriver))
            {
                winAppDriver = Path.Combine(context.TestDeploymentDir, "WinAppDriver.exe");
            }

            Log.Comment($"Attempting to launch WinAppDriver at: {winAppDriver}");
            Log.Comment($"Working directory: {Environment.CurrentDirectory}");

            if (!File.Exists(winAppDriver))
            {
                throw new FileNotFoundException("WinAppDriver was not deployed with the UIA tests.", winAppDriver);
            }

            appDriver = Process.Start(new ProcessStartInfo
            {
                FileName = winAppDriver,
                CreateNoWindow = true,
                UseShellExecute = false,
                WindowStyle = ProcessWindowStyle.Hidden,
            });

            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
            while (DateTime.UtcNow < deadline)
            {
                if (appDriver == null || appDriver.HasExited)
                {
                    throw new InvalidOperationException("WinAppDriver exited before opening port 4723.");
                }

                if (IsDriverReady())
                {
                    Log.Comment("WinAppDriver is ready on 127.0.0.1:4723.");
                    return;
                }

                Thread.Sleep(100);
            }

            throw new TimeoutException("WinAppDriver did not open port 4723 within 10 seconds.");
        }

        [AssemblyCleanup]
        public static void CleanupAll()
        {
            try
            {
                if (appDriver != null && !appDriver.HasExited)
                {
                    appDriver.Kill();
                }
            }
            catch
            {
                // Test cleanup is best effort. A failed test should retain its original result.
            }
            finally
            {
                appDriver?.Dispose();
                appDriver = null;
            }
        }

        private static bool IsDriverReady()
        {
            try
            {
                using (var client = new TcpClient())
                {
                    var result = client.BeginConnect("127.0.0.1", 4723, null, null);
                    using (var waitHandle = result.AsyncWaitHandle)
                    {
                        if (!waitHandle.WaitOne(TimeSpan.FromMilliseconds(100)))
                        {
                            return false;
                        }
                    }

                    client.EndConnect(result);
                    return true;
                }
            }
            catch (SocketException)
            {
                return false;
            }
        }
    }
}
