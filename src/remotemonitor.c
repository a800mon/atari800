#include "config.h"
#include "remotemonitor.h"

#if defined(HAVE_UNISTD_H) && !defined(HAVE_WINDOWS_H)

#include "afile.h"
#include "antic.h"
#include "atari.h"
#include "binload.h"
#include "cartridge.h"
#include "cassette.h"
#include "colours.h"
#include "cpu.h"
#include "gtia.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "monitor.h"
#include "pia.h"
#include "pokey.h"
#include "screen.h"
#include "sio.h"
#include "ui.h"
#include "util.h"
#include "sysrom.h"
#ifdef STEREO_SOUND
#include "pokeysnd.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define REMOTE_MONITOR_MAX_CLIENTS 8
#define REMOTE_MONITOR_MAX_PAYLOAD 4096
#define REMOTE_MONITOR_VIDEO_DEFAULT_FPS 60
#define REMOTE_MONITOR_VIDEO_DEFAULT_PORT 6502
#define REMOTE_MONITOR_VIDEO_MAX_DATAGRAM 1400
#define REMOTE_MONITOR_VIDEO_HEADER_SIZE 24

#if defined(__linux__)
#define REMOTE_MONITOR_DEFAULT_PATH "/tmp/atari.sock"
#endif

enum {
	REMOTE_MONITOR_TRANSPORT_NONE = 0,
	REMOTE_MONITOR_TRANSPORT_SOCKET = 1
};

static int remote_monitor_transport = REMOTE_MONITOR_TRANSPORT_NONE;
static char remote_monitor_socket_path[FILENAME_MAX];
static int remote_monitor_enabled = FALSE;
static int remote_monitor_listen_fd = -1;
static int remote_monitor_owned_socket = FALSE;
static char remote_monitor_owned_socket_path[FILENAME_MAX];
static int remote_monitor_clients_ready = FALSE;
static int remote_monitor_init_failed = FALSE;
static uint32_t remote_monitor_state_seq = 0;
static int remote_monitor_video_enabled = FALSE;
static int remote_monitor_video_fps = REMOTE_MONITOR_VIDEO_DEFAULT_FPS;
static int remote_monitor_video_udp_port = REMOTE_MONITOR_VIDEO_DEFAULT_PORT;
static char remote_monitor_video_udp_host[FILENAME_MAX] = "127.0.0.1";
static int remote_monitor_video_fd = -1;
static int remote_monitor_video_init_failed = FALSE;
static double remote_monitor_video_last_time = 0.0;
static int remote_monitor_video_logged = FALSE;
static struct sockaddr_storage remote_monitor_video_addr;
static socklen_t remote_monitor_video_addr_len = 0;

struct RemoteMonitorClient {
	int fd;
	unsigned char buf[REMOTE_MONITOR_MAX_PAYLOAD + 16];
	size_t len;
};

static struct RemoteMonitorClient remote_monitor_clients[REMOTE_MONITOR_MAX_CLIENTS];

static void RemoteMonitor_VideoClose(void);
static int RemoteMonitor_VideoInit(void);
static void RemoteMonitor_VideoEncodeRow(unsigned char *dst, const UBYTE *src, int width);
static void RemoteMonitor_ResetInput(void);


static unsigned char RemoteMonitor_StatusMachineType(void)
{
	if (Atari800_machine_type == Atari800_MACHINE_5200)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_5200;
	if (Atari800_machine_type == Atari800_MACHINE_800) {
		if (MEMORY_ram_size <= 16)
			return REMOTE_MONITOR_STATUS_MACHINE_ATARI_400;
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_800;
	}
	if (Atari800_builtin_game)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_XEGS;
	if (MEMORY_ram_size == 64 && Atari800_keyboard_leds && Atari800_f_keys && !Atari800_builtin_basic)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_1200XL;
	if (MEMORY_ram_size == 16)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_600XL;
	if (MEMORY_ram_size == 64)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_800XL;
	if (MEMORY_ram_size == 128)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_130XE;
	if (MEMORY_ram_size == MEMORY_RAM_320_COMPY_SHOP)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_320XE_COMPY_SHOP;
	if (MEMORY_ram_size == MEMORY_RAM_320_RAMBO)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_320XE_RAMBO;
	if (MEMORY_ram_size == 576)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_576XE;
	if (MEMORY_ram_size == 1088)
		return REMOTE_MONITOR_STATUS_MACHINE_ATARI_1088XE;
	return REMOTE_MONITOR_STATUS_MACHINE_XL_XE;
}

static unsigned char RemoteMonitor_SysInfoMachineFamily(void)
{
	switch (Atari800_machine_type) {
	case Atari800_MACHINE_800:
		return REMOTE_MONITOR_SYSINFO_MACHINE_FAMILY_ATARI_800;
	case Atari800_MACHINE_XLXE:
		return REMOTE_MONITOR_SYSINFO_MACHINE_FAMILY_XL_XE;
	default:
		return REMOTE_MONITOR_SYSINFO_MACHINE_FAMILY_ATARI_5200;
	}
}

static int RemoteMonitor_SysInfoBasicEnabled(void)
{
	UBYTE portb;
	if (Atari800_machine_type == Atari800_MACHINE_5200)
		return FALSE;
	if (!MEMORY_have_basic)
		return FALSE;
	if (Atari800_machine_type == Atari800_MACHINE_800)
		return !Atari800_disable_basic;
	if (!Atari800_builtin_basic)
		return FALSE;
	portb = PIA_PORTB | PIA_PORTB_mask;
	if ((portb & 0x02) != 0)
		return FALSE;
	if ((portb & 0x10) == 0 && (MEMORY_ram_size == 576 || MEMORY_ram_size == 1088))
		return FALSE;
	return TRUE;
}

