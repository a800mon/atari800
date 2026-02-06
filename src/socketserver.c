#include "config.h"
#include "socketserver.h"

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

#define SOCKET_SERVER_MAX_CLIENTS 8
#define SOCKET_SERVER_MAX_PAYLOAD 4096

static char socket_server_path[FILENAME_MAX];
static int socket_server_listen_fd = -1;
static int socket_server_clients_ready = FALSE;
static uint32_t socket_server_state_seq = 0;

struct SocketServerClient {
	int fd;
	unsigned char buf[SOCKET_SERVER_MAX_PAYLOAD + 16];
	size_t len;
};

static struct SocketServerClient socket_server_clients[SOCKET_SERVER_MAX_CLIENTS];

#define SOCKET_SERVER_OK 0
/* Error status codes returned in the frame status byte. */
#define SOCKET_SERVER_ERR_GENERIC 1
#define SOCKET_SERVER_ERR_INVALID_LENGTH 2
#define SOCKET_SERVER_ERR_INVALID_VALUE 3
#define SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE 4
#define SOCKET_SERVER_ERR_FILE_NOT_FOUND 5
#define SOCKET_SERVER_ERR_FILE_OPEN_FAILED 6
#define SOCKET_SERVER_ERR_FILE_RUN_FAILED 7
#define SOCKET_SERVER_ERR_UNSUPPORTED_FILE 8
#define SOCKET_SERVER_ERR_UNKNOWN_COMMAND 9
#define SOCKET_SERVER_CMD_PING 1
#define SOCKET_SERVER_CMD_DLIST_PTR 2
#define SOCKET_SERVER_CMD_READ_MEM 3
#define SOCKET_SERVER_CMD_DLIST_DUMP 4
#define SOCKET_SERVER_CMD_CPU_STATE 5
#define SOCKET_SERVER_CMD_PAUSE 6
#define SOCKET_SERVER_CMD_CONTINUE 7
#define SOCKET_SERVER_CMD_STEP 8
#define SOCKET_SERVER_CMD_STEP_FRAME 9
#define SOCKET_SERVER_CMD_STATUS 10
#define SOCKET_SERVER_CMD_READ_MEMV 11
#define SOCKET_SERVER_CMD_RUN 12
#define SOCKET_SERVER_CMD_COLDSTART 13
#define SOCKET_SERVER_CMD_WARMSTART 14
#define SOCKET_SERVER_CMD_REMOVE_CARTRIDGE 15
#define SOCKET_SERVER_CMD_STOP_EMULATOR 16
#define SOCKET_SERVER_CMD_REMOVE_TAPE 17
#define SOCKET_SERVER_CMD_REMOVE_DISKS 18
#define SOCKET_SERVER_CMD_HISTORY 19
#define SOCKET_SERVER_CMD_BUILTIN_MONITOR 20
#define SOCKET_SERVER_CMD_WRITE_MEMORY 21
#define SOCKET_SERVER_CMD_BP_CLEAR 22
#define SOCKET_SERVER_CMD_BP_ADD_CLAUSE 23
#define SOCKET_SERVER_CMD_BP_DELETE_CLAUSE 24
#define SOCKET_SERVER_CMD_BP_SET_ENABLED 25
#define SOCKET_SERVER_CMD_BP_LIST 26
#define SOCKET_SERVER_CMD_CONFIG 27
#define SOCKET_SERVER_CMD_RESTART_EMULATOR 28

#define SOCKET_SERVER_CAP_VIDEO_SDL2 0x0001
#define SOCKET_SERVER_CAP_VIDEO_SDL 0x0002
#define SOCKET_SERVER_CAP_SOUND 0x0003
#define SOCKET_SERVER_CAP_SOUND_CALLBACK 0x0004
#define SOCKET_SERVER_CAP_AUDIO_RECORDING 0x0005
#define SOCKET_SERVER_CAP_VIDEO_RECORDING 0x0006
#define SOCKET_SERVER_CAP_MONITOR_BREAK 0x0007
#define SOCKET_SERVER_CAP_MONITOR_BREAKPOINTS 0x0008
#define SOCKET_SERVER_CAP_MONITOR_READLINE 0x0009
#define SOCKET_SERVER_CAP_MONITOR_HINTS 0x000A
#define SOCKET_SERVER_CAP_MONITOR_UTF8 0x000B
#define SOCKET_SERVER_CAP_MONITOR_ANSI 0x000C
#define SOCKET_SERVER_CAP_MONITOR_ASSEMBLER 0x000D
#define SOCKET_SERVER_CAP_MONITOR_PROFILE 0x000E
#define SOCKET_SERVER_CAP_MONITOR_TRACE 0x000F
#define SOCKET_SERVER_CAP_NETSIO 0x0010
#define SOCKET_SERVER_CAP_IDE 0x0011
#define SOCKET_SERVER_CAP_R_IO_DEVICE 0x0012
#define SOCKET_SERVER_CAP_PBI_BB 0x0013
#define SOCKET_SERVER_CAP_PBI_MIO 0x0014
#define SOCKET_SERVER_CAP_PBI_PROTO80 0x0015
#define SOCKET_SERVER_CAP_PBI_XLD 0x0016
#define SOCKET_SERVER_CAP_VOICEBOX 0x0017
#define SOCKET_SERVER_CAP_AF80 0x0018
#define SOCKET_SERVER_CAP_BIT3 0x0019
#define SOCKET_SERVER_CAP_XEP80_EMULATION 0x001A
#define SOCKET_SERVER_CAP_NTSC_FILTER 0x001B
#define SOCKET_SERVER_CAP_PAL_BLENDING 0x001C
#define SOCKET_SERVER_CAP_CRASH_MENU 0x001D
#define SOCKET_SERVER_CAP_NEW_CYCLE_EXACT 0x001E
#define SOCKET_SERVER_CAP_HAVE_LIBPNG 0x001F
#define SOCKET_SERVER_CAP_HAVE_LIBZ 0x0020

