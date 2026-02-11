#include "config.h"
#include "remotemonitor.h"

#if defined(HAVE_UNISTD_H) && !defined(HAVE_WINDOWS_H)

#include "afile.h"
#include "antic.h"
#include "atari.h"
#include "binload.h"
#include "cartridge.h"
#include "cassette.h"
#include "cpu.h"
#include "log.h"
#include "memory.h"
#include "monitor.h"
#include "sio.h"
#include "ui.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define REMOTE_MONITOR_MAX_CLIENTS 8
#define REMOTE_MONITOR_MAX_PAYLOAD 4096

#if defined(__linux__)
#define REMOTE_MONITOR_DEFAULT_PATH "/tmp/atari.sock"
#endif

enum {
	REMOTE_MONITOR_TRANSPORT_NONE = 0,
	REMOTE_MONITOR_TRANSPORT_SOCKET = 1
};

static int remote_monitor_transport = REMOTE_MONITOR_TRANSPORT_NONE;
static char remote_monitor_socket_path[FILENAME_MAX];
static int remote_monitor_listen_fd = -1;
static int remote_monitor_owned_socket = FALSE;
static char remote_monitor_owned_socket_path[FILENAME_MAX];
static int remote_monitor_clients_ready = FALSE;
static int remote_monitor_init_failed = FALSE;
static uint32_t remote_monitor_state_seq = 0;

struct RemoteMonitorClient {
	int fd;
	unsigned char buf[REMOTE_MONITOR_MAX_PAYLOAD + 16];
	size_t len;
};

static struct RemoteMonitorClient remote_monitor_clients[REMOTE_MONITOR_MAX_CLIENTS];

#define REMOTE_MONITOR_OK 0
/* Error status codes returned in the frame status byte. */
#define REMOTE_MONITOR_ERR_GENERIC 1
#define REMOTE_MONITOR_ERR_INVALID_LENGTH 2
#define REMOTE_MONITOR_ERR_INVALID_VALUE 3
#define REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE 4
#define REMOTE_MONITOR_ERR_FILE_NOT_FOUND 5
#define REMOTE_MONITOR_ERR_FILE_OPEN_FAILED 6
#define REMOTE_MONITOR_ERR_FILE_RUN_FAILED 7
#define REMOTE_MONITOR_ERR_UNSUPPORTED_FILE 8
#define REMOTE_MONITOR_ERR_UNKNOWN_COMMAND 9
#define REMOTE_MONITOR_CMD_PING 1
#define REMOTE_MONITOR_CMD_DLIST_PTR 2
#define REMOTE_MONITOR_CMD_READ_MEM 3
#define REMOTE_MONITOR_CMD_DLIST_DUMP 4
#define REMOTE_MONITOR_CMD_CPU_STATE 5
#define REMOTE_MONITOR_CMD_PAUSE 6
#define REMOTE_MONITOR_CMD_CONTINUE 7
#define REMOTE_MONITOR_CMD_STEP 8
#define REMOTE_MONITOR_CMD_STEP_FRAME 9
#define REMOTE_MONITOR_CMD_STATUS 10
#define REMOTE_MONITOR_CMD_READ_MEMV 11
#define REMOTE_MONITOR_CMD_RUN 12
#define REMOTE_MONITOR_CMD_COLDSTART 13
#define REMOTE_MONITOR_CMD_WARMSTART 14
#define REMOTE_MONITOR_CMD_REMOVE_CARTRIDGE 15
#define REMOTE_MONITOR_CMD_STOP_EMULATOR 16
#define REMOTE_MONITOR_CMD_REMOVE_TAPE 17
#define REMOTE_MONITOR_CMD_REMOVE_DISKS 18
#define REMOTE_MONITOR_CMD_HISTORY 19
#define REMOTE_MONITOR_CMD_BUILTIN_MONITOR 20
#define REMOTE_MONITOR_CMD_WRITE_MEMORY 21
#define REMOTE_MONITOR_CMD_BP_CLEAR 22
#define REMOTE_MONITOR_CMD_BP_ADD_CLAUSE 23
#define REMOTE_MONITOR_CMD_BP_DELETE_CLAUSE 24
#define REMOTE_MONITOR_CMD_BP_SET_ENABLED 25
#define REMOTE_MONITOR_CMD_BP_LIST 26
#define REMOTE_MONITOR_CMD_BUILD_FEATURES 27
#define REMOTE_MONITOR_CMD_RESTART_EMULATOR 28

#define REMOTE_MONITOR_CAP_VIDEO_SDL2 0x0001
#define REMOTE_MONITOR_CAP_VIDEO_SDL 0x0002
#define REMOTE_MONITOR_CAP_SOUND 0x0003
#define REMOTE_MONITOR_CAP_SOUND_CALLBACK 0x0004
#define REMOTE_MONITOR_CAP_AUDIO_RECORDING 0x0005
#define REMOTE_MONITOR_CAP_VIDEO_RECORDING 0x0006
#define REMOTE_MONITOR_CAP_MONITOR_BREAK 0x0007
#define REMOTE_MONITOR_CAP_MONITOR_BREAKPOINTS 0x0008
#define REMOTE_MONITOR_CAP_MONITOR_READLINE 0x0009
#define REMOTE_MONITOR_CAP_MONITOR_HINTS 0x000A
#define REMOTE_MONITOR_CAP_MONITOR_UTF8 0x000B
#define REMOTE_MONITOR_CAP_MONITOR_ANSI 0x000C
#define REMOTE_MONITOR_CAP_MONITOR_ASSEMBLER 0x000D
#define REMOTE_MONITOR_CAP_MONITOR_PROFILE 0x000E
#define REMOTE_MONITOR_CAP_MONITOR_TRACE 0x000F
#define REMOTE_MONITOR_CAP_NETSIO 0x0010
#define REMOTE_MONITOR_CAP_IDE 0x0011
#define REMOTE_MONITOR_CAP_R_IO_DEVICE 0x0012
#define REMOTE_MONITOR_CAP_PBI_BB 0x0013
#define REMOTE_MONITOR_CAP_PBI_MIO 0x0014
#define REMOTE_MONITOR_CAP_PBI_PROTO80 0x0015
#define REMOTE_MONITOR_CAP_PBI_XLD 0x0016
#define REMOTE_MONITOR_CAP_VOICEBOX 0x0017
#define REMOTE_MONITOR_CAP_AF80 0x0018
#define REMOTE_MONITOR_CAP_BIT3 0x0019
#define REMOTE_MONITOR_CAP_XEP80_EMULATION 0x001A
#define REMOTE_MONITOR_CAP_NTSC_FILTER 0x001B
#define REMOTE_MONITOR_CAP_PAL_BLENDING 0x001C
#define REMOTE_MONITOR_CAP_CRASH_MENU 0x001D
#define REMOTE_MONITOR_CAP_NEW_CYCLE_EXACT 0x001E
#define REMOTE_MONITOR_CAP_HAVE_LIBPNG 0x001F
#define REMOTE_MONITOR_CAP_HAVE_LIBZ 0x0020

