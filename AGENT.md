# Winmux Repository Agent Notes

## Required Incremental Development Workflow

These rules apply to AI agents and automated development workflows. The default loop is an incremental Release build; preserving the build cache is required unless a cold build is explicitly justified.

### Default edit, validate, and publish loop

Run commands from the repository root with PowerShell 7:

```powershell
# Build the package and native tests in one MSBuild graph, then run the full gate.
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-workspace-release.ps1

# Repackage the outputs, update the portable deployment, and restart the app.
pwsh -NoLogo -NoProfile -File .\tools\deploy\build-portable-release.ps1 -SkipBuild
```

The first command intentionally builds `CascadiaPackage`, `TerminalApp.LocalTests`, `TestHostApp`, `SettingsModel.UnitTests`, `Control.UnitTests`, and the TerminalCore unit tests in one solution-level MSBuild invocation with one global-property set. Keep `bin/`, `obj/`, generated source directories, and the vcpkg install tree between runs so that later invocations remain incremental.

On the current reference machine, a clean full gate takes about 29 minutes, while an unchanged incremental full gate takes about 2 minutes. Treat a hot run that recompiles hundreds of C++ files or repeats MIDL/metadata generation as a build-graph regression. Inspect the invoked targets and global properties instead of hiding the problem with a clean build.

For a focused loop, use the gate's skip switches rather than invoking projects directly:

```powershell
# Native test build and execution, reusing an existing package output.
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-workspace-release.ps1 `
    -SkipPackageBuild -SkipHookTests -SkipCliSmoke

# Package build and CLI smoke, reusing existing native-test outputs.
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-workspace-release.ps1 `
    -SkipTestBuild -SkipNativeTests -SkipHookTests
```

Do not build these projects directly as separate MSBuild commands:

- `src\cascadia\LocalTests_TerminalApp\TerminalApp.LocalTests.vcxproj`
- `src\cascadia\LocalTests_TerminalApp\TestHostApp\TestHostApp.vcxproj`
- `src\cascadia\UnitTests_SettingsModel\SettingsModel.UnitTests.vcxproj`
- `src\cascadia\UnitTests_Control\Control.UnitTests.vcxproj`
- `src\cascadia\UnitTests_TerminalCore\UnitTests.vcxproj`

Do not build the package and tests with different global-property sets against the same `bin/obj` tree. Alternating those modes invalidates the shared TerminalApp, Settings, Control, MIDL, and metadata outputs.

### Cold builds and cleanup

Do not clean before a normal edit/build/test cycle. A root or deep clean is appropriate only when:

- the user explicitly requests a from-zero or cold-build measurement;
- the compiler, SDK, architecture, configuration, or foundational build properties changed; or
- concrete diagnostics show that generated artifacts are corrupt and an incremental rebuild cannot repair them.

When cleanup is necessary, state the reason, verify that repository build processes have stopped, and use `tools/deploy/clean-build-artifacts.ps1`. Never use `git clean -fdX` at the repository root and never clean merely to retry an unexplained build failure.

### Meaning of hot update

For this native C++/WinRT Release workflow, "hot update" means incremental compilation followed by staged portable replacement and application restart. It is not in-process native code hot reload. After a successful gate, use `build-portable-release.ps1 -SkipBuild`; do not rebuild the package a second time and do not copy `winmux.exe` by itself.

## PowerShell Release Workflows

Run release scripts from the repository root with PowerShell 7 (`pwsh`). The machine must have the Visual Studio C++/UWP build tools and a compatible Windows SDK installed.

### Portable Workspace Sidebar release

Use the portable workflow for the self-contained build that users launch through `winmux.exe`:

```powershell
# Build x64 Release, create the portable ZIP, update dist, and launch it.
.\tools\deploy\build-portable-release.ps1

# Repackage the existing Release MSIX without compiling again.
.\tools\deploy\build-portable-release.ps1 -SkipBuild

# Build and deploy without launching the application.
.\tools\deploy\build-portable-release.ps1 -NoLaunch

# Write the ZIP and extracted deployment to another directory.
.\tools\deploy\build-portable-release.ps1 -OutputDirectory .\artifacts
```

The script:

- builds the x64 Release package unless `-SkipBuild` is specified;
- creates an unpackaged portable distribution using the repository's official packaging script;
- writes a clean default `settings/settings.json` with Workspace Sidebar, session layout restoration, and Vim navigation enabled, terminal padding set to `3`, and the scrollbar hidden into the ZIP;
- deploys the extracted application under `dist/winmux-<version>`;
- preserves the existing deployed `settings` directory across rebuilds, migrates older `terminal-<version>` portable settings when needed, and adds missing session-restoration and Vim-navigation defaults while preserving explicit preferences and profile overrides;
- replaces the deployed directory through a staged update and starts its `winmux.exe` unless `-NoLaunch` is specified;
- emits the package, deployment, launcher, and SHA256 paths when complete.

