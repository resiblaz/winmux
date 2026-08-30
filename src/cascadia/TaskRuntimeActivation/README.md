# Task Runtime activation foundation

This static library owns the bounded synchronization primitives used by
on-demand Task Runtime activation. M2D2a supplies synchronization and M2D2b
adds the bounded Windows process-launch transaction. Connect-first launch
coordination remains a later slice.

The library provides:

- saturating absolute monotonic deadlines;
- an injectable synchronous clock/wait seam;
- the production `GetTickCount64` / `WaitForMultipleObjects` implementation;
- a per-Runtime-identity launch mutex using `RuntimeIdentity.LaunchMutexName`;
- current-user/System security inherited from the Runtime transport identity;
- cancellation-first waits over one unchanged absolute deadline;
- explicit abandoned-mutex acquisition reporting; and
- thread-affine, move-only RAII mutex ownership;
- strict local absolute trusted roots, direct-child `TaskRuntime.exe` paths,
  per-component reparse rejection, and Windows argv quoting;
- zero inherited handles, standard streams, or handle-list attributes;
- parent-Job breakaway preflight plus suspended-child post-verification; and
- a public process observation that can wait, query, or close but never
  terminate the shared Runtime.

Launch-lock handles are always non-inheritable. Different Runtime identities
use different mutex names and therefore never serialize each other.

The launcher uses `CREATE_SUSPENDED` only as a pre-commit containment proof.
It rejects a parent Job that cannot break away, verifies the child is outside
every Job, and treats successful `ResumeThread` as the ownership commit point.
Only the private suspended guard may terminate a process during rollback.
After commit, cancellation or a caller deadline may end that caller's attempt,
but must never terminate a Runtime that another client can already share.
If an immediate rollback cannot prove process exit, control is transferred to
an internal lazy quarantine worker. It retains the process handle and retries
termination/wait until exit; normal launches create no quarantine thread.
