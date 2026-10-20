# Winmux

[简体中文](README.md) | English

**A terminal workspace for multiple projects on Windows.**

Winmux builds on Windows Terminal's native engine and brings project workspaces, terminal tabs, split panes, keyboard selection, and local Codex history into one desktop application. Use it to switch between projects, run development services, and work with command-line coding tools.

[Downloads](https://github.com/resiblaz/winmux/releases) · [User guide](doc/user-docs/WorkspaceSidebar.md) · [Build and deploy](AGENT.md) · [Release workflow](doc/releasing.md) · [Report an issue](https://github.com/resiblaz/winmux/issues)

![Actual Winmux window with three project workspaces, two terminal tabs, and split panes showing a Git diff and an HTTP response](doc/images/readme/workspaces.png)

*Captured from the actual `0.0.1.0` Windows x64 portable build. The left sidebar switches projects; the top row switches terminals within a project. The panes show a real Git diff and HTTP 200 response from a local API listening on port `4317` in another workspace. Screenshots use isolated sample projects and a custom color scheme and font size. See [capture notes](doc/images/readme/README.md).*

## Three levels of organization

| Level | Purpose | Example in the screenshot |
| --- | --- | --- |
| Workspace | Group a project's terminals; name, pin, and reorder the group | Storefront, Payments API, Team notes |
| Terminal tab | Separate tasks within one workspace | Git review and Shell inside Storefront |
| Split pane | Run terminals side by side within a terminal tab | Review a diff while querying the API |

Workspace names are saved independently of terminal titles. A project can keep its name as you switch terminals, close one, or restore the window layout.

## Get started

**Release target: Intel/AMD Windows x64. First release: `1.26.0.0` (tag `v1.26.0.0`).** Requires Windows 10 version 2004 (build 19041) or later, including Windows 11.

1. Download `winmux_<version>_windows_x64.zip` from [Releases](https://github.com/resiblaz/winmux/releases). If no ZIP has been published yet, use the [source build](#build-from-source) below.
2. Extract the entire archive. Keep its DLLs, resources, `settings/` directory, and `.portable` marker together.
3. Run **`winmux.exe`** inside `winmux-<version>`.

The portable build enables Workspace Sidebar, layout restoration, and Vim navigation by default. Settings and local state live beside the application in `settings/`.

`winmux.exe` opens the desktop application. **`wmx.exe`** provides console output and workspace snapshot commands. For example, list saved snapshots:

```powershell
.\wmx.exe workspace list --format json
```

## Everyday shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+N` | Create a workspace |
| Hold `Ctrl`, type `1`–`99`, release to confirm | Select a workspace; for example, hold `Ctrl`, type `12`, then release |
| Hold `Alt`, type `1`–`99`, release to confirm | Select a terminal within the current workspace, using independent numbering |
| `Ctrl+R` | Rename the active workspace |
| `Alt+N` / `Ctrl+Shift+T` | Add a terminal in the active pane's working directory |
| `Ctrl+Shift+P` | Open the command palette, including split-pane actions |
| `Alt+V` | Enter Vim navigation for terminal output |
| `Ctrl+Shift+U` | Jump to the latest unread notification's source |

Right-click a workspace to pin, rename, close, or manage notifications. Drag workspaces to reorder them and drag the sidebar edge to resize it. Shortcuts can be changed in Settings → Actions.

## Implemented capabilities

| Capability | Behavior |
| --- | --- |
| Project context | Show working directories, local Git branches, and Windows TCP listeners belonging to terminal process trees |
| Workspaces and panes | Keep multiple terminal tabs in each workspace and split individual tabs |
| Layout restoration | Restore workspaces, terminals, pane layouts, directories, selection, names, and pin order after a normal exit |
| Notifications and status | Receive terminal notifications, show unread and progress states, and jump to the source pane |
| Local Codex history | With the Hook configured, store prompts and conclusions; search, filter, pin, and copy them |
| Snapshots and automation | List, open, import, and export saved snapshots using `wmx workspace`; use the local Named Pipe API for automation |

**Layout restoration starts new shells.** Running services, commands, and process memory must be started again. Closing a terminal removes it from the layout to restore. Leave the terminals you need open and exit the application normally to save the session.

Directory tracking relies on the shell reporting its directory through OSC 7 or OSC 9;9. Git lookup targets local Windows directories. Port discovery covers Windows child-process TCP listeners; remote SSH ports and listeners owned by WSL service processes are outside that scope. See [configuration details](doc/user-docs/WorkspaceSidebar.md#workspace-context-and-notifications).

### Browse and copy terminal output with the keyboard

Press `Alt+V` for NORMAL mode. Move with `h/j/k/l` or arrow keys and use `b/w/e` for word movement. Press `v` for a VISUAL selection, move, then press `y` to copy. `i` or `a` returns to INSERT.

![Actual Vim VISUAL mode with a keyboard-created selection in terminal output and the VISUAL indicator in the bottom-right corner](doc/images/readme/vim-navigation.png)

*The selection was made with actual keyboard navigation. Each split pane maintains its own mode.*

Navigation uses English keys; switch an input method to English input first. In INSERT, `Esc` continues to reach your shell or Codex. Alternate-screen applications such as nvim retain their own keyboard behavior.

### Review Codex prompts and conclusions

Open the workspace tools page from the top-right document icon and select **History**. View the current workspace or all workspaces, search content, filter by session and date, and pin or copy entries. Records are stored locally.

![Actual History tools page showing a prompt, a Markdown conclusion, session filters, and a pinned sample record](doc/images/readme/agent-history.png)

*The messages are labeled Sample data and were inserted through the local `history.ingest` API to demonstrate the actual history storage and UI.*

**Automatic Codex recording requires the Hook.** Obtain this repository's source and run the following from its root:

```powershell
pwsh -NoLogo -NoProfile -File .\.codex\hooks\install-workspace-history.ps1
```

This configures the current user's Codex Hook. To remove it, run `uninstall-workspace-history.ps1` from the same directory. Other command-line coding tools can run in the terminal; automatic history integration currently uses the supplied Codex Hook.

## Settings and upgrades

- Portable settings, session state, and local history live in `settings/`. Registered installations use a separate settings store.
- Before a manual upgrade, close Winmux normally, back up `settings/`, extract the new release into a new directory, and migrate your settings.
- To disable Vim navigation, set `"experimental.vimMode": false` in `profiles.defaults` or an individual profile.
- With existing settings, enable the sidebar through Settings → Appearance → Tab layout → Workspace sidebar.

History records are stored on the device. Shells, coding tools, and AI services running inside the terminal manage their own network behavior.

## Build from source

Requires Windows, PowerShell 7, Visual Studio C++/UWP build tools, and the Windows SDK used by this repository. Read [AGENT.md](AGENT.md) for the full workflow.

```powershell
git clone --branch develop https://github.com/resiblaz/winmux.git
cd winmux

# Build the app and native tests together, then run the release gate.
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-workspace-release.ps1

# Package the validated outputs and launch the portable application.
pwsh -NoLogo -NoProfile -File .\tools\deploy\build-portable-release.ps1 -SkipBuild
```

Preserve `bin/`, `obj/`, generated files, and dependency caches between builds. GitHub releases follow preparation → draft upload → verification and publication. See [the release workflow](doc/releasing.md).

## Current scope

- GitHub packages target **Windows x64**.
- The application has no embedded browser. It runs command-line programs in terminals.
- Notification history lasts for the current process; local Codex history is persisted separately.
- Terminal input and content-reading permissions for the local automation API are disabled by default and enabled separately.

## Documentation and credits

- [Workspaces, Vim, notifications, and automation](doc/user-docs/WorkspaceSidebar.md)
- [Incremental builds and local deployment](AGENT.md)
- [GitHub releases](doc/releasing.md)
- [1.26.0.0 first release notes](doc/releases/1.26.0.0.md)
- [Report an issue](https://github.com/resiblaz/winmux/issues)

Winmux is based on [Microsoft Windows Terminal](https://github.com/microsoft/terminal), with workspace interaction inspired by [cmux](https://github.com/manaflow-ai/cmux). Original copyright notices, the [MIT license](LICENSE), and third-party notices are retained.