#define REMOTE_MONITOR_BP_TYPE_PC 1
#define REMOTE_MONITOR_BP_TYPE_A 2
#define REMOTE_MONITOR_BP_TYPE_X 3
#define REMOTE_MONITOR_BP_TYPE_Y 4
#define REMOTE_MONITOR_BP_TYPE_S 5
#define REMOTE_MONITOR_BP_TYPE_READ 6
#define REMOTE_MONITOR_BP_TYPE_WRITE 7
#define REMOTE_MONITOR_BP_TYPE_ACCESS 8
#define REMOTE_MONITOR_BP_TYPE_MEM 9

#define REMOTE_MONITOR_BP_OP_LT 1
#define REMOTE_MONITOR_BP_OP_LE 2
#define REMOTE_MONITOR_BP_OP_EQ 3
#define REMOTE_MONITOR_BP_OP_NE 4
#define REMOTE_MONITOR_BP_OP_GE 5
#define REMOTE_MONITOR_BP_OP_GT 6

int RemoteMonitor_SetTransport(const char *transport)
{
	if (transport == NULL || transport[0] == '\0')
		remote_monitor_transport = REMOTE_MONITOR_TRANSPORT_NONE;
	else if (strcmp(transport, "socket") == 0)
		remote_monitor_transport = REMOTE_MONITOR_TRANSPORT_SOCKET;
	else
		return FALSE;
	remote_monitor_init_failed = FALSE;
	return TRUE;
}

const char *RemoteMonitor_GetTransport(void)
{
	switch (remote_monitor_transport) {
	case REMOTE_MONITOR_TRANSPORT_SOCKET:
		return "socket";
	default:
		return NULL;
	}
}

void RemoteMonitor_SetSocketPath(const char *path)
{
	if (path == NULL)
		remote_monitor_socket_path[0] = '\0';
	else
		Util_strlcpy(remote_monitor_socket_path, path, sizeof(remote_monitor_socket_path));
	remote_monitor_init_failed = FALSE;
}

const char *RemoteMonitor_GetSocketPath(void)
{
	return remote_monitor_socket_path[0] != '\0' ? remote_monitor_socket_path : NULL;
}

const char *RemoteMonitor_DefaultSocketPath(void)
{
#ifdef REMOTE_MONITOR_DEFAULT_PATH
	return REMOTE_MONITOR_DEFAULT_PATH;
#else
	return NULL;
#endif
}

void RemoteMonitor_EnableDefault(void)
{
	const char *default_socket_path = RemoteMonitor_DefaultSocketPath();
	if (default_socket_path == NULL) {
		remote_monitor_transport = REMOTE_MONITOR_TRANSPORT_NONE;
		remote_monitor_socket_path[0] = '\0';
	}
	else {
		remote_monitor_transport = REMOTE_MONITOR_TRANSPORT_SOCKET;
		Util_strlcpy(remote_monitor_socket_path, default_socket_path, sizeof(remote_monitor_socket_path));
	}
	remote_monitor_init_failed = FALSE;
}

void RemoteMonitor_Disable(void)
{
	remote_monitor_transport = REMOTE_MONITOR_TRANSPORT_NONE;
	remote_monitor_socket_path[0] = '\0';
	remote_monitor_init_failed = FALSE;
}

int RemoteMonitor_Enabled(void)
{
	return remote_monitor_transport == REMOTE_MONITOR_TRANSPORT_SOCKET &&
	       remote_monitor_socket_path[0] != '\0';
}

int RemoteMonitor_HasClients(void)
{
	int i;

	if (!remote_monitor_clients_ready)
		return FALSE;
	for (i = 0; i < REMOTE_MONITOR_MAX_CLIENTS; i++) {
		if (remote_monitor_clients[i].fd >= 0)
			return TRUE;
	}
	return FALSE;
}

void RemoteMonitor_NotifyStateChanged(void)
{
	remote_monitor_state_seq++;
}

static void RemoteMonitor_CloseClient(struct RemoteMonitorClient *client)
{
	if (client->fd >= 0) {
		close(client->fd);
		client->fd = -1;
	}
	client->len = 0;
}

static void RemoteMonitor_CloseByFd(int fd)
{
	int i;
	for (i = 0; i < REMOTE_MONITOR_MAX_CLIENTS; i++) {
		if (remote_monitor_clients[i].fd == fd) {
			RemoteMonitor_CloseClient(&remote_monitor_clients[i]);
			break;
		}
	}
}

static void RemoteMonitor_CloseListen(void)
{
	if (remote_monitor_listen_fd >= 0) {
		close(remote_monitor_listen_fd);
		remote_monitor_listen_fd = -1;
	}
	if (remote_monitor_owned_socket && remote_monitor_owned_socket_path[0] != '\0')
		(void)unlink(remote_monitor_owned_socket_path);
	remote_monitor_owned_socket = FALSE;
	remote_monitor_owned_socket_path[0] = '\0';
}

void RemoteMonitor_CloseAll(void)
{
	int i;

	if (remote_monitor_clients_ready) {
		for (i = 0; i < REMOTE_MONITOR_MAX_CLIENTS; i++)
			RemoteMonitor_CloseClient(&remote_monitor_clients[i]);
		remote_monitor_clients_ready = FALSE;
	}
	RemoteMonitor_CloseListen();
	remote_monitor_init_failed = FALSE;
}

