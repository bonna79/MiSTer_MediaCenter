#ifndef MISTER_VCD_H
#define MISTER_VCD_H

// Video CD support for Physical Disc Auto Disc Detection.
//
// Video CDs hold MPEG-1 streams that no console core can decode, so they are
// played by the standalone HPS application tools/vcd_player, installed at
// VCD_PLAYER_PATH. Without that file nothing changes: Video CDs are handled
// exactly as before.

// MediaCenter (tools/mediacenter) also plays Video CDs and is preferred
// when installed; vcd_player is the fallback.
#define MEDIACENTER_PATH "/media/fat/linux/mediacenter"
#define MEDIACENTER_PROCESS "mediacenter"
#define VCD_PLAYER_PATH "/media/fat/linux/vcd_player"
#define VCD_PLAYER_PROCESS "vcd_player"

int vcd_player_installed(void);

// Requires the drive to be open through physical_disc_open().
// Returns 1 for Video CD, Super Video CD and HQ-VCD discs (any region).
int vcd_disc_detect(void);

// Queues the player as a graphical script (drive must be closed first).
int vcd_launch_player(const char *device);

// MiSTer MediaCenter also plays DVD-Video discs, menus included.
int mediacenter_installed(void);
int mediacenter_launch_dvd(const char *device);
// Data disc with video or music files: MediaCenter opens its file list.
int mediacenter_launch_disc(const char *device);

// An MGL file whose <rbf> is "mediacenter" (for example
// _Utility/Mister_Media_Center.mgl) opens MiSTer MediaCenter from the core
// list, like Scripts/Mister_Media_Center.sh. Started outside the menu, it
// returns to the menu first and opens the player from there.
#define MEDIACENTER_MGL_RBF "mediacenter"
int mediacenter_launch_from_mgl(void);
// Called from the menu's periodic tick: opens the player when an MGL asked
// for it from inside a core.
void mediacenter_mgl_tick(void);

#endif
