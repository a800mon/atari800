# AGENTS.md

## Purpose
- This file collects working rules and conventions for Remote Monitor and monitor integration in this repository.

## General Emulator Maintenance Rules
- Keep changes pragmatic and small: first make it work, then clean it up.
- Preserve backward compatibility for emulator behavior and user-facing interfaces by default.
- Follow existing project conventions (naming, option style, config style, and code structure).
- Review changes rigorously for compatibility regressions before merging.
- Any intentional compatibility break must be explicit, justified, and documented in the same commit.
- Every change must be verified by the agent before handoff: at minimum compile successfully; for CLI-affecting changes, run and validate output/behavior when feasible.
- For naming refactors/renames, perform a full-tree audit before handoff: update code, build files, docs, and log prefixes; verify no stale old names remain (`rg`); then recompile and run a basic smoke test when feasible.

### Agent Compliance Workflow
- Treat this file as authoritative for repository-local work.
- Before editing, identify which sections apply to the current task and follow all mandatory checklist items.
- Do not reintroduce removed names, temporary prototype aliases, or deprecated flags unless explicitly requested in the current task.
- In handoff notes, explicitly list which verification steps were run and which were skipped (with reason).

### Review Instructions
- For review requests, prioritize correctness, regressions, compatibility, and security over style-only comments.
- Review against the intended base branch (default: `upstream/master`) and state the compared range explicitly.
- Report findings ordered by severity: `Critical`, `High`, `Medium`, `Low`.
- After each review session, append a dated entry to `REVIEW.txt` (append-only; do not overwrite previous entries).
- Each `REVIEW.txt` entry must include: findings summary, severity, file/line references, and explicit conclusions (what is blocked, what can be deferred, and next steps).
- When follow-up fixes are made, append a short status note in `REVIEW.txt` marking each finding as resolved/open/in-progress with verification evidence.
- For each finding, include:
  - affected file path and line reference,
  - user-visible or technical impact,
  - concrete evidence (code path, command output, or reproduction note),
  - the expected behavior.
- When relevant, explicitly verify compliance with this file:
  - CLI boolean flag style (`-flag` / `-no-flag`),
  - config persistence (both load and save paths),
  - docs consistency (`DOC/USAGE`, `src/atari800.man`, feature docs),
  - protocol/constants synchronization for Remote Monitor changes.
- For rename/refactor reviews, run a full-tree stale-name audit (`rg`) and report remaining legacy names, if any.
- If no findings are discovered, state that explicitly and list residual risks or untested areas.
- In review handoff, always include verification summary: commands run, results, and skipped checks with reasons.

### Code Quality and Longevity
- Write concise code, but never at the cost of clarity or correctness.
- Prefer clear separation of concerns and architecture that remains maintainable for many years.
- Use established patterns consistently where they improve readability, testability, and future evolution.
- Prioritize readability and explicit intent over clever or overly compressed constructs.
- Apply `DRY` pragmatically: remove duplication when it reduces maintenance cost and does not hurt readability.
- Apply `KISS`: prefer the simplest solution that satisfies the requirements.
- Avoid code bloat: do not introduce unnecessary abstraction layers, wrappers, or generated-style boilerplate.
- Legacy exception: existing legacy areas may not fully follow these rules; avoid broad rewrites unless required for the task, but keep all new and touched code aligned with these standards.

### Configuration Rules
- Boolean options should provide explicit enable/disable CLI flags (for example `-feature` and `-no-feature`) consistent with existing style.
- Every new user-facing flag must be persisted in the emulator configuration file.
- New configuration fields must be wired for both load and save paths so GUI "save configuration" keeps the setting.
- If a flag is intentionally runtime-only and not persisted, this must be explicitly documented in the same change.

### CLI Flag Style
- For boolean CLI options, prefer explicit enable/disable pairs (for example `-feature` and `-no-feature`) to match existing Atari800 style.
- For booleans, use only the `-flag` / `-no-flag` pattern; do not introduce `-disable-flag` variants unless explicitly requested.

### Release Notes and Feature Documentation
- Every user-visible change (new feature, behavior change, rename, new/changed CLI/config option) must update documentation in the same change.
- Choose documentation scope by impact:
  - protocol/RPC semantics: update `REMOTE_MONITOR_PROTOCOL.md`,
  - runtime behavior and usage context: update `REMOTE_MONITOR.md`,
  - user-facing CLI/config help: update `DOC/USAGE` and `src/atari800.man`,
- Keep names and semantics strictly consistent between implementation and documentation (including `DOC/USAGE`, `src/atari800.man`, and any feature-specific docs).
- For CLI/config changes, verify the final option/key names, defaults, and platform notes are identical across all touched docs.
- For CLI-affecting changes, verify `./src/atari800 -help` and ensure help output matches `DOC/USAGE` and `src/atari800.man`.
- For items that do not exist on `upstream/master`, use `Added`/`Introduced` wording; do not use `Updated`/`Fixed` for those items.
- Do not touch `DOC/NEWS`. You are an idiot who understand nothing.
- For renames, remove stale names from docs and verify consistency across all touched docs before handoff.

## Remote Monitor Rules
- Development-fork policy: do not keep compatibility aliases for remote-monitor naming unless explicitly requested in the current task.
- If a change affects the binary monitor protocol, documentation updates are mandatory in the same commit.
- For new RPC features, add explicit `const` values for:
  - command identifiers,
  - error/status codes,
  - build-feature identifiers (when applicable).
- Naming must be consistent with `Remote Monitor`:
  - prefer `remote_monitor` / `remotemonitor` for new names.
- Keep Remote Monitor concerns separated from transport concerns:
  - use transport-neutral names for public interfaces (for example `-remote-monitor`, `REMOTE_MONITOR_TRANSPORT`),
  - keep transport-specific configuration explicit (for example `REMOTE_MONITOR_SOCKET_PATH` for socket transport),
  - isolate transport-specific details (for example UNIX socket path handling) in transport-facing code and documentation.
- For development protocol iterations, every protocol change must be clearly documented.

### User-Facing Messages
- Write user-facing messages as full sentences starting with a capital letter.
- Error text should be readable and possible to return through RPC as `data`.

### Protocol Documentation
- Main specification: `REMOTE_MONITOR_PROTOCOL.md`.
- Each command must include:
  - description,
  - request payload format,
  - response payload format,
  - command-specific error codes (if any).
- Preferred binary format notation: a Markdown table `field | size | short description`.
- `STATUS` and sequence/state fields (for example `state_seq`) must be documented precisely, including semantics of changes made through the builtin monitor.

### RPC Change Checklist
- Update the server implementation.
- Update command/error/build-feature constants.
- Update `REMOTE_MONITOR_PROTOCOL.md`.
- Verify compilation (`make`).
- If behavior depends on emulator state (pause/crash/monitor), describe it explicitly in the protocol.