static void RemoteMonitor_Init(void)
{
	struct sockaddr_un addr;
	size_t path_len;
	struct stat st;
	int i;

	if (remote_monitor_transport != REMOTE_MONITOR_TRANSPORT_SOCKET ||
	    remote_monitor_socket_path[0] == '\0' ||
	    remote_monitor_listen_fd >= 0 || remote_monitor_init_failed)
		return;

	path_len = strlen(remote_monitor_socket_path);
	if (path_len >= sizeof(addr.sun_path)) {
		Log_print("Remote Monitor: Path is too long: \"%s\".", remote_monitor_socket_path);
		remote_monitor_init_failed = TRUE;
		return;
	}

	if (!remote_monitor_clients_ready) {
		for (i = 0; i < REMOTE_MONITOR_MAX_CLIENTS; i++) {
			remote_monitor_clients[i].fd = -1;
			remote_monitor_clients[i].len = 0;
		}
		remote_monitor_clients_ready = TRUE;
	}

	if (stat(remote_monitor_socket_path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			Log_print("Remote Monitor: Path \"%s\" exists but is not a socket.", remote_monitor_socket_path);
			remote_monitor_init_failed = TRUE;
			return;
		}
		Log_print("Remote Monitor: Cannot bind \"%s\": Address already in use (path already exists).",
			remote_monitor_socket_path);
		remote_monitor_init_failed = TRUE;
		return;
	}
	else if (errno != ENOENT) {
		Log_print("Remote Monitor: Cannot stat \"%s\": %s.",
			remote_monitor_socket_path, strerror(errno));
		remote_monitor_init_failed = TRUE;
		return;
	}

	remote_monitor_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (remote_monitor_listen_fd < 0) {
		Log_print("Remote Monitor: Cannot create socket: %s.", strerror(errno));
		remote_monitor_init_failed = TRUE;
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	Util_strlcpy(addr.sun_path, remote_monitor_socket_path, sizeof(addr.sun_path));
	if (bind(remote_monitor_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		Log_print("Remote Monitor: Cannot bind \"%s\": %s.",
			remote_monitor_socket_path, strerror(errno));
		RemoteMonitor_CloseListen();
		remote_monitor_init_failed = TRUE;
		return;
	}
	remote_monitor_owned_socket = TRUE;
	Util_strlcpy(remote_monitor_owned_socket_path,
		remote_monitor_socket_path, sizeof(remote_monitor_owned_socket_path));
	if (chmod(remote_monitor_socket_path, 0600) < 0) {
		Log_print("Remote Monitor: Cannot change mode for \"%s\": %s.",
			remote_monitor_socket_path, strerror(errno));
		RemoteMonitor_CloseListen();
		remote_monitor_init_failed = TRUE;
		return;
	}
	if (listen(remote_monitor_listen_fd, 8) < 0) {
		Log_print("Remote Monitor: Cannot listen on socket: %s.", strerror(errno));
		RemoteMonitor_CloseListen();
		remote_monitor_init_failed = TRUE;
		return;
	}

	remote_monitor_init_failed = FALSE;
	Log_print("Remote Monitor: Listening on %s.", remote_monitor_socket_path);
}

static void RemoteMonitor_Accept(void)
{
	int fd;
	struct pollfd pfd;
	int pr;
	int i;
	int slot = -1;

	if (remote_monitor_listen_fd < 0)
		return;

	pfd.fd = remote_monitor_listen_fd;
	pfd.events = POLLIN;
	pr = poll(&pfd, 1, 0);
	if (pr <= 0)
		return;
	if (!(pfd.revents & POLLIN))
		return;

	fd = accept(remote_monitor_listen_fd, NULL, NULL);
	if (fd < 0)
		return;

	for (i = 0; i < REMOTE_MONITOR_MAX_CLIENTS; i++) {
		if (remote_monitor_clients[i].fd < 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		close(fd);
		return;
	}

	remote_monitor_clients[slot].fd = fd;
	remote_monitor_clients[slot].len = 0;
}

static int RemoteMonitor_SendAll(int fd, const unsigned char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = send(fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			return 0;
		}
		if (n == 0)
			return 0;
		buf += n;
		len -= (size_t)n;
	}
	return 1;
}

static void RemoteMonitor_Reply(int fd, unsigned char status,
		const unsigned char *payload, uint16_t len)
{
	unsigned char header[3];

	header[0] = status;
	header[1] = (unsigned char)(len & 0xff);
	header[2] = (unsigned char)((len >> 8) & 0xff);
	if (!RemoteMonitor_SendAll(fd, header, sizeof(header))) {
		RemoteMonitor_CloseByFd(fd);
		return;
	}
	if (len > 0 && !RemoteMonitor_SendAll(fd, payload, len))
		RemoteMonitor_CloseByFd(fd);
}

static void RemoteMonitor_ReplyError(struct RemoteMonitorClient *client,
		unsigned char code, const char *format, ...)
{
	unsigned char msgbuf[REMOTE_MONITOR_MAX_PAYLOAD];
	va_list args;
	int n;

	if (format == NULL) {
		RemoteMonitor_Reply(client->fd, code, NULL, 0);
		return;
	}

	va_start(args, format);
	n = vsnprintf((char *)msgbuf, sizeof(msgbuf), format, args);
	va_end(args);
	if (n < 0) {
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_ERR_GENERIC, NULL, 0);
		return;
	}
	if ((size_t)n >= sizeof(msgbuf))
		n = (int)sizeof(msgbuf) - 1;
	RemoteMonitor_Reply(client->fd, code, msgbuf, (uint16_t)n);
}

/* Matches monitor command "C" semantics: allow debug patching RAM/ROM and
   route hardware writes through the current write map. */
static void RemoteMonitor_WriteDebugByte(UWORD addr, UBYTE value)
{
#ifdef PAGED_ATTRIB
	if (MEMORY_writemap[addr >> 8] != NULL &&
	    MEMORY_writemap[addr >> 8] != MEMORY_ROM_PutByte)
		(*MEMORY_writemap[addr >> 8])(addr, value);
	else
		MEMORY_dPutByte(addr, value);
#else
	if (MEMORY_attrib[addr] == MEMORY_HARDWARE)
		MEMORY_HwPutByte(addr, value);
	else
		MEMORY_dPutByte(addr, value);
#endif
}

#ifdef MONITOR_BREAKPOINTS
struct RemoteMonitorBpClause {
	MONITOR_breakpoint_cond conds[MONITOR_BREAKPOINT_TABLE_MAX];
	int count;
};

static int RemoteMonitor_BpMaskFromOperator(unsigned char op, UWORD *mask)
{
	switch (op) {
	case REMOTE_MONITOR_BP_OP_LT:
		*mask = MONITOR_BREAKPOINT_LESS;
		return 1;
	case REMOTE_MONITOR_BP_OP_LE:
		*mask = MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL;
		return 1;
	case REMOTE_MONITOR_BP_OP_EQ:
		*mask = MONITOR_BREAKPOINT_EQUAL;
		return 1;
	case REMOTE_MONITOR_BP_OP_NE:
		*mask = MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_GREATER;
		return 1;
	case REMOTE_MONITOR_BP_OP_GE:
		*mask = MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL;
		return 1;
	case REMOTE_MONITOR_BP_OP_GT:
		*mask = MONITOR_BREAKPOINT_GREATER;
		return 1;
	default:
		return 0;
	}
}

static int RemoteMonitor_BpBaseFromType(unsigned char type, UWORD *base)
{
	switch (type) {
	case REMOTE_MONITOR_BP_TYPE_PC:
		*base = MONITOR_BREAKPOINT_PC;
		return 1;
	case REMOTE_MONITOR_BP_TYPE_A:
		*base = MONITOR_BREAKPOINT_A;
		return 1;
	case REMOTE_MONITOR_BP_TYPE_X:
		*base = MONITOR_BREAKPOINT_X;
		return 1;
	case REMOTE_MONITOR_BP_TYPE_Y:
		*base = MONITOR_BREAKPOINT_Y;
		return 1;
	case REMOTE_MONITOR_BP_TYPE_S:
		*base = MONITOR_BREAKPOINT_S;
		return 1;
	case REMOTE_MONITOR_BP_TYPE_READ:
		*base = MONITOR_BREAKPOINT_READ;
		return 1;
	case REMOTE_MONITOR_BP_TYPE_WRITE:
		*base = MONITOR_BREAKPOINT_WRITE;
		return 1;
	case REMOTE_MONITOR_BP_TYPE_ACCESS:
		*base = MONITOR_BREAKPOINT_ACCESS;
		return 1;
	case REMOTE_MONITOR_BP_TYPE_MEM:
		*base = MONITOR_BREAKPOINT_MEMORY;
		return 1;
	default:
		return 0;
	}
}

static int RemoteMonitor_BpOperatorFromMask(UWORD mask, unsigned char *op)
{
	switch (mask) {
	case MONITOR_BREAKPOINT_LESS:
		*op = REMOTE_MONITOR_BP_OP_LT;
		return 1;
	case MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL:
		*op = REMOTE_MONITOR_BP_OP_LE;
		return 1;
	case MONITOR_BREAKPOINT_EQUAL:
		*op = REMOTE_MONITOR_BP_OP_EQ;
		return 1;
	case MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_GREATER:
		*op = REMOTE_MONITOR_BP_OP_NE;
		return 1;
	case MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL:
		*op = REMOTE_MONITOR_BP_OP_GE;
		return 1;
	case MONITOR_BREAKPOINT_GREATER:
		*op = REMOTE_MONITOR_BP_OP_GT;
		return 1;
	default:
		return 0;
	}
}

static int RemoteMonitor_BpTypeFromBase(UWORD base, unsigned char *type)
{
	switch (base) {
	case MONITOR_BREAKPOINT_PC:
		*type = REMOTE_MONITOR_BP_TYPE_PC;
		return 1;
	case MONITOR_BREAKPOINT_A:
		*type = REMOTE_MONITOR_BP_TYPE_A;
		return 1;
	case MONITOR_BREAKPOINT_X:
		*type = REMOTE_MONITOR_BP_TYPE_X;
		return 1;
	case MONITOR_BREAKPOINT_Y:
		*type = REMOTE_MONITOR_BP_TYPE_Y;
		return 1;
	case MONITOR_BREAKPOINT_S:
		*type = REMOTE_MONITOR_BP_TYPE_S;
		return 1;
	case MONITOR_BREAKPOINT_READ:
		*type = REMOTE_MONITOR_BP_TYPE_READ;
		return 1;
	case MONITOR_BREAKPOINT_WRITE:
		*type = REMOTE_MONITOR_BP_TYPE_WRITE;
		return 1;
	case MONITOR_BREAKPOINT_ACCESS:
		*type = REMOTE_MONITOR_BP_TYPE_ACCESS;
		return 1;
	case MONITOR_BREAKPOINT_MEMORY:
		*type = REMOTE_MONITOR_BP_TYPE_MEM;
		return 1;
	default:
		return 0;
	}
}

static int RemoteMonitor_BpDecodeCondition(const unsigned char *data, MONITOR_breakpoint_cond *cond)
{
	unsigned char type = data[0];
	unsigned char op = data[1];
	UWORD addr = (UWORD)(data[2] | (data[3] << 8));
	UWORD value = (UWORD)(data[4] | (data[5] << 8));
	UWORD base;
	UWORD mask;

	if (!RemoteMonitor_BpBaseFromType(type, &base))
		return 0;
	if (!RemoteMonitor_BpMaskFromOperator(op, &mask))
		return 0;

	cond->enabled = TRUE;
	cond->condition = base | mask;
	cond->value = value;
	cond->m_addr = base == MONITOR_BREAKPOINT_MEMORY ? addr : 0;
	return 1;
}

static int RemoteMonitor_BpEncodeCondition(const MONITOR_breakpoint_cond *cond, unsigned char *out)
{
	UWORD base = cond->condition & (UWORD)~7;
	UWORD mask = cond->condition & 7;
	unsigned char type;
	unsigned char op;
	UWORD addr = 0;

	if (!RemoteMonitor_BpTypeFromBase(base, &type))
		return 0;
	if (!RemoteMonitor_BpOperatorFromMask(mask, &op))
		return 0;
	if (base == MONITOR_BREAKPOINT_MEMORY)
		addr = cond->m_addr;

	out[0] = type;
	out[1] = op;
	out[2] = (unsigned char)(addr & 0xff);
	out[3] = (unsigned char)((addr >> 8) & 0xff);
	out[4] = (unsigned char)(cond->value & 0xff);
	out[5] = (unsigned char)((cond->value >> 8) & 0xff);
	return 1;
}

static int RemoteMonitor_BpParseClauses(struct RemoteMonitorBpClause *clauses, int *clause_count)
{
	int i;
	int ci = 0;

	memset(clauses, 0, sizeof(*clauses) * MONITOR_BREAKPOINT_TABLE_MAX);
	for (i = 0; i < MONITOR_breakpoint_table_size; i++) {
		if (MONITOR_breakpoint_table[i].condition == MONITOR_BREAKPOINT_OR) {
			if (ci == 0 || clauses[ci - 1].count == 0)
				continue;
			if (ci >= MONITOR_BREAKPOINT_TABLE_MAX)
				return 0;
			ci++;
			continue;
		}
		if (ci == 0)
			ci = 1;
		if (clauses[ci - 1].count >= MONITOR_BREAKPOINT_TABLE_MAX)
			return 0;
		clauses[ci - 1].conds[clauses[ci - 1].count++] = MONITOR_breakpoint_table[i];
	}
	while (ci > 0 && clauses[ci - 1].count == 0)
		ci--;
	*clause_count = ci;
	return 1;
}

static int RemoteMonitor_BpSerializeClauses(const struct RemoteMonitorBpClause *clauses, int clause_count)
{
	int i;
	int j;
	int out = 0;

	for (i = 0; i < clause_count; i++) {
		if (clauses[i].count <= 0)
			continue;
		if (out > 0) {
			if (out >= MONITOR_BREAKPOINT_TABLE_MAX)
				return 0;
			MONITOR_breakpoint_table[out].enabled = TRUE;
			MONITOR_breakpoint_table[out].condition = MONITOR_BREAKPOINT_OR;
			MONITOR_breakpoint_table[out].value = 0;
			MONITOR_breakpoint_table[out].m_addr = 0;
			out++;
		}
		for (j = 0; j < clauses[i].count; j++) {
			if (out >= MONITOR_BREAKPOINT_TABLE_MAX)
				return 0;
			MONITOR_breakpoint_table[out++] = clauses[i].conds[j];
		}
	}
	MONITOR_breakpoint_table_size = out;
	return 1;
}
#endif /* MONITOR_BREAKPOINTS */

static void RemoteMonitor_HandleFrame(struct RemoteMonitorClient *client,
		unsigned char cmd, const unsigned char *payload, uint16_t len)
{
	unsigned char outbuf[REMOTE_MONITOR_MAX_PAYLOAD];
	unsigned char memv_buf[REMOTE_MONITOR_MAX_PAYLOAD];
	size_t out_len;
	switch (cmd) {
	case REMOTE_MONITOR_CMD_PING:
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK,
			(const unsigned char *)"PONG", 4);
		break;
	case REMOTE_MONITOR_CMD_DLIST_PTR:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"DLIST_PTR expects empty payload");
			break;
		}
		outbuf[0] = (unsigned char)(ANTIC_dlist & 0xff);
		outbuf[1] = (unsigned char)((ANTIC_dlist >> 8) & 0xff);
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, 2);
		break;
	case REMOTE_MONITOR_CMD_READ_MEM:
		if (len != 4) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"READ_MEM expects 4-byte payload");
			break;
		}
		{
			uint16_t addr = (uint16_t)(payload[0] | (payload[1] << 8));
			uint16_t count = (uint16_t)(payload[2] | (payload[3] << 8));
			if (count > REMOTE_MONITOR_MAX_PAYLOAD) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
					"READ_MEM requested %u bytes (max %u)", (unsigned int)count,
					(unsigned int)REMOTE_MONITOR_MAX_PAYLOAD);
				break;
			}
			for (out_len = 0; out_len < count; out_len++)
				outbuf[out_len] = MEMORY_SafeGetByte((UWORD)(addr + out_len));
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)count);
		}
		break;
	case REMOTE_MONITOR_CMD_DLIST_DUMP:
		{
			UWORD pc;
			size_t count = 0;
			int steps = 0;

			if (len == 0) {
				pc = ANTIC_dlist;
			}
			else if (len == 2) {
				pc = (UWORD)(payload[0] | (payload[1] << 8));
			}
			else {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
					"DLIST_DUMP expects 0 or 2-byte payload");
				break;
			}

			while (steps++ < 1024) {
				UWORD dl_ptr = pc;
				UBYTE ir;
				int mode;

				ir = ANTIC_GetDLByte(&dl_ptr);
				pc = dl_ptr;
				if (count + 1 > REMOTE_MONITOR_MAX_PAYLOAD) {
					RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
						"DLIST_DUMP result too large");
					return;
				}
				outbuf[count++] = ir;
				mode = ir & 0x0f;
				if (mode == 0)
					continue;
				if (mode == 1) {
					UBYTE lo = ANTIC_GetDLByte(&dl_ptr);
					UBYTE hi = ANTIC_GetDLByte(&dl_ptr);
					UWORD target = (UWORD)(lo | (hi << 8));
					pc = dl_ptr;
					if (count + 2 > REMOTE_MONITOR_MAX_PAYLOAD) {
						RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
							"DLIST_DUMP result too large");
						return;
					}
					outbuf[count++] = lo;
					outbuf[count++] = hi;
					if (ir & 0x40) {
						RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK,
							outbuf, (uint16_t)count);
						return;
					}
					pc = target;
					continue;
				}
				if (ir & 0x40) {
					UBYTE lo = ANTIC_GetDLByte(&dl_ptr);
					UBYTE hi = ANTIC_GetDLByte(&dl_ptr);
					pc = dl_ptr;
					if (count + 2 > REMOTE_MONITOR_MAX_PAYLOAD) {
						RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
							"DLIST_DUMP result too large");
						return;
					}
					outbuf[count++] = lo;
					outbuf[count++] = hi;
				}
			}
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_GENERIC,
				"DLIST_DUMP did not terminate");
		}
		break;
	case REMOTE_MONITOR_CMD_CPU_STATE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"CPU_STATE expects empty payload");
			break;
		}
		outbuf[0] = (unsigned char)(ANTIC_ypos & 0xff);
		outbuf[1] = (unsigned char)((ANTIC_ypos >> 8) & 0xff);
		outbuf[2] = (unsigned char)(ANTIC_xpos & 0xff);
		outbuf[3] = (unsigned char)((ANTIC_xpos >> 8) & 0xff);
		outbuf[4] = (unsigned char)(CPU_regPC & 0xff);
		outbuf[5] = (unsigned char)((CPU_regPC >> 8) & 0xff);
		outbuf[6] = CPU_regA;
		outbuf[7] = CPU_regX;
		outbuf[8] = CPU_regY;
		outbuf[9] = CPU_regS;
		CPU_GetStatus();
		outbuf[10] = CPU_regP;
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, 11);
		break;
	case REMOTE_MONITOR_CMD_STATUS: {
		uint64_t emu_ms;
		uint64_t reset_ms;
		uint32_t state_seq;
		unsigned char paused = 0;
		size_t off = 0;
		double emu = Atari800_GetEmulationSeconds();
		double since_reset = Atari800_GetEmulationSecondsSinceReset();

		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"STATUS expects empty payload");
			break;
		}
		if (MONITOR_IsActive() || UI_is_active)
			paused = 1;