The distributable archive is named like:

```text
dist/winmux_<version>_windows_x64.zip
```

The GUI entry is `winmux.exe`; use `wmx.exe` for console output and workspace CLI commands. The upstream Dev build channel and package identity are retained for compatibility with existing registered settings. Internal Microsoft/Terminal namespaces, GUIDs, protocol names, and historical implementation records are not branding replacement targets.

Keep the current source directory in place during migration so existing `bin/obj` caches remain usable. New clones naturally use the repository name `winmux`.

Do not copy `winmux.exe` by itself. The entire extracted `winmux-<version>` directory is required at runtime.

### GitHub releases (Windows x64 only)

The release baseline is package version `0.0.1.0` at commit `76ab6165fc13d45a90d75e5f5978efb6a2056dfc`. `tools/deploy/github-release.json` records this fixed baseline. GitHub release tags are `winmux-v<version>`. Only Intel/AMD Windows x64 portable packages are supported.

Read `doc/releasing.md` for the complete procedure. Commit the scripts, package-version changes, and versioned release notes before preparing a release. Run all stages from that clean source commit.

```powershell
# Run the existing full incremental gate, then package once using -SkipBuild.
pwsh -NoLogo -NoProfile -File .\tools\deploy\prepare-github-release.ps1

# Push only this release tag and create/resume a draft with verified attachments.
pwsh -NoLogo -NoProfile -File .\tools\deploy\publish-github-release.ps1 `
    -Manifest .\dist\releases\0.0.1.0\release.json

# After manual acceptance, verify the downloads and publish the existing draft.
pwsh -NoLogo -NoProfile -File .\tools\deploy\publish-github-release.ps1 `
    -Manifest .\dist\releases\0.0.1.0\release.json -Publish
```

Preparation preserves the incremental build graph and selects an exact versioned Release MSIX. It packages into a separate temporary directory, leaving the active portable deployment and settings alone. The frozen bundle in `dist/releases/<version>/` binds the ZIP, notes, checksums, full-gate result, and source commit. Re-running with the same source reuses it; a different source or conflicting hash requires a new version.

Publishing requires GitHub CLI authentication with write access to `resiblaz/winmux` and the release commit pushed to `develop`. It defaults to a draft, downloads and verifies attachments, and never force-pushes a tag or overwrites an attachment. `-Publish` publishes an existing complete draft. Both scripts support `-WhatIf`. The offline integration tests in `test-github-release.ps1` run as part of the full gate without network or GitHub changes.

### Registered local Release

Use the registered MSIX workflow when package identity, execution aliases, or Start menu registration are needed:

```powershell
# Build, register, and launch the local x64 Release package.
.\tools\deploy\deploy-local-release.ps1

# Register existing outputs without rebuilding.
.\tools\deploy\deploy-local-release.ps1 -SkipBuild

# Build and register without launching.
.\tools\deploy\deploy-local-release.ps1 -NoLaunch

# Launch the package registered by tools/deploy/deploy-local-release.ps1.
.\tools\deploy\launch-local-release.ps1
```

`tools/deploy/deploy-local-release.ps1` delegates to `tools/Deploy-LocalRelease.ps1`. The tools script also has an internal `-BuildOnly` switch used by the portable workflow so that creating a portable package does not register the MSIX.

The registered package and portable deployment use separate settings stores. Configure `Settings -> Appearance -> Tab layout -> Workspace sidebar` in the registered package if it should also show Workspace Sidebar.

## Cleaning Build Artifacts

Use `tools/deploy/clean-build-artifacts.ps1` instead of running `git clean -fdX` at the repository root. The cleanup script validates every target, displays estimated sizes, requires confirmation, and always preserves `dist/`, `.git/`, and `.PowershellModules/`.

```powershell
# Preview the default cleanup without deleting anything.
.\tools\deploy\clean-build-artifacts.ps1 -WhatIf

# Remove root bin/ and obj/ outputs (default scope).
.\tools\deploy\clean-build-artifacts.ps1

# Remove only root x64 Debug outputs.
.\tools\deploy\clean-build-artifacts.ps1 -Scope Debug

# Also remove ignored generated outputs below src/.
.\tools\deploy\clean-build-artifacts.ps1 -Scope Deep

# Deep cleanup plus the restorable NuGet packages directory.
.\tools\deploy\clean-build-artifacts.ps1 -Scope Deep -IncludePackages
```

Use `-Confirm:$false` only in non-interactive automation where deletion has already been reviewed. After cleaning root or deep outputs, the next portable release must be built without `-SkipBuild`.

## Generated Output

`dist/` contains generated ZIPs, extracted runtime files, and portable user settings. It is intentionally ignored by Git and must not be committed.

## Local Working Notes

Keep `work_path/` and `workpath/` local and untracked. These directories hold personal planning notes and acceptance artifacts; do not force-add them to Git. Put documentation intended for repository users under `doc/`.
