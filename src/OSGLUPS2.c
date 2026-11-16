/*
	OSGLUPS2.c

	Operating System GLUe for the Sony PlayStation 2 (Version 2.0).

	Enhanced platform layer for Mini vMac on PS2:
	- Robust IOP and IRX module management with USB delay/sync loop
	- Double buffered gsKit video output (no tearing) with 3 scale modes:
	  1:1 Centered, Aspect-Correct (640x428), Fullscreen (640x480)
	- Hybrid input: DualShock 2 (analog stick with smooth curve & deadzone,
	  D-pad 1px nudges, full button mapping) + USB keyboard & mouse
	- 22255 Hz mono 8-bit sound via audsrv + 60Hz EE hardware bus clock limiter
	- Flexible disk loading (.dsk, .img, .image, .hfv, boot disks) with safe
	  fflush writes to prevent FAT filesystem corruption on shutdown

	You can redistribute this file and/or modify it under the terms
	of version 2 of the GNU General Public License as published by
	the Free Software Foundation.
*/

#include "OSGCOMUI.h"
#include "OSGCOMUD.h"
#include "CNFUIPIC.h"

#ifdef WantOSGLUPS2

/* Core accessors used by the PS2 scheduler between emulation slices. */
extern ui5r m68k_get_pc_snapshot(void);
extern ui3p Screen_GetCurrentBuff(void);
extern blnr SCC_InterruptsEnabled(void);

#include <delaythread.h>
#include <timer.h>
#include <libpad.h>
#include <debug.h>
#include <dmaKit.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "ps2_platform.h"
#include "ps2_video.h"
#include "ps2_input.h"
#include "ps2_audio.h"

/* --- debug tracing --- */

/*
	The ps2sdk text console (init_scr/scr_printf) programs the GS and
	uses the GIF DMA channel directly.  On this port it can wedge the
	GIF channel after the USB ROM read, and it also conflicts with the
	gsKit initialisation.  For a clean boot we keep it disabled and use
	BOOTLOG.TXT on the USB stick instead.  Set to 1 only for bring-up.
*/
#define PS2_TEXT_CONSOLE 1

/*
	While the ps2sdk text console is active we can print straight to the
	GS with scr_printf().  Once gsKit owns the Graphics Synthesizer that
	console must not be touched any more (it would stomp gsKit's VRAM /
	display state), so later traces go to the PCSX2 stdout console instead.
*/
LOCALVAR blnr TraceToScreen = falseblnr;
LOCALVAR blnr TraceSilent = falseblnr;

LOCALPROC PS2Trace(const char *fmt, ...) {
	char buf[192];
	va_list ap;

	if (TraceSilent) {
		return;
	}

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (TraceToScreen) {
		scr_printf("%s", buf);
	} else {
		printf("%s", buf);
	}
}

/* --- some simple utilities --- */

GLOBALOSGLUPROC MyMoveBytes(anyp srcPtr, anyp destPtr, si5b byteCount) {
	/* Mini vMac also uses this for overlapping screen scrolling. */
	(void) memmove((char *)destPtr, (char *)srcPtr, byteCount);
}

/* --- control mode and internationalization --- */

#define NeedCell2PlainAsciiMap 1

#include "INTLCHAR.h"

/* --- sending debugging info to file --- */

#if dbglog_HAVE

#ifndef dbglog_ToStdErr
#define dbglog_ToStdErr 1
#endif

#if ! dbglog_ToStdErr
LOCALVAR FILE *dbglog_File = NULL;
#endif

LOCALFUNC blnr dbglog_open0(void) {
#if dbglog_ToStdErr
	return trueblnr;
#else
	dbglog_File = fopen("mass:/minivmac/dbglog.txt", "w");
	return (NULL != dbglog_File);
#endif
}

LOCALPROC dbglog_write0(char *s, uimr L) {
#if dbglog_ToStdErr
	(void) fwrite(s, 1, L, stderr);
#else
	if (dbglog_File != NULL) {
		(void) fwrite(s, 1, L, dbglog_File);
	}
#endif
}

LOCALPROC dbglog_close0(void) {
#if ! dbglog_ToStdErr
	if (dbglog_File != NULL) {
		fclose(dbglog_File);
		dbglog_File = NULL;
	}
#endif
}

#endif

/* --- debug settings and utilities --- */

#if ! dbglog_HAVE
#define WriteExtraErr(s)
#else
LOCALPROC WriteExtraErr(char *s) {
	dbglog_writeCStr("*** error: ");
	dbglog_writeCStr(s);
	dbglog_writeReturn();
}
#endif

/* --- information about the environment --- */

#include "COMOSGLU.h"

#include "PBUFSTDC.h"

#include "CONTROLM.h"

/* --- text translation --- */

LOCALPROC NativeStrFromCStr(char *r, char *s) {
	ui3b ps[ClStrMaxLength];
	int i;
	int L;

	ClStrFromSubstCStr(&L, ps, s);

	for (i = 0; i < L; ++i) {
		r[i] = Cell2PlainAsciiMap[ps[i]];
	}

	r[L] = 0;
}

/* --- drives --- */

typedef struct {
	u8 *data;
	ui5r size;
	ui5r fileOffset; /* DC42 data starts after its 84-byte header */
	int fd; /* open USB image for demand reads when too large for EE RAM */
	blnr writable;
	blnr dirty;
	char path[256];
} DriveImage;

LOCALVAR DriveImage Drives[NumDrives];
LOCALVAR blnr DrivesInited = falseblnr;

#define MaxDiskImageBytes (8UL * 1024UL * 1024UL)
#define PS2_DISK_READ_CACHE_SIZE (32UL * 1024UL)

/* Large HFS images stay on USB. Cache nearby sectors so the Mac's
   small, frequent reads do not each turn into a separate BDM request. */
LOCALVAR u8 DiskReadCache[PS2_DISK_READ_CACHE_SIZE];
LOCALVAR tDrive DiskReadCacheDrive = (tDrive)-1;
LOCALVAR ui5r DiskReadCacheOffset = 0;
LOCALVAR ui5r DiskReadCacheLength = 0;

LOCALPROC InitDriveFds(void) {
	tDrive i;

	if (! DrivesInited) {
		for (i = 0; i < NumDrives; ++i) {
			Drives[i].data = NULL;
			Drives[i].size = 0;
			Drives[i].fileOffset = 0;
			Drives[i].fd = -1;
			Drives[i].writable = falseblnr;
			Drives[i].dirty = falseblnr;
			Drives[i].path[0] = 0;
		}
		DrivesInited = trueblnr;
		DiskReadCacheDrive = (tDrive)-1;
		DiskReadCacheOffset = 0;
		DiskReadCacheLength = 0;
	}
}

LOCALFUNC ui5r ReadDriveCached(tDrive Drive_No, DriveImage *d,
	ui5r offset, ui3p buffer, ui5r count)
{
	ui5r copied = 0;

	while (copied < count) {
		ui5r absolute = d->fileOffset + offset + copied;
		ui5r cacheOffset = absolute & ~(PS2_DISK_READ_CACHE_SIZE - 1);
		ui5r inCache = absolute - cacheOffset;
		ui5r available;

		if ((DiskReadCacheDrive != Drive_No)
			|| (DiskReadCacheOffset != cacheOffset)
			|| (inCache >= DiskReadCacheLength)) {
			ui5r toRead = d->size - cacheOffset;
			ui5r loaded = 0;

			if (toRead > PS2_DISK_READ_CACHE_SIZE) {
				toRead = PS2_DISK_READ_CACHE_SIZE;
			}
			if (lseek(d->fd, (off_t)cacheOffset, SEEK_SET) < 0) {
				DiskReadCacheLength = 0;
				return copied;
			}
			while (loaded < toRead) {
				ssize_t n = read(d->fd, DiskReadCache + loaded,
					(size_t)(toRead - loaded));
				if (n <= 0) break;
				loaded += (ui5r)n;
			}
			DiskReadCacheDrive = Drive_No;
			DiskReadCacheOffset = cacheOffset;
			DiskReadCacheLength = loaded;
			if (inCache >= loaded) return copied;
		}

		available = DiskReadCacheLength - inCache;
		if (available > count - copied) {
			available = count - copied;
		}
		memcpy(buffer + copied, DiskReadCache + inCache, (size_t)available);
		copied += available;
	}

	return copied;
}