#ifdef CRASH_MENU
		if (UI_crash_code >= 0)
			paused |= 0x80;
#endif
		if (emu < 0.0)
			emu = 0.0;
		if (since_reset < 0.0)
			since_reset = 0.0;
		emu_ms = (uint64_t)(emu * 1000.0 + 0.5);
		reset_ms = (uint64_t)(since_reset * 1000.0 + 0.5);
		state_seq = remote_monitor_state_seq;

		outbuf[off++] = paused;
		outbuf[off++] = (unsigned char)(emu_ms & 0xff);
		outbuf[off++] = (unsigned char)((emu_ms >> 8) & 0xff);
		outbuf[off++] = (unsigned char)((emu_ms >> 16) & 0xff);
		outbuf[off++] = (unsigned char)((emu_ms >> 24) & 0xff);
		outbuf[off++] = (unsigned char)((emu_ms >> 32) & 0xff);
		outbuf[off++] = (unsigned char)((emu_ms >> 40) & 0xff);
		outbuf[off++] = (unsigned char)((emu_ms >> 48) & 0xff);
		outbuf[off++] = (unsigned char)((emu_ms >> 56) & 0xff);
		outbuf[off++] = (unsigned char)(reset_ms & 0xff);
		outbuf[off++] = (unsigned char)((reset_ms >> 8) & 0xff);
		outbuf[off++] = (unsigned char)((reset_ms >> 16) & 0xff);
		outbuf[off++] = (unsigned char)((reset_ms >> 24) & 0xff);
		outbuf[off++] = (unsigned char)((reset_ms >> 32) & 0xff);
		outbuf[off++] = (unsigned char)((reset_ms >> 40) & 0xff);
		outbuf[off++] = (unsigned char)((reset_ms >> 48) & 0xff);
		outbuf[off++] = (unsigned char)((reset_ms >> 56) & 0xff);
		outbuf[off++] = (unsigned char)(state_seq & 0xff);
		outbuf[off++] = (unsigned char)((state_seq >> 8) & 0xff);
		outbuf[off++] = (unsigned char)((state_seq >> 16) & 0xff);
		outbuf[off++] = (unsigned char)((state_seq >> 24) & 0xff);
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		break;
	}
	case REMOTE_MONITOR_CMD_READ_MEMV:
		if (len < 2 || (len - 2) % 4 != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"READ_MEMV expects 2 + N*4 payload bytes");
			break;
		}
		{
			uint16_t count = (uint16_t)(payload[0] | (payload[1] << 8));
			size_t need = 2 + (size_t)count * 4;
			size_t off = 2;
			size_t out_off = 0;
			unsigned int i;
			if (need != len) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
					"READ_MEMV length mismatch");
				break;
			}
			for (i = 0; i < count; i++) {
				uint16_t addr = (uint16_t)(payload[off] | (payload[off + 1] << 8));
				uint16_t mlen = (uint16_t)(payload[off + 2] | (payload[off + 3] << 8));
				off += 4;
				if (out_off + mlen > sizeof(memv_buf)) {
					RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
						"READ_MEMV result too large");
					break;
				}
				for (out_len = 0; out_len < mlen; out_len++)
					memv_buf[out_off + out_len] = MEMORY_SafeGetByte((UWORD)(addr + out_len));
				out_off += mlen;
			}
			if (i == count)
				RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, memv_buf, (uint16_t)out_off);
		}
		break;
	case REMOTE_MONITOR_CMD_WRITE_MEMORY:
		if (len < 4) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"WRITE_MEMORY expects 4 + N payload bytes");
			break;
		}
		{
			UWORD addr = (UWORD)(payload[0] | (payload[1] << 8));
			UWORD count = (UWORD)(payload[2] | (payload[3] << 8));
			size_t i;
			if ((size_t)count + 4 != len) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
					"WRITE_MEMORY length mismatch");
				break;
			}
			for (i = 0; i < count; i++)
				RemoteMonitor_WriteDebugByte((UWORD)(addr + i), payload[4 + i]);
			if (count > 0)
				RemoteMonitor_NotifyStateChanged();
		}
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_BP_CLEAR:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"BP_CLEAR expects empty payload");
			break;
		}