#define SOCKET_SERVER_BP_TYPE_PC 1
#define SOCKET_SERVER_BP_TYPE_A 2
#define SOCKET_SERVER_BP_TYPE_X 3
#define SOCKET_SERVER_BP_TYPE_Y 4
#define SOCKET_SERVER_BP_TYPE_S 5
#define SOCKET_SERVER_BP_TYPE_READ 6
#define SOCKET_SERVER_BP_TYPE_WRITE 7
#define SOCKET_SERVER_BP_TYPE_ACCESS 8
#define SOCKET_SERVER_BP_TYPE_MEM 9

#define SOCKET_SERVER_BP_OP_LT 1
#define SOCKET_SERVER_BP_OP_LE 2
#define SOCKET_SERVER_BP_OP_EQ 3
#define SOCKET_SERVER_BP_OP_NE 4
#define SOCKET_SERVER_BP_OP_GE 5
#define SOCKET_SERVER_BP_OP_GT 6

void SocketServer_SetPath(const char *path)
{
	if (path == NULL)
		socket_server_path[0] = '\0';
	else
		Util_strlcpy(socket_server_path, path, sizeof(socket_server_path));
}

int SocketServer_Enabled(void)
{
	return socket_server_path[0] != '\0';
}

void SocketServer_NotifyStateChanged(void)
{
	socket_server_state_seq++;
}

static void SocketServer_CloseClient(struct SocketServerClient *client)
{
	if (client->fd >= 0) {
		close(client->fd);
		client->fd = -1;
	}
	client->len = 0;
}

static void SocketServer_CloseByFd(int fd)
{
	int i;
	for (i = 0; i < SOCKET_SERVER_MAX_CLIENTS; i++) {
		if (socket_server_clients[i].fd == fd) {
			SocketServer_CloseClient(&socket_server_clients[i]);
			break;
		}
	}
}

static void SocketServer_CloseListen(void)
{
	if (socket_server_listen_fd >= 0) {
		close(socket_server_listen_fd);
		socket_server_listen_fd = -1;
	}
	if (socket_server_path[0] != '\0')
		unlink(socket_server_path);
}

void SocketServer_CloseAll(void)
{
	int i;

	if (socket_server_clients_ready) {
		for (i = 0; i < SOCKET_SERVER_MAX_CLIENTS; i++)
			SocketServer_CloseClient(&socket_server_clients[i]);
		socket_server_clients_ready = FALSE;
	}
	SocketServer_CloseListen();
}

