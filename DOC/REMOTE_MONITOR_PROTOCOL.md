# Atari800 Remote Monitor Binary Protocol (RPC)

This document describes the Remote Monitor binary RPC protocol implemented in `src/remotemonitor.c`.

## Transport

- Transport: UNIX domain stream socket (`AF_UNIX`, `SOCK_STREAM`)
- Enable in emulator:
  - `-remote-monitor`
  - `-remote-monitor-transport socket`
  - `-remote-monitor-socket-path <path>` (optional; on Linux default is `/tmp/atari.sock`)
  - config: `REMOTE_MONITOR=1`, `REMOTE_MONITOR_TRANSPORT=socket`, `REMOTE_MONITOR_SOCKET_PATH=<path>`
- Multiple clients supported: up to 8
- Model: request/response (no unsolicited server messages)

## Endianness

All multi-byte integers are little-endian.

## Frame Format

### Request Frame (client -> server)

| Field | Size | Description |
| --- | --- | --- |
| `cmd` | `u8` | Command identifier. |
| `payload_len` | `u16` | Number of bytes in `payload`. |
| `payload` | `payload_len` | Command-specific bytes. |

### Response Frame (server -> client)

| Field | Size | Description |
| --- | --- | --- |
| `status` | `u8` | `0` on success, non-zero on error. |
| `data_len` | `u16` | Number of bytes in `data`. |
| `data` | `data_len` | Command-specific success data or error text. |

## Limits and Behavior

- Max payload/data bytes: `4096` (`REMOTE_MONITOR_MAX_PAYLOAD`)
- If declared frame size exceeds internal input buffer, server closes client connection
- Queued `STATUS` requests are coalesced: only newest pending `STATUS` is handled
- Server send path is non-blocking; send failure closes client connection

## Status Codes

| Code | Name | Description |
| --- | --- | --- |
| `0` | `OK` | Success. |
| `1` | `GENERIC` | Generic failure. |
| `2` | `INVALID_LENGTH` | Invalid payload length/shape. |
| `3` | `INVALID_VALUE` | Invalid payload value. |
| `4` | `PAYLOAD_TOO_LARGE` | Requested/produced payload too large. |
| `5` | `FILE_NOT_FOUND` | File path does not exist. |
| `6` | `FILE_OPEN_FAILED` | File open/stat error. |
| `7` | `FILE_RUN_FAILED` | File detected but execution failed. |
| `8` | `UNSUPPORTED_FILE` | File type unsupported. |
| `9` | `UNKNOWN_COMMAND` | Unknown command id. |

Error responses can include human-readable text in `data`.

## Commands

### `1` `PING`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `text` | `4` | ASCII `PONG`. |

---

### `2` `DLIST_PTR`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `dlist_ptr` | `u16` | Current ANTIC display list pointer. |

---

### `3` `READ_MEM`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| `addr` | `u16` | Start address. |
| `count` | `u16` | Number of bytes to read. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `bytes` | `count` | Memory bytes from `addr` (`MEMORY_SafeGetByte`). |

---

### `4` `DLIST_DUMP`

Request payload (variant A):

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Start at current display list pointer. |

Request payload (variant B):

| Field | Size | Description |
| --- | --- | --- |
| `start_addr` | `u16` | Start at provided display list address. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `dlist_raw` | variable | Raw display list bytes emitted by parser. |

---

### `5` `CPU_STATE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data (11 bytes):

| Field | Size | Description |
| --- | --- | --- |
| `ypos` | `u16` | Current scanline. |
| `xpos` | `u16` | Current cycle/position. |
| `pc` | `u16` | CPU PC register. |
| `a` | `u8` | CPU A register. |
| `x` | `u8` | CPU X register. |
| `y` | `u8` | CPU Y register. |
| `s` | `u8` | CPU stack pointer. |
| `p` | `u8` | CPU status flags. |

---

### `6` `PAUSE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Effect: enters monitor (remote-enabled mode) if not active.

---

### `7` `CONTINUE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Effect: monitor action `CONT`.

---

### `8` `STEP`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Effect: monitor action `STEP` (enters remote-enabled monitor if needed).

---