#ifdef MONITOR_BREAKPOINTS
		if (MONITOR_breakpoint_table_size > 0) {
			MONITOR_breakpoint_table_size = 0;
			RemoteMonitor_NotifyStateChanged();
		}
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
#else
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case REMOTE_MONITOR_CMD_BP_ADD_CLAUSE:
#ifdef MONITOR_BREAKPOINTS
		{
			UWORD insert_index;
			unsigned int cond_count;
			unsigned int i;
			size_t expected_len;
			struct RemoteMonitorBpClause clauses[MONITOR_BREAKPOINT_TABLE_MAX];
			int clause_count;

			if (len < 4) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
					"BP_ADD_CLAUSE expects at least 4-byte payload");
				break;
			}
			insert_index = (UWORD)(payload[0] | (payload[1] << 8));
			cond_count = payload[2];
			if (payload[3] != 0) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"BP_ADD_CLAUSE reserved byte must be 0");
				break;
			}
			if (cond_count == 0) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"BP_ADD_CLAUSE cond_count must be at least 1");
				break;
			}
			if (cond_count > MONITOR_BREAKPOINT_TABLE_MAX) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
					"BP_ADD_CLAUSE cond_count exceeds limit");
				break;
			}
			expected_len = 4 + (size_t)cond_count * 6;
			if (len != expected_len) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
					"BP_ADD_CLAUSE length mismatch");
				break;
			}
			if (!RemoteMonitor_BpParseClauses(clauses, &clause_count)) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_GENERIC,
					"Failed to parse existing breakpoint clauses");
				break;
			}
			if (insert_index == 0xffff)
				insert_index = (UWORD)clause_count;
			if (insert_index > (UWORD)clause_count) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"BP_ADD_CLAUSE insert_index out of range");
				break;
			}
			if (clause_count >= MONITOR_BREAKPOINT_TABLE_MAX) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
					"Breakpoint clause table is full");
				break;
			}
			for (i = (unsigned int)clause_count; i > insert_index; i--)
				clauses[i] = clauses[i - 1];
			memset(&clauses[insert_index], 0, sizeof(clauses[insert_index]));
			clauses[insert_index].count = (int)cond_count;
			for (i = 0; i < cond_count; i++) {
				const unsigned char *cond_data = payload + 4 + i * 6;
				if (!RemoteMonitor_BpDecodeCondition(cond_data, &clauses[insert_index].conds[i])) {
					RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
						"BP_ADD_CLAUSE has invalid condition at index %u", i);
					return;
				}
			}
			clause_count++;
			if (!RemoteMonitor_BpSerializeClauses(clauses, clause_count)) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
					"Breakpoint table is full");
				break;
			}
			RemoteMonitor_NotifyStateChanged();
			outbuf[0] = (unsigned char)(insert_index & 0xff);
			outbuf[1] = (unsigned char)((insert_index >> 8) & 0xff);
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, 2);
		}
