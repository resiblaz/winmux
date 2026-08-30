# Task Runtime host foundation

This directory contains the Windows host boundary for the per-user Task
Runtime. The current M2B slice deliberately stops before accepting a client or
routing Task commands.

Implemented foundations:

- stable identity from normalized product channel, settings root, and current
  user SID;
- distinct protocol identity, Global instance/launch mutex names, and control
  pipe name per Runtime identity;
- protected, non-inheritable security descriptors granting access only to the
  owning user and Local System, with a medium-integrity no-write-up label;
- RAII single-instance and overlapped byte-mode named-pipe server handles;
- remote named-pipe clients rejected at the kernel pipe mode;
- `TaskRuntime.exe --bootstrap-check` composition/smoke entry point.

`--bootstrap-check` creates and validates the identity, security, mutex, and
first control-pipe instance, prints the protocol identity, and exits. It is not
the long-running Runtime mode. M2C will add control connection acceptance,
handshake/session state, bounded async I/O, and client request routing without
changing these identity or ACL contracts.
