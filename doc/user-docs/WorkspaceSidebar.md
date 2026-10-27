# Workspace Sidebar

Winmux organizes workspaces in a vertical, cmux-style sidebar, with multiple terminal tabs and split panes in each workspace. Panes, profiles, terminal rendering, and command behavior use the Windows Terminal engine. This feature does not include an embedded browser.

## Enable the sidebar

Open **Settings → Appearance → Tab layout** and select **Workspace sidebar**, or add this global setting to `settings.json`:

```json
{
    "tabLayout": "workspaceSidebar"
}
```

The Winmux portable build enables `workspaceSidebar` by default. Registered installations retain their existing preference; the underlying settings default is `horizontal`.

Fresh portable settings use `JetBrainsMono Nerd Font Mono` at size `14` for all profiles. Install that font to get the intended appearance; if it is unavailable, the renderer falls back to Consolas. On first launch, the app prefers an installed PowerShell 7 profile and otherwise uses Windows PowerShell 5.1. The choice is made on the machine running the portable app. Portable updates preserve existing shell and font preferences, including profile overrides.

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
- With up to nine workspaces, `Ctrl+1` through `Ctrl+9` switch immediately when you press the digit. With ten or more workspaces, hold `Ctrl`, type a workspace number from `1` through `99` using the top-row number keys, then release `Ctrl` to select it. For example, hold `Ctrl`, type `1` then `2`, and release `Ctrl` to select workspace 12. `Ctrl+9` selects workspace 9.
- With up to nine terminal tabs in the current workspace, `Alt+1` through `Alt+9` switch immediately when you press the digit. With ten or more terminal tabs, hold `Alt`, type `1` through `99`, then release `Alt` to select that terminal. The threshold is evaluated separately for workspaces and for terminals in the current workspace. Workspace and terminal numbering are independent, and each workspace remembers its selected terminal.
- Press `Ctrl+R` to quickly rename the active workspace, or `F2` while the sidebar list is focused.
- Press `Delete` to close the selected workspace.
- Press `Shift+F10` for Pin, rename, close, notification, and Agent Resume actions.
- Drag workspaces to reorder them. Pinned workspaces remain grouped before unpinned workspaces.
- Drag the right edge to resize the sidebar, or focus the resize handle and use Left/Right Arrow. `Home` and `End` choose the minimum and maximum width.

The shortcuts use the regular Actions settings and can be reassigned.

With ten or more destinations, number entry accepts one or two digits and switches only once, on modifier release. With fewer than ten destinations, each fresh digit press switches immediately, so you can keep the modifier held while switching between single-digit numbers. Within number entry, zero, leading zeroes, more than two digits, and numbers that do not exist leave the selection unchanged. Held-key repetition neither adds digits nor repeats an immediate switch. Press `Esc` to cancel a pending number; using another shortcut, clicking, or leaving the window also cancels it. Use the top-row digits; Alt+numpad keeps its existing character-entry behavior. Bindings or explicit unbindings of a starting digit take precedence, including the existing `Ctrl+0` font-reset action. The 99 limit applies to direct numeric navigation, not the number of workspaces or terminals you can create. Horizontal layout retains its existing single-digit shortcuts.

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
| NORMAL | `h/j/k/l` or arrow keys move within the available text. `b/w/e` move to the current or previous word's beginning, the next word's beginning, or a word's end. `{` and `}` jump to the previous or next paragraph boundary. `v` selects the browsing character and anchors a selection. `i/a` return to Insert. |
| VISUAL | `h/j/k/l`, arrows, `b/w/e`, and `{`/`}` extend or shrink the selection within the text bounds. `y` copies a nonempty selection and returns to Normal. `Esc/v` cancel the range while keeping the browsing position. `i/a` return to Insert. |

Normal and Visual show a steady filled block over the browsing character, using the configured cursor color and native terminal cell rendering. The selection endpoint handles are hidden in these modes. Returning to Insert restores the application cursor, including any position, visibility, or style changes made by the shell while browsing.

Press `v` to select the browsing character, move to extend the range, then copy with `y`. Pressing `v` then `y` copies that single character, including punctuation such as `.` or `。`. Character, word, and paragraph movements include both the anchor and destination characters, so moving onto a sentence-final period includes it in the copied text. `bvey` copies a whole word, including its final character. Copying or cancelling Visual returns to its active character instead of the space after it. An empty selection leaves the clipboard unchanged. The `i/a` used to leave navigation are consumed; subsequent text goes to the shell's original cursor. Unsupported plain keys are consumed while navigating. Commands use English keys without adding input-method composition behavior.

`b` moves to the current word's beginning, or the previous word's beginning when already at the start. `e` moves to the current word's final character, or the next word's end when already at the end. `w` advances to the next word's first character; at the end of output it stops on the last character. All three support held-key repetition. `b/e` skip whitespace; `w` also stops at empty interior lines, following [Vim word motions](https://vimhelp.org/motion.txt.html#word-motions). Word classes follow the existing `wordDelimiters` setting: ordinary characters and delimiter runs are separate words. Soft-wrapped words remain continuous; explicit line breaks separate words. This does not implement Vim's language-specific `iskeyword` rules.

`{` (`Shift+[` on an English keyboard) and `}` (`Shift+]`) move backward and forward to paragraph boundaries at column zero of empty lines. Starting on an empty line skips the rest of that empty-line group before finding the next boundary. At the start or end of output, movement stops on the first or last available character. Both keys support held-key repetition and Visual selection, including the destination character. Soft-wrapped rows remain part of one logical line, including rows containing only spaces. As with terminal text bounds, a separate line containing only spaces counts as empty. Paragraph macros and section motions are not implemented.

Normal and Visual navigation stay within the available text. Entering Normal, copying, or cancelling Visual places the cursor on a character rather than beyond the line end. In Normal, horizontal movement stops on the first/last character; in Visual, endpoints may reach the boundary after the last selected character. In both modes, vertical movement clamps to short lines and remembers the desired column for later longer lines. Output that erases or shortens text also adjusts the Normal cursor immediately. Interior blank lines retain a position at column zero. Indentation and spaces within text remain selectable; the empty area below the final output and unused line-end cells are excluded. Soft-wrap spaces within a logical line are retained. An entirely empty buffer has no selectable text and remains in Normal. Multiline highlighting keeps the terminal's native appearance. Explicit trailing spaces are treated like other line-end spaces.

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

## Workspace command line

Use `wmx.exe` beside the portable `winmux.exe` to list, open, import, and export saved workspace snapshots. The registered package exposes `wmx.exe` as an execution alias.

```powershell
.\wmx.exe workspace list --format json
```

The GUI entry is `winmux.exe`; `wmx.exe` provides console output and command exit codes. Existing layout restoration continues to use the portable `settings` directory.

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