#else
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case REMOTE_MONITOR_CMD_BP_DELETE_CLAUSE:
#ifdef MONITOR_BREAKPOINTS
		{
			UWORD clause_index;
			struct RemoteMonitorBpClause clauses[MONITOR_BREAKPOINT_TABLE_MAX];
			int clause_count;
			int i;

			if (len != 2) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
					"BP_DELETE_CLAUSE expects 2-byte payload");
				break;
			}
			clause_index = (UWORD)(payload[0] | (payload[1] << 8));
			if (!RemoteMonitor_BpParseClauses(clauses, &clause_count)) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_GENERIC,
					"Failed to parse existing breakpoint clauses");
				break;
			}
			if (clause_index >= (UWORD)clause_count) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"BP_DELETE_CLAUSE clause_index out of range");
				break;
			}
			for (i = (int)clause_index; i < clause_count - 1; i++)
				clauses[i] = clauses[i + 1];
			clause_count--;
			if (!RemoteMonitor_BpSerializeClauses(clauses, clause_count)) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_GENERIC,
					"Failed to serialize breakpoint clauses");
				break;
			}
			RemoteMonitor_NotifyStateChanged();
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		}
#else
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case REMOTE_MONITOR_CMD_BP_SET_ENABLED:
		if (len != 1) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"BP_SET_ENABLED expects 1-byte payload");
			break;
		}
#ifdef MONITOR_BREAKPOINTS
		if (payload[0] > 1) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
				"BP_SET_ENABLED payload must be 0 or 1");
			break;
		}
		if (MONITOR_breakpoints_enabled != (int)payload[0]) {
			MONITOR_breakpoints_enabled = payload[0];
			RemoteMonitor_NotifyStateChanged();
		}
		outbuf[0] = (unsigned char)MONITOR_breakpoints_enabled;
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, 1);
#else
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case REMOTE_MONITOR_CMD_BP_LIST:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"BP_LIST expects empty payload");
			break;
		}
#ifdef MONITOR_BREAKPOINTS
		{
			struct RemoteMonitorBpClause clauses[MONITOR_BREAKPOINT_TABLE_MAX];
			int clause_count;
			int i;
			size_t off = 0;

			if (!RemoteMonitor_BpParseClauses(clauses, &clause_count)) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_GENERIC,
					"Failed to parse existing breakpoint clauses");
				break;
			}
			if (off + 3 > sizeof(outbuf)) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
					"BP_LIST result too large");
				break;
			}
			outbuf[off++] = (unsigned char)MONITOR_breakpoints_enabled;
			outbuf[off++] = (unsigned char)(clause_count & 0xff);
			outbuf[off++] = (unsigned char)((clause_count >> 8) & 0xff);

			for (i = 0; i < clause_count; i++) {
				int j;
				if (off + 2 > sizeof(outbuf)) {
					RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
						"BP_LIST result too large");
					return;
				}
				outbuf[off++] = (unsigned char)clauses[i].count;
				outbuf[off++] = 0;
				for (j = 0; j < clauses[i].count; j++) {
					if (off + 6 > sizeof(outbuf)) {
						RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
							"BP_LIST result too large");
						return;
					}
					if (!RemoteMonitor_BpEncodeCondition(&clauses[i].conds[j], outbuf + off)) {
						RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
							"BP_LIST cannot encode unsupported breakpoint condition");
						return;
					}
					off += 6;
				}
			}
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		}
#else
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case REMOTE_MONITOR_CMD_BUILD_FEATURES:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"BUILD_FEATURES expects empty payload");
			break;
		}
		{
			size_t off = 2;
			UWORD count = 0;
#define REMOTE_MONITOR_ADD_CAP(_cap_id) do { \
			if (off + 2 > sizeof(outbuf)) { \
					RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE, \
						"BUILD_FEATURES result too large"); \
					return; \
				} \
			outbuf[off++] = (unsigned char)((_cap_id) & 0xff); \
			outbuf[off++] = (unsigned char)(((_cap_id) >> 8) & 0xff); \
			count++; \
		} while (0)