### `9` `STEP_FRAME`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Effect: monitor action `GF` (continue emulation until next VBL boundary, then
re-enter monitor).
Break traps are deferred during this run and become active again after re-entry.

---

### `10` `STATUS`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data (22 bytes):

| Field | Size | Description |
| --- | --- | --- |
| `flags` | `u8` | State flags (pause/crash, see bit table below). |
| `emulation_ms` | `u64` | Total emulation time in milliseconds. |
| `since_reset_ms` | `u64` | Emulation time since last reset in milliseconds. |
| `state_seq` | `u32` | Monotonic state-change counter. |
| `machine_type` | `u8` | Current emulated machine type id (see constants below). |

`flags` bits:

| Bit | Meaning |
| --- | --- |
| `0` | Paused / monitor active. |
| `7` | Crash screen active (`The Atari computer has crashed`) when available. |

`flags` constants:

| Value | Name | Meaning |
| --- | --- | --- |
| `0x01` | `STATUS_FLAG_PAUSED` | Pause/monitor-active bit. |
| `0x80` | `STATUS_FLAG_CRASHED` | Crash-screen-active bit. |

`machine_type` constants:

| Value | Name | Meaning |
| --- | --- | --- |
| `0` | `STATUS_MACHINE_ATARI_800` | Atari 800 (48 KB or RAM-expanded 400/800 configurations). |
| `1` | `STATUS_MACHINE_XL_XE` | Custom XL/XE configuration without a standard profile match. |
| `2` | `STATUS_MACHINE_ATARI_5200` | Atari 5200 machine family. |
| `3` | `STATUS_MACHINE_ATARI_1200XL` | Atari 1200XL (64 KB). |
| `4` | `STATUS_MACHINE_ATARI_800XL` | Atari 800XL (64 KB). |
| `5` | `STATUS_MACHINE_ATARI_130XE` | Atari 130XE (128 KB). |
| `6` | `STATUS_MACHINE_ATARI_320XE_COMPY_SHOP` | Atari 320XE (Compy-Shop). |
| `7` | `STATUS_MACHINE_ATARI_320XE_RAMBO` | Atari 320XE (Rambo XL). |
| `8` | `STATUS_MACHINE_ATARI_576XE` | Atari 576XE. |
| `9` | `STATUS_MACHINE_ATARI_1088XE` | Atari 1088XE. |
| `10` | `STATUS_MACHINE_ATARI_XEGS` | Atari XEGS. |
| `11` | `STATUS_MACHINE_ATARI_400` | Atari 400 (16 KB). |
| `12` | `STATUS_MACHINE_ATARI_600XL` | Atari 600XL (16 KB). |

Note: XL/XE machine types are inferred from RAM size and built-in feature flags; unmatched XL/XE configurations return `STATUS_MACHINE_XL_XE`.

---

### `11` `READ_MEMV`

Request payload header:

| Field | Size | Description |
| --- | --- | --- |
| `count` | `u16` | Number of descriptors. |

Request payload descriptor (repeated `count` times):

| Field | Size | Description |
| --- | --- | --- |
| `addr` | `u16` | Start address for block. |
| `len` | `u16` | Number of bytes for block. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `concat_bytes` | variable | Concatenated blocks in descriptor order. |

---

### `12` `RUN`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| `path_bytes` | `1..FILENAME_MAX-1` | File path bytes (trailing spaces/`\0` trimmed; quoted path unwrapped). |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Command-specific error codes:

| Code | Name | Condition |
| --- | --- | --- |
| `2` | `INVALID_LENGTH` | Payload length is outside `1..FILENAME_MAX-1`. |
| `3` | `INVALID_VALUE` | Path becomes empty after trimming/unquoting. |
| `5` | `FILE_NOT_FOUND` | Target path does not exist. |
| `6` | `FILE_OPEN_FAILED` | File exists but cannot be opened/stat'ed. |
| `7` | `FILE_RUN_FAILED` | Runnable file detected but execution failed. |
| `8` | `UNSUPPORTED_FILE` | File exists but type is unsupported. |

---

### `13` `COLDSTART`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

---

### `14` `WARMSTART`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

---

### `15` `REMOVE_CARTRIDGE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

