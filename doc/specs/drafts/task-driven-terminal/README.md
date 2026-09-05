# Task-driven terminal design

Status: product and architecture baseline accepted; staged native
implementation authorized and active since 2026-08-30.

This directory is the design source of truth for the task-driven terminal
feature:

- [Product and interaction specification](product-interaction-spec.md) records
  confirmed requirements, working assumptions, state semantics, ownership
  rules, and unresolved product decisions.
- [Architecture decision register](architecture-decision-register.md) records
  the technical decisions that must be resolved before implementation and the
  evidence each decision must include.
- [Multica interaction reference](multica-interaction-reference.md) records how
  Multica actually handles each unclear interaction, where that behavior is
  supported in the local reference checkout, and which adaptation still needs
  user confirmation.
- [Implementation plan](implementation-plan.md) defines the accepted first
  native slice, component placement, milestone order, current progress, and
  verification gates.
- [Custom Agent proposal](../custom-agents/README.md) defines reusable Agent
  profiles, versioned Task execution configuration, and a staged native design
  informed by Multica. It is a proposal, not an implemented or accepted milestone.
- [M6 closure](m6-closure-report.md), [M7 closure](m7-closure-report.md), and
  [M8 closure](m8-closure-report.md) record the final Surface, orchestration,
  packaging, and verification evidence. [M8 release notes](m8-release-notes.md)
  cover migration, rollback, security/privacy, and upstream sync.
- The browser prototype under
  [`doc/prototypes/task-page`](../../../prototypes/task-page/README.md) is a
  non-normative interaction aid. When it disagrees with the specification, the
  specification wins.

## Development gate status

The development gate was satisfied on 2026-08-30: all product decisions and
blocking ADRs are accepted, the browser prototype was reviewed, the native
Tasks fixed-page skeleton was reviewed, and upstream hotspots remain bounded.
The following conditions remain mandatory for every later milestone:

1. Every product decision marked `OPEN` in the product specification is either
   accepted or explicitly deferred.
2. The architecture decision register has an accepted answer for every
   decision marked `BLOCKING`.
3. State machines, persistence ownership, shutdown recovery, and Surface close
   policy have testable contracts.
4. The browser prototype has been updated to the accepted interactions and
   reviewed by the user.
5. The implementation plan identifies Microsoft Terminal upstream hotspots and
   minimizes edits to them.
6. Component/dependency boundaries in ADR-TASK-031 are reflected in the change
   plan; no milestone introduces a catch-all Task/Runtime/UI file.

Current implementation position:

- M0 design baseline: complete;
- M1-M3 domain, Runtime, transport, and Git foundations: complete;
- M4 Codex App Server adapter and shared Runtime Provider: complete;
- M5 Runtime-backed fixed Tasks page and projection coordinator: complete;
- M6 task-owned Surface and close policy: complete;
- M7 background orchestration, lifecycle intents, retries, and resource
  suspension: complete;
- M8 protocol drain, packaging hardening, documentation, and release review:
  complete.

The integration baseline includes M0-M8 and the subsequent Task fixes and
[architecture hardening](architecture-hardening-2026-09-05.md). Further feature
designs and implementations use dedicated branches from `main`.

## Reference-first rule

When an interaction is unclear:

1. inspect the pinned local Multica checkout and its tests/documentation;
2. record Multica's actual behavior in the reference matrix;
3. explain where this terminal product has different constraints;
4. ask the user to confirm the adaptation;
5. only then promote the item from `OPEN` to `CONFIRMED`.

Multica behavior is evidence, not an automatic product decision.

Multica is available locally at `.references/multica` as an ignored,
read-only architecture reference. Its concepts may inform this design, but its
source and UI assets must not be copied into this MIT-licensed fork.
