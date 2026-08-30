# Task Runtime activation foundation

This static library owns the bounded synchronization primitives used by
on-demand Task Runtime activation. M2D2a intentionally stops before process
launch or connect/launch coordination.

The library provides:

- saturating absolute monotonic deadlines;
- an injectable synchronous clock/wait seam;
- the production `GetTickCount64` / `WaitForMultipleObjects` implementation;
- a per-Runtime-identity launch mutex using `RuntimeIdentity.LaunchMutexName`;
- current-user/System security inherited from the Runtime transport identity;
- cancellation-first waits over one unchanged absolute deadline;
- explicit abandoned-mutex acquisition reporting; and
- thread-affine, move-only RAII mutex ownership.

Launch-lock handles are always non-inheritable. Different Runtime identities
use different mutex names and therefore never serialize each other. Process
path resolution, Windows argv quoting, `CreateProcess`, connect-first
coordination, and crash restart belong to later M2D2 slices.