---

### `16` `STOP_EMULATOR`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Effect:
- UI active: schedules exit
- UI inactive: exits process after sending response

---

### `17` `REMOVE_TAPE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

---

### `18` `REMOVE_DISKS`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Effect: dismounts all SIO drives (`1..SIO_MAX_DRIVES`).

---

### `19` `HISTORY`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data header:

| Field | Size | Description |
| --- | --- | --- |
| `count` | `u8` | Number of history entries (`CPU_REMEMBER_PC_STEPS`). |

Response `OK` entry (repeated `count` times):

| Field | Size | Description |
| --- | --- | --- |
| `ypos_hi` | `u8` | High byte of packed scanline component. |
| `xpos_lo` | `u8` | Low byte of packed scan position component. |
| `pc` | `u16` | Program counter for entry. |
| `op0` | `u8` | Opcode byte 0. |
| `op1` | `u8` | Opcode byte 1. |
| `op2` | `u8` | Opcode byte 2. |

Order: newest entry first.

---

### `20` `BUILTIN_MONITOR`

Request payload variant A:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Enable builtin monitor and request monitor entry. |

Request payload variant B:

| Field | Size | Description |
| --- | --- | --- |
| `enabled` | `u8` | `0` disable, `1` enable. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `enabled` | `u8` | Current builtin monitor flag (`0` or `1`). |

---

### `21` `WRITE_MEMORY`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| `addr` | `u16` | Start address. |
| `length` | `u16` | Number of bytes to write. |
| `bytes` | `length` | Data bytes to store. |

Validation:

| Rule | Description |
| --- | --- |
| Length match | `payload_len` must be exactly `4 + length`. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Write semantics: debug-style write path (same intent as monitor `C`), including hardware-map writes and monitor-style ROM patch behavior.

---

### `22` `BP_CLEAR`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Clears user breakpoint table. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

---

### `23` `BP_ADD_CLAUSE`

Adds one `AND` clause. Multiple clauses are connected by `OR`.

Request payload header:

| Field | Size | Description |
| --- | --- | --- |
| `insert_index` | `u16` | Clause insertion index (`0xFFFF` = append). |
| `cond_count` | `u8` | Number of conditions in this clause (`1..20`). |
| `reserved` | `u8` | Must be `0`. |

Request payload condition (repeated `cond_count` times):

| Field | Size | Description |
| --- | --- | --- |
| `type` | `u8` | Condition type (see table below). |
| `op` | `u8` | Comparison operator (see table below). |
| `addr` | `u16` | Used only for `type=MEM`, otherwise `0`. |
| `value` | `u16` | Comparison value. |

`type` values:

| Value | Name |
| --- | --- |
| `1` | `PC` |
| `2` | `A` |
| `3` | `X` |
| `4` | `Y` |
| `5` | `S` |
| `6` | `READ` |
| `7` | `WRITE` |
| `8` | `ACCESS` |
| `9` | `MEM` |

`op` values:

| Value | Operator |
| --- | --- |
| `1` | `<` |
| `2` | `<=` |
| `3` | `==` |
| `4` | `!=` |
| `5` | `>=` |
| `6` | `>` |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `clause_index` | `u16` | Final index of inserted clause. |

---

### `24` `BP_DELETE_CLAUSE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| `clause_index` | `u16` | Clause index to remove. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

---

### `25` `BP_SET_ENABLED`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| `enabled` | `u8` | `0` disable all user breakpoints, `1` enable. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `enabled` | `u8` | Current global enabled flag. |

---

### `26` `BP_LIST`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Response `OK` data header:

| Field | Size | Description |
| --- | --- | --- |
| `enabled` | `u8` | Global user-breakpoint enabled flag. |
| `clause_count` | `u16` | Number of OR clauses. |

Response `OR` clause (repeated `clause_count` times):

| Field | Size | Description |
| --- | --- | --- |
| `cond_count` | `u8` | Number of conditions in clause (AND). |
| `reserved` | `u8` | `0`. |
| `conditions` | `cond_count * 6` | Conditions in the same format as `BP_ADD_CLAUSE`. |