#ifdef SDL2
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_VIDEO_SDL2);
#endif
#ifdef SDL
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_VIDEO_SDL);
#endif
#ifdef SOUND
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_SOUND);
#endif
#ifdef SOUND_CALLBACK
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_SOUND_CALLBACK);
#endif
#ifdef AUDIO_RECORDING
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_AUDIO_RECORDING);
#endif
#ifdef VIDEO_RECORDING
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_VIDEO_RECORDING);
#endif
#ifdef MONITOR_BREAK
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_BREAK);
#endif
#ifdef MONITOR_BREAKPOINTS
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_BREAKPOINTS);
#endif
#ifdef MONITOR_READLINE
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_READLINE);
#endif
#ifdef MONITOR_HINTS
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_HINTS);
#endif
#ifdef MONITOR_UTF8
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_UTF8);
#endif
#ifdef MONITOR_ANSI
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_ANSI);
#endif
#ifdef MONITOR_ASSEMBLER
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_ASSEMBLER);
#endif
#ifdef MONITOR_PROFILE
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_PROFILE);
#endif
#ifdef MONITOR_TRACE
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_TRACE);
#endif
#ifdef NETSIO
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_NETSIO);
#endif
#ifdef IDE
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_IDE);
#endif
#ifdef R_IO_DEVICE
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_R_IO_DEVICE);
#endif
#ifdef PBI_BB
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_PBI_BB);
#endif
#ifdef PBI_MIO
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_PBI_MIO);
#endif
#ifdef PBI_PROTO80
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_PBI_PROTO80);
#endif
#ifdef PBI_XLD
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_PBI_XLD);
#endif
#ifdef VOICEBOX
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_VOICEBOX);
#endif
#ifdef AF80
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_AF80);
#endif
#ifdef BIT3
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_BIT3);
#endif
#ifdef XEP80_EMULATION
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_XEP80_EMULATION);
#endif
#ifdef NTSC_FILTER
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_NTSC_FILTER);
#endif
#ifdef PAL_BLENDING
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_PAL_BLENDING);
#endif
#ifdef CRASH_MENU
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_CRASH_MENU);
#endif
#ifdef NEW_CYCLE_EXACT
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_NEW_CYCLE_EXACT);
#endif
#ifdef HAVE_LIBPNG
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_HAVE_LIBPNG);
#endif
#ifdef HAVE_LIBZ
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_HAVE_LIBZ);
#endif

			outbuf[0] = (unsigned char)(count & 0xff);
			outbuf[1] = (unsigned char)((count >> 8) & 0xff);
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
#undef REMOTE_MONITOR_ADD_CAP
		}
		break;
	case REMOTE_MONITOR_CMD_PAUSE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"PAUSE expects empty payload");
			break;
		}
		if (!MONITOR_IsActive())
			Atari800_RequestMonitorHeadless();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_CONTINUE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"CONTINUE expects empty payload");
			break;
		}
		if (MONITOR_IsActive())
			MONITOR_RequestAction(MONITOR_ACTION_CONT);
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_STEP:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"STEP expects empty payload");
			break;
		}
		MONITOR_RequestAction(MONITOR_ACTION_STEP);
		if (!MONITOR_IsActive())
			Atari800_RequestMonitorHeadless();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_STEP_FRAME:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"STEP_FRAME expects empty payload");
			break;
		}
		MONITOR_RequestAction(MONITOR_ACTION_GF);
		if (!MONITOR_IsActive())
			Atari800_RequestMonitorHeadless();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_RUN:
		if (len == 0 || len >= FILENAME_MAX) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"RUN expects 1..%u payload bytes", (unsigned int)(FILENAME_MAX - 1));
			break;
		}
		{
			char path[FILENAME_MAX];
			size_t path_len = len;
			int ok = 0;

			memcpy(path, payload, path_len);
			path[path_len] = '\0';
			while (path_len > 0 &&
			       (path[path_len - 1] == '\0' ||
			        isspace((unsigned char)path[path_len - 1])))
				path[--path_len] = '\0';
			if (path_len >= 2 && path[0] == '"' && path[path_len - 1] == '"') {
				path[path_len - 1] = '\0';
				memmove(path, path + 1, path_len);
				path_len--;
			}
			if (path_len == 0) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"RUN path is empty");
				break;
			}
			{
				int type = AFILE_DetectFileType(path);
				if (type == AFILE_XEX || type == AFILE_BAS || type == AFILE_LST) {
					if (!BINLOAD_Loader(path)) {
						Log_print("Remote Monitor: Failed to run \"%s\".", path);
						RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_FILE_RUN_FAILED,
							"Failed to run \"%s\".", path);
					}
					else
						ok = 1;
				}
				else if (type != AFILE_ERROR) {
					if (AFILE_OpenFile(path, TRUE, 1, FALSE) == AFILE_ERROR) {
						Log_print("Remote Monitor: Failed to open \"%s\".", path);
						RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_FILE_OPEN_FAILED,
							"Failed to open \"%s\".", path);
					}
					else
						ok = 1;
				}
				else {
					struct stat st;
					if (stat(path, &st) != 0) {
						if (errno == ENOENT) {
							Log_print("Remote Monitor: File not found \"%s\".", path);
							RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_FILE_NOT_FOUND,
								"File not found: \"%s\".", path);
						}
						else {
							Log_print("Remote Monitor: Cannot stat \"%s\": %s.", path, strerror(errno));
							RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_FILE_OPEN_FAILED,
								"Cannot stat \"%s\": %s.", path, strerror(errno));
						}
					}
					else {
						Log_print("Remote Monitor: Unsupported file \"%s\".", path);
						RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_UNSUPPORTED_FILE,
							"Unsupported file type: \"%s\".", path);
					}
				}
			}
			if (ok) {
				RemoteMonitor_NotifyStateChanged();
				RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
			}
		}
		break;
	case REMOTE_MONITOR_CMD_COLDSTART:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"COLDSTART expects empty payload");
			break;
		}
		if (UI_is_active)
			UI_alt_function = UI_MENU_RESETC;
		else
			Atari800_Coldstart();
		RemoteMonitor_NotifyStateChanged();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_WARMSTART:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"WARMSTART expects empty payload");
			break;
		}
		if (UI_is_active)
			UI_alt_function = UI_MENU_RESETW;
		else
			Atari800_Warmstart();
		RemoteMonitor_NotifyStateChanged();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_REMOVE_CARTRIDGE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"REMOVE_CARTRIDGE expects empty payload");
			break;
		}
		CARTRIDGE_RemoveAutoReboot();
		RemoteMonitor_NotifyStateChanged();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_STOP_EMULATOR:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"STOP_EMULATOR expects empty payload");
			break;
		}
		if (UI_is_active) {
			UI_alt_function = UI_MENU_EXIT;
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		}
		else {
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
			Atari800_Exit(FALSE);
			exit(0);
		}
		break;
	case REMOTE_MONITOR_CMD_RESTART_EMULATOR:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"RESTART_EMULATOR expects empty payload");
			break;
		}
		if (!Atari800_CanRestartProcess()) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
				"Process restart is not available.");
			break;
		}
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		if (!Atari800_RestartProcess()) {
			Log_print("Remote Monitor: Process restart failed.");
		}
		break;
	case REMOTE_MONITOR_CMD_REMOVE_TAPE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"REMOVE_TAPE expects empty payload");
			break;
		}
		CASSETTE_Remove();
		RemoteMonitor_NotifyStateChanged();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_REMOVE_DISKS:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"REMOVE_DISKS expects empty payload");
			break;
		}
		{
			int i;
			for (i = 1; i <= SIO_MAX_DRIVES; i++)
				SIO_Dismount(i);
		}
		RemoteMonitor_NotifyStateChanged();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_HISTORY:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"HISTORY expects empty payload");
			break;
		}
		{
			size_t off = 0;
			unsigned int i;
			outbuf[off++] = (unsigned char)CPU_REMEMBER_PC_STEPS;
			for (i = 0; i < CPU_REMEMBER_PC_STEPS; i++) {
				unsigned int idx = (CPU_remember_PC_curpos + CPU_REMEMBER_PC_STEPS - 1 - i) % CPU_REMEMBER_PC_STEPS;
				int j = CPU_remember_xpos[idx];
				UWORD pc = CPU_remember_PC[idx];
				if (off + 7 > REMOTE_MONITOR_MAX_PAYLOAD) {
					RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_PAYLOAD_TOO_LARGE,
						"HISTORY result too large");
					break;
				}
				outbuf[off++] = (unsigned char)((j >> 8) & 0xff); /* y */
				outbuf[off++] = (unsigned char)(j & 0xff);        /* x */
				outbuf[off++] = (unsigned char)(pc & 0xff);
				outbuf[off++] = (unsigned char)((pc >> 8) & 0xff);
				outbuf[off++] = CPU_remember_op[idx][0];
				outbuf[off++] = CPU_remember_op[idx][1];
				outbuf[off++] = CPU_remember_op[idx][2];
			}
			if (i == CPU_REMEMBER_PC_STEPS)
				RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		}
		break;
	case REMOTE_MONITOR_CMD_BUILTIN_MONITOR:
		if (len > 1) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"BUILTIN_MONITOR expects 0 or 1-byte payload");
			break;
		}
		if (len == 0) {
			Atari800_SetBuiltinMonitor(TRUE);
			Atari800_RequestMonitor();
		}
		else {
			if (payload[0] > 1) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"BUILTIN_MONITOR payload must be 0 or 1");
				break;
			}
			Atari800_SetBuiltinMonitor(payload[0] != 0);
		}
		RemoteMonitor_NotifyStateChanged();
		outbuf[0] = (unsigned char)Atari800_GetBuiltinMonitor();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, 1);
		break;
	default:
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_UNKNOWN_COMMAND,
			"Unknown command id: %u.", (unsigned int)cmd);
		break;
	}
}

