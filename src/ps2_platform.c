/*
	ps2_platform.c

	PlayStation 2 low level support: IOP reset, embedded IOP module
	loading, USB 1.1 delay/synchronization, and file path resolution.
*/

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <debug.h>
#include <delaythread.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "irx_embedded.h"
#include "ps2_platform.h"
#include "ps2_video.h"

static int DebugScreenReady = 0;
static int BootTraceFd = -1;
static int BootTraceHasStorage = 0;
static int ModuleLoadFailed = 0;
static char LastModuleError[64] = "";
static const char *BootStage = "platform init";

void PS2_SetStage(const char *stage)
{
	if (stage != NULL) {
		BootStage = stage;
		PS2_BootTrace("stage: ");
		PS2_BootTrace(stage);
		PS2_BootTrace("\n");
	}
}

const char *PS2_GetStage(void)
{
	return BootStage;
}

int PS2_BootTraceStart(void)
{
	static const char *const paths[] = {
		"mass:/BOOTLOG.TXT", "mass0:/BOOTLOG.TXT",
		"mass1:/BOOTLOG.TXT", "mass2:/BOOTLOG.TXT",
		"mass3:/BOOTLOG.TXT", "mass4:/BOOTLOG.TXT",
		"mass5:/BOOTLOG.TXT", "mass6:/BOOTLOG.TXT",
		"mass7:/BOOTLOG.TXT", "mass8:/BOOTLOG.TXT",
		"mass9:/BOOTLOG.TXT"
	};
	int i;

	for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); ++i) {
		BootTraceFd = open(paths[i], O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (BootTraceFd >= 0) {
			BootTraceHasStorage = 1;
			PS2_BootTrace("Mini vMac PS2 boot trace\n");
			return 1;
		}
	}
	return 0;
}

void PS2_BootTraceStop(void)
{
	/* Do not close BDM descriptors: close() can hang on the real IOP.
	   Stopping writes is enough; reset will reclaim the handle. */
	BootTraceFd = -1;
}

void PS2_BootTrace(const char *message)
{
	/* BOOTLOG is a boot diagnostic only. Repeated writes during the
	   emulation keep waking the USB stack and are unnecessary because
	   live PC/tick diagnostics are already on screen. */
	if ((BootTraceFd >= 0) && (message != NULL)) {
		if (strncmp(message, "Emulation heartbeat", 19) == 0
			|| strncmp(message, "PC held at ", 11) == 0) {
			return;
		}
		const char *p = message;
		int left = (int)strlen(message);

		while (left > 0) {
			int written = (int)write(BootTraceFd, p, (size_t)left);
			if (written <= 0) {
				break;
			}
			p += written;
			left -= written;
		}
	}
}

#define PS2_MAX_RECORDED_MODULES 16
static char ModuleNames[PS2_MAX_RECORDED_MODULES][20];
static int ModuleResults[PS2_MAX_RECORDED_MODULES];
static int ModuleCount = 0;

static int PS2_LoadEmbedded(const char *name,
	unsigned char *data, unsigned int size)
{
	int ret;

	ret = SifExecModuleBuffer(data, size, 0, NULL, NULL);
	if (ModuleCount < PS2_MAX_RECORDED_MODULES) {
		strncpy(ModuleNames[ModuleCount], name,
			sizeof(ModuleNames[ModuleCount]) - 1);
		ModuleNames[ModuleCount][sizeof(ModuleNames[ModuleCount]) - 1] = 0;
		ModuleResults[ModuleCount] = ret;
		++ModuleCount;
	}
	if (ret < 0) {
		ModuleLoadFailed = 1;
		snprintf(LastModuleError, sizeof(LastModuleError),
			"%s (%d)", name, ret);
	}
	return ret;
}

void PS2_DumpModuleStatus(void)
{
	int i;

	PS2_DebugScreenInit();
	scr_printf("[DEBUG] IOP modules loaded: %d\n", ModuleCount);
	for (i = 0; i < ModuleCount; ++i) {
		scr_printf("  %-16s %d\n", ModuleNames[i], ModuleResults[i]);
	}
	if (LastModuleError[0] != 0) {
		scr_printf("[!] IRX failure: %s\n", LastModuleError);
	}
}

