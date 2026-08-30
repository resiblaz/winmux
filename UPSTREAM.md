# Microsoft Terminal Upstream Synchronization

This fork synchronizes Microsoft Terminal through a dedicated integration branch. Do not copy source files from upstream releases and do not merge Microsoft directly into long-lived feature branches.

```text
github.com/microsoft/terminal (upstream/main)
                    |
                    v
sync/microsoft-terminal/<timestamp>-<sha>
                    |
                    v
main (this fork's product integration branch)
                    |
                    v
feature/*
```

The synchronization tool never pushes, never advances `main`, and never commits a merge before validation succeeds. It also sets the upstream push URL to `DISABLED`; all product pushes go through `origin`.

## One-time product-branch preparation

`main` must contain the product state that should receive Microsoft changes. If the current feature branch is a strict descendant of `main`, finish its review and fast-forward it first:

```powershell
git switch main
git merge --ff-only <current-product-branch>
git push origin main
```

The sync tool refuses `Prepare` when the current branch contains commits that are not present in the selected integration branch. This prevents a successful upstream merge from being based on a stale product snapshot.

## Fast path

Run all commands from the repository root with PowerShell 7.

```powershell
# Add/verify the official remote, fetch main and tags, and report drift.
pwsh -NoLogo -NoProfile -File .\tools\upstream\sync-microsoft-terminal.ps1 -Mode Check

# Create a timestamped sync branch and apply upstream/main without committing.
pwsh -NoLogo -NoProfile -File .\tools\upstream\sync-microsoft-terminal.ps1 -Mode Prepare

# After any conflicts are resolved and staged, run the full gate, update the
# portable deployment, and create the merge commit.
pwsh -NoLogo -NoProfile -File .\tools\upstream\sync-microsoft-terminal.ps1 -Mode Continue
```

After review, promote the completed branch through a PR or a local fast-forward:

```powershell
git switch main
git merge --ff-only <sync-branch>
git push origin main
```

No push is performed by the script.

## Modes

| Mode | Mutation | Result |
|---|---|---|
| `Check` | Adds/verifies `upstream` and fetches | Writes a bounded drift report; does not change branches or files |
| `Prepare` | Creates a `sync/microsoft-terminal/...` branch and starts a no-commit merge | Stops cleanly before commit or leaves standard conflict state for repair |
| `Continue` | Validates, builds the portable package, and commits | Produces a reviewed two-parent merge commit on the sync branch |
| `Abort` | Runs `git merge --abort` | Restores the pre-merge files but keeps the sync branch for inspection |
| `SelfTest` | Uses isolated repositories under the system temp directory | Exercises clean merge, conflict, abort, and dirty-tree safety behavior |

Useful options:

```powershell
# Inspect without network access, after upstream/main has already been fetched.
pwsh -NoLogo -NoProfile -File .\tools\upstream\sync-microsoft-terminal.ps1 -Mode Check -NoFetch

# Synchronize a reviewed Microsoft release tag or exact commit instead of the
# moving main branch.
pwsh -NoLogo -NoProfile -File .\tools\upstream\sync-microsoft-terminal.ps1 `
    -Mode Prepare -UpstreamRef <tag-or-commit>
```

`-SkipValidation` and `-SkipPortable` exist for the script's isolated self-tests and exceptional diagnostics. They also require the explicit `-AllowUnsafeSkip` acknowledgement and are not the normal product synchronization path.

## AI conflict handoff

Every run writes machine-readable state and an AI-oriented Markdown report outside the worktree:

```text
.git/upstream-sync/state.json
.git/upstream-sync/report.md
```

When `Prepare` exits with code `2`, the merge has ordinary resolvable conflicts. Give the AI this instruction:

> Read `AGENT.md`, `UPSTREAM.md`, and `.git/upstream-sync/report.md` completely. Continue the existing Microsoft Terminal upstream merge. Inspect the merge base, upstream version, and fork version of every conflict. Do not resolve an entire hotspot file with blanket ours/theirs. Preserve Microsoft renderer/core bug fixes and this fork's Workspace/Surface, persistence, automation, security, and test contracts. Stage every resolved path, leave unrelated files untouched, then run the upstream script in `Continue` mode.

The report classifies conflicts into fork hotspots, upstream core, build/generated boundaries, and general three-way merges. It also records the exact upstream SHA and a bounded upstream commit preview.

## Conflict policy

- Renderer, TerminalCore, ConPTY, host, and control changes should normally retain Microsoft's implementation unless the fork intentionally changed the same behavior.
- Workspace, Surface, History, Snapshot, Automation, Hook, and permission contracts must remain compatible with this fork's persisted data and local API.
- Treat `TerminalPage*`, `TabManagement.cpp`, `AppActionHandlers.cpp`, Workspace XAML, action maps, defaults, MIDL, and project files as manual merge hotspots.
- Never use whole-file `ours` or `theirs` on a hotspot merely to make the merge compile. Reconstruct the combined behavior from all three versions.
- Reconcile project/MIDL inputs and let the unified build regenerate outputs. Do not hand-copy generated binaries or clean the repository to hide stale-graph problems.
- Preserve the external contracts listed in the architecture notes: Named Pipe v1, Snapshot/Exchange schemas, `wt workspace` exit codes, Hook ACK/inbox idempotency, and high-risk permission boundaries.

## Validation and build-cache policy

`Continue` runs the repository's required workflow:

```powershell
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-workspace-release.ps1
pwsh -NoLogo -NoProfile -File .\tools\deploy\build-portable-release.ps1 -SkipBuild
```

Keep the existing build cache for the first attempt. If Microsoft changed the compiler, SDK, architecture, foundational MSBuild properties, or generated artifacts are demonstrably corrupt, use `tools/deploy/clean-build-artifacts.ps1` with a justified scope. Never run a root `git clean -fdX`.

For renderer or input changes, add a focused manual smoke after the automated gate: text rendering, resize/DPI, selection, keyboard input, pane split, settings reload, session restore, and the Workspace/Surface shortcut matrix.

## Selective urgent backports

An isolated Microsoft bug or security fix may be backported before the next full synchronization:

```powershell
git fetch upstream --prune --tags
git switch -c backport/microsoft-<topic> main
git cherry-pick -x <microsoft-commit-sha>
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-workspace-release.ps1
```

Use `-x` so provenance remains visible. Do not selectively cherry-pick renderer architecture, SDK, dependency, MIDL, or broad refactor commits without auditing their prerequisite range; use the normal upstream merge for those changes.

## Recommended cadence

- Run `Check` weekly or before starting a substantial feature.
- Prepare a reviewed upstream merge at least monthly or per Microsoft stable release.
- Backport only urgent, demonstrably isolated fixes.
- Record the final Microsoft SHA and gate result in the merge commit and generated report.