GLOBALOSGLUFUNC tMacErr vSonyTransfer(blnr IsWrite, ui3p Buffer,
	tDrive Drive_No, ui5r Sony_Start, ui5r Sony_Count,
	ui5r *Sony_ActCount) {
	DriveImage *d;
	tMacErr err = mnvm_miscErr;
	ui5r done = 0;

	if (Drive_No >= NumDrives) {
		if (nullpr != Sony_ActCount) {
			*Sony_ActCount = 0;
		}
		return mnvm_miscErr;
	}
	d = &Drives[Drive_No];

	if ((Sony_Start <= d->size)
		&& (Sony_Count <= (d->size - Sony_Start))) {
		if (d->data != NULL) {
			if (IsWrite) {
				if (d->writable) {
					memcpy(d->data + Sony_Start, Buffer, (size_t)Sony_Count);
					d->dirty = trueblnr;
					done = Sony_Count;
					err = mnvm_noErr;
				}
			} else {
				memcpy(Buffer, d->data + Sony_Start, (size_t)Sony_Count);
				done = Sony_Count;
				err = mnvm_noErr;
			}
		} else if (d->fd >= 0 && (! IsWrite || d->writable)) {
			if (IsWrite) {
				if (DiskReadCacheDrive == Drive_No) {
					DiskReadCacheLength = 0;
				}
				if (lseek(d->fd, (off_t)(d->fileOffset + Sony_Start),
					SEEK_SET) >= 0) {
					while (done < Sony_Count) {
						ui5r left = Sony_Count - done;
						ssize_t n;

						if (left > 65536) left = 65536;
						n = write(d->fd, Buffer + done, (size_t)left);
						if (n <= 0) break;
						done += (ui5r)n;
					}
					if (done == Sony_Count) {
						d->dirty = trueblnr;
						err = mnvm_noErr;
					}
				}
			} else {
				done = ReadDriveCached(Drive_No, d, Sony_Start,
					Buffer, Sony_Count);
				if (done == Sony_Count) {
					err = mnvm_noErr;
				}
			}
		}
	}

	if (nullpr != Sony_ActCount) {
		*Sony_ActCount = done;
	}
	return err;
}

GLOBALOSGLUFUNC tMacErr vSonyGetSize(tDrive Drive_No, ui5r *Sony_Count) {
	if ((Drives[Drive_No].data == NULL) && (Drives[Drive_No].fd < 0)) {
		return mnvm_miscErr;
	}
	*Sony_Count = Drives[Drive_No].size;
	return mnvm_noErr;
}

LOCALPROC FlushDriveIfWritable(tDrive Drive_No) {
	DriveImage *d = &Drives[Drive_No];

	if (d->writable && d->dirty && (d->path[0] != 0) && (d->data != NULL)) {
		int fd = open(d->path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd >= 0) {
			ui5r off = 0;
			while (off < d->size) {
				ui5r left = d->size - off;
				int want = (left > 65536) ? 65536 : (int)left;
				ssize_t n = write(fd, d->data + off, (size_t)want);
				if (n <= 0) {
					break;
				}
				off += (ui5r)n;
			}
			close(fd);
		}
	}
}

LOCALFUNC tMacErr vSonyEject0(tDrive Drive_No, blnr deleteit) {
	DriveImage *d = &Drives[Drive_No];

	if (DiskReadCacheDrive == Drive_No) {
		DiskReadCacheDrive = (tDrive)-1;
		DiskReadCacheLength = 0;
	}
	DiskEjectedNotify(Drive_No);

	FlushDriveIfWritable(Drive_No);
	if (deleteit && (d->path[0] != 0)) {
		(void) remove(d->path);
	}
	if (d->data != NULL) {
		free(d->data);
	}
	d->data = NULL;
	d->size = 0;
	d->fileOffset = 0;
	d->fd = -1;
	d->writable = falseblnr;
	d->dirty = falseblnr;
	d->path[0] = 0;

	return mnvm_noErr;
}

GLOBALOSGLUFUNC tMacErr vSonyEject(tDrive Drive_No) {
	return vSonyEject0(Drive_No, falseblnr);
}

#if IncludeSonyNew
GLOBALOSGLUFUNC tMacErr vSonyEjectDelete(tDrive Drive_No) {
	return vSonyEject0(Drive_No, trueblnr);
}
#endif

LOCALPROC UnInitDrives(void) {
	tDrive i;

	for (i = 0; i < NumDrives; ++i) {
		if (vSonyIsInserted(i)) {
			(void) vSonyEject(i);
		}
	}
}

LOCALFUNC blnr Sony_InsertImage(u8 *data, ui5r size, ui5r fileOffset,
	blnr writable, const char *path, int backingFd) {
	tDrive Drive_No;
	blnr locked = writable ? falseblnr : trueblnr;
	DriveImage *d;

	if (! FirstFreeDisk(&Drive_No)) {
		MacMsg(kStrTooManyImagesTitle, kStrTooManyImagesMessage,
			falseblnr);
		free(data);
		return falseblnr;
	}

	d = &Drives[Drive_No];
	d->data = data;
	d->size = size;
	d->fileOffset = fileOffset;
	d->fd = (data == NULL) ? backingFd : -1;
	d->writable = writable;
	d->dirty = falseblnr;
	if (path != NULL) {
		snprintf(d->path, sizeof(d->path), "%s", path);
	} else {
		d->path[0] = 0;
	}
	DiskInsertNotify(Drive_No, locked);
	return trueblnr;
}

LOCALFUNC blnr Sony_Insert1(const char *drivepath, blnr silentfail) {
	char resolved[256];
	u8 *data = NULL;
	ui5r size = 0;
	ui5r offset = 0;
	blnr ok = falseblnr;

	if (! PS2_ResolvePath(drivepath, resolved, sizeof(resolved))) {
		if (! silentfail) {
			MacMsg(kStrOpenFailTitle, kStrOpenFailMessage, falseblnr);
		}
		return falseblnr;
	}

	{
		int fd = open(resolved, O_RDONLY);
		struct stat st;

		if (fd >= 0 && stat(resolved, &st) == 0) {
			size = (ui5r)st.st_size;
			PS2_BootTrace("disk candidate: ");
			PS2_BootTrace(resolved);
			PS2_BootTrace("\n");
			{
				char sizeLine[48];
				snprintf(sizeLine, sizeof(sizeLine), "disk bytes: %u\n",
					(unsigned int)size);
				PS2_BootTrace(sizeLine);
			}
			/* Leave DC42 headers intact; SONYEMDV parses them through
			   vSonyTransfer and applies the image data offset itself. */
			if (size > MaxDiskImageBytes) {
				PS2_BootTrace("disk using demand-read USB backing\n");
				return Sony_InsertImage(NULL, size, 0, falseblnr,
					resolved, fd);
			}
			if ((size > 0) && (size <= MaxDiskImageBytes)) {
				data = (u8 *)malloc((size_t)size);
			} else {
				PS2_BootTrace("disk rejected: image exceeds 8 MiB RAM cache limit\n");
			}
			if (data != NULL) {
				ui5r total = 0;
				(void) lseek(fd, (off_t)offset, SEEK_SET);
				while (total < size) {
					ui5r left = size - total;
					int want = (left > 65536) ? 65536 : (int)left;
					ssize_t n = read(fd, data + total, (size_t)want);
					if (n <= 0) {
						break;
					}
					total += (ui5r)n;
				}
				if (total == size) {
					ok = trueblnr;
					PS2_BootTrace("disk image fully loaded into EE RAM\n");
				} else {
					free(data);
					data = NULL;
				}
			}
		}
		/*
			Do not close the BDM file here.  close() on this USB
			stack can block the EE; the image is already fully in
			RAM, so leaking the descriptor is harmless (at most
			NumDrives descriptors).
		*/
	}

	if (ok) {
		return Sony_InsertImage(data, size, 0, falseblnr, resolved, -1);
	}

	if (! silentfail) {
		MacMsg(kStrOpenFailTitle, kStrOpenFailMessage, falseblnr);
	}
	return falseblnr;
}

static const char *DiskFileExtensions[] = {
	".dsk", ".img", ".image", ".hfv",
	".DSK", ".IMG", ".IMAGE", ".HFV"
};

LOCALFUNC blnr Sony_InsertWithExts(const char *baseName) {
	char resolved[256];

	if (PS2_ResolvePathExt(baseName, DiskFileExtensions,
		(int)(sizeof(DiskFileExtensions) / sizeof(DiskFileExtensions[0])),
		resolved, sizeof(resolved))) {
		PS2Trace("    -> Mounted: %s\n", resolved);
		return Sony_Insert1(resolved, trueblnr);
	}

	return falseblnr;
}

LOCALFUNC blnr Sony_InsertIth(int i) {
	char name[32];

	if ((i > 9) || ! FirstFreeDisk(nullpr)) {
		return falseblnr;
	}

	snprintf(name, sizeof(name), "disk%d", i);
	if (Sony_InsertWithExts(name)) {
		return trueblnr;
	}

	snprintf(name, sizeof(name), "Disk%d", i);
	if (Sony_InsertWithExts(name)) {
		return trueblnr;
	}

	snprintf(name, sizeof(name), "DISK%d", i);
	if (Sony_InsertWithExts(name)) {
		return trueblnr;
	}

	return falseblnr;
}

LOCALFUNC int LoadInitialImages(void) {
	int i;
	int mounted = 0;
	static const char *StandaloneCandidates[] = {
		"System608", "System_Startup", "System", "system",
		"MacOS_6.0.8_System_Startup", "MacOS_6.0.8", "boot", "Boot", "mac"
	};

	/* 1. Try disk1 through disk9 */
	for (i = 1; i <= 9; ++i) {
		if (Sony_InsertIth(i)) {
			++mounted;
		}
	}

	/* 2. If no numbered disks were found, check standalone boot images */
	if (mounted == 0) {
		for (i = 0; i < (int)(sizeof(StandaloneCandidates) / sizeof(StandaloneCandidates[0])); ++i) {
			if (Sony_InsertWithExts(StandaloneCandidates[i])) {
				++mounted;
				break;
			}
		}
	}

	{
		char line[64];
		snprintf(line, sizeof(line), "disk images mounted: %d\n", mounted);
		PS2_BootTrace(line);
	}
	if (mounted == 0) {
		PS2_BootTrace("no disk image mounted at startup\n");
	}

	return mounted;
}

