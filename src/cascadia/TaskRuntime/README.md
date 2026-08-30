# Task Runtime host foundation

This directory contains the Windows host boundary for the per-user Task
Runtime. The current M2C2a slice accepts one control client and completes the
Hello handshake, but deliberately stops before multi-client serving or routing
Task commands.

Implemented foundations:

- stable identity from normalized product channel, settings root, and current
  user SID;
- distinct protocol identity, Global instance/launch mutex names, and control
  pipe name per Runtime identity;
- protected, non-inheritable security descriptors granting access only to the
  owning user and Local System, with a medium-integrity no-write-up label;
- RAII single-instance and overlapped byte-mode named-pipe server handles;
- remote named-pipe clients rejected at the kernel pipe mode;
- bounded overlapped accept, connect, read, and write operations with
  cancellation, disconnect classification, and fail-closed frame decoding;
- client/server Hello exchange with Runtime identity, protocol, capability,
  and negotiated payload-limit validation;
- `TaskRuntime.exe --bootstrap-check` composition/smoke entry point.

`--bootstrap-check` creates and validates the identity, security, mutex, and
first control-pipe instance, prints the protocol identity, and exits. It is not
the long-running Runtime mode. M2C2b will add the multi-client acceptor,
Hello-first session driver, per-client queue isolation, and an RPC dispatcher
seam without changing these identity, ACL, or channel contracts.