static void SocketServer_Init(void)
{
	struct sockaddr_un addr;
	size_t path_len;
	struct stat st;
	int i;

	if (socket_server_path[0] == '\0' || socket_server_listen_fd >= 0)
		return;

	path_len = strlen(socket_server_path);
	if (path_len >= sizeof(addr.sun_path)) {
		Log_print("socket-server: Path too long \"%s\"", socket_server_path);
		return;
	}

	if (!socket_server_clients_ready) {
		for (i = 0; i < SOCKET_SERVER_MAX_CLIENTS; i++) {
			socket_server_clients[i].fd = -1;
			socket_server_clients[i].len = 0;
		}
		socket_server_clients_ready = TRUE;
	}

	if (stat(socket_server_path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			Log_print("socket-server: Path \"%s\" exists but is not a socket", socket_server_path);
			return;
		}
		if (unlink(socket_server_path) < 0) {
			Log_print("socket-server: Unlink failed for \"%s\": %s",
				socket_server_path, strerror(errno));
			return;
		}
	}
	else if (errno != ENOENT) {
		Log_print("socket-server: Stat failed for \"%s\": %s",
			socket_server_path, strerror(errno));
		return;
	}

	socket_server_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socket_server_listen_fd < 0) {
		Log_print("socket-server: Socket failed: %s", strerror(errno));
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	Util_strlcpy(addr.sun_path, socket_server_path, sizeof(addr.sun_path));
	if (bind(socket_server_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		Log_print("socket-server: Bind failed for \"%s\": %s",
			socket_server_path, strerror(errno));
		SocketServer_CloseListen();
		return;
	}
	if (chmod(socket_server_path, 0600) < 0) {
		Log_print("socket-server: Chmod failed for \"%s\": %s",
			socket_server_path, strerror(errno));
		SocketServer_CloseListen();
		return;
	}
	if (listen(socket_server_listen_fd, 8) < 0) {
		Log_print("socket-server: Listen failed: %s", strerror(errno));
		SocketServer_CloseListen();
		return;
	}

	Log_print("socket-server: Listening on %s", socket_server_path);
}

static void SocketServer_Accept(void)
{
	int fd;
	struct pollfd pfd;
	int pr;
	int i;
	int slot = -1;

	if (socket_server_listen_fd < 0)
		return;

	pfd.fd = socket_server_listen_fd;
	pfd.events = POLLIN;
	pr = poll(&pfd, 1, 0);
	if (pr <= 0)
		return;
	if (!(pfd.revents & POLLIN))
		return;

	fd = accept(socket_server_listen_fd, NULL, NULL);
	if (fd < 0)
		return;

	for (i = 0; i < SOCKET_SERVER_MAX_CLIENTS; i++) {
		if (socket_server_clients[i].fd < 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		close(fd);
		return;
	}

	socket_server_clients[slot].fd = fd;
	socket_server_clients[slot].len = 0;
}

static int SocketServer_SendAll(int fd, const unsigned char *buf, size_t len)
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

static void SocketServer_Reply(int fd, unsigned char status,
		const unsigned char *payload, uint16_t len)
{
	unsigned char header[3];

	header[0] = status;
	header[1] = (unsigned char)(len & 0xff);
	header[2] = (unsigned char)((len >> 8) & 0xff);
	if (!SocketServer_SendAll(fd, header, sizeof(header))) {
		SocketServer_CloseByFd(fd);
		return;
	}
	if (len > 0 && !SocketServer_SendAll(fd, payload, len))
		SocketServer_CloseByFd(fd);
}

static void SocketServer_ReplyError(struct SocketServerClient *client,
		unsigned char code, const char *format, ...)
{
	unsigned char msgbuf[SOCKET_SERVER_MAX_PAYLOAD];
	va_list args;
	int n;

	if (format == NULL) {
		SocketServer_Reply(client->fd, code, NULL, 0);
		return;
	}

	va_start(args, format);
	n = vsnprintf((char *)msgbuf, sizeof(msgbuf), format, args);
	va_end(args);
	if (n < 0) {
		SocketServer_Reply(client->fd, SOCKET_SERVER_ERR_GENERIC, NULL, 0);
		return;
	}
	if ((size_t)n >= sizeof(msgbuf))
		n = (int)sizeof(msgbuf) - 1;
	SocketServer_Reply(client->fd, code, msgbuf, (uint16_t)n);
}

/* Matches monitor command "C" semantics: allow debug patching RAM/ROM and
   route hardware writes through the current write map. */
static void SocketServer_WriteDebugByte(UWORD addr, UBYTE value)
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
struct SocketServerBpClause {
	MONITOR_breakpoint_cond conds[MONITOR_BREAKPOINT_TABLE_MAX];
	int count;
};

static int SocketServer_BpMaskFromOperator(unsigned char op, UWORD *mask)
{
	switch (op) {
	case SOCKET_SERVER_BP_OP_LT:
		*mask = MONITOR_BREAKPOINT_LESS;
		return 1;
	case SOCKET_SERVER_BP_OP_LE:
		*mask = MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL;
		return 1;
	case SOCKET_SERVER_BP_OP_EQ:
		*mask = MONITOR_BREAKPOINT_EQUAL;
		return 1;
	case SOCKET_SERVER_BP_OP_NE:
		*mask = MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_GREATER;
		return 1;
	case SOCKET_SERVER_BP_OP_GE:
		*mask = MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL;
		return 1;
	case SOCKET_SERVER_BP_OP_GT:
		*mask = MONITOR_BREAKPOINT_GREATER;
		return 1;
	default:
		return 0;
	}
}

static int SocketServer_BpBaseFromType(unsigned char type, UWORD *base)
{
	switch (type) {
	case SOCKET_SERVER_BP_TYPE_PC:
		*base = MONITOR_BREAKPOINT_PC;
		return 1;
	case SOCKET_SERVER_BP_TYPE_A:
		*base = MONITOR_BREAKPOINT_A;
		return 1;
	case SOCKET_SERVER_BP_TYPE_X:
		*base = MONITOR_BREAKPOINT_X;
		return 1;
	case SOCKET_SERVER_BP_TYPE_Y:
		*base = MONITOR_BREAKPOINT_Y;
		return 1;
	case SOCKET_SERVER_BP_TYPE_S:
		*base = MONITOR_BREAKPOINT_S;
		return 1;
	case SOCKET_SERVER_BP_TYPE_READ:
		*base = MONITOR_BREAKPOINT_READ;
		return 1;
	case SOCKET_SERVER_BP_TYPE_WRITE:
		*base = MONITOR_BREAKPOINT_WRITE;
		return 1;
	case SOCKET_SERVER_BP_TYPE_ACCESS:
		*base = MONITOR_BREAKPOINT_ACCESS;
		return 1;
	case SOCKET_SERVER_BP_TYPE_MEM:
		*base = MONITOR_BREAKPOINT_MEMORY;
		return 1;
	default:
		return 0;
	}
}

static int SocketServer_BpOperatorFromMask(UWORD mask, unsigned char *op)
{
	switch (mask) {
	case MONITOR_BREAKPOINT_LESS:
		*op = SOCKET_SERVER_BP_OP_LT;
		return 1;
	case MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL:
		*op = SOCKET_SERVER_BP_OP_LE;
		return 1;
	case MONITOR_BREAKPOINT_EQUAL:
		*op = SOCKET_SERVER_BP_OP_EQ;
		return 1;
	case MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_GREATER:
		*op = SOCKET_SERVER_BP_OP_NE;
		return 1;
	case MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL:
		*op = SOCKET_SERVER_BP_OP_GE;
		return 1;
	case MONITOR_BREAKPOINT_GREATER:
		*op = SOCKET_SERVER_BP_OP_GT;
		return 1;
	default:
		return 0;
	}
}

static int SocketServer_BpTypeFromBase(UWORD base, unsigned char *type)
{
	switch (base) {
	case MONITOR_BREAKPOINT_PC:
		*type = SOCKET_SERVER_BP_TYPE_PC;
		return 1;
	case MONITOR_BREAKPOINT_A:
		*type = SOCKET_SERVER_BP_TYPE_A;
		return 1;
	case MONITOR_BREAKPOINT_X:
		*type = SOCKET_SERVER_BP_TYPE_X;
		return 1;
	case MONITOR_BREAKPOINT_Y:
		*type = SOCKET_SERVER_BP_TYPE_Y;
		return 1;
	case MONITOR_BREAKPOINT_S:
		*type = SOCKET_SERVER_BP_TYPE_S;
		return 1;
	case MONITOR_BREAKPOINT_READ:
		*type = SOCKET_SERVER_BP_TYPE_READ;
		return 1;
	case MONITOR_BREAKPOINT_WRITE:
		*type = SOCKET_SERVER_BP_TYPE_WRITE;
		return 1;
	case MONITOR_BREAKPOINT_ACCESS:
		*type = SOCKET_SERVER_BP_TYPE_ACCESS;
		return 1;
	case MONITOR_BREAKPOINT_MEMORY:
		*type = SOCKET_SERVER_BP_TYPE_MEM;
		return 1;
	default:
		return 0;
	}
}

static int SocketServer_BpDecodeCondition(const unsigned char *data, MONITOR_breakpoint_cond *cond)
{
	unsigned char type = data[0];
	unsigned char op = data[1];
	UWORD addr = (UWORD)(data[2] | (data[3] << 8));
	UWORD value = (UWORD)(data[4] | (data[5] << 8));
	UWORD base;
	UWORD mask;

	if (!SocketServer_BpBaseFromType(type, &base))
		return 0;
	if (!SocketServer_BpMaskFromOperator(op, &mask))
		return 0;

	cond->enabled = TRUE;
	cond->condition = base | mask;
	cond->value = value;
	cond->m_addr = base == MONITOR_BREAKPOINT_MEMORY ? addr : 0;
	return 1;
}

static int SocketServer_BpEncodeCondition(const MONITOR_breakpoint_cond *cond, unsigned char *out)
{
	UWORD base = cond->condition & (UWORD)~7;
	UWORD mask = cond->condition & 7;
	unsigned char type;
	unsigned char op;
	UWORD addr = 0;

	if (!SocketServer_BpTypeFromBase(base, &type))
		return 0;
	if (!SocketServer_BpOperatorFromMask(mask, &op))
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

static int SocketServer_BpParseClauses(struct SocketServerBpClause *clauses, int *clause_count)
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

static int SocketServer_BpSerializeClauses(const struct SocketServerBpClause *clauses, int clause_count)
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

static void SocketServer_HandleFrame(struct SocketServerClient *client,
		unsigned char cmd, const unsigned char *payload, uint16_t len)
{
	unsigned char outbuf[SOCKET_SERVER_MAX_PAYLOAD];
	unsigned char memv_buf[SOCKET_SERVER_MAX_PAYLOAD];
	size_t out_len;
	switch (cmd) {
	case SOCKET_SERVER_CMD_PING:
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK,
			(const unsigned char *)"PONG", 4);
		break;
	case SOCKET_SERVER_CMD_DLIST_PTR:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"DLIST_PTR expects empty payload");
			break;
		}
		outbuf[0] = (unsigned char)(ANTIC_dlist & 0xff);
		outbuf[1] = (unsigned char)((ANTIC_dlist >> 8) & 0xff);
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, 2);
		break;
	case SOCKET_SERVER_CMD_READ_MEM:
		if (len != 4) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"READ_MEM expects 4-byte payload");
			break;
		}
		{
			uint16_t addr = (uint16_t)(payload[0] | (payload[1] << 8));
			uint16_t count = (uint16_t)(payload[2] | (payload[3] << 8));
			if (count > SOCKET_SERVER_MAX_PAYLOAD) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
					"READ_MEM requested %u bytes (max %u)", (unsigned int)count,
					(unsigned int)SOCKET_SERVER_MAX_PAYLOAD);
				break;
			}
			for (out_len = 0; out_len < count; out_len++)
				outbuf[out_len] = MEMORY_SafeGetByte((UWORD)(addr + out_len));
			SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, (uint16_t)count);
		}
		break;
	case SOCKET_SERVER_CMD_DLIST_DUMP:
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
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
					"DLIST_DUMP expects 0 or 2-byte payload");
				break;
			}

			while (steps++ < 1024) {
				UWORD dl_ptr = pc;
				UBYTE ir;
				int mode;

				ir = ANTIC_GetDLByte(&dl_ptr);
				pc = dl_ptr;
				if (count + 1 > SOCKET_SERVER_MAX_PAYLOAD) {
					SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
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
					if (count + 2 > SOCKET_SERVER_MAX_PAYLOAD) {
						SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
							"DLIST_DUMP result too large");
						return;
					}
					outbuf[count++] = lo;
					outbuf[count++] = hi;
					if (ir & 0x40) {
						SocketServer_Reply(client->fd, SOCKET_SERVER_OK,
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
					if (count + 2 > SOCKET_SERVER_MAX_PAYLOAD) {
						SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
							"DLIST_DUMP result too large");
						return;
					}
					outbuf[count++] = lo;
					outbuf[count++] = hi;
				}
			}
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_GENERIC,
				"DLIST_DUMP did not terminate");
		}
		break;
	case SOCKET_SERVER_CMD_CPU_STATE:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
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
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, 11);
		break;
	case SOCKET_SERVER_CMD_STATUS: {
		uint64_t emu_ms;
		uint64_t reset_ms;
		uint32_t state_seq;
		unsigned char paused = 0;
		size_t off = 0;
		double emu = Atari800_GetEmulationSeconds();
		double since_reset = Atari800_GetEmulationSecondsSinceReset();

		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
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
		state_seq = socket_server_state_seq;

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
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, (uint16_t)off);
		break;
	}
	case SOCKET_SERVER_CMD_READ_MEMV:
		if (len < 2 || (len - 2) % 4 != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
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
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
					"READ_MEMV length mismatch");
				break;
			}
			for (i = 0; i < count; i++) {
				uint16_t addr = (uint16_t)(payload[off] | (payload[off + 1] << 8));
				uint16_t mlen = (uint16_t)(payload[off + 2] | (payload[off + 3] << 8));
				off += 4;
				if (out_off + mlen > sizeof(memv_buf)) {
					SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
						"READ_MEMV result too large");
					break;
				}
				for (out_len = 0; out_len < mlen; out_len++)
					memv_buf[out_off + out_len] = MEMORY_SafeGetByte((UWORD)(addr + out_len));
				out_off += mlen;
			}
			if (i == count)
				SocketServer_Reply(client->fd, SOCKET_SERVER_OK, memv_buf, (uint16_t)out_off);
		}
		break;
	case SOCKET_SERVER_CMD_WRITE_MEMORY:
		if (len < 4) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"WRITE_MEMORY expects 4 + N payload bytes");
			break;
		}
		{
			UWORD addr = (UWORD)(payload[0] | (payload[1] << 8));
			UWORD count = (UWORD)(payload[2] | (payload[3] << 8));
			size_t i;
			if ((size_t)count + 4 != len) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
					"WRITE_MEMORY length mismatch");
				break;
			}
			for (i = 0; i < count; i++)
				SocketServer_WriteDebugByte((UWORD)(addr + i), payload[4 + i]);
			if (count > 0)
				SocketServer_NotifyStateChanged();
		}
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_BP_CLEAR:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"BP_CLEAR expects empty payload");
			break;
		}
