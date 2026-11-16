#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../menu.h"
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
