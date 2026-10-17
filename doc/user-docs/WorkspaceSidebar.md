# Workspace Sidebar

Workspace Sidebar presents each Windows Terminal tab as a workspace in a vertical, cmux-style navigator. Panes, profiles, terminal rendering, and command behavior continue to use the existing Windows Terminal implementation. This feature does not include an embedded browser.

## Enable the sidebar

Open **Settings → Appearance → Tab layout** and select **Workspace sidebar**, or add this global setting to `settings.json`:

```json
{
    "tabLayout": "workspaceSidebar"
}
```

The default is `horizontal`.

### Migrate from the experimental setting

The legacy setting remains readable:

```json
{
    "experimental.workspaceSidebar": true
}
```

Replace it with `"tabLayout": "workspaceSidebar"`. If both keys exist, `tabLayout` wins. New settings are serialized with `tabLayout`; the legacy Boolean is not written back.

## Work with workspaces

- Select a workspace with the mouse or Up/Down Arrow.
- Press `Ctrl+N` to create a workspace.
- Press `Alt+N` or `Ctrl+Shift+T` to create a terminal in the current workspace, using the current terminal's active pane directory. The new-terminal button and workspace menu use the same behavior. An explicitly supplied starting directory takes precedence; if the current directory is unavailable or no longer exists, the selected profile's starting directory is used. `Ctrl+N` creates a separate workspace using the default profile's starting directory.
- Press `Ctrl+1` through `Ctrl+8` to select the corresponding workspace; `Ctrl+9` selects the last workspace.
- Press `Ctrl+R` to quickly rename the active workspace, or `F2` while the sidebar list is focused.
- Press `Delete` to close the selected workspace.
- Press `Shift+F10` for Pin, rename, close, notification, and Agent Resume actions.
- Drag workspaces to reorder them. Pinned workspaces remain grouped before unpinned workspaces.
- Drag the right edge to resize the sidebar, or focus the resize handle and use Left/Right Arrow. `Home` and `End` choose the minimum and maximum width.

The shortcuts use the regular Actions settings and can be reassigned.

A custom workspace name belongs to the workspace. It survives terminal switches, terminal closure, window restoration, and snapshot reopening. Clear the name in the rename dialog to follow the active terminal's title again; terminal titles can still be customized independently.

When the window is narrower than 640 DIP, the sidebar temporarily uses its 48-DIP compact layout. Widening the window restores the saved expanded/collapsed state and width.

## Vim navigation and copying

The portable build enables Vim navigation by default. Configure it in `profiles.defaults`, or override it on an individual profile:

```json
{
    "profiles": {
        "defaults": {
            "experimental.vimMode": true
        }
    }
}
```

Set the value to `false` to disable it. The setting reloads without restarting: disabling it clears Vim navigation and returns to normal terminal input; enabling it starts in Insert. Portable updates preserve an explicit value, including profile-specific opt-outs.

The indicator belongs to each terminal pane:

| Mode | Keys |
| --- | --- |
| INSERT | Type normally, including `Esc`; `Alt+V` enters Normal. |
| NORMAL | `h/j/k/l` or arrow keys move the native Mark Mode cursor. `v` anchors a selection. `i/a` return to Insert. |
| VISUAL | Movement extends or shrinks the selection. `y` copies a nonempty selection and returns to Normal at its current endpoint. `Esc/v` cancel the range while keeping the browsing position. `i/a` return to Insert. |

Selection uses the terminal's existing endpoint semantics: press `v`, move, then copy with `y`. Pressing `y` on an empty selection leaves the clipboard unchanged. The `i/a` used to leave navigation are consumed; subsequent text goes to the shell's original cursor. Unsupported plain keys, including `b/e`, are consumed while navigating. This first version supports English command keys and does not add input-method composition behavior.

Entry is the configurable `enterVimMode` action (`Terminal.EnterVimMode`), bound to `Alt+V` by default. It can also be invoked from the command palette. Escape is not reserved in Insert, so inline terminal applications such as Codex can receive it to cancel menus or interrupt work. Binding this action to Escape yourself reintroduces that conflict. Repeating the entry action while already navigating preserves the current mode and position.

Alternate-screen applications such as a normally configured nvim receive input through the existing path; the Vim indicator is hidden while the alternate screen is active. Returning to the main screen starts in Insert. If a terminal application is configured not to use the alternate screen, disable this feature on its profile. A tmux session that stays in the alternate screen also keeps this feature inactive.

Resizing the terminal, replacing its connection, clearing its buffer, pressing a mouse button, or pasting ends Vim navigation. Existing mouse selection and legacy `Ctrl+Shift+M` Mark Mode keep their usual behavior. Workspace and terminal shortcuts remain available. Vim modes are not saved across application restarts.

## Workspace context and notifications

The sidebar can show:

- the current folder name reported by the leftmost terminal tab, with the full path in its tooltip;
- the current local Git branch;
- TCP ports listened to by the Windows process tree under any pane;
- connection, unread, progress, broadcast-input, and read-only status;
- unread counts and the latest notification.

