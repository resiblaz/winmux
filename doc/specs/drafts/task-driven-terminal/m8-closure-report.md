---
author: RicherLife / Codex collaboration
created on: 2026-09-01
status: M8 closure passed on feature/task-runtime-foundation; not merged
---

# Task-driven Terminal M8 closure report

## Outcome

M8 hardens update, distribution, documentation, and review boundaries for the
M0-M8 Task slice. Protocol 1.1 introduced Task terminal surfaces,
orchestration, and Runtime drain while preserving the 1.0 request shapes.
Protocol 1.2 keeps those wire shapes and identifies the Runtime generation that
launches the shared Codex provider in `--yolo` mode.

A newer UI can ask an older drain-capable Runtime to stop after durable work is
quiescent, then reconnect to and activate the matching packaged helper. Drain
has a real child-process test and never exits while TaskStore activity remains.

## Packaging and documentation

- The registered MSIX and portable ZIP gates require non-empty
  `WindowsTerminal.exe`, `TaskRuntime.exe`, and `rtask.exe` payloads.
- The portable package was produced with `-SkipBuild -NoLaunch`; no user
  terminal was closed or restarted. All three deployed payload hashes match
  their MSIX and ZIP entries.
- The browser prototype now models Stop/Esc, Task-terminal close, Cancel,
  Retry, safe Worktree finalization, Delete, and durable message edits.
- User documentation covers ownership, creation/execution, follow-ups, retry,
  completion, cleanup, storage, and privacy. Separate release notes record
  migration, rollback, security/privacy, and upstream-sync constraints.

## Final gate evidence

The unified incremental Release gate passed on 2026-09-01, including Hook
246/246, the complete Task/Runtime suites, TerminalApp and Settings selections,
real Runtime child-process checks, MSIX payload validation, and workspace CLI
smoke. No clean build was used.

- MSIX: 8,917,191 bytes; SHA256
  `E4CC93B921EB7ED68679BEB7B3BCE32D30CF73C456FAA298AB4BD228426D4A10`.
- Portable ZIP: 12,774,418 bytes; SHA256
  `C7A28D20D28684595C94A50FF5BFB25D76B50CB953748C553E4E0A9B58684D8F`.
- `WindowsTerminal.exe`: 675,328 bytes; SHA256
  `B8D85C53DB7A12D3B39CA2CDD37830927030035970106003C4C383589722D1E8`.
- `TaskRuntime.exe`: 1,853,440 bytes; SHA256
  `5040A6A015CFEA5C50A37402A9353C5FEA999C0E7F0ADB77772C1089F0E31892`.
- `rtask.exe`: 283,648 bytes; SHA256
  `042B4B9FBE543D6CBEDB2FB79B439ACA7C54BC47A19DBABC66DD4EF1A5FF12F7`.

## Remaining manual acceptance

JavaScript syntax validation passed. Automated Chrome click/screenshot
regression could not run because Chrome was not running and the selected
profile had neither the ChatGPT browser extension nor its native host. No
alternate browser automation was substituted. Manual Narrator, mixed-DPI,
high-contrast, large-text, and final visual review remain explicit release
acceptance checks rather than automated-pass claims.