#if IncludeSonyNew
LOCALPROC MakeNewDisk(ui5b L, char *drivepath) {
	u8 *data;

	if ((L == 0) || (L > MaxDiskImageBytes)) {
		MacMsg(kStrOpenFailTitle, kStrOpenFailMessage, falseblnr);
		return;
	}
	data = (u8 *)calloc(1, (size_t)L);
	if (data == NULL) {
		MacMsg(kStrOutOfMemTitle, kStrOutOfMemMessage, falseblnr);
		return;
	}
	if (! Sony_InsertImage(data, L, 0, trueblnr, drivepath, -1)) {
		(void) remove(drivepath);
	}
}

LOCALPROC MakeNewDiskAtDefault(ui5b L) {
	char s[ClStrMaxLength + 1];

	NativeStrFromCStr(s, "untitled.dsk");
	MakeNewDisk(L, s);
}
#endif

/* --- ROM --- */

LOCALVAR FILE *RomFileRef = NULL;
LOCALVAR int RomFd = -1;
LOCALVAR int LastRomStatSize = -1;
LOCALVAR int LastRomReadBytes = 0;
LOCALVAR int LastRomErr = 0;
LOCALVAR int LastRomOpenOk = 0;

LOCALFUNC tMacErr LoadMacRomFrom(const char *path) {
	tMacErr err = mnvm_miscErr;
	int File_Size = 0;

	struct stat st;
	(void) ROM_IsValid; /* keep the helper referenced for optional checks */

	LastRomReadBytes = 0;
	LastRomErr = 0;
	LastRomStatSize = -1;
	LastRomOpenOk = 0;
	RomFd = -1;
	if (stat(path, &st) == 0) {
		LastRomStatSize = (int)st.st_size;
	}

	/*
		Use the POSIX layer, as recommended by ps2sdk, instead of
		stdio.  On real BDM USB hardware fopen/fread can fail even
		when stat() sees the file and the directory listing is fine.
	*/
	PS2Trace("[DEBUG] ROM: stat size=%d, open(%s)\n",
		LastRomStatSize, path);
	RomFd = open(path, O_RDONLY);
	if (RomFd < 0) {
		PS2Trace("[DEBUG] ROM: open FAILED (errno=%d)\n", errno);
		LastRomErr = errno;
		err = mnvm_fnfErr;
	} else {
		LastRomOpenOk = 1;
		PS2Trace("[DEBUG] ROM: fd=%d, reading %d bytes...\n",
			RomFd, kROM_Size);
		while (File_Size < kROM_Size) {
			int want = kROM_Size - File_Size;
			int got;

			if (want > 16384) {
				want = 16384;
			}
			got = (int)read(RomFd, ROM + File_Size, want);
			if (got < 0) {
				LastRomErr = errno;
				break;
			}
			if (got == 0) {
				break;
			}
			File_Size += got;
			PS2Trace("[DEBUG] ROM: read %d/%d bytes\n",
				File_Size, kROM_Size);
		}
		LastRomReadBytes = File_Size;

		if (kROM_Size == File_Size) {
			/* Match the upstream loader: validate before starting emulation. */
			err = ROM_IsValid();
			if (err == mnvm_noErr) {
				PS2_BootTrace("ROM checksum accepted by Mini vMac core\n");
			} else {
				PS2_BootTrace("ROM checksum rejected by Mini vMac core\n");
			}
		} else if ((LastRomStatSize >= 0) &&
			(LastRomStatSize < kROM_Size)) {
			MacMsgOverride(kStrShortROMTitle, kStrShortROMMessage);
			err = mnvm_eofErr;
		} else {
			MacMsgOverride(kStrNoReadROMTitle, kStrNoReadROMMessage);
			err = mnvm_miscErr;
		}

		if (mnvm_noErr == err) {
			/*
				Keep the ROM descriptor open.  The emulator never
				needs it again, and not calling close() avoids the
				BDM close hang seen on real hardware.
			*/
			RomFileRef = NULL;
		} else {
			RomFd = -1;
		}
	}

	/*
		Fallback for the rare case where POSIX open() itself is
		unavailable.  Keep it only as a diagnostic path.
	*/
	if ((mnvm_noErr != err) && (0 == LastRomOpenOk)) {
		FILE *f = fopen(path, "rb");

		if (f != NULL) {
			int got;

			LastRomOpenOk = 2;
			setvbuf(f, NULL, _IONBF, 0);
			File_Size = 0;
			while (File_Size < kROM_Size) {
				int want = kROM_Size - File_Size;

				if (want > 16384) {
					want = 16384;
				}
				got = (int)fread(ROM + File_Size, 1, want, f);
				if (got <= 0) {
					break;
				}
				File_Size += got;
			}
			LastRomReadBytes = File_Size;
			if (kROM_Size == File_Size) {
				err = ROM_IsValid();
				if (err == mnvm_noErr) {
					RomFileRef = f;
				}
			}
		}
	}

	return err;
}

static const char *RomCandidates[] = {
	"vMac.ROM", "vmac.rom", "VMAC.ROM",
	"Mac-Plus.ROM", "mac-plus.rom",
	"MacPlus.rom", "macplus.rom",
	"ROM", "rom"
};

LOCALFUNC blnr LoadMacRom(void) {
	char path[256];

	PS2Trace("[+] Searching for Macintosh Plus ROM...\n");
	if (! PS2_WaitForAnyPath(RomCandidates,
		(int)(sizeof(RomCandidates) / sizeof(RomCandidates[0])),
		path, sizeof(path), 20000)) {
		PS2_Panic("Could not find the Macintosh ROM file (vMac.ROM or Mac-Plus.ROM).");
	}

	PS2Trace("[+] Found Macintosh ROM: %s\n", path);

	if (mnvm_noErr != LoadMacRomFrom(path)) {
		char msg[200];

		snprintf(msg, sizeof(msg),
			"ROM read failed: stat=%d, read=%d, expected=%d, "
			"open=%d, errno=%d.",
			LastRomStatSize, LastRomReadBytes, (int)kROM_Size,
			LastRomOpenOk, (int)LastRomErr);
		PS2_Panic(msg);
	}

	PS2Trace("[+] ROM verified successfully (128 KB Mac Plus).\n");
	return trueblnr;
}

/* --- command line parsing --- */

LOCALFUNC blnr ScanCommandLine(void) {
	return trueblnr;
}

/* --- video out --- */

/* Match the upstream UI state; CheckForSavedTasks synchronizes it
   with SpeedStopped before the first emulated tick. */
LOCALVAR blnr CurSpeedStopped = trueblnr;
LOCALVAR blnr MacMessageSticky = falseblnr;

LOCALPROC MyDrawChangesAndClear(void) {
	static int FrameTraceCount = 0;
	/*
		Use the portable renderer's prepared image first. Besides tracking
		updates from the live video page, GetCurDrawBuff() applies the
		core's special display modes and control overlays, as the upstream
		SDL and desktop front-ends do. Keep the raw page only as a fallback
		for an early/failed core buffer initialization.
	*/
	ui3p frame = GetCurDrawBuff();

	if (frame == nullpr) {
		frame = Screen_GetCurrentBuff();
	}
	if (frame != nullpr) {
		if (FrameTraceCount < 5) {
			const u8 *pixels = (const u8 *)frame;
			ui5r hash = 2166136261u;
			ui5r ones = 0;
			ui5r i;
			char line[96];

			for (i = 0; i < (ui5r)vMacScreenMonoNumBytes; ++i) {
				u8 b = pixels[i];
				int bit;

				hash = (hash ^ b) * 16777619u;
				for (bit = 0; bit < 8; ++bit) {
					ones += (b >> bit) & 1;
				}
			}
			snprintf(line, sizeof(line),
				"Mac frame %d: hash=%08x black-bits=%u/%u\n",
				FrameTraceCount + 1, (unsigned int)hash,
				(unsigned int)ones,
				(unsigned int)(vMacScreenMonoNumBytes * 8));
			PS2_BootTrace(line);
			++FrameTraceCount;
		}
		PS2Video_DrawFrame((const u8 *)frame);
	} else {
		PS2_BootTrace("Mac frame source is NULL; retaining last frame\n");
	}
	ScreenClearChanges();
}

/* --- gamepad derived keyboard and mouse --- */

LOCALVAR float PadAccumX = 0.0f;
LOCALVAR float PadAccumY = 0.0f;
LOCALVAR int PadCursorX = vMacScreenWidth / 2;
LOCALVAR int PadCursorY = vMacScreenHeight / 2;
LOCALVAR int PadLastMouseX = vMacScreenWidth / 2;
LOCALVAR int PadLastMouseY = vMacScreenHeight / 2;
LOCALVAR int PadClickDown = 0;
LOCALVAR int PadReturnHeld = 0;
LOCALVAR int PadDeleteHeld = 0;
LOCALVAR int PadCommandHeld = 0;
LOCALVAR int PadControlHeld = 0;
LOCALVAR int PadOptionHeld = 0;
LOCALVAR int PadShiftHeld = 0;
LOCALVAR int PadFastForward = 0;
LOCALVAR ui3b PadMkcDown[MKC_None + 1];