#ifdef MONITOR_BREAKPOINTS
		if (MONITOR_breakpoint_table_size > 0) {
			MONITOR_breakpoint_table_size = 0;
			SocketServer_NotifyStateChanged();
		}
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
#else
		SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case SOCKET_SERVER_CMD_BP_ADD_CLAUSE:
#ifdef MONITOR_BREAKPOINTS
		{
			UWORD insert_index;
			unsigned int cond_count;
			unsigned int i;
			size_t expected_len;
			struct SocketServerBpClause clauses[MONITOR_BREAKPOINT_TABLE_MAX];
			int clause_count;

			if (len < 4) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
					"BP_ADD_CLAUSE expects at least 4-byte payload");
				break;
			}
			insert_index = (UWORD)(payload[0] | (payload[1] << 8));
			cond_count = payload[2];
			if (payload[3] != 0) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
					"BP_ADD_CLAUSE reserved byte must be 0");
				break;
			}
			if (cond_count == 0) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
					"BP_ADD_CLAUSE cond_count must be at least 1");
				break;
			}
			if (cond_count > MONITOR_BREAKPOINT_TABLE_MAX) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
					"BP_ADD_CLAUSE cond_count exceeds limit");
				break;
			}
			expected_len = 4 + (size_t)cond_count * 6;
			if (len != expected_len) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
					"BP_ADD_CLAUSE length mismatch");
				break;
			}
			if (!SocketServer_BpParseClauses(clauses, &clause_count)) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_GENERIC,
					"Failed to parse existing breakpoint clauses");
				break;
			}
			if (insert_index == 0xffff)
				insert_index = (UWORD)clause_count;
			if (insert_index > (UWORD)clause_count) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
					"BP_ADD_CLAUSE insert_index out of range");
				break;
			}
			if (clause_count >= MONITOR_BREAKPOINT_TABLE_MAX) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
					"Breakpoint clause table is full");
				break;
			}
			for (i = (unsigned int)clause_count; i > insert_index; i--)
				clauses[i] = clauses[i - 1];
			memset(&clauses[insert_index], 0, sizeof(clauses[insert_index]));
			clauses[insert_index].count = (int)cond_count;
			for (i = 0; i < cond_count; i++) {
				const unsigned char *cond_data = payload + 4 + i * 6;
				if (!SocketServer_BpDecodeCondition(cond_data, &clauses[insert_index].conds[i])) {
					SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
						"BP_ADD_CLAUSE has invalid condition at index %u", i);
					return;
				}
			}
			clause_count++;
			if (!SocketServer_BpSerializeClauses(clauses, clause_count)) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
					"Breakpoint table is full");
				break;
			}
			SocketServer_NotifyStateChanged();
			outbuf[0] = (unsigned char)(insert_index & 0xff);
			outbuf[1] = (unsigned char)((insert_index >> 8) & 0xff);
			SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, 2);
		}