Note: this binary API covers comparison conditions (`PC/A/X/Y/S/READ/WRITE/ACCESS/MEM`). Legacy monitor-only condition forms like `SETFLAG`/`CLRFLAG` are not part of the payload format.

---

### `27` `BUILD_FEATURES`

Wire compatibility note:
- This command uses id `27` and keeps the same payload/response format as before.
- Older code may refer to this command as `CONFIG`.

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Response `OK` data header:

| Field | Size | Description |
| --- | --- | --- |
| `count` | `u16` | Number of build-feature ids that follow. |

Response `OK` build-feature item (repeated `count` times):

| Field | Size | Description |
| --- | --- | --- |
| `cap_id` | `u16` | Build-feature id present in current binary build. |

Build-feature constants:

| `cap_id` | Const name | Meaning |
| --- | --- | --- |
| `0x0001` | `MONITOR_BREAK` | Code breakpoints/history enabled (`MONITOR_BREAK`). |
| `0x0002` | `MONITOR_BREAKPOINTS` | User breakpoint table enabled (`MONITOR_BREAKPOINTS`). |
| `0x0003` | `EMULATION_ATARI_800` | Atari 400/800 emulation available in this build. |
| `0x0004` | `EMULATION_XE_XL` | Atari XL/XE emulation available in this build. |
| `0x0005` | `EMULATION_ATARI_5200` | Atari 5200 emulation available in this build. |

`BUILD_FEATURES` is intentionally limited to breakpoint and emulation-type capabilities so the protocol can be shared across different emulator families.

---

### `28` `RESTART_EMULATOR`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Effect:
- Restarts the emulator process using the original startup command line (`execvp(argv[0], argv)`).
- Available in current POSIX Remote Monitor builds.
- If restart setup is unavailable, command returns non-zero status with error text.

---

### `29` `GTIA_STATE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `HPOSP0..HPOSP3` | `4 * u8` | Player horizontal positions. |
| `HPOSM0..HPOSM3` | `4 * u8` | Missile horizontal positions. |
| `SIZEP0..SIZEP3` | `4 * u8` | Player size registers. |
| `SIZEM` | `u8` | Missile size register. |
| `GRAFP0..GRAFP3` | `4 * u8` | Player graphics registers. |
| `GRAFM` | `u8` | Missile graphics register. |
| `COLPM0..COLPM3` | `4 * u8` | Player/missile colors. |
| `COLPF0..COLPF3` | `4 * u8` | Playfield colors. |
| `COLBK` | `u8` | Background color. |
| `PRIOR` | `u8` | Priority register. |
| `VDELAY` | `u8` | Vertical delay register. |
| `GRACTL` | `u8` | Graphics control register. |

---

### `30` `ANTIC_STATE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `DMACTL` | `u8` | DMA control register. |
| `CHACTL` | `u8` | Character control register. |
| `DLIST` | `u16` | Display list pointer. |
| `HSCROL` | `u8` | Horizontal scroll register. |
| `VSCROL` | `u8` | Vertical scroll register. |
| `PMBASE` | `u8` | Player/missile base register. |
| `CHBASE` | `u8` | Character base register. |
| `VCOUNT` | `u8` | Current scanline counter register (`safe` read). |
| `NMIEN` | `u8` | NMI enable register. |
| `ypos` | `u16` | Current internal scanline position (`ANTIC_ypos`). |

---

### `31` `CART_STATE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `autoreboot` | `u8` | Cartridge auto-reboot flag (`0`/`1`). |
| `main_present` | `u8` | Main cartridge mounted (`0`/`1`). |
| `main_type` | `s16` | Main cartridge type id (`CARTRIDGE_*`). |
| `main_state` | `u32` | Main cartridge state bits. |
| `main_size_kb` | `u32` | Main cartridge image size in KB. |
| `main_raw` | `u8` | Main cartridge image raw flag (`0`/`1`). |
| `piggy_present` | `u8` | Piggyback cartridge mounted (`0`/`1`). |
| `piggy_type` | `s16` | Piggyback cartridge type id. |
| `piggy_state` | `u32` | Piggyback cartridge state bits. |
| `piggy_size_kb` | `u32` | Piggyback cartridge image size in KB. |
| `piggy_raw` | `u8` | Piggyback cartridge image raw flag (`0`/`1`). |

