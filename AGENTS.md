# AGENTS.md

## Purpose
- This file collects working rules and conventions for socket RPC and monitor development in this repository.

## Implementation Rules
- Keep changes pragmatic and small: first make it work, then clean it up.
- If a change affects the socket protocol, documentation updates are mandatory in the same commit.
- For new RPC features, add explicit `const` values for:
  - command identifiers,
  - error/status codes,
  - capabilities (when applicable).
- Naming must be consistent with `socket server`:
  - prefer `socketserver` / `socket_server`,
  - avoid historical names like `cmdsocket`, `cmdsocket-bin`.
- Backward compatibility is not required at all costs for the development version, but every protocol change must be clearly documented.

## User-Facing Messages
- Write user-facing messages as full sentences starting with a capital letter.
- Error text should be readable and possible to return through RPC as `data`.

## Protocol Documentation
- Main specification: `SOCKET_PROTOCOL.md`.
- Each command must include:
  - description,
  - request payload format,
  - response payload format,
  - command-specific error codes (if any).
- Preferred binary format notation: a Markdown table `field | size | short description`.
- `STATUS` and sequence/state fields (for example `state_seq`) must be documented precisely, including semantics of changes made through the builtin monitor.

## RPC Change Checklist
- Update the server implementation.
- Update command/error/capability constants.
- Update `SOCKET_PROTOCOL.md`.
- Verify compilation (`make`).
- If behavior depends on emulator state (pause/crash/monitor), describe it explicitly in the protocol.