static unsigned char RemoteMonitor_SysInfoOSRevision(void)
{
	if (Atari800_os_version < 0)
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_NONE;
	switch (Atari800_os_version) {
	case SYSROM_A_NTSC:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_800_A_NTSC;
	case SYSROM_A_PAL:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_800_A_PAL;
	case SYSROM_B_NTSC:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_800_B_NTSC;
	case SYSROM_800_CUSTOM:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_800_CUSTOM;
#if EMUOS_ALTIRRA
	case SYSROM_ALTIRRA_800:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_800_ALTIRRA;
#endif
	case SYSROM_AA00R10:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_10;
	case SYSROM_AA01R11:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_11;
	case SYSROM_BB00R1:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_1;
	case SYSROM_BB01R2:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_2;
	case SYSROM_BB02R3:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_3A;
	case SYSROM_BB02R3V4:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_3B;
	case SYSROM_CC01R4:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_5;
	case SYSROM_BB01R3:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_3;
	case SYSROM_BB01R4_OS:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_4;
	case SYSROM_BB01R59:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_59;
	case SYSROM_BB01R59A:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_59A;
	case SYSROM_XL_CUSTOM:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_CUSTOM;
#if EMUOS_ALTIRRA
	case SYSROM_ALTIRRA_XL:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_XL_ALTIRRA;
#endif
	case SYSROM_5200:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_5200_ORIG;
	case SYSROM_5200A:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_5200_A;
	case SYSROM_5200_CUSTOM:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_5200_CUSTOM;
#if EMUOS_ALTIRRA
	case SYSROM_ALTIRRA_5200:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_5200_ALTIRRA;
#endif
	default:
		return REMOTE_MONITOR_SYSINFO_OS_REVISION_NONE;
	}
}

static unsigned char RemoteMonitor_SysInfoBasicRevision(void)
{
	if (Atari800_basic_version < 0)
		return REMOTE_MONITOR_SYSINFO_BASIC_REVISION_NONE;
	switch (Atari800_basic_version) {
	case SYSROM_BASIC_A:
		return REMOTE_MONITOR_SYSINFO_BASIC_REVISION_A;
	case SYSROM_BASIC_B:
		return REMOTE_MONITOR_SYSINFO_BASIC_REVISION_B;
	case SYSROM_BASIC_C:
		return REMOTE_MONITOR_SYSINFO_BASIC_REVISION_C;
	case SYSROM_BASIC_CUSTOM:
		return REMOTE_MONITOR_SYSINFO_BASIC_REVISION_CUSTOM;
#if EMUOS_ALTIRRA
	case SYSROM_ALTIRRA_BASIC:
		return REMOTE_MONITOR_SYSINFO_BASIC_REVISION_ALTIRRA;
#endif
	default:
		return REMOTE_MONITOR_SYSINFO_BASIC_REVISION_NONE;
	}
}

static unsigned char RemoteMonitor_SysInfoBuiltinGameRevision(void)
{
	if (Atari800_xegame_version < 0)
		return REMOTE_MONITOR_SYSINFO_BUILTIN_GAME_REVISION_NONE;
	switch (Atari800_xegame_version) {
	case SYSROM_XEGAME:
		return REMOTE_MONITOR_SYSINFO_BUILTIN_GAME_REVISION_ORIG;
	case SYSROM_XEGAME_CUSTOM:
		return REMOTE_MONITOR_SYSINFO_BUILTIN_GAME_REVISION_CUSTOM;
	default:
		return REMOTE_MONITOR_SYSINFO_BUILTIN_GAME_REVISION_NONE;
	}
}

static void RemoteMonitor_ResetInput(void)
{
	INPUT_RemoteReset();
}

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

void RemoteMonitor_SetVideoEnabled(int enabled)
{
	remote_monitor_video_enabled = enabled ? TRUE : FALSE;
	remote_monitor_video_init_failed = FALSE;
	remote_monitor_video_last_time = 0.0;
	remote_monitor_video_logged = FALSE;
	if (!remote_monitor_video_enabled)
		RemoteMonitor_VideoClose();
}

int RemoteMonitor_VideoEnabled(void)
{
	return remote_monitor_video_enabled;
}

void RemoteMonitor_SetVideoUdpHost(const char *host)
{
	if (host == NULL || host[0] == '\0')
		remote_monitor_video_udp_host[0] = '\0';
	else
		Util_strlcpy(remote_monitor_video_udp_host, host, sizeof(remote_monitor_video_udp_host));
	remote_monitor_video_init_failed = FALSE;
	RemoteMonitor_VideoClose();
}

const char *RemoteMonitor_GetVideoUdpHost(void)
{
	return remote_monitor_video_udp_host[0] != '\0' ? remote_monitor_video_udp_host : NULL;
}

void RemoteMonitor_SetVideoUdpPort(int port)
{
	remote_monitor_video_udp_port = port;
	remote_monitor_video_init_failed = FALSE;
	RemoteMonitor_VideoClose();
}

int RemoteMonitor_GetVideoUdpPort(void)
{
	return remote_monitor_video_udp_port;
}

void RemoteMonitor_SetVideoFps(int fps)
{
	remote_monitor_video_fps = fps;
}

