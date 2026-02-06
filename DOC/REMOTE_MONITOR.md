# Remote Monitor

Remote Monitor is a remote debugger interface for Atari800. It lets an external
client control pause/continue/step and inspect emulator state using the binary
RPC protocol documented in `REMOTE_MONITOR_PROTOCOL.md`.

## What It Changes

Remote Monitor affects how debugging is entered and how it interacts with the
built-in monitor:

- With no remote client connected, emulator behavior remains local:
  - `Ctrl+C` and `F8` open the built-in monitor as usual.
- With a remote client connected and built-in monitor disabled:
  - debug flow is handled remotely,
  - `Ctrl+C` terminates emulator instead of entering local monitor.

This keeps normal local debugging available when no remote debugger is attached,
while allowing remote-enabled behavior during an active remote session.

## Command-Line Options

- `-remote-monitor`
  - Enable Remote Monitor using default transport settings.
- `-no-remote-monitor`
  - Disable Remote Monitor.
- `-remote-monitor-transport <name>`
  - Select transport (currently: `socket`).
- `-remote-monitor-socket-path <path>`
  - Set UNIX socket path for socket transport.
  - `DEFAULT` uses platform default path when available.
- `-remote-monitor-audio-on-debug`
  - Keep audio enabled while debugging (default).
- `-no-remote-monitor-audio-on-debug`
  - Disable audio while debugging.

## Configuration

Remote Monitor can be enabled and configured without command-line flags:

- `REMOTE_MONITOR=1` enables Remote Monitor (required to activate it).
- `REMOTE_MONITOR=0` disables Remote Monitor.
- `REMOTE_MONITOR_TRANSPORT` (configuration only; does not enable on its own).
- `REMOTE_MONITOR_SOCKET_PATH` (configuration only; does not enable on its own).
- `REMOTE_MONITOR_AUDIO_ON_DEBUG`

These values are read on startup and written by configuration save, so GUI
"Save configuration" persists them.

## Built-In Monitor Interaction

- Built-in monitor is still used for local debugging.
- Remote Monitor and built-in monitor are runtime modes of one debugging system.
- In remote-enabled monitor mode, `Ctrl+C` while paused exits immediately.
- During `GF` execution, monitor break traps are deferred until command handling
  returns at the next VBL boundary.
- If a remote monitor client disconnects during remote-enabled monitor I/O:
  - without pending `SIGINT`: monitor loop exits back to emulation flow,
  - with pending `SIGINT`: emulator terminates.

## Audio During Debug

- `REMOTE_MONITOR_AUDIO_ON_DEBUG=1`: audio may stay active during remote debug refresh commands.
- `REMOTE_MONITOR_AUDIO_ON_DEBUG=0`: audio stays paused in debug flow to avoid unwanted output
  during step/frame refresh.

## Socket Transport Notes

- Current transport implementation is UNIX socket.
- On Linux, default socket path is `/tmp/atari.sock`.
- If the socket path already exists, Remote Monitor initialization fails.
  Remove stale socket files manually or use a different path.
- Atari800 removes only the socket file created by the current process.
- Socket setup failures (bind/stat/socket/listen/chmod/path) are reported once
  and repeated retry spam is suppressed until full close/reset.

## Platform Support

- Remote Monitor is available on POSIX-like builds with UNIX socket support.
- On unsupported platforms, remote-monitor options are reported as unsupported.