void PS2_DebugScreenInit(void)
{
	if (! DebugScreenReady) {
		init_scr();
		DebugScreenReady = 1;
	}
}

void PS2_Panic(const char *message)
{
	/*
		If gsKit is already running, keep the same display path and
		append the panic to the boot log.  Switching back to the
		ps2sdk text console here erased the boot log and made it
		impossible to see which stage failed on real hardware.
	*/
	if (PS2Video_IsReady()) {
		char line[160];

		PS2Video_BootLog("\n*** SYSTEM PANIC ***\n");
		if (message != NULL) {
			PS2Video_BootLog(message);
			PS2Video_BootLog("\n");
		}
		PS2Video_BootLog("Stage: ");
		PS2Video_BootLog(PS2_GetStage());
		PS2Video_BootLog("\n");
		(void) line;

		for (;;) {
			SleepThread();
		}
	}

	PS2_DebugScreenInit();
	scr_clear();
	scr_printf("==================================================\n");
	scr_printf("  Mini vMac PS2 - System Halt / Panic\n");
	scr_printf("==================================================\n\n");
	scr_printf("Error: %s\n", message);
	scr_printf("Stage: %s\n\n", PS2_GetStage());
	if (LastModuleError[0] != 0) {
		scr_printf("Last IOP module error: %s\n\n", LastModuleError);
	}
	scr_printf("USB contents seen by the PS2:\n");
	PS2_DumpStorage();
	scr_printf("\nCheck your USB drive (FAT32) or Memory Card:\n");
	scr_printf("  - Macintosh ROM: mass:/vMac.ROM or mass:/minivmac/vMac.ROM\n");
	scr_printf("                   (also accepts Mac-Plus.ROM, vmac.rom)\n");
	scr_printf("  - Boot Disks:    mass:/disk1.dsk, disk1.img, System.dsk, etc.\n\n");
	scr_printf("Note: Ensure the USB drive is formatted as FAT32 / FAT16.\n");
	scr_printf("The console is halted. Please reset the PlayStation 2.\n");

	for (;;) {
		SleepThread();
	}
}

void PS2_PlatformInit(void)
{
	SifInitRpc(0);

	/*
		A full IOP reset gives us a clean, known state.
		Required for robust IRX module loading on both real hardware and PCSX2.
	*/
	while (! SifIopReset(NULL, 0)) {
	}
	while (! SifIopSync()) {
	}
	SifInitRpc(0);

	/*
		Apply standard ps2sdk patches:
		- sbv_patch_enable_lmb: allows SifExecModuleBuffer from RAM
		- sbv_patch_disable_prefix_check: allows loading from any path/buffer
	*/
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();

	SifInitIopHeap();

	/*
		Keep the IOP footprint minimal during the critical ROM/disk
		read.  fileio must be able to allocate its read/write buffer;
		loading every optional module first can exhaust IOP RAM on a
		real PS2, making fread() fail with a perfectly valid file.
	*/
	PS2_LoadEmbedded("iomanX.irx", iomanX_irx, size_iomanX_irx);
	PS2_LoadEmbedded("fileio.irx", fileio_irx, size_fileio_irx);

	PS2_LoadEmbedded("usbd.irx", usbd_irx, size_usbd_irx);
	PS2_LoadEmbedded("bdm.irx", bdm_irx, size_bdm_irx);
	PS2_LoadEmbedded("bdmfs_fatfs.irx", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx);
	PS2_LoadEmbedded("usbmass_bd.irx", usbmass_bd_irx, size_usbmass_bd_irx);
}

static int InputCaps = 0;

int PS2_InputCaps(void)
{
	return InputCaps;
}

