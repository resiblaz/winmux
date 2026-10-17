# Winmux

Winmux is a Windows-only terminal workspace for multiple projects and coding agents, built on Windows Terminal's native terminal engine.

It combines workspace navigation, terminal tabs, session layout restoration, agent history, and keyboard-driven text selection in one desktop application.

## Features

- Organize projects in a workspace sidebar, with multiple terminal tabs and split panes per workspace.
- Restore workspace layouts, terminal counts, active tabs, and working directories after restarting.
- Open a new terminal in the active terminal's directory with `Alt+N`.
- Enter Vim navigation with `Alt+V`; use `hjkl`, `b/e`, `v`, and `y` to browse, select, and copy text.
- Keep normal input in Insert mode, including Escape for Codex; alternate-screen applications such as nvim receive their own input.
- Track coding-agent notifications, conclusions, and local history.
- Save and exchange workspace snapshots through the `wmx workspace` CLI.

See the [workspace user guide](doc/user-docs/WorkspaceSidebar.md) for shortcuts, settings, and behavior.

## Download and run

Portable builds are published under [Releases](https://github.com/resiblaz/winmux/releases) when a release is available.

Extract the entire `winmux_<version>_windows_x64.zip` archive and run `winmux.exe` inside `winmux-<version>`. Keep the accompanying DLLs, resources, and `.portable` marker together.

`winmux.exe` opens the desktop application. `wmx.exe` is the console launcher and workspace CLI:

```powershell
.\wmx.exe workspace list --format json
```

## Build

Requirements: Windows, PowerShell 7, Visual Studio C++/UWP build tools, and the Windows SDK used by this repository. Read [AGENT.md](AGENT.md) for the incremental build workflow and tooling requirements.

```powershell
git clone https://github.com/resiblaz/winmux.git
cd winmux
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-workspace-release.ps1
pwsh -NoLogo -NoProfile -File .\tools\deploy\build-portable-release.ps1 -SkipBuild
```

The release gate builds the application and native tests together. The second command packages those outputs, updates the portable deployment, and starts Winmux. Preserve `bin/`, `obj/`, generated files, and dependencies between builds.

The current development line is `develop`. Historical feature branches and upstream commits are retained when migrating repositories.

## Settings and existing installations

Portable settings, session state, and local history live in the application's `settings` directory. The deployment script preserves that entire directory. When a new Winmux deployment has no settings, it first looks for a previous Winmux portable installation, then for the old `terminal-<version>` layout under the same output directory.

The script closes the previous application normally before copying its data. It retains the old portable directory during the initial migration. Explicit preferences, including `experimental.vimMode: false`, remain effective.

Vim navigation can be enabled or disabled per profile:

```json
{
    "profiles": {
        "defaults": {
            "experimental.vimMode": true
        }
    }
}
```

The registered package currently retains the upstream Dev package identity so existing registered settings continue to resolve. Its display name and launch alias are Winmux and `wmx.exe`. Internal project names, namespaces, GUIDs, and protocol identifiers continue to support upstream integration.

## Data and privacy

Workspace layouts, settings, snapshots, and agent history are stored locally. Portable installations keep their data beside the application. Registered installations use their Windows application data location.

Programs launched inside the terminal, including shells and coding agents, have their own network behavior and account settings. Opening documentation, feedback, or release links connects to GitHub. Windows platform diagnostics follow the inherited terminal implementation.

## Development notes

- [Build and deployment workflow](AGENT.md)
- [Workspace and Vim usage](doc/user-docs/WorkspaceSidebar.md)
- [Report an issue](https://github.com/resiblaz/winmux/issues)

The inherited Microsoft-specific GitHub workflows are restricted to the upstream repository. Winmux validation currently uses the local release gate described above.

## Upstream and license

Winmux is based on [Microsoft Windows Terminal](https://github.com/microsoft/terminal), with workspace interaction inspired by [cmux](https://github.com/manaflow-ai/cmux).

The original copyright notices, [MIT license](LICENSE), and third-party notices are retained. Keep `upstream` pointing to `https://github.com/microsoft/terminal.git` when following future upstream changes.
