#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "../../menu.h"
#include "../../user_io.h"
#include "../../fpga_io.h"
#include "../physical_disc/physical_disc_launch.h"
#include "../physical_disc/physical_disc.h"
#include "vcd.h"

// INFO.VCD / INFO.SVD is always mastered at MSF 00:04:00 (LBA 150).
#define VCD_INFO_LBA 150

int vcd_player_installed(void)
{
	return access(MEDIACENTER_PATH, X_OK) == 0 || access(VCD_PLAYER_PATH, X_OK) == 0;
}

int vcd_disc_detect(void)
{
	uint8_t info[2048];
	if (physical_disc_read_data2048(VCD_INFO_LBA, info)) return 0;

	return !memcmp(info, "VIDEO_CD", 8) || !memcmp(info, "SUPERVCD", 8) || !memcmp(info, "HQ-VCD  ", 8);
}

int vcd_launch_player(const char *device)
{
	int mc = access(MEDIACENTER_PATH, X_OK) == 0;
	const char *path = mc ? MEDIACENTER_PATH : VCD_PLAYER_PATH;
	const char *process = mc ? MEDIACENTER_PROCESS : VCD_PLAYER_PROCESS;

	char command[256];
	if (device && *device) snprintf(command, sizeof(command), "%s %s >/tmp/%s.log 2>&1", path, device, process);
	else snprintf(command, sizeof(command), "%s >/tmp/%s.log 2>&1", path, process);

	printf("DISC: Video CD detected, launching %s\n", path);
	return menu_launch_graphical_script_command(command, "Video CD", process);
}

int mediacenter_installed(void)
{
	return access(MEDIACENTER_PATH, X_OK) == 0;
}

int mediacenter_launch_dvd(const char *device)
{
	char command[256];
	snprintf(command, sizeof(command), "%s %s >/tmp/%s.log 2>&1", MEDIACENTER_PATH,
		device && *device ? device : "/dev/sr0", MEDIACENTER_PROCESS);
	printf("DISC: DVD-Video detected, launching %s\n", MEDIACENTER_PATH);
	return menu_launch_graphical_script_command(command, "DVD", MEDIACENTER_PROCESS);
}

int mediacenter_launch_disc(const char *device)
{
	char command[256];
	snprintf(command, sizeof(command), "%s %s >/tmp/%s.log 2>&1", MEDIACENTER_PATH,
		device && *device ? device : "/dev/sr0", MEDIACENTER_PROCESS);
	printf("DISC: launching %s for the files on the disc\n", MEDIACENTER_PATH);
	return menu_launch_graphical_script_command(command, "MediaCenter", MEDIACENTER_PROCESS);
}

// MiSTer MediaCenter from an MGL file in the core list.
#define MEDIACENTER_SCRIPT "/media/fat/Scripts/Mister_Media_Center.sh"
#define MEDIACENTER_MGL_PENDING "/tmp/mediacenter_mgl_pending"

static int mediacenter_start_script(void)
{
	char command[256];
	// Never on top of a script or player already running (for example
	// MediaCenter opened for a disc): that would lose its pipe.
	if (menu_script_busy())
	{
		printf("MGL: a script is already running, MiSTer MediaCenter not started\n");
		return 0;
	}
	// The script (as from the Scripts menu): the player's splash and file
	// list, its log in /tmp/mediacenter.log, the console restored at the end.
	// Without the script the player is started the same way directly.
	if (access(MEDIACENTER_SCRIPT, R_OK) == 0)
		snprintf(command, sizeof(command), "/bin/bash %s >/dev/null 2>&1", MEDIACENTER_SCRIPT);
	else if (mediacenter_installed())
		snprintf(command, sizeof(command), "%s >/tmp/%s.log 2>&1", MEDIACENTER_PATH, MEDIACENTER_PROCESS);
	else
	{
		printf("MGL: MiSTer MediaCenter is not installed (%s)\n", MEDIACENTER_PATH);
		Info("MiSTer MediaCenter not installed", 3000);
		return 0;
	}
	printf("MGL: launching MiSTer MediaCenter: %s\n", command);
	return menu_launch_graphical_script_command(command, "MediaCenter", MEDIACENTER_PROCESS);
}

int mediacenter_launch_from_mgl(void)
{
	if (is_menu()) return mediacenter_start_script();

	// Inside a core: back to the menu (this Main is started again there),
	// the player opens as soon as the menu is ready.
	FILE *f = fopen(MEDIACENTER_MGL_PENDING, "w");
	if (f) fclose(f);
	printf("MGL: MiSTer MediaCenter asked from a core: loading the menu first\n");
	fpga_load_rbf("menu.rbf");
	return 1;
}

void mediacenter_mgl_tick(void)
{
	struct stat st;
	if (stat(MEDIACENTER_MGL_PENDING, &st)) return;
	// A disc being detected or started goes first, and a script or player
	// already running is never replaced: the request waits (up to 60 s).
	if (physical_disc_launch_busy() || menu_script_busy()) return;
	unlink(MEDIACENTER_MGL_PENDING);
	// Only a request made just before this menu started (not a leftover).
	if (time(NULL) - st.st_mtime > 60)
	{
		printf("MGL: old MiSTer MediaCenter request ignored\n");
		return;
	}
	mediacenter_start_script();
}
