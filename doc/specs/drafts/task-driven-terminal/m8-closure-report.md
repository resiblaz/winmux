---
author: RicherLife / Codex collaboration
created on: 2026-09-01
status: M8 closure passed on feature/task-runtime-foundation; not merged
---

# Task-driven Terminal M8 closure report

## Outcome

M8 hardens update, distribution, documentation, and review boundaries for the
M0-M8 Task slice. Protocol 1.1 advertises Task terminal surfaces,
orchestration, and Runtime drain while preserving the 1.0 request shapes.

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

- MSIX: 8,917,188 bytes; SHA256
  `199987A269B39AE7AF8C296768D22EC2BEC957B5599ECA37B3954FFF046AAEDF`.
- Portable ZIP: 12,774,389 bytes; SHA256
  `B0EB615CDD9B0F68FE992B9B319022E42E05AEACA1F17CD3A2E9895F2FBFC05C`.
- `WindowsTerminal.exe`: 675,328 bytes; SHA256
  `B8D85C53DB7A12D3B39CA2CDD37830927030035970106003C4C383589722D1E8`.
- `TaskRuntime.exe`: 1,853,440 bytes; SHA256
  `F85DF62F927F6DB01DAECCC5EB0C7D867B89B849CB8E279EFC2CBFF1CBE18238`.
- `rtask.exe`: 283,648 bytes; SHA256
  `949B23107A703CE89077D1F1EC6DB366BF49A951DC761D6FEF094A3C1E7DB53A`.

## Remaining manual acceptance

JavaScript syntax validation passed. Automated Chrome click/screenshot
regression could not run because Chrome was not running and the selected
profile had neither the ChatGPT browser extension nor its native host. No
alternate browser automation was substituted. Manual Narrator, mixed-DPI,
high-contrast, large-text, and final visual review remain explicit release
acceptance checks rather than automated-pass claims.
