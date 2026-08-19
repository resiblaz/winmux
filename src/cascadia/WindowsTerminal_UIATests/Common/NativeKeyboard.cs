//----------------------------------------------------------------------------------------------------------------------
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//----------------------------------------------------------------------------------------------------------------------

namespace WindowsTerminal.UIA.Tests.Common.NativeMethods
{
    using System;
    using System.ComponentModel;
    using System.Collections.Generic;
    using System.Runtime.InteropServices;
    using System.Threading;

    public static class NativeKeyboard
    {
        public const ushort VirtualKeyControl = 0x11;
        public const ushort VirtualKeyShift = 0x10;
        public const ushort VirtualKeyReturn = 0x0D;
        public const ushort VirtualKeyU = 0x55;

        private const uint InputKeyboard = 1;
        private const uint KeyEventKeyUp = 0x0002;
        private const uint KeyEventUnicode = 0x0004;
        private const uint MouseEventLeftDown = 0x0002;
        private const uint MouseEventLeftUp = 0x0004;
        private const int ShowWindowRestore = 9;

        public static void FocusTerminalSurface(IntPtr windowHandle)
        {
            if (windowHandle == IntPtr.Zero)
            {
                throw new ArgumentException("A valid Terminal window handle is required.", nameof(windowHandle));
            }

            if (!GetWindowRect(windowHandle, out var windowRect))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to read the Terminal window bounds.");
            }

            if (!GetCursorPos(out var originalCursor))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to read the current cursor position.");
            }

            try
            {
                // The workspace Sidebar occupies the left side of the window. A
                // point at 75% width and 60% height is inside the active terminal
                // surface for expanded, compact, and disabled Sidebar layouts.
                var x = windowRect.Left + ((windowRect.Right - windowRect.Left) * 3 / 4);
                var y = windowRect.Top + ((windowRect.Bottom - windowRect.Top) * 3 / 5);
                if (!SetCursorPos(x, y))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to position the mouse over the Terminal surface.");
                }

                if (IsIconic(windowHandle))
                {
                    ShowWindowAsync(windowHandle, ShowWindowRestore);
                }
                for (var attempt = 0; attempt < 5; ++attempt)
                {
                    BringWindowToTop(windowHandle);
                    SetForegroundWindow(windowHandle);
                    mouse_event(MouseEventLeftDown, 0, 0, 0, UIntPtr.Zero);
                    mouse_event(MouseEventLeftUp, 0, 0, 0, UIntPtr.Zero);
                    Thread.Sleep(100);

                    if (GetForegroundWindow() == windowHandle)
                    {
                        return;
                    }
                }

                var foregroundWindow = GetForegroundWindow();
                throw new InvalidOperationException(
                    $"The Terminal window did not become foreground. Expected 0x{windowHandle.ToInt64():X}, got 0x{foregroundWindow.ToInt64():X}.");
            }
            finally
            {
                SetCursorPos(originalCursor.X, originalCursor.Y);
            }
        }

        public static void SendText(string text)
        {
            if (text == null)
            {
                throw new ArgumentNullException(nameof(text));
            }

            var inputs = new List<Input>(text.Length * 2);
            foreach (var character in text)
            {
                inputs.Add(CreateUnicodeInput(character, false));
                inputs.Add(CreateUnicodeInput(character, true));
            }

            Send(inputs);
        }

        public static void SendKey(ushort virtualKey)
        {
            Send(new[]
            {
                CreateVirtualKeyInput(virtualKey, false),
                CreateVirtualKeyInput(virtualKey, true),
            });
        }

        public static void SendShortcut(params ushort[] virtualKeys)
        {
            if (virtualKeys == null || virtualKeys.Length == 0)
            {
                throw new ArgumentException("At least one virtual key is required.", nameof(virtualKeys));
            }

            var inputs = new List<Input>(virtualKeys.Length * 2);
            foreach (var virtualKey in virtualKeys)
            {
                inputs.Add(CreateVirtualKeyInput(virtualKey, false));
            }

            for (var index = virtualKeys.Length - 1; index >= 0; --index)
            {
                inputs.Add(CreateVirtualKeyInput(virtualKeys[index], true));
            }

            Send(inputs);
        }

        private static Input CreateUnicodeInput(char character, bool keyUp)
        {
            return new Input
            {
                Type = InputKeyboard,
                Data = new InputUnion
                {
                    Keyboard = new KeyboardInput
                    {
                        Scan = character,
                        Flags = KeyEventUnicode | (keyUp ? KeyEventKeyUp : 0),
                    },
                },
            };
        }

        private static Input CreateVirtualKeyInput(ushort virtualKey, bool keyUp)
        {
            return new Input
            {
                Type = InputKeyboard,
                Data = new InputUnion
                {
                    Keyboard = new KeyboardInput
                    {
                        VirtualKey = virtualKey,
                        Flags = keyUp ? KeyEventKeyUp : 0,
                    },
                },
            };
        }

        private static void Send(IList<Input> inputs)
        {
            if (inputs.Count == 0)
            {
                return;
            }

            var buffer = new Input[inputs.Count];
            inputs.CopyTo(buffer, 0);
            var sent = SendInput((uint)buffer.Length, buffer, Marshal.SizeOf(typeof(Input)));
            if (sent != buffer.Length)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), $"SendInput sent {sent} of {buffer.Length} events.");
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Point
        {
            public int X;
            public int Y;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Rect
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct Input
        {
            public uint Type;
            public InputUnion Data;
        }

        [StructLayout(LayoutKind.Explicit)]
        private struct InputUnion
        {
            [FieldOffset(0)]
            public MouseInput Mouse;

            [FieldOffset(0)]
            public KeyboardInput Keyboard;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct MouseInput
        {
            public int X;
            public int Y;
            public uint MouseData;
            public uint Flags;
            public uint Time;
            public UIntPtr ExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct KeyboardInput
        {
            public ushort VirtualKey;
            public ushort Scan;
            public uint Flags;
            public uint Time;
            public UIntPtr ExtraInfo;
        }

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool GetWindowRect(IntPtr windowHandle, out Rect rect);

        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr windowHandle);

        [DllImport("user32.dll")]
        private static extern bool BringWindowToTop(IntPtr windowHandle);

        [DllImport("user32.dll")]
        private static extern bool ShowWindowAsync(IntPtr windowHandle, int command);

        [DllImport("user32.dll")]
        private static extern bool IsIconic(IntPtr windowHandle);

        [DllImport("user32.dll")]
        private static extern IntPtr GetForegroundWindow();

        [DllImport("user32.dll")]
        private static extern bool GetCursorPos(out Point point);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool SetCursorPos(int x, int y);

        [DllImport("user32.dll")]
        private static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extraInfo);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint SendInput(uint inputCount, Input[] inputs, int inputSize);
    }
}
