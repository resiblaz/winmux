# Tasks fixed-page prototype

This browser-only prototype records the accepted Tasks information architecture
and remains a lightweight interaction regression aid for the native XAML/C++
implementation.

It intentionally contains no production Runtime integration. The simulated
actions operate only on in-memory sample data and reset when the page reloads.
The native fixed-page skeleton is implemented on the Task feature branch; when
the prototype and native code disagree, the product specification remains the
source of truth.

Preview from the repository root:

```powershell
python -m http.server 4173 --directory .\doc\prototypes\task-page
```

Then open `http://127.0.0.1:4173/`.

Design references are conceptual only. Multica source is kept under the locally
ignored `.references/multica` checkout; do not copy its source or UI assets.