void RemoteMonitor_Poll(void)
{
	int i;

	if (!RemoteMonitor_Enabled())
		return;

	if (remote_monitor_listen_fd < 0)
		RemoteMonitor_Init();
	if (remote_monitor_listen_fd < 0)
		return;

	RemoteMonitor_Accept();

	for (i = 0; i < REMOTE_MONITOR_MAX_CLIENTS; i++) {
		struct RemoteMonitorClient *client = &remote_monitor_clients[i];
		unsigned char buf[256];
		ssize_t n;

		if (client->fd < 0)
			continue;

		for (;;) {
			n = recv(client->fd, buf, sizeof(buf), MSG_DONTWAIT);
			if (n > 0) {
				if (client->len + (size_t)n > sizeof(client->buf)) {
					Log_print("Remote Monitor: Command is too long. Dropping client input.");
					client->len = 0;
				}
				else {
					memcpy(client->buf + client->len, buf, (size_t)n);
					client->len += (size_t)n;
				}
			}
			else if (n == 0) {
				RemoteMonitor_CloseClient(client);
				break;
			}
			else {
				if (errno == EINTR)
					continue;
				if (errno != EAGAIN && errno != EWOULDBLOCK)
					RemoteMonitor_CloseClient(client);
				break;
			}
		}

		while (client->fd >= 0 && client->len >= 3) {
			unsigned char cmd = client->buf[0];
			uint16_t plen = (uint16_t)(client->buf[1] | (client->buf[2] << 8));
			size_t frame_len = (size_t)plen + 3;
			if (frame_len > sizeof(client->buf)) {
				Log_print("Remote Monitor: Invalid frame size.");
				RemoteMonitor_CloseClient(client);
				break;
			}
			if (client->len < frame_len)
				break;
			/* Collapse queued STATUS polls: process only the newest one. */
			if (cmd == REMOTE_MONITOR_CMD_STATUS && plen == 0) {
				if (client->len >= frame_len + 3) {
					unsigned char next_cmd = client->buf[frame_len];
					uint16_t next_plen = (uint16_t)(client->buf[frame_len + 1] |
						(client->buf[frame_len + 2] << 8));
					size_t next_frame_len = (size_t)next_plen + 3;
					if (client->len >= frame_len + next_frame_len &&
					    next_cmd == REMOTE_MONITOR_CMD_STATUS &&
					    next_plen == 0) {
						client->len -= frame_len;
						memmove(client->buf, client->buf + frame_len, client->len);
						continue;
					}
				}
			}
			RemoteMonitor_HandleFrame(client, cmd, client->buf + 3, plen);
			client->len -= frame_len;
			memmove(client->buf, client->buf + frame_len, client->len);
		}
	}
}

#else

int RemoteMonitor_SetTransport(const char *transport)
{
	(void)transport;
	return FALSE;
}

const char *RemoteMonitor_GetTransport(void)
{
	return NULL;
}

void RemoteMonitor_SetSocketPath(const char *path)
{
	(void)path;
}

const char *RemoteMonitor_GetSocketPath(void)
{
	return NULL;
}

const char *RemoteMonitor_DefaultSocketPath(void)
{
	return NULL;
}

void RemoteMonitor_EnableDefault(void)
{
}

void RemoteMonitor_Disable(void)
{
}

int RemoteMonitor_Enabled(void)
{
	return 0;
}

int RemoteMonitor_HasClients(void)
{
	return 0;
}

void RemoteMonitor_Poll(void)
{
}

void RemoteMonitor_CloseAll(void)
{
}

void RemoteMonitor_NotifyStateChanged(void)
{
}

#endif