LOCALPROC PadSetKey(ui3r mkc, blnr down) {
	if (mkc >= MKC_None) {
		return;
	}
	if (down) {
		if (PadMkcDown[mkc] == 0) {
			Keyboard_UpdateKeyMap2(mkc, trueblnr);
		}
		if (PadMkcDown[mkc] < 255) {
			++PadMkcDown[mkc];
		}
	} else if (PadMkcDown[mkc] != 0) {
		--PadMkcDown[mkc];
		if (PadMkcDown[mkc] == 0) {
			Keyboard_UpdateKeyMap2(mkc, falseblnr);
		}
	}
}

LOCALPROC PublishMouseInput(int mouseButtons) {
	/* The classic Mac consumes mouse events only while its SCC mouse
	   interface is enabled. A mouse event queued before then sits at the
	   head of the shared ADB/keyboard event queue, preventing key inquiries
	   from reaching keyboard events behind it. */
	Mouse_PositionSet(PadCursorX, PadCursorY);
	if (SCC_InterruptsEnabled()) {
		MyMouseButtonSet((PadClickDown || mouseButtons) ? trueblnr : falseblnr);
	}
}

LOCALPROC UpdatePadInput(void) {
	PS2PadState pad;
	int mousePresent = 0;
	int mouseX = PadCursorX;
	int mouseY = PadCursorY;
	int mouseButtons = 0;
	float stickX, stickY, mag;
	const float deadzone = 18.0f;
	float maxSpeed;
	int stepX, stepY;

	PS2Input_GetPad(&pad);
	PS2Input_GetMouse(&mousePresent, &mouseX, &mouseY, &mouseButtons);
	if (mousePresent) {
		/* Merge USB relative movement into the shared cursor instead of
		   snapping it back to the mouse's private position. This lets the
		   gamepad and USB mouse both control the pointer independently. */
		PadCursorX += mouseX - PadLastMouseX;
		PadCursorY += mouseY - PadLastMouseY;
		PadLastMouseX = mouseX;
		PadLastMouseY = mouseY;
	}

	if (! pad.connected) {
		if (PadReturnHeld) {
			PadSetKey(MKC_Return, falseblnr);
			PadReturnHeld = 0;
		}
		if (PadDeleteHeld) {
			PadSetKey(MKC_BackSpace, falseblnr);
			PadDeleteHeld = 0;
		}
		if (PadCommandHeld) {
			PadSetKey(MKC_formac_Command, falseblnr);
			PadCommandHeld = 0;
		}
		if (PadControlHeld) {
			PadSetKey(MKC_CM, falseblnr);
			PadControlHeld = 0;
		}
		if (PadOptionHeld) {
			PadSetKey(MKC_formac_Option, falseblnr);
			PadOptionHeld = 0;
		}
		if (PadShiftHeld) {
			PadSetKey(MKC_formac_Shift, falseblnr);
			PadShiftHeld = 0;
		}
		PadClickDown = 0;
		if (PadCursorX < 0) PadCursorX = 0;
		if (PadCursorX >= vMacScreenWidth) PadCursorX = vMacScreenWidth - 1;
		if (PadCursorY < 0) PadCursorY = 0;
		if (PadCursorY >= vMacScreenHeight) PadCursorY = vMacScreenHeight - 1;
		PublishMouseInput(mouseButtons);
		return;
	}

	/*
		Left analog stick controls the Macintosh mouse cursor.
		Uses a deadzone followed by a quadratic acceleration curve.
		Small deflections produce sub-pixel movements for clicking small
		12-pixel menus; full deflection moves briskly across the screen.
	*/
	stickX = (float)((int)pad.lx - 128);
	stickY = (float)((int)pad.ly - 128);
	mag = sqrtf(stickX * stickX + stickY * stickY);

	if (mag < deadzone) {
		stickX = 0.0f;
		stickY = 0.0f;
	} else {
		float scale = (mag - deadzone) / (128.0f - deadzone);
		if (scale > 1.0f) scale = 1.0f;
		float curve = scale * scale; /* quadratic precision curve */
		stickX = (stickX / mag) * curve;
		stickY = (stickY / mag) * curve;
	}

	/* L1 modifier: Slow/Precision mode (cuts speed for pixel-art / tight menus) */
	maxSpeed = (pad.buttons & PAD_L1) ? 2.5f : 9.0f;

	PadAccumX += stickX * maxSpeed;
	PadAccumY += stickY * maxSpeed;

	stepX = (int)PadAccumX;
	stepY = (int)PadAccumY;
	PadAccumX -= (float)stepX;
	PadAccumY -= (float)stepY;

	PadCursorX += stepX;
	PadCursorY += stepY;

	/* D-Pad single-pixel nudging */
	if (pad.pressed & PAD_LEFT)  PadCursorX -= 1;
	if (pad.pressed & PAD_RIGHT) PadCursorX += 1;
	if (pad.pressed & PAD_UP)    PadCursorY -= 1;
	if (pad.pressed & PAD_DOWN)  PadCursorY += 1;

	/* Clamp cursor to Mac screen bounds */
	if (PadCursorX < 0) {
		PadCursorX = 0;
	} else if (PadCursorX >= vMacScreenWidth) {
		PadCursorX = vMacScreenWidth - 1;
	}
	if (PadCursorY < 0) {
		PadCursorY = 0;
	} else if (PadCursorY >= vMacScreenHeight) {
		PadCursorY = vMacScreenHeight - 1;
	}

	/* Cross is the primary Macintosh mouse button */
	PadClickDown = ((pad.buttons & PAD_CROSS) != 0);
	PublishMouseInput(mouseButtons);

	/* Circle: Macintosh Return / Enter */
	if ((pad.pressed & PAD_CIRCLE) && ! PadReturnHeld) {
		PadSetKey(MKC_Return, trueblnr);
		PadReturnHeld = 1;
	}
	if (PadReturnHeld && ! (pad.buttons & PAD_CIRCLE)) {
		PadSetKey(MKC_Return, falseblnr);
		PadReturnHeld = 0;
	}

	/* Square: Macintosh Backspace / Delete */
	if ((pad.pressed & PAD_SQUARE) && ! PadDeleteHeld) {
		PadSetKey(MKC_BackSpace, trueblnr);
		PadDeleteHeld = 1;
	}
	if (PadDeleteHeld && ! (pad.buttons & PAD_SQUARE)) {
		PadSetKey(MKC_BackSpace, falseblnr);
		PadDeleteHeld = 0;
	}

	/* Triangle: Macintosh Command key (held while pressed) */
	if ((pad.pressed & PAD_TRIANGLE) && ! PadCommandHeld) {
		PadSetKey(MKC_formac_Command, trueblnr);
		PadCommandHeld = 1;
	}
	if (PadCommandHeld && ! (pad.buttons & PAD_TRIANGLE)) {
		PadSetKey(MKC_formac_Command, falseblnr);
		PadCommandHeld = 0;
	}

	/* Start: Mini vMac Control Mode (MKC_CM) */
	if ((pad.pressed & PAD_START) && ! PadControlHeld) {
		PadSetKey(MKC_CM, trueblnr);
		PadControlHeld = 1;
	}
	if (PadControlHeld && ! (pad.buttons & PAD_START)) {
		PadSetKey(MKC_CM, falseblnr);
		PadControlHeld = 0;
	}

	/* R1: Macintosh Option / Alt key */
	if ((pad.pressed & PAD_R1) && ! PadOptionHeld) {
		PadSetKey(MKC_formac_Option, trueblnr);
		PadOptionHeld = 1;
	}
	if (PadOptionHeld && ! (pad.buttons & PAD_R1)) {
		PadSetKey(MKC_formac_Option, falseblnr);
		PadOptionHeld = 0;
	}

	/* L2: Macintosh Shift key */
	if ((pad.pressed & PAD_L2) && ! PadShiftHeld) {
		PadSetKey(MKC_formac_Shift, trueblnr);
		PadShiftHeld = 1;
	}
	if (PadShiftHeld && ! (pad.buttons & PAD_L2)) {
		PadSetKey(MKC_formac_Shift, falseblnr);
		PadShiftHeld = 0;
	}

	/* R2: Cycle Video Scale Mode (1:1 -> Aspect-Fit -> Fullscreen) */
	if (pad.pressed & PAD_R2) {
		PS2Video_CycleScaleMode();
	}

	/* R3: Toggle Fast-Forward (1x vs 4x speed) */
	if (pad.pressed & PAD_R3) {
		PadFastForward = ! PadFastForward;
		SpeedValue = PadFastForward ? 2 : 0;
	}
}

/* --- mouse --- */

LOCALPROC CheckMouseState(void) {
	int mousePresent = 0;
	int mouseX = PadCursorX;
	int mouseY = PadCursorY;
	int mouseButtons = 0;

	PS2Input_GetMouse(&mousePresent, &mouseX, &mouseY, &mouseButtons);
	(void)mousePresent;
	(void)mouseX;
	(void)mouseY;
	PublishMouseInput(mouseButtons);
}

