/*
 * selfrestart.c - process self-restart helpers
 *
 * Copyright (C) 2026 Atari800 development team (see DOC/CREDITS)
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "selfrestart.h"

#include <stdlib.h>
#include <string.h>
#if defined(HAVE_UNISTD_H) && !defined(HAVE_WINDOWS_H)
#include <errno.h>
#include <unistd.h>
#endif

#include "atari.h"
#include "log.h"
#include "util.h"

static int atari800_saved_argc = 0;
static char **atari800_saved_argv = NULL;

static void SelfRestart_ClearSavedArgv(void)
{
	int i;
	if (atari800_saved_argv == NULL)
		return;
	for (i = 0; i < atari800_saved_argc; i++) {
		free(atari800_saved_argv[i]);
		atari800_saved_argv[i] = NULL;
	}
	free(atari800_saved_argv);
	atari800_saved_argv = NULL;
	atari800_saved_argc = 0;
}

void SelfRestart_SaveProcessArgv(int argc, char *argv[])
{
	int i;
	char **copy;

	SelfRestart_ClearSavedArgv();
	if (argc <= 0 || argv == NULL || argv[0] == NULL)
		return;

	copy = (char **)calloc((size_t)argc + 1, sizeof(char *));
	if (copy == NULL) {
		Log_print("Failed to save startup arguments: out of memory.");
		return;
	}

	for (i = 0; i < argc; i++) {
		if (argv[i] == NULL)
			break;
		copy[i] = Util_strdup(argv[i]);
	}
	if (i == 0 || copy[0] == NULL || copy[0][0] == '\0') {
		int j;
		for (j = 0; j < i; j++)
			free(copy[j]);
		free(copy);
		Log_print("Failed to save startup arguments: invalid argv[0].");
		return;
	}

	atari800_saved_argv = copy;
	atari800_saved_argc = i;
}

int Atari800_CanRestartProcess(void)
{
#if defined(HAVE_UNISTD_H) && !defined(HAVE_WINDOWS_H)
	return atari800_saved_argc > 0
		&& atari800_saved_argv != NULL
		&& atari800_saved_argv[0] != NULL
		&& atari800_saved_argv[0][0] != '\0';
#else
	return FALSE;
#endif
}

int Atari800_RestartProcess(void)
{
#if defined(HAVE_UNISTD_H) && !defined(HAVE_WINDOWS_H)
	if (!Atari800_CanRestartProcess()) {
		Log_print("Process restart is not available.");
		return FALSE;
	}

	Atari800_Exit(FALSE);
	execvp(atari800_saved_argv[0], atari800_saved_argv);
	Log_print("Failed to restart process \"%s\": %s.",
		atari800_saved_argv[0], strerror(errno));
	exit(1);
#else
	return FALSE;
#endif
}