int RemoteMonitor_GetVideoFps(void)
{
	return remote_monitor_video_fps;
}

void RemoteMonitor_SetEnabled(int enabled)
{
	remote_monitor_enabled = enabled ? TRUE : FALSE;
	remote_monitor_init_failed = FALSE;
	remote_monitor_video_init_failed = FALSE;
}

void RemoteMonitor_EnableDefault(void)
{
	const char *default_socket_path = RemoteMonitor_DefaultSocketPath();
	remote_monitor_enabled = TRUE;
	if (default_socket_path == NULL) {
		remote_monitor_transport = REMOTE_MONITOR_TRANSPORT_NONE;
		remote_monitor_socket_path[0] = '\0';
	}
	else {
		remote_monitor_transport = REMOTE_MONITOR_TRANSPORT_SOCKET;
		Util_strlcpy(remote_monitor_socket_path, default_socket_path, sizeof(remote_monitor_socket_path));
	}
	remote_monitor_init_failed = FALSE;
	remote_monitor_video_init_failed = FALSE;
}

void RemoteMonitor_Disable(void)
{
	RemoteMonitor_SetEnabled(FALSE);
	RemoteMonitor_CloseAll();
	RemoteMonitor_VideoClose();
}

int RemoteMonitor_Enabled(void)
{
	return remote_monitor_enabled &&
	       remote_monitor_transport == REMOTE_MONITOR_TRANSPORT_SOCKET &&
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

void RemoteMonitor_VideoFrame(int display_screen)
{
	unsigned char packet[REMOTE_MONITOR_VIDEO_MAX_DATAGRAM];
	unsigned char *dst;
	const UBYTE *screen;
	uint32_t frame_seq;
	double now;
	double interval;
	size_t row_bytes;
	size_t max_payload;
	int rows_per_packet;
	int width;
	int height;
	int left;
	int top;
	int y;

	if (!display_screen)
		return;
	if (!remote_monitor_enabled || !remote_monitor_video_enabled)
		return;
	if (remote_monitor_video_fps <= 0)
		return;
	if (Screen_atari == NULL)
		return;
	if (!RemoteMonitor_VideoInit())
		return;

	now = Util_time();
	interval = 1.0 / (double)remote_monitor_video_fps;
	if (remote_monitor_video_last_time > 0.0 &&
	    (now - remote_monitor_video_last_time) < interval)
		return;
	remote_monitor_video_last_time = now;

	left = Screen_visible_x1;
	top = Screen_visible_y1;
	width = Screen_visible_x2 - Screen_visible_x1;
	height = Screen_visible_y2 - Screen_visible_y1;
	if (width <= 0 || height <= 0) {
		Log_print("Remote Monitor video: Visible screen area is invalid.");
		remote_monitor_video_init_failed = TRUE;
		RemoteMonitor_VideoClose();
		return;
	}
	if (left < 0 || top < 0 ||
	    left + width > Screen_WIDTH ||
	    top + height > Screen_HEIGHT) {
		Log_print("Remote Monitor video: Visible screen area is out of bounds.");
		remote_monitor_video_init_failed = TRUE;
		RemoteMonitor_VideoClose();
		return;
	}
	row_bytes = (size_t)width * 3;
	if (row_bytes > UINT16_MAX) {
		Log_print("Remote Monitor video: Row size exceeds supported payload size.");
		remote_monitor_video_init_failed = TRUE;
		RemoteMonitor_VideoClose();
		return;
	}
	max_payload = REMOTE_MONITOR_VIDEO_MAX_DATAGRAM - REMOTE_MONITOR_VIDEO_HEADER_SIZE;
	if (row_bytes > max_payload) {
		Log_print("Remote Monitor video: Row size exceeds UDP payload limit.");
		remote_monitor_video_init_failed = TRUE;
		RemoteMonitor_VideoClose();
		return;
	}
	rows_per_packet = (int)(max_payload / row_bytes);
	if (rows_per_packet < 1)
		rows_per_packet = 1;

	screen = (const UBYTE *)Screen_atari;
	frame_seq = (uint32_t)Atari800_nframes;

	for (y = 0; y < height; y += rows_per_packet) {
		int rows = height - y;
		size_t data_len;
		unsigned char flags = 0;
		int row;

		if (rows > rows_per_packet)
			rows = rows_per_packet;
		if (y == 0)
			flags |= REMOTE_MONITOR_VIDEO_FLAG_FIRST;
		if (y + rows >= height)
			flags |= REMOTE_MONITOR_VIDEO_FLAG_LAST;

		memcpy(packet, "RMV1", 4);
		packet[4] = 1;
		packet[5] = REMOTE_MONITOR_VIDEO_FORMAT_RGB888;
		packet[6] = flags;
		packet[7] = 0;
		packet[8] = (unsigned char)(frame_seq & 0xff);
		packet[9] = (unsigned char)((frame_seq >> 8) & 0xff);
		packet[10] = (unsigned char)((frame_seq >> 16) & 0xff);
		packet[11] = (unsigned char)((frame_seq >> 24) & 0xff);
		packet[12] = (unsigned char)(width & 0xff);
		packet[13] = (unsigned char)((width >> 8) & 0xff);
		packet[14] = (unsigned char)(height & 0xff);
		packet[15] = (unsigned char)((height >> 8) & 0xff);
		packet[16] = 0;
		packet[17] = 0;
		packet[18] = (unsigned char)(y & 0xff);
		packet[19] = (unsigned char)((y >> 8) & 0xff);
		packet[20] = (unsigned char)(rows & 0xff);
		packet[21] = (unsigned char)((rows >> 8) & 0xff);
		packet[22] = (unsigned char)(row_bytes & 0xff);
		packet[23] = (unsigned char)((row_bytes >> 8) & 0xff);

		dst = packet + REMOTE_MONITOR_VIDEO_HEADER_SIZE;
		for (row = 0; row < rows; row++) {
			const UBYTE *src = screen + (top + y + row) * Screen_WIDTH + left;
			RemoteMonitor_VideoEncodeRow(dst, src, width);
			dst += row_bytes;
		}
		data_len = (size_t)rows * row_bytes;

		if (sendto(remote_monitor_video_fd, packet,
		           REMOTE_MONITOR_VIDEO_HEADER_SIZE + data_len,
		           MSG_DONTWAIT,
		           (struct sockaddr *)&remote_monitor_video_addr,
		           remote_monitor_video_addr_len) < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			Log_print("Remote Monitor video: UDP send failed.");
			remote_monitor_video_init_failed = TRUE;
			RemoteMonitor_VideoClose();
			return;
		}
	}
}

static void RemoteMonitor_CloseClient(struct RemoteMonitorClient *client)
{
	if (client->fd >= 0) {
		close(client->fd);
		client->fd = -1;
	}
	client->len = 0;
	if (!RemoteMonitor_HasClients())
		RemoteMonitor_ResetInput();
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
	RemoteMonitor_ResetInput();
}

static void RemoteMonitor_VideoClose(void)
{
	if (remote_monitor_video_fd >= 0) {
		close(remote_monitor_video_fd);
		remote_monitor_video_fd = -1;
	}
	remote_monitor_video_addr_len = 0;
	remote_monitor_video_last_time = 0.0;
	remote_monitor_video_logged = FALSE;
}

static int RemoteMonitor_VideoInit(void)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *cur;
	char port_buf[16];
	int fd = -1;

	if (remote_monitor_video_fd >= 0)
		return TRUE;
	if (remote_monitor_video_init_failed)
		return FALSE;
	if (remote_monitor_video_udp_host[0] == '\0') {
		Log_print("Remote Monitor video: UDP host is not set.");
		remote_monitor_video_init_failed = TRUE;
		return FALSE;
	}
	if (remote_monitor_video_udp_port < 1 || remote_monitor_video_udp_port > 65535) {
		Log_print("Remote Monitor video: UDP port is invalid.");
		remote_monitor_video_init_failed = TRUE;
		return FALSE;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_family = AF_UNSPEC;
	snprintf(port_buf, sizeof(port_buf), "%d", remote_monitor_video_udp_port);
	if (getaddrinfo(remote_monitor_video_udp_host, port_buf, &hints, &res) != 0) {
		Log_print("Remote Monitor video: UDP host resolution failed.");
		remote_monitor_video_init_failed = TRUE;
		return FALSE;
	}

	for (cur = res; cur != NULL; cur = cur->ai_next) {
		fd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
		if (fd >= 0) {
			remote_monitor_video_addr_len = cur->ai_addrlen;
			memcpy(&remote_monitor_video_addr, cur->ai_addr, cur->ai_addrlen);
			break;
		}
	}
	freeaddrinfo(res);
	if (fd < 0) {
		Log_print("Remote Monitor video: Cannot create UDP socket.");
		remote_monitor_video_init_failed = TRUE;
		return FALSE;
	}

	{
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags >= 0)
			(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	remote_monitor_video_fd = fd;
	if (!remote_monitor_video_logged) {
		Log_print("Remote Monitor video: Streaming UDP to %s:%d at %d FPS.",
			remote_monitor_video_udp_host, remote_monitor_video_udp_port,
			remote_monitor_video_fps);
		remote_monitor_video_logged = TRUE;
	}
	return TRUE;
}

static void RemoteMonitor_VideoEncodeRow(unsigned char *dst, const UBYTE *src, int width)
{
	int x;

	for (x = 0; x < width; x++) {
		int rgb = Colours_table[src[x]];
		dst[0] = (unsigned char)(rgb >> 16);
		dst[1] = (unsigned char)(rgb >> 8);
		dst[2] = (unsigned char)rgb;
		dst += 3;
	}
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
	unsigned char search_pattern[256];
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
		unsigned char flags = 0;
		unsigned char machine_type = RemoteMonitor_StatusMachineType();
		size_t off = 0;
		double emu = Atari800_GetEmulationSeconds();
		double since_reset = Atari800_GetEmulationSecondsSinceReset();

		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"STATUS expects empty payload");
			break;
		}
		if (MONITOR_IsActive() || UI_is_active)
			flags |= REMOTE_MONITOR_STATUS_FLAG_PAUSED;
#ifdef CRASH_MENU
		if (UI_crash_code >= 0)
			flags |= REMOTE_MONITOR_STATUS_FLAG_CRASHED;
#endif
		if (emu < 0.0)
			emu = 0.0;
		if (since_reset < 0.0)
			since_reset = 0.0;
		emu_ms = (uint64_t)(emu * 1000.0 + 0.5);
		reset_ms = (uint64_t)(since_reset * 1000.0 + 0.5);
		state_seq = remote_monitor_state_seq;

		outbuf[off++] = flags;
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
		outbuf[off++] = machine_type;
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

#ifdef MONITOR_BREAK
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_BREAK);
#endif
#ifdef MONITOR_BREAKPOINTS
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_MONITOR_BREAKPOINTS);
#endif
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_EMULATION_ATARI_800);
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_EMULATION_XE_XL);
			REMOTE_MONITOR_ADD_CAP(REMOTE_MONITOR_CAP_EMULATION_ATARI_5200);

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
			Atari800_RequestMonitorRemoteEnabled();
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
			Atari800_RequestMonitorRemoteEnabled();
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
			Atari800_RequestMonitorRemoteEnabled();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		break;
	case REMOTE_MONITOR_CMD_STEP_OVER:
#ifdef MONITOR_BREAK
		if (len != 0 && len != 2) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"STEP_OVER expects empty or 2-byte payload");
			break;
		}
		if (len == 2)
			CPU_regPC = (UWORD)(payload[0] | (payload[1] << 8));
		MONITOR_RequestAction(MONITOR_ACTION_STEP_OVER);
		if (!MONITOR_IsActive())
			Atari800_RequestMonitorRemoteEnabled();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
#else
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
			"STEP_OVER is not supported in this build");
#endif
		break;
	case REMOTE_MONITOR_CMD_RUN_UNTIL_RETURN:
#ifdef MONITOR_BREAK
		if (len != 0 && len != 2) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"RUN_UNTIL_RETURN expects empty or 2-byte payload");
			break;
		}
		if (len == 2)
			CPU_regPC = (UWORD)(payload[0] | (payload[1] << 8));
		MONITOR_RequestAction(MONITOR_ACTION_RUN_RETURN);
		if (!MONITOR_IsActive())
			Atari800_RequestMonitorRemoteEnabled();
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
#else
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
			"RUN_UNTIL_RETURN is not supported in this build");
#endif
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
	case REMOTE_MONITOR_CMD_GTIA_STATE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"GTIA_STATE expects empty payload");
			break;
		}
		{
			size_t off = 0;
			outbuf[off++] = GTIA_HPOSP0;
			outbuf[off++] = GTIA_HPOSP1;
			outbuf[off++] = GTIA_HPOSP2;
			outbuf[off++] = GTIA_HPOSP3;
			outbuf[off++] = GTIA_HPOSM0;
			outbuf[off++] = GTIA_HPOSM1;
			outbuf[off++] = GTIA_HPOSM2;
			outbuf[off++] = GTIA_HPOSM3;
			outbuf[off++] = GTIA_SIZEP0;
			outbuf[off++] = GTIA_SIZEP1;
			outbuf[off++] = GTIA_SIZEP2;
			outbuf[off++] = GTIA_SIZEP3;
			outbuf[off++] = GTIA_SIZEM;
			outbuf[off++] = GTIA_GRAFP0;
			outbuf[off++] = GTIA_GRAFP1;
			outbuf[off++] = GTIA_GRAFP2;
			outbuf[off++] = GTIA_GRAFP3;
			outbuf[off++] = GTIA_GRAFM;
			outbuf[off++] = GTIA_COLPM0;
			outbuf[off++] = GTIA_COLPM1;
			outbuf[off++] = GTIA_COLPM2;
			outbuf[off++] = GTIA_COLPM3;
			outbuf[off++] = GTIA_COLPF0;
			outbuf[off++] = GTIA_COLPF1;
			outbuf[off++] = GTIA_COLPF2;
			outbuf[off++] = GTIA_COLPF3;
			outbuf[off++] = GTIA_COLBK;
			outbuf[off++] = GTIA_PRIOR;
			outbuf[off++] = GTIA_VDELAY;
			outbuf[off++] = GTIA_GRACTL;
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		}
		break;
	case REMOTE_MONITOR_CMD_ANTIC_STATE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"ANTIC_STATE expects empty payload");
			break;
		}
		{
			size_t off = 0;
			outbuf[off++] = ANTIC_DMACTL;
			outbuf[off++] = ANTIC_CHACTL;
			outbuf[off++] = (unsigned char)(ANTIC_dlist & 0xff);
			outbuf[off++] = (unsigned char)((ANTIC_dlist >> 8) & 0xff);
			outbuf[off++] = ANTIC_HSCROL;
			outbuf[off++] = ANTIC_VSCROL;
			outbuf[off++] = ANTIC_PMBASE;
			outbuf[off++] = ANTIC_CHBASE;
			outbuf[off++] = ANTIC_GetByte(ANTIC_OFFSET_VCOUNT, TRUE);
			outbuf[off++] = ANTIC_NMIEN;
			outbuf[off++] = (unsigned char)(ANTIC_ypos & 0xff);
			outbuf[off++] = (unsigned char)((ANTIC_ypos >> 8) & 0xff);
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		}
		break;
	case REMOTE_MONITOR_CMD_CART_STATE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"CART_STATE expects empty payload");
			break;
		}
		{
			const CARTRIDGE_image_t *carts[2];
			size_t off = 0;
			int ci;
			carts[0] = &CARTRIDGE_main;
			carts[1] = &CARTRIDGE_piggyback;
			outbuf[off++] = (unsigned char)(CARTRIDGE_autoreboot ? 1 : 0);
			for (ci = 0; ci < 2; ci++) {
				const CARTRIDGE_image_t *cart = carts[ci];
				int16_t type = (int16_t)cart->type;
				uint32_t state = (uint32_t)cart->state;
				uint32_t size_kb = (uint32_t)cart->size;
				outbuf[off++] = (unsigned char)(cart->type != CARTRIDGE_NONE ? 1 : 0);
				outbuf[off++] = (unsigned char)(type & 0xff);
				outbuf[off++] = (unsigned char)((type >> 8) & 0xff);
				outbuf[off++] = (unsigned char)(state & 0xff);
				outbuf[off++] = (unsigned char)((state >> 8) & 0xff);
				outbuf[off++] = (unsigned char)((state >> 16) & 0xff);
				outbuf[off++] = (unsigned char)((state >> 24) & 0xff);
				outbuf[off++] = (unsigned char)(size_kb & 0xff);
				outbuf[off++] = (unsigned char)((size_kb >> 8) & 0xff);
				outbuf[off++] = (unsigned char)((size_kb >> 16) & 0xff);
				outbuf[off++] = (unsigned char)((size_kb >> 24) & 0xff);
				outbuf[off++] = (unsigned char)(cart->raw ? 1 : 0);
			}
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		}
		break;
	case REMOTE_MONITOR_CMD_JUMPS:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"JUMPS expects empty payload");
			break;
		}
		{
			size_t off = 0;
			unsigned int i;
			outbuf[off++] = (unsigned char)CPU_REMEMBER_JMP_STEPS;
			for (i = 0; i < CPU_REMEMBER_JMP_STEPS; i++) {
				UWORD pc = CPU_remember_JMP[(CPU_remember_jmp_curpos + i) % CPU_REMEMBER_JMP_STEPS];
				outbuf[off++] = (unsigned char)(pc & 0xff);
				outbuf[off++] = (unsigned char)((pc >> 8) & 0xff);
			}
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		}
		break;
	case REMOTE_MONITOR_CMD_PIA_STATE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"PIA_STATE expects empty payload");
			break;
		}
		outbuf[0] = PIA_PACTL;
		outbuf[1] = PIA_PBCTL;
		outbuf[2] = PIA_PORTA;
		outbuf[3] = PIA_PORTB;
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, 4);
		break;
	case REMOTE_MONITOR_CMD_POKEY_STATE:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"POKEY_STATE expects empty payload");
			break;
		}
		{
			size_t off = 0;
			int stereo_enabled = 0;
#ifdef STEREO_SOUND
			stereo_enabled = POKEYSND_stereo_enabled;
#endif
			outbuf[off++] = (unsigned char)(stereo_enabled ? 1 : 0);
			outbuf[off++] = POKEY_AUDF[POKEY_CHAN1];
			outbuf[off++] = POKEY_AUDF[POKEY_CHAN2];
			outbuf[off++] = POKEY_AUDF[POKEY_CHAN3];
			outbuf[off++] = POKEY_AUDF[POKEY_CHAN4];
			outbuf[off++] = POKEY_AUDC[POKEY_CHAN1];
			outbuf[off++] = POKEY_AUDC[POKEY_CHAN2];
			outbuf[off++] = POKEY_AUDC[POKEY_CHAN3];
			outbuf[off++] = POKEY_AUDC[POKEY_CHAN4];
			outbuf[off++] = POKEY_AUDCTL[0];
			outbuf[off++] = POKEY_KBCODE;
			outbuf[off++] = POKEY_IRQEN;
			outbuf[off++] = POKEY_IRQST;
			outbuf[off++] = POKEY_SKSTAT;
			outbuf[off++] = POKEY_SKCTL;
			if (stereo_enabled) {
				outbuf[off++] = POKEY_AUDF[POKEY_CHAN1 + POKEY_CHIP2];
				outbuf[off++] = POKEY_AUDF[POKEY_CHAN2 + POKEY_CHIP2];
				outbuf[off++] = POKEY_AUDF[POKEY_CHAN3 + POKEY_CHIP2];
				outbuf[off++] = POKEY_AUDF[POKEY_CHAN4 + POKEY_CHIP2];
				outbuf[off++] = POKEY_AUDC[POKEY_CHAN1 + POKEY_CHIP2];
				outbuf[off++] = POKEY_AUDC[POKEY_CHAN2 + POKEY_CHIP2];
				outbuf[off++] = POKEY_AUDC[POKEY_CHAN3 + POKEY_CHIP2];
				outbuf[off++] = POKEY_AUDC[POKEY_CHAN4 + POKEY_CHIP2];
				outbuf[off++] = POKEY_AUDCTL[1];
			}
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		}
		break;
	case REMOTE_MONITOR_CMD_STACK:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"STACK expects empty payload");
			break;
		}
		{
			size_t off = 0;
			int sp;
			unsigned int count = (unsigned int)(0xff - CPU_regS);
			outbuf[off++] = CPU_regS;
			outbuf[off++] = (unsigned char)count;
			for (sp = (int)CPU_regS + 1; sp <= 0xff; sp++) {
				outbuf[off++] = (unsigned char)sp;
				outbuf[off++] = MEMORY_dGetByte((UWORD)(0x0100 + sp));
			}
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		}
		break;
	case REMOTE_MONITOR_CMD_BBRK:
#ifdef MONITOR_BREAK
		if (len != 0 && len != 1) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"BBRK expects 0 or 1-byte payload");
			break;
		}
		if (len == 1) {
			if (payload[0] > 1) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"BBRK payload must be 0 or 1");
				break;
			}
			MONITOR_break_brk = payload[0];
			RemoteMonitor_NotifyStateChanged();
		}
		outbuf[0] = MONITOR_break_brk;
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, 1);
#else
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
			"BBRK is not supported in this build");
#endif
		break;
	case REMOTE_MONITOR_CMD_BLINE:
#if defined(MONITOR_BREAK) || !defined(NO_YPOS_BREAK_FLICKER)
		if (len != 0 && len != 2) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"BLINE expects 0 or 2-byte payload");
			break;
		}
		if (len == 2) {
			ANTIC_break_ypos = (int)(payload[0] | (payload[1] << 8));
			RemoteMonitor_NotifyStateChanged();
		}
		outbuf[0] = (unsigned char)(ANTIC_break_ypos & 0xff);
		outbuf[1] = (unsigned char)((ANTIC_break_ypos >> 8) & 0xff);
		if (ANTIC_break_ypos >= 1008 && ANTIC_break_ypos <= 1247)
			outbuf[2] = 2;
#ifdef MONITOR_BREAK
		else if (ANTIC_break_ypos >= 0 && ANTIC_break_ypos <= 311)
			outbuf[2] = 1;