/* --- keyboard input --- */

LOCALVAR const ui3r Hid2Mkc[256] = {
	[0 ... 255] = MKC_None,

	[0x04] = MKC_A,		[0x05] = MKC_B,		[0x06] = MKC_C,
	[0x07] = MKC_D,		[0x08] = MKC_E,		[0x09] = MKC_F,
	[0x0A] = MKC_G,		[0x0B] = MKC_H,		[0x0C] = MKC_I,
	[0x0D] = MKC_J,		[0x0E] = MKC_K,		[0x0F] = MKC_L,
	[0x10] = MKC_M,		[0x11] = MKC_N,		[0x12] = MKC_O,
	[0x13] = MKC_P,		[0x14] = MKC_Q,		[0x15] = MKC_R,
	[0x16] = MKC_S,		[0x17] = MKC_T,		[0x18] = MKC_U,
	[0x19] = MKC_V,		[0x1A] = MKC_W,		[0x1B] = MKC_X,
	[0x1C] = MKC_Y,		[0x1D] = MKC_Z,

	[0x1E] = MKC_1,		[0x1F] = MKC_2,		[0x20] = MKC_3,
	[0x21] = MKC_4,		[0x22] = MKC_5,		[0x23] = MKC_6,
	[0x24] = MKC_7,		[0x25] = MKC_8,		[0x26] = MKC_9,
	[0x27] = MKC_0,

	[0x28] = MKC_Return,
	[0x29] = MKC_formac_Escape,
	[0x2A] = MKC_BackSpace,
	[0x2B] = MKC_Tab,
	[0x2C] = MKC_Space,
	[0x2D] = MKC_Minus,
	[0x2E] = MKC_Equal,
	[0x2F] = MKC_LeftBracket,
	[0x30] = MKC_RightBracket,
	[0x31] = MKC_formac_BackSlash,
	[0x32] = MKC_formac_BackSlash,
	[0x33] = MKC_SemiColon,
	[0x34] = MKC_SingleQuote,
	[0x35] = MKC_formac_Grave,
	[0x36] = MKC_Comma,
	[0x37] = MKC_Period,
	[0x38] = MKC_formac_Slash,
	[0x39] = MKC_formac_CapsLock,

	[0x3A] = MKC_formac_F1,	[0x3B] = MKC_formac_F2,
	[0x3C] = MKC_formac_F3,	[0x3D] = MKC_formac_F4,
	[0x3E] = MKC_formac_F5,	[0x3F] = MKC_F6,
	[0x40] = MKC_F7,	[0x41] = MKC_F8,
	[0x42] = MKC_F9,	[0x43] = MKC_F10,
	[0x44] = MKC_F11,	[0x45] = MKC_F12,

	[0x46] = MKC_Print,
	[0x47] = MKC_ScrollLock,
	[0x48] = MKC_Pause,
	[0x49] = MKC_formac_Help,
	[0x4A] = MKC_formac_Home,
	[0x4B] = MKC_formac_PageUp,
	[0x4C] = MKC_formac_ForwardDel,
	[0x4D] = MKC_formac_End,
	[0x4E] = MKC_formac_PageDown,
	[0x4F] = MKC_Right,
	[0x50] = MKC_Left,
	[0x51] = MKC_Down,
	[0x52] = MKC_Up,

	[0x53] = MKC_formac_ForwardDel,
	[0x54] = MKC_KPDevide,
	[0x55] = MKC_KPMultiply,
	[0x56] = MKC_KPSubtract,
	[0x57] = MKC_KPAdd,
	[0x58] = MKC_formac_Enter,
	[0x59] = MKC_KP1,	[0x5A] = MKC_KP2,	[0x5B] = MKC_KP3,
	[0x5C] = MKC_KP4,	[0x5D] = MKC_KP5,	[0x5E] = MKC_KP6,
	[0x5F] = MKC_KP7,	[0x60] = MKC_KP8,	[0x61] = MKC_KP9,
	[0x62] = MKC_KP0,	[0x63] = MKC_Decimal,
	[0x64] = MKC_formac_BackSlash,
	[0x67] = MKC_KPEqual,

	[0xE0] = MKC_formac_Control,
	[0xE1] = MKC_formac_Shift,
	[0xE2] = MKC_formac_Option,
	[0xE3] = MKC_formac_Command,
	[0xE4] = MKC_formac_RControl,
	[0xE5] = MKC_formac_RShift,
	[0xE6] = MKC_formac_ROption,
	[0xE7] = MKC_formac_RCommand
};

LOCALVAR ui3b MkcDown[MKC_None + 1];

LOCALPROC CheckKeyboardState(void) {
	PS2KeyEvent event;
	ui3r mkc;
	while (PS2Input_GetKey(&event)) {
		mkc = Hid2Mkc[event.hid];
		if (MKC_None == mkc) {
			continue;
		}
		if (MKC_CM == mkc) {
			/* never generated by the USB keyboard */
			continue;
		}

		if (event.down) {
			if (MkcDown[mkc] == 0) {
				/* Deliver USB keys as physical keyboard events; the
				   frontend shortcut filter does not apply here. */
				Keyboard_UpdateKeyMap1(mkc, trueblnr);
			}
			if (MkcDown[mkc] < 255) ++MkcDown[mkc];
		} else {
			if (MkcDown[mkc] > 0) --MkcDown[mkc];
			if (MkcDown[mkc] == 0) {
				Keyboard_UpdateKeyMap1(mkc, falseblnr);
			}
		}
	}
}

LOCALPROC ToggleWantFullScreen(void) {
	PS2Video_CycleScaleMode();
}

/* --- time, date, location --- */

LOCALVAR ui5b TrueEmulatedTime = 0;

#include "DATE2SEC.h"

#define TicksPerSecond 1000000
/* Mini vMac advances the Macintosh clock at 60.14742 ticks/second. */
#define PS2_MAC_TICK_USEC 16626

LOCALVAR ui5b NewMacDateInSeconds;
LOCALVAR ui5b BootMacDate;

LOCALVAR ui5b LastTimeSec;
LOCALVAR ui5b LastTimeUsec;

LOCALPROC GetCurrentTicks(void) {
	u64 clocks = GetTimerSystemTime();
	u32 sec;
	u32 usec;

	TimerBusClock2USec(clocks, &sec, &usec);
	LastTimeSec = (ui5b)sec;
	LastTimeUsec = (ui5b)usec;
	NewMacDateInSeconds = (ui5b)(BootMacDate + sec);
}

#define MyInvTimeStep 16626 /* TicksPerSecond / 60.14742 */

LOCALVAR ui5b NextTimeSec;
LOCALVAR ui5b NextTimeUsec;

LOCALPROC IncrNextTime(void) {
	NextTimeUsec += MyInvTimeStep;
	if (NextTimeUsec >= TicksPerSecond) {
		NextTimeUsec -= TicksPerSecond;
		NextTimeSec += 1;
	}
}

LOCALPROC InitNextTime(void) {
	NextTimeSec = LastTimeSec;
	NextTimeUsec = LastTimeUsec;
	IncrNextTime();
}

LOCALPROC StartUpTimeAdjust(void) {
	GetCurrentTicks();
	InitNextTime();
}

LOCALFUNC si5b GetTimeDiff(void) {
	return ((si5b)(LastTimeSec - NextTimeSec)) * TicksPerSecond
		+ ((si5b)(LastTimeUsec - NextTimeUsec));
}

LOCALPROC UpdateTrueEmulatedTime(void) {
	si5b TimeDiff;

	GetCurrentTicks();

	TimeDiff = GetTimeDiff();
	if (TimeDiff >= 0) {
		if (TimeDiff > 16 * MyInvTimeStep) {
			/* emulation interrupted, catch up smoothly */
			++TrueEmulatedTime;
			InitNextTime();
		} else {
			do {
				++TrueEmulatedTime;
				IncrNextTime();
				TimeDiff = GetTimeDiff();
			} while (TimeDiff >= 0);
		}
	} else if (TimeDiff < - 16 * MyInvTimeStep) {
		/* clock set back, reset */
		InitNextTime();
	}
}

LOCALFUNC blnr CheckDateTime(void) {
	if (CurMacDateInSeconds != NewMacDateInSeconds) {
		CurMacDateInSeconds = NewMacDateInSeconds;
		return trueblnr;
	} else {
		return falseblnr;
	}
}

LOCALFUNC blnr InitLocationDat(void) {
	BootMacDate = Date2MacSeconds(0, 0, 12, 1, 1, 2024);
	CurMacDateInSeconds = BootMacDate;
	CurMacDelta = 0;
	GetCurrentTicks();
	InitNextTime();
	NewMacDateInSeconds = CurMacDateInSeconds;
	return trueblnr;
}

/* --- sound --- */

#if MySoundEnabled

#define kLn2SoundBuffers 4
#define kSoundBuffers (1 << kLn2SoundBuffers)
#define kSoundBuffMask (kSoundBuffers - 1)

#define DesiredMinFilledSoundBuffs 3