#else
		SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case SOCKET_SERVER_CMD_BP_DELETE_CLAUSE:
#ifdef MONITOR_BREAKPOINTS
		{
			UWORD clause_index;
			struct SocketServerBpClause clauses[MONITOR_BREAKPOINT_TABLE_MAX];
			int clause_count;
			int i;

			if (len != 2) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
					"BP_DELETE_CLAUSE expects 2-byte payload");
				break;
			}
			clause_index = (UWORD)(payload[0] | (payload[1] << 8));
			if (!SocketServer_BpParseClauses(clauses, &clause_count)) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_GENERIC,
					"Failed to parse existing breakpoint clauses");
				break;
			}
			if (clause_index >= (UWORD)clause_count) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
					"BP_DELETE_CLAUSE clause_index out of range");
				break;
			}
			for (i = (int)clause_index; i < clause_count - 1; i++)
				clauses[i] = clauses[i + 1];
			clause_count--;
			if (!SocketServer_BpSerializeClauses(clauses, clause_count)) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_GENERIC,
					"Failed to serialize breakpoint clauses");
				break;
			}
			SocketServer_NotifyStateChanged();
			SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		}
#else
		SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case SOCKET_SERVER_CMD_BP_SET_ENABLED:
		if (len != 1) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"BP_SET_ENABLED expects 1-byte payload");
			break;
		}