The workspace directory follows the leftmost terminal tab even while another tab is selected. Reordering or closing that tab switches the directory source to the new leftmost tab. Within a split terminal tab, the directory comes from its active terminal pane.

The shell must report directory changes using OSC 7 or OSC 9;9. If the folder stays at the startup directory after running cd, enable directory reporting in the shell prompt. For Oh My Posh on PowerShell, set "pwd": "osc99" in the prompt configuration. The visible prompt can keep its existing theme.

Git lookup applies to local Windows directories. Remote WSL/SSH paths without a reliable local directory remain blank. Listening ports are refreshed in the background while Workspace Sidebar is enabled. The compact row shows up to three sorted ports and an overflow count; the tooltip and accessibility name contain the full deduplicated list.

OSC 777 notifications and bells can mark a pane for attention. Open the notification center for read and unread history, or press `Ctrl+Shift+U` to jump to the latest unread source pane. Each workspace retains up to 100 notifications in memory; the notification center displays the newest 1000 across all open workspaces. Notification history is not persisted across process restarts.

## Persistence and Agent Resume

The portable Workspace Sidebar build restores the previous session by default. Close the application normally with the workspaces and terminals you want to keep still open. On the next launch, it reopens the same workspace groups and their terminal tabs, including split panes, each pane's working directory, the selected workspace and terminal, custom names, Pin order, sidebar width, and expanded/collapsed state. Closing individual terminals or workspaces removes them from the session to restore. Workspace metadata uses stable IDs in the versioned window layout.

This uses **Settings → Startup → When Terminal starts → Restore window layout**, or the global setting:

```json
{
    "firstWindowPreference": "persistedLayout"
}
```

Portable upgrades add this setting if it is absent and preserve an explicit startup preference. Set it to `defaultProfile` to disable restoration, or `persistedLayoutAndContent` to also retain terminal output. Launch without extra window arguments to restore the saved session without opening an additional window.

Each pane restores its current local directory reported by its shell through OSC 7 or OSC 9;9. If the reported directory is invalid or directory reporting is unavailable, it falls back to its startup directory. Shell processes are started again, so running commands and process memory are not resumed. A normal exit saves the current layout; unexpected termination can only recover the last periodic save. Portable updates wait for normal shutdown and abort if the application remains open, so a pending close confirmation cannot cause a forced termination that loses session state.

Agent Resume currently supports a constrained Codex session UUID. It stores neither arbitrary commands nor prompts, tokens, or sensitive environment variables. Resume uses a fixed command template, resolves absolute executable paths, opens a new pane in the target workspace, and requires confirmation every time.

## Local automation permissions

Workspace Sidebar starts a current-user local Named Pipe API while the sidebar layout is enabled. Workspace, pane, notification, status, and progress operations use strict versioned request fields. Workspace query results include a `listeningPorts` number array.

Sending terminal input and reading terminal content are separate high-risk capabilities and are disabled by default. Enable only the capability a trusted local client needs, either in Settings or with:

```json
{
    "experimental.workspaceAutomation.allowInput": true,
    "experimental.workspaceAutomation.allowContentRead": false
}
```

Requests must identify an exact workspace, terminal surface, and pane and remain subject to length/range limits. `pane.list` returns panes from every terminal in the workspace, with a `surfaceId` on each pane and an `activeSurfaceId` alongside `activePaneId` in the result. Pane IDs are only unique within a surface.

`pane.focus`, `pane.split`, `notification.create`, `terminal.sendInput`, and `terminal.readContent` require `workspaceId`, `surfaceId`, and `paneId`. `notification.markRead` accepts either `workspaceId` alone for the entire workspace or all three fields for one pane. Use the `surfaceId` returned by `pane.list`; switching terminals does not change a request's target. Older pane requests without `surfaceId` return `invalid_params`, and removed or foreign surfaces return `pane_not_found` instead of targeting the selected terminal.

Enabling one permission never enables the other. Switching back to the horizontal layout stops the automation server.

## Window modes and accessibility

- Focus mode hides the sidebar.
- Quake mode uses Focus mode and therefore hides the sidebar.
- Fullscreen hides the sidebar unless `showTabsFullscreen` is enabled.
- Workspace and notification automation names include their relevant context.
- High Contrast uses the system Highlight and HighlightText colors for selection.

## Known limits

- No embedded browser or browser automation.
- No second-level Surface tabs inside a pane.
- Remote Git branch discovery is not included.
- Port discovery covers Windows TCP listeners owned by the ConPTY root process or its descendants. UDP, remote SSH hosts, and listeners owned by WSL service processes outside that tree are not included.
- Notification history is process-local.
- The global notification-center view shows the newest 1000 records.
- Narrator, High Contrast visual behavior, mixed-DPI movement, and long production soaks should be validated on the target Windows build before broad deployment.