#define kLnOneBuffLen 9
#define kLnAllBuffLen (kLn2SoundBuffers + kLnOneBuffLen)
#define kOneBuffLen (1UL << kLnOneBuffLen)
#define kAllBuffLen (1UL << kLnAllBuffLen)
#define kLnOneBuffSz (kLnOneBuffLen + kLn2SoundSampSz - 3)
#define kLnAllBuffSz (kLnAllBuffLen + kLn2SoundSampSz - 3)
#define kOneBuffSz (1UL << kLnOneBuffSz)
#define kAllBuffSz (1UL << kLnAllBuffSz)
#define kOneBuffMask (kOneBuffLen - 1)
#define kAllBuffMask (kAllBuffLen - 1)
#define dbhBufferSize (kAllBuffSz + kOneBuffSz)

LOCALVAR tpSoundSamp TheSoundBuffer = nullpr;
LOCALVAR volatile ui4b ThePlayOffset;
LOCALVAR volatile ui4b TheFillOffset;
LOCALVAR volatile ui4b MinFilledSoundBuffs;
LOCALVAR ui4b TheWriteOffset;
LOCALVAR blnr HaveSoundOut = falseblnr;
LOCALVAR blnr SoundWantPlaying = falseblnr;
LOCALVAR blnr SoundHaveStarted = falseblnr;
LOCALVAR blnr SoundErrorReported = falseblnr;
LOCALVAR ui4b SoundLastv = kCenterSound;

#define SoundStepVal 0x40

LOCALPROC MySound_ReportError(void) {
	const char *error = PS2Audio_GetError();

	if (! SoundErrorReported && (error != nullpr) && (error[0] != 0)) {
		char message[96];
		snprintf(message, sizeof(message), "AUDIO ERROR: %.75s", error);
		PS2Video_SetStatus(message);
		PS2Video_SetOsd(message, 300);
		SoundErrorReported = trueblnr;
	}
}

LOCALPROC MySound_Init0(void) {
	ThePlayOffset = 0;
	TheFillOffset = 0;
	TheWriteOffset = 0;
	SoundErrorReported = falseblnr;
}

LOCALPROC MySound_Start0(void) {
	MinFilledSoundBuffs = kSoundBuffers + 1;
}

GLOBALOSGLUFUNC tpSoundSamp MySound_BeginWrite(ui4r n, ui4r *actL) {
	ui4b ToFillLen = kAllBuffLen - (TheWriteOffset - ThePlayOffset);
	ui4b WriteBuffContig =
		kOneBuffLen - (TheWriteOffset & kOneBuffMask);

	if (WriteBuffContig < n) {
		n = WriteBuffContig;
	}
	if (ToFillLen < n) {
		/* overwrite previous buffer */
		TheWriteOffset -= kOneBuffLen;
	}

	*actL = n;
	return TheSoundBuffer + (TheWriteOffset & kAllBuffMask);
}

LOCALPROC MySound_WroteABlock(void) {
	TheFillOffset = TheWriteOffset;
}

LOCALFUNC blnr MySound_EndWrite0(ui4r actL) {
	blnr v;

	TheWriteOffset += actL;

	if (0 != (TheWriteOffset & kOneBuffMask)) {
		v = falseblnr;
	} else {
		MySound_WroteABlock();
		v = trueblnr;
	}

	return v;
}

GLOBALOSGLUPROC MySound_EndWrite(ui4r actL) {
	(void) MySound_EndWrite0(actL);
}

LOCALPROC MySound_Pump(void) {
	u8 out[512];
	int want = 370;	/* Core rate uses 370 samples per emulation tick. */
	int pos = 0;

	if (! HaveSoundOut) {
		return;
	}

	/* audsrv reports free output bytes; PCM is 4 bytes per mono source
	   sample after conversion to signed 16-bit stereo. */
	if (PS2Audio_Available() < want * 4) {
		MySound_ReportError();
		return;
	}

	while (pos < want) {
		ui4b ToPlayLen = TheFillOffset - ThePlayOffset;
		ui4b FilledBuffs = ToPlayLen >> kLnOneBuffLen;
		ui4b contig;
		ui4b take;
		int i;

		if (! SoundWantPlaying) {
			/*
				Ramp gracefully to silence and then stop
				advancing the sound buffer.
			*/
			while ((pos < want) && (SoundLastv != kCenterSound)) {
				if (SoundLastv > kCenterSound) {
					if ((SoundLastv - kCenterSound) > SoundStepVal) {
						SoundLastv -= SoundStepVal;
					} else {
						SoundLastv = kCenterSound;
					}
				} else {
					if ((kCenterSound - SoundLastv) > SoundStepVal) {
						SoundLastv += SoundStepVal;
					} else {
						SoundLastv = kCenterSound;
					}
				}
				out[pos++] = (u8)SoundLastv;
			}
			while (pos < want) {
				out[pos++] = (u8)kCenterSound;
			}
			break;
		}

		if (! SoundHaveStarted) {
			ui4b v2;

			if (FilledBuffs < DesiredMinFilledSoundBuffs) {
				for (i = pos; i < want; ++i) {
					out[i] = (u8)kCenterSound;
				}
				pos = want;
				break;
			}

			v2 = TheSoundBuffer[ThePlayOffset & kAllBuffMask];
			while ((pos < want) && (SoundLastv != v2)) {
				if (SoundLastv > v2) {
					if ((SoundLastv - v2) > SoundStepVal) {
						SoundLastv -= SoundStepVal;
					} else {
						SoundLastv = v2;
					}
				} else {
					if ((v2 - SoundLastv) > SoundStepVal) {
						SoundLastv += SoundStepVal;
					} else {
						SoundLastv = v2;
					}
				}
				out[pos++] = (u8)SoundLastv;
			}
			if (SoundLastv == v2) {
				SoundHaveStarted = trueblnr;
			}
			continue;
		}

		if (0 == ToPlayLen) {
			while (pos < want) {
				out[pos++] = (u8)SoundLastv;
			}
			break;
		}

		contig = kAllBuffLen - (ThePlayOffset & kAllBuffMask);
		take = ToPlayLen;
		if (take > contig) {
			take = contig;
		}
		if (take > (ui4b)(want - pos)) {
			take = (ui4b)(want - pos);
		}

		memcpy(&out[pos],
			(TheSoundBuffer + (ThePlayOffset & kAllBuffMask)),
			take);
		SoundLastv = TheSoundBuffer[
			(ThePlayOffset + take - 1) & kAllBuffMask];
		ThePlayOffset += take;
		pos += take;

		if (FilledBuffs < MinFilledSoundBuffs) {
			MinFilledSoundBuffs = FilledBuffs;
		}
	}

	if (PS2Audio_Write(out, want) != want) {
		MySound_ReportError();
	}
}

LOCALPROC MySound_SecondNotify0(void) {
	if (MinFilledSoundBuffs <= kSoundBuffers) {
		if (MinFilledSoundBuffs > DesiredMinFilledSoundBuffs) {
			IncrNextTime();
		} else if (MinFilledSoundBuffs < DesiredMinFilledSoundBuffs) {
			++TrueEmulatedTime;
		}
		MinFilledSoundBuffs = kSoundBuffers + 1;
	}
}

LOCALPROC MySound_SecondNotify(void) {
	if (HaveSoundOut) {
		MySound_SecondNotify0();
	}
}

LOCALPROC MySound_Stop(void) {
	SoundWantPlaying = falseblnr;
	PS2Audio_Stop();
	if (! PS2Audio_IsReady()) {
		MySound_ReportError();
	}
}

LOCALPROC MySound_Start(void) {
	if (HaveSoundOut && ! SoundWantPlaying) {
		MySound_Start0();
		SoundLastv = kCenterSound;
		SoundHaveStarted = falseblnr;
		SoundWantPlaying = trueblnr;
	}
}

LOCALFUNC blnr MySound_Init(void) {
	MySound_Init0();

	HaveSoundOut = (PS2Audio_Init() != 0);
	if (HaveSoundOut) {
		MySound_Start();
	}

	return HaveSoundOut;
}

LOCALPROC MySound_UnInit(void) {
	MySound_Stop();
	PS2Audio_Quit();
	HaveSoundOut = falseblnr;
}

#endif /* MySoundEnabled */

/* --- basic dialogs --- */

LOCALPROC CheckSavedMacMsg(void) {
	if (nullpr != SavedBriefMsg) {
		char briefMsg0[ClStrMaxLength + 1];
		char longMsg0[ClStrMaxLength + 1];

		NativeStrFromCStr(briefMsg0, SavedBriefMsg);
		NativeStrFromCStr(longMsg0, SavedLongMsg);

		fprintf(stderr, "%s\n", briefMsg0);
		fprintf(stderr, "%s\n", longMsg0);

		SavedBriefMsg = nullpr;
	}
}

/* --- SavedTasks --- */

LOCALPROC LeaveSpeedStopped(void) {
#if MySoundEnabled
	MySound_Start();
#endif
	StartUpTimeAdjust();
}

LOCALPROC EnterSpeedStopped(void) {
#if MySoundEnabled
	MySound_Stop();
#endif
}