#ifdef MONITOR_BREAKPOINTS
		if (payload[0] > 1) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
				"BP_SET_ENABLED payload must be 0 or 1");
			break;
		}
		if (MONITOR_breakpoints_enabled != (int)payload[0]) {
			MONITOR_breakpoints_enabled = payload[0];
			SocketServer_NotifyStateChanged();
		}
		outbuf[0] = (unsigned char)MONITOR_breakpoints_enabled;
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, 1);
#else
		SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case SOCKET_SERVER_CMD_BP_LIST:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"BP_LIST expects empty payload");
			break;
		}
#ifdef MONITOR_BREAKPOINTS
		{
			struct SocketServerBpClause clauses[MONITOR_BREAKPOINT_TABLE_MAX];
			int clause_count;
			int i;
			size_t off = 0;

			if (!SocketServer_BpParseClauses(clauses, &clause_count)) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_GENERIC,
					"Failed to parse existing breakpoint clauses");
				break;
			}
			if (off + 3 > sizeof(outbuf)) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
					"BP_LIST result too large");
				break;
			}
			outbuf[off++] = (unsigned char)MONITOR_breakpoints_enabled;
			outbuf[off++] = (unsigned char)(clause_count & 0xff);
			outbuf[off++] = (unsigned char)((clause_count >> 8) & 0xff);

			for (i = 0; i < clause_count; i++) {
				int j;
				if (off + 2 > sizeof(outbuf)) {
					SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
						"BP_LIST result too large");
					return;
				}
				outbuf[off++] = (unsigned char)clauses[i].count;
				outbuf[off++] = 0;
				for (j = 0; j < clauses[i].count; j++) {
					if (off + 6 > sizeof(outbuf)) {
						SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
							"BP_LIST result too large");
						return;
					}
					if (!SocketServer_BpEncodeCondition(&clauses[i].conds[j], outbuf + off)) {
						SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
							"BP_LIST cannot encode unsupported breakpoint condition");
						return;
					}
					off += 6;
				}
			}
			SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, (uint16_t)off);
		}
#else
		SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
			"User breakpoints are not supported in this build");
#endif
		break;
	case SOCKET_SERVER_CMD_CONFIG:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"CONFIG expects empty payload");
			break;
		}
		{
			size_t off = 2;
			UWORD count = 0;
#define SOCKET_SERVER_ADD_CAP(_cap_id) do { \
			if (off + 2 > sizeof(outbuf)) { \
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE, \
					"CONFIG result too large"); \
				return; \
			} \
			outbuf[off++] = (unsigned char)((_cap_id) & 0xff); \
			outbuf[off++] = (unsigned char)(((_cap_id) >> 8) & 0xff); \
			count++; \
		} while (0)

