# Atari800 Socket RPC Protocol (Binary)

This document describes the UNIX socket binary RPC protocol implemented in `src/socketserver.c`.

## Transport

- Transport: UNIX domain stream socket (`AF_UNIX`, `SOCK_STREAM`)
- Enable in emulator: `-socket <path>`
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

- Max payload/data bytes: `4096` (`SOCKET_SERVER_MAX_PAYLOAD`)
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

Effect: enters monitor (headless) if not active.

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

Effect: monitor action `STEP` (enters headless monitor if needed).

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

Effect: monitor action `GF` (step frame).

---

### `10` `STATUS`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Payload must be empty. |

Response `OK` data (21 bytes):

| Field | Size | Description |
| --- | --- | --- |
| `flags` | `u8` | State flags (see bit table below). |
| `emulation_ms` | `u64` | Total emulation time in milliseconds. |
| `since_reset_ms` | `u64` | Emulation time since last reset in milliseconds. |
| `state_seq` | `u32` | Monotonic state-change counter. |

`flags` bits:

| Bit | Meaning |
| --- | --- |
| `0` | Paused / monitor active. |
| `7` | Crash screen active (`The Atari computer has crashed`) when available. |

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

Response `OK` clause (repeated `clause_count` times):

| Field | Size | Description |
| --- | --- | --- |
| `cond_count` | `u8` | Number of conditions in clause (AND). |
| `reserved` | `u8` | `0`. |
| `conditions` | `cond_count * 6` | Conditions in the same format as `BP_ADD_CLAUSE`. |

Note: this binary API covers comparison conditions (`PC/A/X/Y/S/READ/WRITE/ACCESS/MEM`). Legacy monitor-only condition forms like `SETFLAG`/`CLRFLAG` are not part of the payload format.

---

### `27` `CONFIG`

Request payload:

| Field | Size | Description |
| --- | --- | --- |
| (none) | `0` | Empty. |

Response `OK` data header:

| Field | Size | Description |
| --- | --- | --- |
| `count` | `u16` | Number of capability ids that follow. |

Response `OK` capability item (repeated `count` times):

| Field | Size | Description |
| --- | --- | --- |
| `cap_id` | `u16` | Capability id present in current binary build. |

Capability constants:

| `cap_id` | Const name | Meaning |
| --- | --- | --- |
| `0x0001` | `VIDEO_SDL2` | Built with SDL2 video backend (`SDL2`). |
| `0x0002` | `VIDEO_SDL` | Built with SDL1 video backend (`SDL`). |
| `0x0003` | `SOUND` | Sound support enabled (`SOUND`). |
| `0x0004` | `SOUND_CALLBACK` | Callback-driven sound backend (`SOUND_CALLBACK`). |
| `0x0005` | `AUDIO_RECORDING` | Audio recording support (`AUDIO_RECORDING`). |
| `0x0006` | `VIDEO_RECORDING` | Video recording support (`VIDEO_RECORDING`). |
| `0x0007` | `MONITOR_BREAK` | Code breakpoints/history enabled (`MONITOR_BREAK`). |
| `0x0008` | `MONITOR_BREAKPOINTS` | User breakpoint table enabled (`MONITOR_BREAKPOINTS`). |
| `0x0009` | `MONITOR_READLINE` | Readline monitor support (`MONITOR_READLINE`). |
| `0x000A` | `MONITOR_HINTS` | Disassembler label hints (`MONITOR_HINTS`). |
| `0x000B` | `MONITOR_UTF8` | UTF-8 monitor output (`MONITOR_UTF8`). |
| `0x000C` | `MONITOR_ANSI` | ANSI terminal monitor output (`MONITOR_ANSI`). |
| `0x000D` | `MONITOR_ASSEMBLER` | Monitor assembler command support (`MONITOR_ASSEMBLER`). |
| `0x000E` | `MONITOR_PROFILE` | Monitor profiling/coverage support (`MONITOR_PROFILE`). |
| `0x000F` | `MONITOR_TRACE` | Monitor TRACE command support (`MONITOR_TRACE`). |
| `0x0010` | `NETSIO` | NetSIO/FujiNet emulation enabled (`NETSIO`). |
| `0x0011` | `IDE` | IDE emulation enabled (`IDE`). |
| `0x0012` | `R_IO_DEVICE` | R: device support enabled (`R_IO_DEVICE`). |
| `0x0013` | `PBI_BB` | Black Box emulation enabled (`PBI_BB`). |
| `0x0014` | `PBI_MIO` | MIO emulation enabled (`PBI_MIO`). |
| `0x0015` | `PBI_PROTO80` | Prototype80 emulation enabled (`PBI_PROTO80`). |
| `0x0016` | `PBI_XLD` | 1400XL/1450XLD emulation enabled (`PBI_XLD`). |
| `0x0017` | `VOICEBOX` | VoiceBox emulation enabled (`VOICEBOX`). |
| `0x0018` | `AF80` | AF80 card emulation enabled (`AF80`). |
| `0x0019` | `BIT3` | BIT3 card emulation enabled (`BIT3`). |
| `0x001A` | `XEP80_EMULATION` | XEP80 emulation enabled (`XEP80_EMULATION`). |
| `0x001B` | `NTSC_FILTER` | NTSC filter enabled (`NTSC_FILTER`). |
| `0x001C` | `PAL_BLENDING` | PAL blending enabled (`PAL_BLENDING`). |
| `0x001D` | `CRASH_MENU` | Crash menu support enabled (`CRASH_MENU`). |
| `0x001E` | `NEW_CYCLE_EXACT` | New cycle-exact core enabled (`NEW_CYCLE_EXACT`). |
| `0x001F` | `HAVE_LIBPNG` | PNG library support present (`HAVE_LIBPNG`). |
| `0x0020` | `HAVE_LIBZ` | Zlib support present (`HAVE_LIBZ`). |

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
- Available in current POSIX socket-server builds.
- If restart setup is unavailable, command returns non-zero status with error text.

## `state_seq` Semantics

`state_seq` is a monotonic `u32` included in `STATUS`.

It increments when emulator state is changed by socket commands and builtin monitor operations (for example memory writes, media changes, resets). Clients can poll `STATUS` and refresh when `state_seq` changes.

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
flags, emu_ms, reset_ms, state_seq = struct.unpack("<BQQI", data)
paused = bool(flags & 0x01)
crashed = bool(flags & 0x80)
```

### `CONFIG`

```python
status, data = rpc.call(Command.CONFIG, b"")
count = struct.unpack_from("<H", data, 0)[0]
caps = [struct.unpack_from("<H", data, 2 + i * 2)[0] for i in range(count)]
```