LOCALPROC CheckForSavedTasks(void) {
	if (MyEvtQNeedRecover) {
		MyEvtQNeedRecover = falseblnr;
		MyEvtQTryRecoverFromFull();
	}

	if (RequestMacOff) {
		RequestMacOff = falseblnr;
		if (AnyDiskInserted()) {
			MacMsgOverride(kStrQuitWarningTitle,
				kStrQuitWarningMessage);
		} else {
			ForceMacOff = trueblnr;
		}
	}

	if (ForceMacOff) {
		return;
	}

	if (CurSpeedStopped != SpeedStopped) {
		CurSpeedStopped = ! CurSpeedStopped;
		if (CurSpeedStopped) {
			EnterSpeedStopped();
		} else {
			LeaveSpeedStopped();
		}
	}

#if IncludeSonyNew
	if (vSonyNewDiskWanted) {
		MakeNewDiskAtDefault(vSonyNewDiskSize);
		vSonyNewDiskWanted = falseblnr;
	}
#endif

	if ((nullpr != SavedBriefMsg) & ! MacMsgDisplayed) {
		char status[80];
		char message[96];
		ui5r pc = m68k_get_pc_snapshot();

		/* Keep the first Mac/core error visible even if the Mac framebuffer
		   is black or the message screen is overwritten by a later frame. */
		snprintf(message, sizeof(message), "%s: %s",
			SavedBriefMsg, SavedLongMsg != NULL ? SavedLongMsg : "");
		PS2_BootTrace("Mac/core message at tick ");
		{
			char trace[48];
			snprintf(trace, sizeof(trace), "%u PC=%06X: ",
				(unsigned int)TrueEmulatedTime, (unsigned int)(pc & 0xFFFFFF));
			PS2_BootTrace(trace);
		}
		PS2_BootTrace(message);
#if WantAbnormalReports
		if (SavedIDMsg != 0) {
			char idLine[24];
			snprintf(idLine, sizeof(idLine), " (CORE ID %04X)",
				(unsigned int)SavedIDMsg);
			PS2_BootTrace(idLine);
		}
#endif
		PS2_BootTrace("\n");
#if WantAbnormalReports
		if (SavedIDMsg != 0) {
			snprintf(status, sizeof(status), "CORE ID %04X PC%06X T%05u",
				(unsigned int)SavedIDMsg, (unsigned int)(pc & 0xFFFFFF),
				(unsigned int)TrueEmulatedTime);
			PS2Video_SetOsd("CORE ABNORMAL", 600);
		} else
#endif
		{
			snprintf(status, sizeof(status), "MAC MSG: %.30s PC%06X T%05u",
				SavedBriefMsg, (unsigned int)(pc & 0xFFFFFF),
				(unsigned int)TrueEmulatedTime);
			PS2Video_SetOsd(SavedBriefMsg, 600);
		}
		PS2Video_SetStatus(status);
		MacMessageSticky = trueblnr;
		MacMsgDisplayOn();
	}

	if (NeedWholeScreenDraw) {
		NeedWholeScreenDraw = falseblnr;
		ScreenChangedAll();
	}

#if NeedRequestIthDisk
	if (0 != RequestIthDisk) {
		Sony_InsertIth(RequestIthDisk);
		RequestIthDisk = 0;
	}
#endif
}

/* --- main program flow --- */

GLOBALOSGLUPROC DoneWithDrawingForTick(void) {
#if MySoundEnabled
	if (! CurSpeedStopped) {
		MySound_Pump();
	}
#endif
	MyDrawChangesAndClear();
}

GLOBALOSGLUFUNC blnr ExtraTimeNotOver(void) {
	/*
		The deterministic WaitForNextTick above already advances one
		tick per 60 Hz slot.  Do not let the legacy NextTime
		calculation run catch-up ticks or overwrite TrueEmulatedTime.
	*/
	return falseblnr;
}

LOCALPROC CheckForSystemEvents(void) {
	static u64 LastPadUpdateClock = 0;
	u64 now;

	PS2Input_Poll();
	now = GetTimerSystemTime();
	/* PS2Input_Poll samples the mouse and pad every 16 ms, but this
	   function also runs repeatedly in the 1 ms scheduler wait loop.
	   Applying the same analog state on every pass multiplied movement
	   and flooded the Mac's small event queue, starving mouse and keys. */
	if ((LastPadUpdateClock == 0) ||
		((now - LastPadUpdateClock) >= MSec2TimerBusClock(16))) {
		LastPadUpdateClock = now;
		UpdatePadInput();
	}
	CheckKeyboardState();
}

GLOBALOSGLUPROC WaitForNextTick(void) {
	static u64 NextTickClock = 0;
	static int TickCount = 0;
	static ui5r LastStatusPc = 0xFFFFFFFF;
	static ui5r SamePcTicks = 0;

	CheckForSystemEvents();
	CheckForSavedTasks();
	if (ForceMacOff) {
		return;
	}

	if (CurSpeedStopped) {
		DoneWithDrawingForTick();
		DelayThread(2000);
		return;
	}

	/*
		Deterministic Macintosh tick scheduler (60.14742 Hz). The old
		implementation relied on GetTimeDiff/ExtraTimeNotOver and could
		leave OnTrueTime and CurEmulatedTime stuck, producing exactly
		one emulated tick and then a frozen (black) screen. Each call
		waits for one 16.626 ms Mac tick and advances its clock once.
	*/
	if (NextTickClock == 0) {
		NextTickClock = GetTimerSystemTime();
	}
	NextTickClock += USec2TimerBusClock(PS2_MAC_TICK_USEC);
	for (;;) {
		u64 now = GetTimerSystemTime();

		if (now >= NextTickClock) {
			break;
		}
		/*
			Keep the last Mac frame visible while waiting.  Drawing
			GetCurDrawBuff() here showed the comparison buffer instead
			of the live Mac framebuffer and could repeatedly cover it
			with a stale WAIT overlay.
		*/
		DelayThread(1000);
		CheckForSystemEvents();
		CheckForSavedTasks();
		if (ForceMacOff) {
			return;
		}
	}

	/*
		After a long pause (disk scan, USB mount) do not run a burst
		of catch-up ticks; resynchronise to the current time.
	*/
	{
		u64 now = GetTimerSystemTime();
		u64 period = USec2TimerBusClock(PS2_MAC_TICK_USEC);

		if ((now - NextTickClock) > (period * 4)) {
			NextTickClock = now;
		}
	}

	++TrueEmulatedTime;
	OnTrueTime = TrueEmulatedTime;

	{
		static int PcTraceCount = 0;
		static ui5r LastPc = 0;
		ui5r pc = m68k_get_pc_snapshot();
		char status[80];
		int keys = PS2Input_KeyEventCount();
		int mice = PS2Input_MouseEventCount();
		int mouseEnum = PS2Input_MouseEnumCount();
		int mouseDx;
		int mouseDy;
		unsigned int mouseEnumValue = PS2Input_MouseEnumValue();
		unsigned int lastHid = PS2Input_LastKeyHid();
		unsigned int lastMacKey = (lastHid < 256) ?
			(unsigned int)Hid2Mkc[lastHid] : (unsigned int)MKC_None;
		PS2Input_LastMouseDelta(&mouseDx, &mouseDy);

		if (pc == LastStatusPc) {
			if (SamePcTicks < 9999) {
				++SamePcTicks;
			}
		} else {
			LastStatusPc = pc;
			SamePcTicks = 0;
		}

		snprintf(status, sizeof(status),
			"K%d:%d/%02X>%02X M%d%d:%d E%d:%X R%d %d,%d P%d T%u PC%X",
			PS2Input_KeyboardReady(), keys, lastHid, lastMacKey,
			PS2Input_MouseReady(), PS2Input_MousePresent(), mice,
			mouseEnum, mouseEnumValue, PS2Input_MouseReadOk(),
			mouseDx, mouseDy, PS2Input_PadReady(),
			(unsigned int)TrueEmulatedTime, (unsigned int)(pc & 0xFFFFFF));
		if (! MacMessageSticky) {
			PS2Video_SetStatus(status);
		}

		/* Sample the hot ROM loop once per Mac tick, not once per frame. */
		if (((pc & 0x00FFFFFF) >= 0x00000350)
			&& ((pc & 0x00FFFFFF) <= 0x00000364)
			&& (pc != LastPc) && (PcTraceCount < 8)) {
			char trace[96];
			snprintf(trace, sizeof(trace),
				"ROM loop sample %d: T=%u PC=%06X\n",
				PcTraceCount + 1, (unsigned int)TrueEmulatedTime,
				(unsigned int)(pc & 0xFFFFFF));
			PS2_BootTrace(trace);
			LastPc = pc;
			++PcTraceCount;
		}
	}

	/* Low-rate heartbeat distinguishes a frozen Mac frame from a CPU/core
	   that stopped advancing; sustained same-PC samples are recorded too. */
	if ((TrueEmulatedTime % 60) == 0) {
		char heartbeat[80];
		ui5r pc = m68k_get_pc_snapshot();
		snprintf(heartbeat, sizeof(heartbeat),
			"Emulation heartbeat T=%u PC=%06X same=%u ticks\n",
			(unsigned int)TrueEmulatedTime, (unsigned int)(pc & 0xFFFFFF),
			(unsigned int)SamePcTicks);
		PS2_BootTrace(heartbeat);
	}
	if ((SamePcTicks > 0) && ((SamePcTicks % 60) == 0)) {
		char held[80];
		snprintf(held, sizeof(held),
			"PC held at %06X for %u ticks (T=%u)\n",
			(unsigned int)(LastStatusPc & 0xFFFFFF),
			(unsigned int)SamePcTicks, (unsigned int)TrueEmulatedTime);
		PS2Video_SetOsd(held, 90);
	}

	if (TickCount < 5) {
		char tickLine[64];

		PS2Trace("[DEBUG] Loop: 60Hz tick %d\n", TickCount + 1);
		snprintf(tickLine, sizeof(tickLine), "68000 scheduler tick %d\n",
			TickCount + 1);
		PS2_BootTrace(tickLine);
		++TickCount;
	}

	if (CheckDateTime()) {
#if MySoundEnabled
		MySound_SecondNotify();
#endif
#if EnableDemoMsg
		DemoModeSecondNotify();
#endif
	}

	CheckMouseState();
}

