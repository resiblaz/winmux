# README screenshots

These PNG files are direct captures of running Winmux windows.

- Application: Winmux portable `0.0.1.0` for Intel/AMD Windows x64.
- Native source baseline: `76ab6165fc13d45a90d75e5f5978efb6a2056dfc`.
- Captured on: 2026-10-18, using an isolated portable settings directory.
- Source ZIP SHA256: `4f2dd38bc80b1209ff37a4a564a09551e1a63d24deadf7b44c22405a4fc0f56f`.
- Appearance: dark theme, One Half Dark terminal colors, Cascadia Mono at size 14, and custom terminal padding.
- Method: Windows desktop capture at the application's window bounds. PNGs retain the captured window pixels.

| Image | What was running |
| --- | --- |
| [workspaces.png](workspaces.png) | Three isolated sample Git repositories; two terminal tabs and a split view in Storefront; an actual Git diff; a Node HTTP server bound to `127.0.0.1:4317` in Payments API; an actual HTTP request and Windows TCP listener query in another pane |
| [vim-navigation.png](vim-navigation.png) | A text file printed in a real terminal, selected using Winmux's NORMAL/VISUAL keyboard navigation; the live pane indicator reads VISUAL |
| [agent-history.png](agent-history.png) | Two sample prompt/conclusion records ingested through `history.ingest` into the isolated local history store; a record pinned through the actual UI |

The project names, source files, and history messages are demonstration content. History records are explicitly labeled **Sample data**. The terminal and history views are the application's actual UI.

For future updates, capture a fresh portable instance with sample data, verify every image, and update this provenance record with the package version and hash. Keep personal settings, prompts, and session history outside the screenshots.
