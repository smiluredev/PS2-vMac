#ifndef PS2_PLATFORM_H
#define PS2_PLATFORM_H

#include <tamtypes.h>

/* Reset the IOP, load the core/USB modules and bring up file I/O. */
void PS2_PlatformInit(void);

/* Optional modules, loaded after the ROM/disks have been read. */
void PS2_LoadInputModules(void);
int PS2_LoadAudioModules(void);

/* Bit 0 = pad, bit 1 = keyboard, bit 2 = mouse. */
int PS2_InputCaps(void);

/* Track the current boot/emulation stage for diagnostics. */
void PS2_SetStage(const char *stage);
const char *PS2_GetStage(void);

/* Append boot milestones to BOOTLOG.TXT on the mounted USB volume. */
int PS2_BootTraceStart(void);
void PS2_BootTrace(const char *message);
void PS2_BootTraceStop(void);

/* Show a fatal message on the PS2 debug screen and halt. */
void PS2_Panic(const char *message);

/* Return 1 if the file can be opened for reading. */
int PS2_FileExists(const char *path);

/*
	Resolve a bare file name against the usual storage locations
	(mass:/minivmac, mass:/, mass0:/minivmac, mass0:/, mc0:/minivmac, mc0:/, ...).
	Returns 1 and writes the found path into out on success.
*/
int PS2_ResolvePath(const char *name, char *out, int outSize);

/*
	Resolve a base file name trying multiple extensions across usual storage locations.
	Returns 1 and writes the found path into out on success.
*/
int PS2_ResolvePathExt(const char *baseName, const char *exts[], int numExts,
	char *out, int outSize);

/*
	Poll for a file for up to timeoutMs milliseconds.  Returns 1 and
	writes the resolved path into out on success.
*/
int PS2_WaitForPath(const char *name, char *out, int outSize, int timeoutMs);

/*
	Poll for any of the given candidate filenames for up to timeoutMs milliseconds.
	Returns 1 and writes the resolved path into out on success.
*/
int PS2_WaitForAnyPath(const char *names[], int numNames,
	char *out, int outSize, int timeoutMs);

/*
	Wait for USB mass storage to be mounted and ready.
	Returns 1 if mass:/ or mass0:/ becomes ready, 0 on timeout.
*/
int PS2_WaitForStorage(int timeoutMs);

/* Enable/disable the on-screen debug console.  Used by the panic path. */
void PS2_DebugScreenInit(void);

/* Portable filename of the last failed module, for diagnostics. */
const char *PS2_LastModuleError(void);

/* Print every embedded IRX name and its SifExecModuleBuffer result. */
void PS2_DumpModuleStatus(void);

/* List the root of each mass: mount and probe for known ROM/disk names. */
void PS2_DumpStorage(void);

#endif /* PS2_PLATFORM_H */