/* --- platform independent code can be thought of as going here --- */

#include "PROGMAIN.h"

LOCALPROC ReserveAllocAll(void) {
#if dbglog_HAVE
	dbglog_ReserveAlloc();
#endif
	ReserveAllocOneBlock(&ROM, kROM_Size, 5, falseblnr);

	ReserveAllocOneBlock(&screencomparebuff,
		vMacScreenNumBytes, 5, trueblnr);
#if UseControlKeys
	ReserveAllocOneBlock(&CntrlDisplayBuff,
		vMacScreenNumBytes, 5, falseblnr);
#endif

#if MySoundEnabled
	ReserveAllocOneBlock(&TheSoundBuffer, dbhBufferSize, 5, falseblnr);
#endif

	EmulationReserveAlloc();
}

LOCALFUNC blnr AllocMyMemory(void) {
	uimr n;
	blnr IsOk = falseblnr;

	ReserveAllocOffset = 0;
	ReserveAllocBigBlock = nullpr;
	ReserveAllocAll();
	n = ReserveAllocOffset;
	ReserveAllocBigBlock = (ui3p)calloc(1, n);
	if (NULL == ReserveAllocBigBlock) {
		MacMsg(kStrOutOfMemTitle, kStrOutOfMemMessage, trueblnr);
	} else {
		ReserveAllocOffset = 0;
		ReserveAllocAll();
		if (n != ReserveAllocOffset) {
			/* oops, program error */
		} else {
			IsOk = trueblnr;
		}
	}

	return IsOk;
}

LOCALPROC UnallocMyMemory(void) {
	if (nullpr != ReserveAllocBigBlock) {
		free((char *)ReserveAllocBigBlock);
	}
}

LOCALFUNC blnr InitOSGLU(void) {
	int mounted;
	char vmsg[160];

	memset(MkcDown, 0, sizeof(MkcDown));
	memset(PadMkcDown, 0, sizeof(PadMkcDown));
	(void) ScanCommandLine();

	/* Mount USB before GS takeover; diagnostics go to file, not libdebug. */
	TraceToScreen = falseblnr;
	TraceSilent = trueblnr;
	PS2_SetStage("wait for USB storage");
	if (! PS2_WaitForStorage(20000)) {
		PS2_Panic("USB storage did not mount within 20 seconds.");
		return falseblnr;
	}
	PS2_BootTraceStart();
	PS2_BootTrace("platform modules loaded; USB storage mounted\n");
	PS2_SetStage("gsKit video init");
	if (! PS2Video_Init()) {
		snprintf(vmsg, sizeof(vmsg), "gsKit init failed: %s\n",
			PS2Video_GetLastError());
		PS2_BootTrace(vmsg);
		PS2_Panic(vmsg);
		return falseblnr;
	}
	PS2Video_BootLogReset();
	PS2_BootTrace("gsKit video initialization complete\n");
	PS2Video_BootLog("Mini vMac PS2 V8.6\n");
	PS2Video_BootLog("----------------------------\n");
	PS2Video_BootLog("gsKit video ............. OK\n");
	if (PS2_LastModuleError()[0] != 0) {
		PS2Video_BootLog("IOP warning: ");
		PS2Video_BootLog(PS2_LastModuleError());
		PS2Video_BootLog("\n");
	}

	PS2_SetStage("allocate emulator memory");
	PS2Video_BootLog("Emulator memory ......... ");
	if (! AllocMyMemory()) {
		PS2_Panic("Out of EE memory while reserving the Macintosh RAM.");
		return falseblnr;
	}
	PS2Video_BootLog("OK\n");

	PS2Video_BootLog("USB storage ............. OK\n");

	PS2_SetStage("load Macintosh ROM");
	PS2_BootTrace("loading Macintosh ROM\n");
	PS2Video_BootLog("Macintosh ROM ........... ");
	if (! LoadMacRom()) {
		return falseblnr;
	}
	PS2Video_BootLog("OK\n");

	PS2_SetStage("location/date init");
	PS2_BootTrace("initializing RTC and date\n");
	PS2Video_BootLog("RTC / location .......... ");
	if (! InitLocationDat()) {
		PS2_Panic("Failed to initialize the real-time clock/location.");
		return falseblnr;
	}
	PS2Video_BootLog("OK\n");

	PS2_SetStage("scan disk images");
	PS2_BootTrace("scanning and loading disk images\n");
	PS2Video_BootLog("Disk images ............. ");
	InitDriveFds();
	mounted = LoadInitialImages();
	{
		char dline[48];

		snprintf(dline, sizeof(dline), "%d mounted\n", mounted);
		PS2Video_BootLog(dline);
	}

	PS2_SetStage("input modules");
	PS2Video_BootLog("Input modules ........... ");
	PS2_LoadInputModules();
	PS2Input_Init();
	{
		char iline[48];

		snprintf(iline, sizeof(iline), "caps=%d\n", PS2_InputCaps());
		PS2Video_BootLog(iline);
	}

	PS2_SetStage("audio modules");
	PS2Video_BootLog("Audio ................... ");
	if (! PS2_LoadAudioModules()) {
		char message[96];
		PS2Video_BootLog("module load failed: ");
		PS2Video_BootLog(PS2_LastModuleError());
		PS2Video_BootLog("\n");
		snprintf(message, sizeof(message), "AUDIO MODULE FAILED: %.64s",
			PS2_LastModuleError());
		PS2Video_SetStatus(message);
		PS2Video_SetOsd(message, 300);
	} else if (! MySound_Init()) {
		char message[96];
		PS2Video_BootLog("audsrv init failed: ");
		PS2Video_BootLog(PS2Audio_GetError());
		PS2Video_BootLog("\n");
		snprintf(message, sizeof(message), "AUDIO INIT FAILED: %.65s",
			PS2Audio_GetError());
		PS2Video_SetStatus(message);
		PS2Video_SetOsd(message, 300);
	} else {
		PS2Video_BootLog("OK: 22050 Hz, 16-bit stereo\n");
	}

	PS2_SetStage("wait for ROM / start emulation");
	PS2_BootTrace("ROM ready; entering core initialization\n");
	PS2Video_BootLog("Starting Macintosh ...... ");
	if (! WaitForRom()) {
		PS2_Panic("The Macintosh ROM did not become ready.");
		return falseblnr;
	}
	PS2Video_BootLog("OK\n");
	PS2Video_BootLog("Entering 68000 emulation...\n");
	/* BootTrace writes go to the BDM USB filesystem. It is useful during
	   startup, but must not keep the flash device busy during Mac runtime. */
	PS2_BootTraceStop();
	PS2Video_SetStatus("BOOTING 68000");
	/* Hold the log long enough to be readable on a TV. */
	DelayThread(3000 * 1000);
	return trueblnr;
}

LOCALPROC CloseRomFile(void) {
	if (RomFileRef != NULL) {
		PS2Trace("[DEBUG] ROM: leaving ROM handle for IOP reset.\n");
		RomFileRef = NULL;
	}
}

LOCALPROC UnInitOSGLU(void) {
	if (MacMsgDisplayed) {
		MacMsgDisplayOff();
	}

#if MySoundEnabled
	MySound_UnInit();
#endif
	UnInitDrives();
	PS2Input_Quit();
	PS2Video_Quit();
	CloseRomFile();
	UnallocMyMemory();

	CheckSavedMacMsg();
}

int main(int argc, char *argv[]) {
	(void) argc;
	(void) argv;

	/* Do not initialize the ps2sdk text console; use the file trace. */
	PS2_PlatformInit();

	if (InitOSGLU()) {
		PS2_SetStage("ProgramMain / 68000 emulation");
		PS2_BootTrace("calling ProgramMain\n");
		PS2Trace("[DEBUG] main: entering ProgramMain()/MainEventLoop.\n");
		ProgramMain();
		PS2_BootTrace("ProgramMain returned (core init failed or emulation stopped)\n");
		PS2Trace("[DEBUG] main: ProgramMain() returned.\n");
	}
	UnInitOSGLU();

	return 0;
}

#endif /* WantOSGLUPS2 */
