---
author: RicherLife / Codex collaboration
created on: 2026-09-01
status: M3 closure passed on feature/task-runtime-foundation; not merged
---

# Task-driven Terminal M3 closure report

## Outcome

M3A-M3H are complete on `feature/task-runtime-foundation`. The implementation
adds safe Direct/Git concurrency primitives, structured Git execution,
faithful source capture, recoverable persistent per-Task Worktrees, per-Run
checkpointing, explicit finalization/cleanup, and restart/concurrency soak
coverage. It does not merge, rebase, squash, push, rewrite Agent commits, or
mutate the source Worktree while constructing a managed Worktree.

The branch remains separate from `main` for user review.

## Durable lifecycle

```text
setup
  Planning -> CapturingSource -> Provisioning -> RestoringSource
  -> CreatingBaseline -> Ready

each TaskRun
  capture clean start HEAD/state digest
  -> Pending -> Inspecting -> Committing
  -> Succeeded | NotNeeded | Failed

explicit cleanup
  Ready -> Finalizing -> Finalized
                    \-> Preserved -> Finalizing
```

Every database edge uses Task version, record revision, idempotency receipt,
and event publication in one SQLite transaction. Git operations are designed
to be repeated after a crash, including commit-before-publication and
Worktree-removal-before-publication windows.

## Safety properties verified

- Direct execution uses a cancellable in-process FIFO plus mandatory
  current-user cross-process Mutex; Git metadata uses a separate common-dir
  Mutex with bounded waits.
- Deterministic Worktree path, branch, marker, ownership nonce, repository
  identity, source journal, baseline, and exact `git worktree list` evidence
  must agree. Collisions fail closed without timestamp suffix or Direct
  fallback.
- Source capture preserves HEAD, branch, index, Worktree files, and stash refs;
  bounded non-ignored untracked files are copied with handle/path/hash checks.
- Checkpoint/finalization uses `git add -A`, disabled hooks, `--no-verify`, the
  repository identity/signing policy, and no empty commit. Signing/identity,
  ignored-file, path/ref, and persistence failures preserve recoverable work.
- Finalization retains a produced result branch and CAS-deletes only a branch
  equal to baseline. It removes the exact clean Worktree without force and
  deletes only deterministic Runtime sidecar files, marker-last.
- Delete requires finalized, delivered, cleanup-eligible evidence, then keeps
  a bounded tombstone that rejects late commands.

## Verification

- Hook: 246 assertions.
- Native: TaskCore 80/80, TaskExecution 27/27, TaskGit 68/68,
  TaskRuntimeProtocol 71/71, TaskRuntimeActivation 66/66,
  TaskRuntimeClient 132/132, TaskRuntime 194/194, plus the complete TerminalApp
  and Settings selections.
- Soak: 10 cross-process contention rounds (40 child processes), five
  checkpoint concurrency rounds, and nine restart/reopen lifecycle runs.
- Unified incremental Release/MSIX, workspace CLI smoke, and portable
  `-SkipBuild -NoLaunch` all passed.

Artifacts:

- MSIX: 8,127,686 bytes,
  SHA256 `36867FDDFB9522DCA0E041B2E8F84477B7671F31527BD51AB5DED7E6A7D539A2`.
- Portable ZIP: 12,009,951 bytes,
  SHA256 `9E87BB36B998C9729BFA4162786E97923195AEA2D8776B2078CA5EFDDECA103D`.

## Operational closure

All validation used the unified incremental Release graph with `/m:1` and
`CL_MPCount=1`. Build caches were preserved. Memory was checked before every
build/package gate and remained above 19 GiB / 62%. No user application was
terminated, restarted, or launched during M3 closure. Portable packaging used
`-NoLaunch`.