---

### `32` `JUMPS`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `count` | `u8` | Number of entries (`CPU_REMEMBER_JMP_STEPS`, currently `16`). |
| `pc[count]` | `count * u16` | PCs from jump history in monitor order (oldest to newest ring order used by monitor command). |

---

### `33` `PIA_STATE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `PACTL` | `u8` | PIA port A control. |
| `PBCTL` | `u8` | PIA port B control. |
| `PORTA` | `u8` | PIA port A data. |
| `PORTB` | `u8` | PIA port B data. |

---

### `34` `POKEY_STATE`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `stereo_enabled` | `u8` | `1` when stereo POKEY output is enabled, else `0`. |
| `AUDF1..AUDF4` | `4 * u8` | First-chip frequency divisors. |
| `AUDC1..AUDC4` | `4 * u8` | First-chip channel controls. |
| `AUDCTL1` | `u8` | First-chip AUDCTL. |
| `KBCODE` | `u8` | Keyboard code register snapshot. |
| `IRQEN` | `u8` | IRQ enable register. |
| `IRQST` | `u8` | IRQ status register. |
| `SKSTAT` | `u8` | Serial/keyboard status register. |
| `SKCTL` | `u8` | Serial/keyboard control register. |
| `chip2_data` | `9 * u8` | Present only when `stereo_enabled=1`: `AUDF1..4`, `AUDC1..4`, `AUDCTL2`. |

---

### `35` `STACK`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `s` | `u8` | Current CPU stack pointer (`CPU_regS`). |
| `count` | `u8` | Number of stack bytes included (`0xFF - s`). |
| `entries` | `count * (u8+u8)` | Repeated pairs: stack offset (`01xx` low byte), value byte. |

---

### `36` `STEP_OVER`

Request payload variant A:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Step over at current `PC`. |

Request payload variant B:

| Field | Size | Description |
| --- | --- | --- |
| `pc` | `u16` | Set `PC`, then step over. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Build constraint:
- Requires `MONITOR_BREAK` in the emulator build.

---

### `37` `RUN_UNTIL_RETURN`

Request payload variant A:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Run until return from current `PC`. |

Request payload variant B:

| Field | Size | Description |
| --- | --- | --- |
| `pc` | `u16` | Set `PC`, then run until return. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Build constraint:
- Requires `MONITOR_BREAK` in the emulator build.

---

### `38` `BBRK`

Request payload variant A:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Query current break-on-BRK state. |

Request payload variant B:

| Field | Size | Description |
| --- | --- | --- |
| `enabled` | `u8` | Set break-on-BRK (`0` or `1`). |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `enabled` | `u8` | Current break-on-BRK state. |

Build constraint:
- Requires `MONITOR_BREAK` in the emulator build.

---

### `39` `BLINE`

Request payload variant A:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Query current configured scanline value. |

Request payload variant B:

| Field | Size | Description |
| --- | --- | --- |
| `scanline` | `u16` | Set monitor `ANTIC_break_ypos` value. |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `scanline` | `u16` | Current `ANTIC_break_ypos` value. |
| `mode` | `u8` | `0` disabled/other, `1` break scanline, `2` blink scanline. |

Build constraint:
- Requires either `MONITOR_BREAK` or no `NO_YPOS_BREAK_FLICKER`.

---

### `40` `SYSINFO`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data (5 bytes):

| Field | Size | Description |
| --- | --- | --- |
| `flags` | `u8` | System flags (basic/pal, see bit table below). |
| `machine_family` | `u8` | Current emulated machine family id (see constants below). |
| `os_revision` | `u8` | OS ROM revision id (see constants below). |
| `basic_revision` | `u8` | BASIC ROM revision id (see constants below). |
| `builtin_game_revision` | `u8` | XEGS builtin game ROM revision id (see constants below). |

`flags` bits:

| Bit | Meaning |
| --- | --- |
| `0` | BASIC enabled. |
| `1` | PAL video system active (NTSC when clear). |

`flags` constants:

| Value | Name | Meaning |
| --- | --- | --- |
| `0x01` | `SYSINFO_FLAG_BASIC_ENABLED` | BASIC enabled bit. |
| `0x02` | `SYSINFO_FLAG_TV_PAL` | PAL video system bit. |

`machine_family` constants:

| Value | Name | Meaning |
| --- | --- | --- |
| `0` | `SYSINFO_MACHINE_FAMILY_ATARI_800` | Atari 400/800 family (`Atari800_MACHINE_800`). |
| `1` | `SYSINFO_MACHINE_FAMILY_XL_XE` | Atari XL/XE family (`Atari800_MACHINE_XLXE`). |
| `2` | `SYSINFO_MACHINE_FAMILY_ATARI_5200` | Atari 5200 family (`Atari800_MACHINE_5200`). |

`os_revision` is a single `u8` enum. Values are grouped by machine family:
`0x00..0x1f` (400/800), `0x20..0x3f` (XL/XE), `0x40..0x5f` (5200).
`0xff` indicates no OS ROM selected/loaded. The emulator returns exactly one
value for the active machine (never multiple OS revisions).

400/800 OS revisions:

| Value | Name | Meaning |
| --- | --- | --- |
| `0x00` | `SYSINFO_OS_REVISION_800_A_NTSC` | `-800-rev a-ntsc`. |
| `0x01` | `SYSINFO_OS_REVISION_800_A_PAL` | `-800-rev a-pal`. |
| `0x02` | `SYSINFO_OS_REVISION_800_B_NTSC` | `-800-rev b-ntsc`. |
| `0x03` | `SYSINFO_OS_REVISION_800_CUSTOM` | `-800-rev custom`. |
| `0x04` | `SYSINFO_OS_REVISION_800_ALTIRRA` | `-800-rev altirra`. |

XL/XE OS revisions:

| Value | Name | Meaning |
| --- | --- | --- |
| `0x20` | `SYSINFO_OS_REVISION_XL_10` | `-xl-rev 10`. |
| `0x21` | `SYSINFO_OS_REVISION_XL_11` | `-xl-rev 11`. |
| `0x22` | `SYSINFO_OS_REVISION_XL_1` | `-xl-rev 1`. |
| `0x23` | `SYSINFO_OS_REVISION_XL_2` | `-xl-rev 2`. |
| `0x24` | `SYSINFO_OS_REVISION_XL_3A` | `-xl-rev 3a`. |
| `0x25` | `SYSINFO_OS_REVISION_XL_3B` | `-xl-rev 3b`. |
| `0x26` | `SYSINFO_OS_REVISION_XL_5` | `-xl-rev 5`. |
| `0x27` | `SYSINFO_OS_REVISION_XL_3` | `-xl-rev 3`. |
| `0x28` | `SYSINFO_OS_REVISION_XL_4` | `-xl-rev 4`. |
| `0x29` | `SYSINFO_OS_REVISION_XL_59` | `-xl-rev 59`. |
| `0x2a` | `SYSINFO_OS_REVISION_XL_59A` | `-xl-rev 59a`. |
| `0x2b` | `SYSINFO_OS_REVISION_XL_CUSTOM` | `-xl-rev custom`. |
| `0x2c` | `SYSINFO_OS_REVISION_XL_ALTIRRA` | `-xl-rev altirra`. |

5200 OS revisions:

| Value | Name | Meaning |
| --- | --- | --- |
| `0x40` | `SYSINFO_OS_REVISION_5200_ORIG` | `-5200-rev orig`. |
| `0x41` | `SYSINFO_OS_REVISION_5200_A` | `-5200-rev a`. |
| `0x42` | `SYSINFO_OS_REVISION_5200_CUSTOM` | `-5200-rev custom`. |
| `0x43` | `SYSINFO_OS_REVISION_5200_ALTIRRA` | `-5200-rev altirra`. |

`basic_revision` constants (`0xff` when unavailable):