void PS2_LoadInputModules(void)
{
	int padOk;
	int kbdOk;
	int mouseOk;

	padOk = (PS2_LoadEmbedded("sio2man.irx", sio2man_irx,
		size_sio2man_irx) >= 0) &&
		(PS2_LoadEmbedded("padman.irx", padman_irx,
		size_padman_irx) >= 0);
	kbdOk = (PS2_LoadEmbedded("ps2kbd.irx", ps2kbd_irx,
		size_ps2kbd_irx) >= 0);
	mouseOk = (PS2_LoadEmbedded("ps2mouse.irx", ps2mouse_irx,
		size_ps2mouse_irx) >= 0);

	InputCaps = (padOk ? 1 : 0) | (kbdOk ? 2 : 0) | (mouseOk ? 4 : 0);
}

int PS2_LoadAudioModules(void)
{
	int libsd;
	int audsrv;

	libsd = SifLoadModule("rom0:LIBSD", 0, NULL);
	if (libsd < 0) {
		snprintf(LastModuleError, sizeof(LastModuleError),
			"LIBSD (%d)", libsd);
		return 0;
	}
	audsrv = PS2_LoadEmbedded("audsrv.irx", audsrv_irx,
		size_audsrv_irx);
	return audsrv >= 0;
}

int PS2_WaitForStorage(int timeoutMs)
{
	static const char *const mountPoints[] = {
		"mass:/", "mass:", "mass0:/", "mass0:",
		"mass1:/", "mass2:/", "mass3:/", "mass4:/", "mass5:/",
		"mass6:/", "mass7:/", "mass8:/", "mass9:/",
		"usb:/", "usb0:/", "usb1:/"
	};
	int waited = 0;
	const int stepMs = 100;
	int lastLogged = -1;

	/*
		With a real USB 1.1 bus, an external hard drive can take much
		longer than a few seconds to spin up, enumerate, read the MBR
		and mount a FAT partition.  Poll for up to 20 seconds and try
		every BDM mount name, not just mass:/ and mass0:/.
	*/
	while (waited < timeoutMs) {
		int i;

		for (i = 0; i < (int)(sizeof(mountPoints) / sizeof(mountPoints[0])); ++i) {
			struct stat st;

			/*
				Use stat() for the mount probe.  Repeatedly opening
				and closing directories on the BDM USB stack can leak
				handles (fclose/closedir is unreliable on some
				drivers), which later makes fopen() of the ROM fail.
			*/
			if (stat(mountPoints[i], &st) == 0) {
				BootTraceHasStorage = 1;
				if (DebugScreenReady && ! PS2Video_IsReady()) {
					scr_printf("[+] Storage mounted at %s\n",
						mountPoints[i]);
				}
				return 1;
			}
		}

		if (DebugScreenReady && ! PS2Video_IsReady()
			&& ((waited / 2000) != lastLogged)) {
			lastLogged = waited / 2000;
			scr_printf("[.] Waiting for USB storage... %d ms\n", waited);
		}
		if (! BootTraceHasStorage && ((waited / 2000) != lastLogged)) {
			lastLogged = waited / 2000;
			printf("[Mini vMac] waiting for USB storage: %d ms\n", waited);
		}

		DelayThread(stepMs * 1000);
		waited += stepMs;
	}

	return 0;
}

const char *PS2_LastModuleError(void)
{
	return LastModuleError;
}

static void PS2_ProbeDir(const char *path)
{
	static const char *names[] = {
		"vMac.ROM", "vmac.rom", "Mac-Plus.ROM",
		"disk1.img", "disk1.dsk", "disk2.img", "disk2.dsk"
	};
	DIR *dir;
	struct dirent *ent;
	int n = 0;
	int i;

	dir = opendir(path);
	if (dir == NULL) {
		return;
	}

	scr_printf("[DIR] %s\n", path);
	while (((ent = readdir(dir)) != NULL) && (n < 12)) {
		if ((ent->d_name[0] == '.') &&
			((ent->d_name[1] == 0) ||
			((ent->d_name[1] == '.') && (ent->d_name[2] == 0)))) {
			continue;
		}
		scr_printf("  %s\n", ent->d_name);
		++n;
	}
	closedir(dir);

	for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); ++i) {
		char full[256];
		struct stat st;

		snprintf(full, sizeof(full), "%s%s", path, names[i]);
		if (stat(full, &st) == 0) {
			scr_printf("[OK ] found %s\n", full);
			return;
		}
	}
	scr_printf("[!!] no known ROM/disk opens in %s\n", path);
}

