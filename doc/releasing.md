# Publishing Winmux releases

Winmux publishes portable ZIPs for Intel/AMD Windows x64. The fixed baseline is package version `0.0.1.0` at source commit `76ab6165fc13d45a90d75e5f5978efb6a2056dfc`. The first Winmux GitHub release is package version `1.26.0.0` with tag `v1.26.0.0`, continuing the inherited upstream `v1.25.*` tag history. The release itself points to the committed source that passed its gate, including the release tooling and notes.

## Requirements

Use PowerShell 7 from the repository root, with the C++/UWP tools and Windows SDK described in [AGENT.md](../AGENT.md). Preparation uses the existing incremental Release build graph and preserves all build caches.

For uploading, install [GitHub CLI](https://cli.github.com/) and authenticate:

```powershell
gh auth login --hostname github.com
```

The account must have write access to `resiblaz/winmux`. Both the fetch and push URLs for `origin` must identify that repository. The release commit must have been pushed to `develop`.

## 1. Commit the release version and notes

The package version in `src/cascadia/CascadiaPackage/Package-Dev.appxmanifest` is authoritative. The first release version is `1.26.0.0`. For later releases, increment it, for example to `1.26.0.1`, and add `doc/releases/<version>.md` using [the notes template](releases/TEMPLATE.md). Keep the baseline in `tools/deploy/github-release.json` unchanged.

Commit all source and notes before preparation. A dirty worktree, including untracked source files, is rejected. All subsequent stages use that same clean source commit.

Write notes around user-visible features, fixes, upgrade instructions, and known issues. For subsequent releases, review changes since the previous Winmux release's `v<version>` tag. The first release describes the baseline feature set rather than the entire inherited upstream history.

## 2. Prepare and freeze the bundle

```powershell
# Uses the current package version.
pwsh -NoLogo -NoProfile -File .\tools\deploy\prepare-github-release.ps1
```

To prepare a prerelease instead, use:

```powershell
pwsh -NoLogo -NoProfile -File .\tools\deploy\prepare-github-release.ps1 -Version 1.26.0.0 -Prerelease
```

Choose stable or prerelease when preparing; that choice is frozen with the bundle.

Preparation runs the full workspace release gate once, then repackages the exact versioned x64 Release MSIX with `-SkipBuild -NoLaunch`. Packaging uses a separate temporary directory and does not update or close the active portable installation. The script verifies x64 PE headers, required runtime files, clean default settings, and the extracted CLI's expected error handling.

The result is:

```text
dist/releases/1.26.0.0/
  winmux_1.26.0.0_windows_x64.zip
  release.json
  SHA256SUMS.txt
  release-notes.md
```

The manifest records the source commit, fixed baseline, full-gate result, package hash, source MSIX hash, and actual binary product version. The package version identifies the release; the inherited engine's binary product version is recorded separately.

Re-running preparation with unchanged source and metadata validates and reuses the frozen bundle. A conflicting frozen version is rejected. Changes after freezing require an incremented package version and a new bundle.

## 3. Upload a draft

After the release commit is pushed to `develop`:

```powershell
pwsh -NoLogo -NoProfile -File .\tools\deploy\publish-github-release.ps1 `
    -Manifest .\dist\releases\1.26.0.0\release.json
```

This creates an annotated `v<version>` tag at the recorded commit, pushes only that tag, creates a draft, and attaches exactly the ZIP, `release.json`, and `SHA256SUMS.txt`. The notes file becomes the release body. Existing local and remote tags must resolve to the recorded source commit.

The script downloads the attachments and compares their SHA256 hashes with the frozen local files, then runs the extracted portable CLI smoke check. An interrupted upload can be resumed with the same command; already uploaded matching files are retained.

## 4. Review and publish

Review the draft notes and perform manual acceptance from a fresh extraction: launch `winmux.exe`, check workspace navigation and session restoration, and complete the Narrator/DPI checks required by the release gate's manual acceptance guidance.

```powershell
pwsh -NoLogo -NoProfile -File .\tools\deploy\publish-github-release.ps1 `
    -Manifest .\dist\releases\1.26.0.0\release.json -Publish
```

This requires an existing complete draft, rechecks the remote tag and downloaded assets, runs the extracted CLI smoke check, then publishes. Stable releases are set as Latest; prereleases are not. Retrying an identical published release verifies it and makes no changes.

## Preview and failure recovery

Both commands support `-WhatIf`: local validation runs, but builds, tags, uploads, and publication do not. A preview still requires committed source, and publishing preview requires an existing local bundle.

```powershell
pwsh -NoLogo -NoProfile -File .\tools\deploy\prepare-github-release.ps1 -WhatIf
pwsh -NoLogo -NoProfile -File .\tools\deploy\publish-github-release.ps1 `
    -Manifest .\dist\releases\1.26.0.0\release.json -WhatIf
```

- Build or test failure: correct it, commit, and rerun preparation. Preserve `bin/`, `obj/`, generated files, and dependencies.
- Upload interruption: rerun the draft command with the same bundle. No rebuild is needed.
- Conflicting tag, release metadata, incomplete remote asset, or hash mismatch: the script stops without overwriting anything. Inspect the conflict before proceeding.
- A fix to a published release: increment the package version and create a new release.

Validate the release tooling independently with:

```powershell
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-github-release.ps1
```

These are offline integration tests with intercepted git/GitHub operations and a temporary x64 CLI fixture. They are also included in the normal full release gate.
