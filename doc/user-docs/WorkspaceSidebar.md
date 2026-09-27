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
- Press `Alt+N` or `Ctrl+Shift+T` to create a terminal in the current workspace.
- Press `Ctrl+1` through `Ctrl+8` to select the corresponding workspace; `Ctrl+9` selects the last workspace.
- Press `Ctrl+R` to quickly rename the active workspace, or `F2` while the sidebar list is focused.
- Press `Delete` to close the selected workspace.
- Press `Shift+F10` for Pin, rename, close, notification, and Agent Resume actions.
- Drag workspaces to reorder them. Pinned workspaces remain grouped before unpinned workspaces.
- Drag the right edge to resize the sidebar, or focus the resize handle and use Left/Right Arrow. `Home` and `End` choose the minimum and maximum width.

The shortcuts use the regular Actions settings and can be reassigned.

A custom workspace name belongs to the workspace. It survives terminal switches, terminal closure, window restoration, and snapshot reopening. Clear the name in the rename dialog to follow the active terminal's title again; terminal titles can still be customized independently.

When the window is narrower than 640 DIP, the sidebar temporarily uses its 48-DIP compact layout. Widening the window restores the saved expanded/collapsed state and width.

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

Session persistence continues to restore the pane tree, working directories, selected workspace, custom names, Pin order, sidebar width, and expanded/collapsed state. Workspace metadata uses stable IDs in the versioned window layout.

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