void PS2_DumpStorage(void)
{
	static const char *paths[] = {
		"mass:/", "mass0:/", "mass1:/", "mass2:/", "mass3:/"
	};
	int i;

	for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); ++i) {
		PS2_ProbeDir(paths[i]);
	}
}

int PS2_FileExists(const char *path)
{
	struct stat st;

	if (path == NULL) {
		return 0;
	}
	/*
		stat() does not keep a file handle open.  Using fopen/fclose here
		for every candidate leaked descriptors on real BDM USB hardware
		and could make the final ROM fopen fail.
	*/
	return (stat(path, &st) == 0);
}

static int PS2_TryPath(const char *path, char *out, int outSize)
{
	if (! PS2_FileExists(path)) {
		return 0;
	}
	strncpy(out, path, outSize - 1);
	out[outSize - 1] = 0;
	return 1;
}

static const char *SearchDirs[] = {
	"mass:/minivmac/",
	"mass:/",
	"mass0:/minivmac/",
	"mass0:/",
	"mass1:/minivmac/",
	"mass1:/",
	"mass2:/minivmac/",
	"mass2:/",
	"mass3:/minivmac/",
	"mass3:/",
	"mass4:/minivmac/",
	"mass4:/",
	"mass5:/minivmac/",
	"mass5:/",
	"mass6:/minivmac/",
	"mass6:/",
	"mass7:/minivmac/",
	"mass7:/",
	"mass8:/minivmac/",
	"mass8:/",
	"mass9:/minivmac/",
	"mass9:/",
	"usb0:/minivmac/",
	"usb0:/",
	"mc0:/minivmac/",
	"mc0:/",
	"mc1:/minivmac/",
	"mc1:/",
	"host:/minivmac/",
	"host:/",
	"host:"
};

int PS2_ResolvePath(const char *name, char *out, int outSize)
{
	char path[256];
	int i;

	if ((name == NULL) || (out == NULL) || (outSize < 2)) {
		return 0;
	}

	/* Already has a device prefix? */
	if (strchr(name, ':') != NULL) {
		return PS2_TryPath(name, out, outSize);
	}

	for (i = 0; i < (int)(sizeof(SearchDirs) / sizeof(SearchDirs[0])); ++i) {
		snprintf(path, sizeof(path), "%s%s", SearchDirs[i], name);
		if (PS2_TryPath(path, out, outSize)) {
			return 1;
		}
	}

	return 0;
}

int PS2_ResolvePathExt(const char *baseName, const char *exts[], int numExts,
	char *out, int outSize)
{
	char path[256];
	int i, j;

	if ((baseName == NULL) || (out == NULL) || (outSize < 2)) {
		return 0;
	}

	for (i = 0; i < (int)(sizeof(SearchDirs) / sizeof(SearchDirs[0])); ++i) {
		for (j = 0; j < numExts; ++j) {
			snprintf(path, sizeof(path), "%s%s%s",
				SearchDirs[i], baseName, exts[j]);
			if (PS2_TryPath(path, out, outSize)) {
				return 1;
			}
		}
	}

	return 0;
}

int PS2_WaitForPath(const char *name, char *out, int outSize, int timeoutMs)
{
	int i;
	int tries = timeoutMs / 20;

	for (i = 0; i <= tries; ++i) {
		if (PS2_ResolvePath(name, out, outSize)) {
			return 1;
		}
		if (i != tries) {
			DelayThread(20000);
		}
	}

	return 0;
}

int PS2_WaitForAnyPath(const char *names[], int numNames,
	char *out, int outSize, int timeoutMs)
{
	int i, j;
	int tries = timeoutMs / 50;

	for (i = 0; i <= tries; ++i) {
		for (j = 0; j < numNames; ++j) {
			if (PS2_ResolvePath(names[j], out, outSize)) {
				return 1;
			}
		}
		if (i != tries) {
			DelayThread(50000);
		}
	}

	return 0;
}