| Value | Name | Meaning |
| --- | --- | --- |
| `0x00` | `SYSINFO_BASIC_REVISION_A` | `-basic-rev a`. |
| `0x01` | `SYSINFO_BASIC_REVISION_B` | `-basic-rev b`. |
| `0x02` | `SYSINFO_BASIC_REVISION_C` | `-basic-rev c`. |
| `0x03` | `SYSINFO_BASIC_REVISION_CUSTOM` | `-basic-rev custom`. |
| `0x04` | `SYSINFO_BASIC_REVISION_ALTIRRA` | `-basic-rev altirra`. |
| `0xff` | `SYSINFO_BASIC_REVISION_NONE` | No BASIC ROM selected/loaded. |

`builtin_game_revision` constants (`0xff` when unavailable):

| Value | Name | Meaning |
| --- | --- | --- |
| `0x00` | `SYSINFO_BUILTIN_GAME_REVISION_ORIG` | `-xegame-rev orig`. |
| `0x01` | `SYSINFO_BUILTIN_GAME_REVISION_CUSTOM` | `-xegame-rev custom`. |
| `0xff` | `SYSINFO_BUILTIN_GAME_REVISION_NONE` | No XEGS game ROM selected/loaded. |

---

### `41` `SEARCH`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| `mode` | `u8` | Search mode (see below). |
| `start` | `u16` | Search start address. |
| `end` | `u16` | Search end address (inclusive). |
| `pattern_len` | `u8` | Pattern length (`1..255`). |
| `pattern` | `pattern_len` | Pattern bytes. |

Modes:

| Value | Name | Meaning |
| --- | --- | --- |
| `1` | `BYTES` | Raw byte search (`S`). |
| `2` | `ASCII` | ASCII string bytes search (`SSTR`). |
| `3` | `SCREENCODE` | ASCII payload converted to Atari screen-codes before search (`SSCR`). |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| `total_matches` | `u32` | Total number of matches found in range. |
| `returned_count` | `u16` | Number of addresses returned in this payload. |
| `addresses` | `returned_count * u16` | Matched start addresses (truncated to payload limit). |

---

### `42` `SET_REG`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| `target` | `u8` | Register/flag selector (see table). |
| `value` | `u16` | New value (`0/1` for flags). |

`target` values:

| Value | Name | Meaning |
| --- | --- | --- |
| `1` | `PC` | Set `CPU_regPC` (`SETPC`). |
| `2` | `A` | Set accumulator (`SETA`). |
| `3` | `X` | Set index X (`SETX`). |
| `4` | `Y` | Set index Y (`SETY`). |
| `5` | `S` | Set stack pointer (`SETS`). |
| `6` | `N` | Set/clear negative flag (`SETN`). |
| `7` | `V` | Set/clear overflow flag (`SETV`). |
| `8` | `D` | Set/clear decimal flag (`SETD`). |
| `9` | `I` | Set/clear interrupt disable flag (`SETI`). |
| `10` | `Z` | Set/clear zero flag (`SETZ`). |
| `11` | `C` | Set/clear carry flag (`SETC`). |

Response `OK` data:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Validation:
- For flag targets (`N/V/D/I/Z/C`), `value` must be `0` or `1`.

---

## `state_seq` Semantics

`state_seq` is a monotonic `u32` included in `STATUS`.

It increments when emulator state is changed by Remote Monitor commands and builtin monitor operations (for example memory writes, media changes, resets). Clients can poll `STATUS` and refresh when `state_seq` changes.

## Python Examples

### `READ_MEM`

```python
payload = struct.pack("<HH", addr, count)
status, data = rpc.call(Command.READ_MEM, payload)
```

### `WRITE_MEMORY`

```python
buf = bytes([0xA9, 0x00, 0x8D, 0x00, 0xD0])
payload = struct.pack("<HH", addr, len(buf)) + buf
status, data = rpc.call(Command.WRITE_MEMORY, payload)
```

### `STATUS`

```python
flags, emu_ms, reset_ms, state_seq, machine_type = struct.unpack("<BQQIB", data)
paused = bool(flags & 0x01)
crashed = bool(flags & 0x80)
```

### `BUILD_FEATURES`

```python
status, data = rpc.call(Command.BUILD_FEATURES, b"")
count = struct.unpack_from("<H", data, 0)[0]
caps = [struct.unpack_from("<H", data, 2 + i * 2)[0] for i in range(count)]
```
