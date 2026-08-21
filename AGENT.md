# Repository Agent Notes

## PowerShell Release Workflows

Run release scripts from the repository root with PowerShell 7 (`pwsh`). The machine must have the Visual Studio C++/UWP build tools and a compatible Windows SDK installed.

### Portable Workspace Sidebar release

Use the portable workflow for the self-contained build that users launch through `WindowsTerminal.exe`:

```powershell
# Build x64 Release, create the portable ZIP, update dist, and launch it.
.\build-portable-release.ps1

# Repackage the existing Release MSIX without compiling again.
.\build-portable-release.ps1 -SkipBuild

# Build and deploy without launching the application.
.\build-portable-release.ps1 -NoLaunch

# Write the ZIP and extracted deployment to another directory.
.\build-portable-release.ps1 -OutputDirectory .\artifacts
```

The script:

- builds the x64 Release package unless `-SkipBuild` is specified;
- creates an unpackaged portable distribution using the repository's official packaging script;
- writes a clean default `settings/settings.json` with `"tabLayout": "workspaceSidebar"` into the ZIP;
- deploys the extracted application under `dist/terminal-<version>`;
- preserves the existing deployed `settings` directory across rebuilds and keeps Workspace Sidebar enabled;
- replaces the deployed directory through a staged update and starts its `WindowsTerminal.exe` unless `-NoLaunch` is specified;
- emits the package, deployment, launcher, and SHA256 paths when complete.

The distributable archive is named like:

```text
dist/WindowsTerminalDev_<version>_x64_workspace-sidebar.zip
```

Do not copy `WindowsTerminal.exe` by itself. The entire extracted `terminal-<version>` directory is required at runtime.

### Registered local Release

Use the registered MSIX workflow when package identity, execution aliases, or Start menu registration are needed:

```powershell
# Build, register, and launch the local x64 Release package.
.\deploy-local-release.ps1

# Register existing outputs without rebuilding.
.\deploy-local-release.ps1 -SkipBuild

# Build and register without launching.
.\deploy-local-release.ps1 -NoLaunch

# Launch the package registered by deploy-local-release.ps1.
.\launch-local-release.ps1
```

`deploy-local-release.ps1` delegates to `tools/Deploy-LocalRelease.ps1`. The tools script also has an internal `-BuildOnly` switch used by the portable workflow so that creating a portable package does not register the MSIX.

The registered package and portable deployment use separate settings stores. Configure `Settings -> Appearance -> Tab layout -> Workspace sidebar` in the registered package if it should also show Workspace Sidebar.

## Generated Output

`dist/` contains generated ZIPs, extracted runtime files, and portable user settings. It is intentionally ignored by Git and must not be committed.