#endif
		else
			outbuf[2] = 0;
		RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, 3);
#else
		RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
			"BLINE is not supported in this build");
#endif
		break;
	case REMOTE_MONITOR_CMD_SYSINFO:
		if (len != 0) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"SYSINFO expects empty payload");
			break;
		}
		{
			unsigned char flags = 0;
			if (RemoteMonitor_SysInfoBasicEnabled())
				flags |= REMOTE_MONITOR_SYSINFO_FLAG_BASIC_ENABLED;
			if (Atari800_tv_mode == Atari800_TV_PAL)
				flags |= REMOTE_MONITOR_SYSINFO_FLAG_TV_PAL;
			outbuf[0] = flags;
			outbuf[1] = RemoteMonitor_SysInfoMachineFamily();
			outbuf[2] = RemoteMonitor_SysInfoOSRevision();
			outbuf[3] = RemoteMonitor_SysInfoBasicRevision();
			outbuf[4] = RemoteMonitor_SysInfoBuiltinGameRevision();
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, 5);
		}
		break;
	case REMOTE_MONITOR_CMD_SEARCH:
		if (len < 6) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"SEARCH expects at least 6-byte payload");
			break;
		}
		{
			unsigned char mode = payload[0];
			UWORD start = (UWORD)(payload[1] | (payload[2] << 8));
			UWORD end = (UWORD)(payload[3] | (payload[4] << 8));
			unsigned int pat_len = payload[5];
			uint32_t found_total = 0;
			uint16_t returned = 0;
			uint16_t ret_limit = (uint16_t)((sizeof(outbuf) - 6) / 2);
			size_t off = 6;
			uint32_t addr;
			unsigned int i;

			if (mode != REMOTE_MONITOR_SEARCH_MODE_BYTES &&
			    mode != REMOTE_MONITOR_SEARCH_MODE_ASCII &&
			    mode != REMOTE_MONITOR_SEARCH_MODE_SCREENCODE) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"SEARCH mode is invalid");
				break;
			}
			if (pat_len == 0 || pat_len > sizeof(search_pattern)) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"SEARCH pattern must have 1..255 bytes");
				break;
			}
			if (len != (uint16_t)(6 + pat_len)) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
					"SEARCH payload length mismatch");
				break;
			}
			memcpy(search_pattern, payload + 6, pat_len);
			if (mode == REMOTE_MONITOR_SEARCH_MODE_SCREENCODE) {
				for (i = 0; i < pat_len; i++) {
					UBYTE c = search_pattern[i];
					UBYTE bit7 = (UBYTE)(c & 0x80);
					c = (UBYTE)(c & 0x7f);
					if (c < 32)
						c = (UBYTE)(c + 64);
					else if (c < 96)
						c = (UBYTE)(c - 32);
					search_pattern[i] = (UBYTE)(c | bit7);
				}
			}
			for (addr = start; addr <= end; addr++) {
				unsigned int j;
				int match = TRUE;
				for (j = 0; j < pat_len; j++) {
					if (MEMORY_SafeGetByte((UWORD)(addr + j)) != search_pattern[j]) {
						match = FALSE;
						break;
					}
				}
				if (!match)
					continue;
				found_total++;
				if (returned < ret_limit) {
					outbuf[off++] = (unsigned char)(addr & 0xff);
					outbuf[off++] = (unsigned char)((addr >> 8) & 0xff);
					returned++;
				}
			}
			outbuf[0] = (unsigned char)(found_total & 0xff);
			outbuf[1] = (unsigned char)((found_total >> 8) & 0xff);
			outbuf[2] = (unsigned char)((found_total >> 16) & 0xff);
			outbuf[3] = (unsigned char)((found_total >> 24) & 0xff);
			outbuf[4] = (unsigned char)(returned & 0xff);
			outbuf[5] = (unsigned char)((returned >> 8) & 0xff);
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, outbuf, (uint16_t)off);
		}
		break;
	case REMOTE_MONITOR_CMD_SET_REG:
		if (len != 3) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"SET_REG expects 3-byte payload");
			break;
		}
		{
			unsigned char target = payload[0];
			UWORD value = (UWORD)(payload[1] | (payload[2] << 8));
			int ok = TRUE;
			switch (target) {
			case REMOTE_MONITOR_SET_REG_PC:
				CPU_regPC = value;
				break;
			case REMOTE_MONITOR_SET_REG_A:
				CPU_regA = (UBYTE)value;
				break;
			case REMOTE_MONITOR_SET_REG_X:
				CPU_regX = (UBYTE)value;
				break;
			case REMOTE_MONITOR_SET_REG_Y:
				CPU_regY = (UBYTE)value;
				break;
			case REMOTE_MONITOR_SET_REG_S:
				CPU_regS = (UBYTE)value;
				break;
			case REMOTE_MONITOR_SET_REG_N:
			case REMOTE_MONITOR_SET_REG_V:
			case REMOTE_MONITOR_SET_REG_D:
			case REMOTE_MONITOR_SET_REG_I:
			case REMOTE_MONITOR_SET_REG_Z:
			case REMOTE_MONITOR_SET_REG_C:
				if (value > 1) {
					RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
						"SET_REG flag value must be 0 or 1");
					ok = FALSE;
					break;
				}
				CPU_GetStatus();
				switch (target) {
				case REMOTE_MONITOR_SET_REG_N:
					if (value == 0)
						CPU_ClrN;
					else
						CPU_SetN;
					break;
				case REMOTE_MONITOR_SET_REG_V:
					if (value == 0)
						CPU_ClrV;
					else
						CPU_SetV;
					break;
				case REMOTE_MONITOR_SET_REG_D:
					if (value == 0)
						CPU_ClrD;
					else
						CPU_SetD;
					break;
				case REMOTE_MONITOR_SET_REG_I:
					if (value == 0)
						CPU_ClrI;
					else
						CPU_SetI;
					break;
				case REMOTE_MONITOR_SET_REG_Z:
					if (value == 0)
						CPU_ClrZ;
					else
						CPU_SetZ;
					break;
				default:
					if (value == 0)
						CPU_ClrC;
					else
						CPU_SetC;
					break;
				}
				CPU_PutStatus();
				break;
			default:
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"SET_REG target is invalid");
				ok = FALSE;
				break;
			}
			if (ok) {
				RemoteMonitor_NotifyStateChanged();
				RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
			}
		}
		break;
	case REMOTE_MONITOR_CMD_INPUT_KEY:
		if (len != 6) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"INPUT_KEY expects a 6-byte payload.");
			break;
		}
		{
			unsigned char action = payload[0];
			unsigned char keyspace = payload[1];
			unsigned char mods = payload[2];
			unsigned char consol = payload[3];
			int keycode;
			if (keyspace != REMOTE_MONITOR_INPUT_KEYSPACE_HID) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"INPUT_KEY keyspace must be HID (1).");
				break;
			}
			if ((mods & ~REMOTE_MONITOR_INPUT_MOD_MASK) != 0) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"INPUT_KEY modifiers are invalid.");
				break;
			}
			if (consol != 0) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"INPUT_KEY console mask must be 0.");
				break;
			}
			if (action != REMOTE_MONITOR_INPUT_KEY_ACTION_UP &&
			    action != REMOTE_MONITOR_INPUT_KEY_ACTION_DOWN) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"INPUT_KEY action is invalid.");
				break;
			}

			keycode = (int)(payload[4] | (payload[5] << 8));
			if (action == REMOTE_MONITOR_INPUT_KEY_ACTION_DOWN) {
				if (keycode == 0 || keycode > 255) {
					RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
						"INPUT_KEY keycode must be 1..255 for HID key down.");
					break;
				}
			}
			else {
				if (keycode > 255) {
					RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
						"INPUT_KEY keycode must be 0..255 for HID key up.");
					break;
				}
			}
			INPUT_RemoteKeyEvent(keyspace, action, keycode, (int)mods);
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		}
		break;
	case REMOTE_MONITOR_CMD_INPUT_JOYSTICKS:
		if (len != 2) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"INPUT_JOYSTICKS expects a 2-byte payload.");
			break;
		}
		{
			unsigned char joy1 = payload[0];
			unsigned char joy2 = payload[1];

			if ((joy1 & ~REMOTE_MONITOR_INPUT_JOY_MASK) != 0 ||
			    (joy2 & ~REMOTE_MONITOR_INPUT_JOY_MASK) != 0) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"INPUT_JOYSTICKS mask is invalid.");
				break;
			}
			INPUT_RemoteSetJoysticks(joy1, joy2);
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		}
		break;
	case REMOTE_MONITOR_CMD_INPUT_SPECIAL:
		if (len != 1) {
			RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_LENGTH,
				"INPUT_SPECIAL expects a 1-byte payload.");
			break;
		}
		{
			unsigned char special = payload[0];

			if ((special & ~REMOTE_MONITOR_INPUT_SPECIAL_MASK) != 0) {
				RemoteMonitor_ReplyError(client, REMOTE_MONITOR_ERR_INVALID_VALUE,
					"INPUT_SPECIAL mask is invalid.");
				break;
			}
			INPUT_RemoteSetSpecial(special);
			RemoteMonitor_Reply(client->fd, REMOTE_MONITOR_OK, NULL, 0);
		}
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

void RemoteMonitor_SetVideoEnabled(int enabled)
{
	(void)enabled;
}

int RemoteMonitor_VideoEnabled(void)
{
	return 0;
}

void RemoteMonitor_SetVideoUdpHost(const char *host)
{
	(void)host;
}

const char *RemoteMonitor_GetVideoUdpHost(void)
{
	return NULL;
}

void RemoteMonitor_SetVideoUdpPort(int port)
{
	(void)port;
}

int RemoteMonitor_GetVideoUdpPort(void)
{
	return 0;
}

void RemoteMonitor_SetVideoFps(int fps)
{
	(void)fps;
}

int RemoteMonitor_GetVideoFps(void)
{
	return 0;
}

void RemoteMonitor_SetEnabled(int enabled)
{
	(void)enabled;
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

void RemoteMonitor_VideoFrame(int display_screen)
{
	(void)display_screen;
}

#endif
