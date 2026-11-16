#ifndef PS2_VIDEO_H
#define PS2_VIDEO_H

#include <tamtypes.h>

#define PS2_SCALE_1X     0  /* 1:1 Pixel-Perfect Centered (512x342) */
#define PS2_SCALE_ASPECT 1  /* Aspect-Correct Proportional (640x428) */
#define PS2_SCALE_FULL   2  /* Fullscreen Stretched (640x480) */

/* Set up gsKit with double buffering and 512x342 Mac texture. */
int PS2Video_Init(void);

/* Human readable reason for the last PS2Video_Init failure. */
const char *PS2Video_GetLastError(void);

/* True once gsKit is drawing and the texture buffer is valid. */
int PS2Video_IsReady(void);

/* Append a panic message to the gsKit boot log and present it. */
void PS2Video_ShowPanic(const char *message);

/*
	Convert the 512x342 one bit Macintosh framebuffer and present it.
	Bit set means black, bit clear means white.
*/
void PS2Video_DrawFrame(const u8 *macFrameBuffer);

/* Scale mode controls */
void PS2Video_SetScaleMode(int mode);

/* Temporary diagnostic banner; frames = how many presents to keep it. */
void PS2Video_SetOsd(const char *message, int frames);
void PS2Video_SetStatus(const char *message);
void PS2Video_CycleScaleMode(void);
int PS2Video_GetScaleMode(void);
const char *PS2Video_GetScaleModeName(void);

/* Boot-time text output drawn through gsKit (works after PS2Video_Init). */
void PS2Video_BootLogReset(void);
void PS2Video_BootLog(const char *text);

void PS2Video_Quit(void);

#endif /* PS2_VIDEO_H */