#ifdef SDL2
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_VIDEO_SDL2);
#endif
#ifdef SDL
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_VIDEO_SDL);
#endif
#ifdef SOUND
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_SOUND);
#endif
#ifdef SOUND_CALLBACK
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_SOUND_CALLBACK);
#endif
#ifdef AUDIO_RECORDING
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_AUDIO_RECORDING);
#endif
#ifdef VIDEO_RECORDING
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_VIDEO_RECORDING);
#endif
#ifdef MONITOR_BREAK
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_MONITOR_BREAK);
#endif
#ifdef MONITOR_BREAKPOINTS
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_MONITOR_BREAKPOINTS);
#endif
#ifdef MONITOR_READLINE
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_MONITOR_READLINE);
#endif
#ifdef MONITOR_HINTS
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_MONITOR_HINTS);
#endif
#ifdef MONITOR_UTF8
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_MONITOR_UTF8);
#endif
#ifdef MONITOR_ANSI
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_MONITOR_ANSI);
#endif
#ifdef MONITOR_ASSEMBLER
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_MONITOR_ASSEMBLER);
#endif
#ifdef MONITOR_PROFILE
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_MONITOR_PROFILE);
#endif
#ifdef MONITOR_TRACE
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_MONITOR_TRACE);
#endif
#ifdef NETSIO
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_NETSIO);
#endif
#ifdef IDE
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_IDE);
#endif
#ifdef R_IO_DEVICE
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_R_IO_DEVICE);
#endif
#ifdef PBI_BB
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_PBI_BB);
#endif
#ifdef PBI_MIO
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_PBI_MIO);
#endif
#ifdef PBI_PROTO80
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_PBI_PROTO80);
#endif
#ifdef PBI_XLD
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_PBI_XLD);
#endif
#ifdef VOICEBOX
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_VOICEBOX);
#endif
#ifdef AF80
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_AF80);
#endif
#ifdef BIT3
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_BIT3);
#endif
#ifdef XEP80_EMULATION
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_XEP80_EMULATION);
#endif
#ifdef NTSC_FILTER
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_NTSC_FILTER);
#endif
#ifdef PAL_BLENDING
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_PAL_BLENDING);
#endif
#ifdef CRASH_MENU
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_CRASH_MENU);
#endif
#ifdef NEW_CYCLE_EXACT
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_NEW_CYCLE_EXACT);
#endif
#ifdef HAVE_LIBPNG
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_HAVE_LIBPNG);
#endif
#ifdef HAVE_LIBZ
			SOCKET_SERVER_ADD_CAP(SOCKET_SERVER_CAP_HAVE_LIBZ);
#endif

			outbuf[0] = (unsigned char)(count & 0xff);
			outbuf[1] = (unsigned char)((count >> 8) & 0xff);
			SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, (uint16_t)off);
#undef SOCKET_SERVER_ADD_CAP
		}
		break;
	case SOCKET_SERVER_CMD_PAUSE:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"PAUSE expects empty payload");
			break;
		}
		if (!MONITOR_IsActive())
			Atari800_RequestMonitorHeadless();
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_CONTINUE:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"CONTINUE expects empty payload");
			break;
		}
		MONITOR_RequestAction(MONITOR_ACTION_CONT);
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_STEP:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"STEP expects empty payload");
			break;
		}
		MONITOR_RequestAction(MONITOR_ACTION_STEP);
		if (!MONITOR_IsActive())
			Atari800_RequestMonitorHeadless();
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_STEP_FRAME:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"STEP_FRAME expects empty payload");
			break;
		}
		MONITOR_RequestAction(MONITOR_ACTION_GF);
		if (!MONITOR_IsActive())
			Atari800_RequestMonitorHeadless();
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_RUN:
		if (len == 0 || len >= FILENAME_MAX) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
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
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
					"RUN path is empty");
				break;
			}
			{
				int type = AFILE_DetectFileType(path);
				if (type == AFILE_XEX || type == AFILE_BAS || type == AFILE_LST) {
					if (!BINLOAD_Loader(path)) {
						Log_print("socket-server: Failed to run \"%s\"", path);
						SocketServer_ReplyError(client, SOCKET_SERVER_ERR_FILE_RUN_FAILED,
							"Failed to run \"%s\".", path);
					}
					else
						ok = 1;
				}
				else if (type != AFILE_ERROR) {
					if (AFILE_OpenFile(path, TRUE, 1, FALSE) == AFILE_ERROR) {
						Log_print("socket-server: Failed to open \"%s\"", path);
						SocketServer_ReplyError(client, SOCKET_SERVER_ERR_FILE_OPEN_FAILED,
							"Failed to open \"%s\".", path);
					}
					else
						ok = 1;
				}
				else {
					struct stat st;
					if (stat(path, &st) != 0) {
						if (errno == ENOENT) {
							Log_print("socket-server: File not found \"%s\"", path);
							SocketServer_ReplyError(client, SOCKET_SERVER_ERR_FILE_NOT_FOUND,
								"File not found: \"%s\".", path);
						}
						else {
							Log_print("socket-server: Cannot stat \"%s\": %s", path, strerror(errno));
							SocketServer_ReplyError(client, SOCKET_SERVER_ERR_FILE_OPEN_FAILED,
								"Cannot stat \"%s\": %s.", path, strerror(errno));
						}
					}
					else {
						Log_print("socket-server: Unsupported file \"%s\"", path);
						SocketServer_ReplyError(client, SOCKET_SERVER_ERR_UNSUPPORTED_FILE,
							"Unsupported file type: \"%s\".", path);
					}
				}
			}
			if (ok)
			{
				SocketServer_NotifyStateChanged();
				SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
			}
		}
		break;
	case SOCKET_SERVER_CMD_COLDSTART:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"COLDSTART expects empty payload");
			break;
		}
		if (UI_is_active)
			UI_alt_function = UI_MENU_RESETC;
		else
			Atari800_Coldstart();
		SocketServer_NotifyStateChanged();
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_WARMSTART:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"WARMSTART expects empty payload");
			break;
		}
		if (UI_is_active)
			UI_alt_function = UI_MENU_RESETW;
		else
			Atari800_Warmstart();
		SocketServer_NotifyStateChanged();
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_REMOVE_CARTRIDGE:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"REMOVE_CARTRIDGE expects empty payload");
			break;
		}
		CARTRIDGE_RemoveAutoReboot();
		SocketServer_NotifyStateChanged();
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_STOP_EMULATOR:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"STOP_EMULATOR expects empty payload");
			break;
		}
		if (UI_is_active) {
			UI_alt_function = UI_MENU_EXIT;
			SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		}
		else {
			SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
			Atari800_Exit(FALSE);
			exit(0);
		}
		break;
	case SOCKET_SERVER_CMD_RESTART_EMULATOR:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"RESTART_EMULATOR expects empty payload");
			break;
		}
		if (!Atari800_CanRestartProcess()) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
				"Process restart is not available.");
			break;
		}
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		if (!Atari800_RestartProcess()) {
			Log_print("socket-server: Process restart failed.");
		}
		break;
	case SOCKET_SERVER_CMD_REMOVE_TAPE:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"REMOVE_TAPE expects empty payload");
			break;
		}
		CASSETTE_Remove();
		SocketServer_NotifyStateChanged();
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_REMOVE_DISKS:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"REMOVE_DISKS expects empty payload");
			break;
		}
		{
			int i;
			for (i = 1; i <= SIO_MAX_DRIVES; i++)
				SIO_Dismount(i);
		}
		SocketServer_NotifyStateChanged();
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, NULL, 0);
		break;
	case SOCKET_SERVER_CMD_HISTORY:
		if (len != 0) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
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
				if (off + 7 > SOCKET_SERVER_MAX_PAYLOAD) {
					SocketServer_ReplyError(client, SOCKET_SERVER_ERR_PAYLOAD_TOO_LARGE,
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
				SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, (uint16_t)off);
		}
		break;
	case SOCKET_SERVER_CMD_BUILTIN_MONITOR:
		if (len > 1) {
			SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_LENGTH,
				"BUILTIN_MONITOR expects 0 or 1-byte payload");
			break;
		}
		if (len == 0) {
			Atari800_SetBuiltinMonitor(TRUE);
			Atari800_RequestMonitor();
		}
		else {
			if (payload[0] > 1) {
				SocketServer_ReplyError(client, SOCKET_SERVER_ERR_INVALID_VALUE,
					"BUILTIN_MONITOR payload must be 0 or 1");
				break;
			}
			Atari800_SetBuiltinMonitor(payload[0] != 0);
		}
		SocketServer_NotifyStateChanged();
		outbuf[0] = (unsigned char)Atari800_GetBuiltinMonitor();
		SocketServer_Reply(client->fd, SOCKET_SERVER_OK, outbuf, 1);
		break;
	default:
		SocketServer_ReplyError(client, SOCKET_SERVER_ERR_UNKNOWN_COMMAND,
			"Unknown command id: %u.", (unsigned int)cmd);
		break;
	}
}

