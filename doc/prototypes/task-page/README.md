# Tasks fixed-page prototype

This browser-only prototype is used to review the Tasks information architecture
and state transitions before the native XAML/C++ implementation begins.

It intentionally contains no production runtime integration. The simulated
actions operate only on in-memory sample data and reset when the page reloads.

Preview from the repository root:

```powershell
python -m http.server 4173 --directory .\doc\prototypes\task-page
```

Then open `http://127.0.0.1:4173/`.

Design references are conceptual only. Multica source is kept under the locally
ignored `.references/multica` checkout; do not copy its source or UI assets.