void SocketServer_Poll(void)
{
	int i;

	if (socket_server_path[0] == '\0')
		return;

	if (socket_server_listen_fd < 0)
		SocketServer_Init();
	if (socket_server_listen_fd < 0)
		return;

	SocketServer_Accept();

	for (i = 0; i < SOCKET_SERVER_MAX_CLIENTS; i++) {
		struct SocketServerClient *client = &socket_server_clients[i];
		unsigned char buf[256];
		ssize_t n;

		if (client->fd < 0)
			continue;

		for (;;) {
			n = recv(client->fd, buf, sizeof(buf), MSG_DONTWAIT);
			if (n > 0) {
				if (client->len + (size_t)n > sizeof(client->buf)) {
					Log_print("socket-server: Command too long, dropping");
					client->len = 0;
				}
				else {
					memcpy(client->buf + client->len, buf, (size_t)n);
					client->len += (size_t)n;
				}
			}
			else if (n == 0) {
				SocketServer_CloseClient(client);
				break;
			}
			else {
				if (errno != EAGAIN && errno != EWOULDBLOCK)
					SocketServer_CloseClient(client);
				break;
			}
		}

		while (client->fd >= 0 && client->len >= 3) {
			unsigned char cmd = client->buf[0];
			uint16_t plen = (uint16_t)(client->buf[1] | (client->buf[2] << 8));
			size_t frame_len = (size_t)plen + 3;
			if (frame_len > sizeof(client->buf)) {
				Log_print("socket-server: Invalid frame size");
				SocketServer_CloseClient(client);
				break;
			}
			if (client->len < frame_len)
				break;
			/* Collapse queued STATUS polls: process only the newest one. */
			if (cmd == SOCKET_SERVER_CMD_STATUS && plen == 0) {
				if (client->len >= frame_len + 3) {
					unsigned char next_cmd = client->buf[frame_len];
					uint16_t next_plen = (uint16_t)(client->buf[frame_len + 1] |
						(client->buf[frame_len + 2] << 8));
					size_t next_frame_len = (size_t)next_plen + 3;
					if (client->len >= frame_len + next_frame_len &&
					    next_cmd == SOCKET_SERVER_CMD_STATUS &&
					    next_plen == 0) {
						client->len -= frame_len;
						memmove(client->buf, client->buf + frame_len, client->len);
						continue;
					}
				}
			}
			SocketServer_HandleFrame(client, cmd, client->buf + 3, plen);
			client->len -= frame_len;
			memmove(client->buf, client->buf + frame_len, client->len);
		}
	}
}

#else

void SocketServer_SetPath(const char *path)
{
	(void)path;
}

int SocketServer_Enabled(void)
{
	return 0;
}

void SocketServer_Poll(void)
{
}

void SocketServer_CloseAll(void)
{
}

void SocketServer_NotifyStateChanged(void)
{
}

#endif
