/*
	ps2_video.c

	gsKit output for the 512x342 one bit Macintosh Plus screen.
	Supports double buffering (no tearing), multiple scaling modes
	(1:1 Pixel-Perfect, Aspect-Correct, Fullscreen), and an on-screen
	display (OSD) notification banner.
*/

#include <tamtypes.h>
#include <kernel.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

#include <gsKit.h>
#include <dmaKit.h>
#include <debug.h>
#include <delaythread.h>

#include "ps2_video.h"
#include "ps2_platform.h"

#define MAC_WIDTH 512
#define MAC_HEIGHT 342
#define MAC_ROW_BYTES (MAC_WIDTH / 8)

#define TEX_WIDTH 512
#define TEX_HEIGHT 512
#define TEX_PIXELS (TEX_WIDTH * TEX_HEIGHT)

#define COLOR_WHITE 0xFFFFFFFFu
#define COLOR_BLACK 0xFF000000u

static GSGLOBAL *gsGlobal = NULL;
static GSTEXTURE Texture;
static u32 *TextureMem = NULL;
static const char *VideoLastError = "none";

/*
	The texture lives in a static, 128-byte aligned buffer.  Keeping it
	out of the EE heap removes the large memalign() that used to fail
	on real hardware after the emulator reserved its Macintosh RAM.
*/
static u32 TextureStorage[TEX_PIXELS] __attribute__((aligned(128)));
static int TextureIs16 = 0;
static u16 PixelLUT16[256][8];

const char *PS2Video_GetLastError(void)
{
	return VideoLastError;
}

int PS2Video_IsReady(void)
{
	return (gsGlobal != NULL) && (TextureMem != NULL);
}

void PS2Video_ShowPanic(const char *message)
{
	if (! PS2Video_IsReady()) {
		return;
	}
	PS2Video_BootLog("\n*** SYSTEM PANIC ***\n");
	if (message != NULL) {
		PS2Video_BootLog(message);
	}
	PS2Video_BootLog("\n");
}

static int CurrentScaleMode = PS2_SCALE_1X;
static int DstX = 0;
static int DstY = 0;
static int DstW = MAC_WIDTH;
static int DstH = MAC_HEIGHT;

static int OsdFramesRemaining = 0;
static char OsdMessage[64] = "";
static char StatusMessage[96] = "";

static u32 PixelLUT[256][8];

/* Boot-time text screen rendered through the same gsKit texture path. */
static u8 BootScreen[MAC_HEIGHT * MAC_ROW_BYTES];
static int BootX = 0;
static int BootY = 0;

/* 8x8 font for standard ASCII 32..126 */
static const u8 Font8x8[96][8] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
	{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* '!' */
	{0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* '"' */
	{0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* '#' */
	{0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* '$' */
	{0x00,0x66,0xA6,0xD4,0x2B,0x65,0x66,0x00}, /* '%' */
	{0x1C,0x36,0x1C,0x38,0x6F,0x66,0x3B,0x00}, /* '&' */
	{0x30,0x30,0x10,0x20,0x00,0x00,0x00,0x00}, /* ''' */
	{0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* '(' */
	{0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ')' */
	{0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* '*' */
	{0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* '+' */
	{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* ',' */
	{0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* '-' */
	{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* '.' */
	{0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* '/' */
	{0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, /* '0' */
	{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* '1' */
	{0x3C,0x66,0x06,0x0C,0x30,0x60,0x7E,0x00}, /* '2' */
	{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* '3' */
	{0x0C,0x1C,0x34,0x64,0x7E,0x04,0x0E,0x00}, /* '4' */
	{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* '5' */
	{0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* '6' */
	{0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, /* '7' */
	{0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* '8' */
	{0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* '9' */
	{0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, /* ':' */
	{0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, /* ';' */
	{0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* '<' */
	{0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* '=' */
	{0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* '>' */
	{0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00}, /* '?' */
	{0x3C,0x42,0x99,0xA5,0xA5,0x9E,0x40,0x3C}, /* '@' */
	{0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, /* 'A' */
	{0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, /* 'B' */
	{0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, /* 'C' */
	{0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, /* 'D' */
	{0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, /* 'E' */
	{0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, /* 'F' */
	{0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00}, /* 'G' */
	{0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* 'H' */
	{0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'I' */
	{0x0E,0x06,0x06,0x06,0x66,0x66,0x3C,0x00}, /* 'J' */
	{0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, /* 'K' */
	{0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, /* 'L' */
	{0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, /* 'M' */
	{0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, /* 'N' */
	{0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* 'O' */
	{0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* 'P' */
	{0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, /* 'Q' */
	{0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, /* 'R' */
	{0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, /* 'S' */
	{0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* 'T' */
	{0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* 'U' */
	{0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, /* 'V' */
	{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 'W' */
	{0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, /* 'X' */
	{0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, /* 'Y' */
	{0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, /* 'Z' */
	{0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* '[' */
	{0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* '\' */
	{0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ']' */
	{0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* '^' */
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* '_' */
	{0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* '`' */
	{0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, /* 'a' */
	{0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, /* 'b' */
	{0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, /* 'c' */
	{0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, /* 'd' */
	{0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, /* 'e' */
	{0x0E,0x18,0x7E,0x18,0x18,0x18,0x18,0x00}, /* 'f' */
	{0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C}, /* 'g' */
	{0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 'h' */
	{0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* 'i' */
	{0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C}, /* 'j' */
	{0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, /* 'k' */
	{0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'l' */
	{0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, /* 'm' */
	{0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, /* 'n' */
	{0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, /* 'o' */
	{0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, /* 'p' */
	{0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x07}, /* 'q' */
	{0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, /* 'r' */
	{0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, /* 's' */
	{0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00}, /* 't' */
	{0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, /* 'u' */
	{0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, /* 'v' */
	{0x00,0x00,0x63,0x6B,0x7F,0x3E,0x36,0x00}, /* 'w' */
	{0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, /* 'x' */
	{0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, /* 'y' */
	{0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, /* 'z' */
	{0x0C,0x18,0x18,0x30,0x18,0x18,0x0C,0x00}, /* '{' */
	{0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* '|' */
	{0x30,0x18,0x18,0x0C,0x18,0x18,0x30,0x00}, /* '}' */
	{0x00,0x00,0x32,0x4C,0x00,0x00,0x00,0x00}, /* '~' */
	{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}  /* DEL */
};

static void BuildPixelLUT(void)
{
	int value;
	int bit;

	for (value = 0; value < 256; ++value) {
		for (bit = 0; bit < 8; ++bit) {
			if (value & (0x80 >> bit)) {
				PixelLUT[value][bit] = COLOR_BLACK;
				PixelLUT16[value][bit] = 0x8000; /* opaque black */
			} else {
				PixelLUT[value][bit] = COLOR_WHITE;
				PixelLUT16[value][bit] = 0xFFFF; /* opaque white */
			}
		}
	}
}

static void UpdateScaleGeometry(void)
{
	int scrW = (gsGlobal != NULL && gsGlobal->Width > 0) ? gsGlobal->Width : 640;
	int scrH = (gsGlobal != NULL && gsGlobal->Height > 0) ? gsGlobal->Height : 480;

	switch (CurrentScaleMode) {
	case PS2_SCALE_1X:
	default:
		DstW = MAC_WIDTH;
		DstH = MAC_HEIGHT;
		DstX = (scrW - DstW) / 2;
		DstY = (scrH - DstH) / 2;
		Texture.Filter = GS_FILTER_NEAREST;
		break;

	case PS2_SCALE_ASPECT:
		/*
			Macintosh Plus aspect ratio: 512 / 342 = 1.497.
			Fit 640 width -> 640 * 342 / 512 = 427.5 (~428 height).
		*/
		DstW = scrW;
		DstH = (scrW * MAC_HEIGHT) / MAC_WIDTH;
		if (DstH > scrH) {
			DstH = scrH;
			DstW = (scrH * MAC_WIDTH) / MAC_HEIGHT;
		}
		DstX = (scrW - DstW) / 2;
		DstY = (scrH - DstH) / 2;
		Texture.Filter = GS_FILTER_LINEAR;
		break;

	case PS2_SCALE_FULL:
		DstX = 0;
		DstY = 0;
		DstW = scrW;
		DstH = scrH;
		Texture.Filter = GS_FILTER_LINEAR;
		break;
	}

	if (DstX < 0) DstX = 0;
	if (DstY < 0) DstY = 0;
}

const char *PS2Video_GetScaleModeName(void)
{
	switch (CurrentScaleMode) {
	case PS2_SCALE_1X:
		return "1:1 Pixel-Perfect (512x342)";
	case PS2_SCALE_ASPECT:
		return "Aspect-Correct (640x428)";
	case PS2_SCALE_FULL:
		return "Fullscreen Stretch (640x480)";
	default:
		return "Unknown";
	}
}

int PS2Video_GetScaleMode(void)
{
	return CurrentScaleMode;
}

void PS2Video_SetScaleMode(int mode)
{
	int i;

	if (mode < 0 || mode > PS2_SCALE_FULL) {
		mode = PS2_SCALE_1X;
	}
	CurrentScaleMode = mode;
	UpdateScaleGeometry();

	snprintf(OsdMessage, sizeof(OsdMessage), "Scale: %s",
		PS2Video_GetScaleModeName());
	OsdFramesRemaining = 90; /* ~1.5s at 60Hz */

	/* Clear both buffers when mode changes to prevent border artifacts */
	if (gsGlobal != NULL) {
		for (i = 0; i < 4; ++i) {
			gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
			gsKit_queue_exec(gsGlobal);
			gsKit_sync_flip(gsGlobal);
		}
	}
}

void PS2Video_SetOsd(const char *message, int frames)
{
	if (message == NULL) {
		OsdMessage[0] = 0;
		OsdFramesRemaining = 0;
		return;
	}
	snprintf(OsdMessage, sizeof(OsdMessage), "%s", message);
	OsdFramesRemaining = frames;
}

void PS2Video_SetStatus(const char *message)
{
	if (message == NULL) {
		StatusMessage[0] = 0;
	} else {
		snprintf(StatusMessage, sizeof(StatusMessage), "%s", message);
	}
}

void PS2Video_CycleScaleMode(void)
{
	int next = (CurrentScaleMode + 1) % 3;
	PS2Video_SetScaleMode(next);
}

int PS2Video_Init(void)
{
	u32 texSizeVRAM;
	int i;

	VideoLastError = "not started";
	TextureIs16 = 0;
	BuildPixelLUT();

	/* Use the same initialization sequence as the project's original
	   gsKit port.  dmaKit_init/chan_init here reprogrammed GIF DMA a
	   second time and caused a black-screen hang before the first draw. */
	PS2_BootTrace("video: gsKit_init_global begin\n");
	gsGlobal = gsKit_init_global();
	if (gsGlobal == NULL) {
		VideoLastError = "gsKit_init_global returned NULL";
		return 0;
	}

	PS2_BootTrace("video: gsKit_init_global complete\n");
	gsGlobal->Mode = gsKit_check_rom();
	/*
		Use the TV-friendly interlaced mode.  The ps2sdk text console
		(which the user can see) is 480i; many PAL/NTSC televisions
		reject gsKit's 240p non-interlaced signal and show a black
		screen even though the GS is running.
	*/
	gsGlobal->Interlace = GS_INTERLACED;
	gsGlobal->Field = GS_FIELD;
	gsGlobal->DoubleBuffering = GS_SETTING_ON;
	gsGlobal->ZBuffering = GS_SETTING_OFF;
	gsGlobal->Dithering = GS_SETTING_OFF;
	gsGlobal->PSM = GS_PSM_CT32;
	gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

	PS2_BootTrace("video: gsKit_init_screen begin\n");
	gsKit_init_screen(gsGlobal);
	PS2_BootTrace("video: gsKit_init_screen complete\n");
	if ((gsGlobal->Width <= 0) || (gsGlobal->Height <= 0)) {
		VideoLastError = "gsKit_init_screen returned an invalid mode";
		return 0;
	}

	/* Allocate texture VRAM through gsKit after it reserves the screen
	   framebuffers; do not guess a fixed address that may overlap them. */
	Texture.Width = TEX_WIDTH;
	Texture.Height = TEX_HEIGHT;
	Texture.PSM = GS_PSM_CT16;
	Texture.ClutPSM = GS_PSM_CT16;
	Texture.Filter = GS_FILTER_NEAREST;
	Texture.Clut = NULL;
	Texture.VramClut = 0;
	Texture.Delayed = 0;
	Texture.Mem = TextureStorage;
	gsKit_setup_tbw(&Texture);
	TextureIs16 = 1;
	texSizeVRAM = gsKit_texture_size(TEX_WIDTH, TEX_HEIGHT, GS_PSM_CT16);
	Texture.Vram = gsKit_vram_alloc(gsGlobal, texSizeVRAM,
		GSKIT_ALLOC_USERBUFFER);
	if (Texture.Vram == GSKIT_ALLOC_ERROR) {
		VideoLastError = "gsKit could not allocate texture VRAM";
		PS2_BootTrace("video: texture VRAM allocation failed\n");
		return 0;
	}
	VideoLastError = "none (gsKit CT16 texture allocation)";

	memset(TextureStorage, 0, sizeof(TextureStorage));
	TextureMem = TextureStorage;

	gsKit_mode_switch(gsGlobal, GS_ONESHOT);
	gsKit_vram_clear(gsGlobal);
	for (i = 0; i < 4; ++i) {
		gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
		gsKit_queue_exec(gsGlobal);
		gsKit_sync_flip(gsGlobal);
	}

	UpdateScaleGeometry();

	PS2_BootTrace("video: initial GS framebuffers cleared\n");

	/*
		Visible bring-up pattern using untextured GS primitives. Retain
		until a hardware test confirms the GS mode and present path.
	*/
	{
		static const u64 testColors[4] = {
			GS_SETREG_RGBAQ(255, 0, 0, 0x80, 0),
			GS_SETREG_RGBAQ(0, 255, 0, 0x80, 0),
			GS_SETREG_RGBAQ(0, 0, 255, 0x80, 0),
			GS_SETREG_RGBAQ(255, 255, 255, 0x80, 0)
		};
		int c;

		static const char *const labels[4] = {
			"video test: red\n", "video test: green\n",
			"video test: blue\n", "video test: white\n"
		};

		for (c = 0; c < 4; ++c) {
			PS2_BootTrace(labels[c]);
			gsKit_clear(gsGlobal, testColors[c]);
			gsKit_queue_exec(gsGlobal);
			gsKit_sync_flip(gsGlobal);
			DelayThread(700 * 1000);
		}
	}

	/*
		Second self-test: a 64x64 black/white checkerboard goes through
		the exact texture upload + textured sprite path used by the Mac.
		If the colour bars were visible but this is black, the texture
		upload/VRAM address is the problem.
	*/
	{
		int y;
		int x;

		for (y = 0; y < MAC_HEIGHT; ++y) {
			for (x = 0; x < MAC_ROW_BYTES; ++x) {
				BootScreen[y * MAC_ROW_BYTES + x] =
					(((x >> 3) + (y >> 6)) & 1) ? 0xFF : 0x00;
			}
		}
		PS2Video_DrawFrame(BootScreen);
		DelayThread(2000 * 1000);
		memset(BootScreen, 0, sizeof(BootScreen));
	}

	return 1;
}

static void BootScreenScroll(void)
{
	memmove(BootScreen, BootScreen + MAC_ROW_BYTES,
		sizeof(BootScreen) - MAC_ROW_BYTES);
	memset(BootScreen + sizeof(BootScreen) - MAC_ROW_BYTES, 0,
		MAC_ROW_BYTES);
}

static void BootPutChar(char c)
{
	int gi, row, col;

	if (c == '\r') {
		BootX = 0;
		return;
	}
	if (c == '\n') {
		BootX = 0;
		BootY += 8;
	} else {
		if ((c < 32) || (c > 127)) {
			c = '?';
		}
		if (BootX + 8 > MAC_WIDTH) {
			BootX = 0;
			BootY += 8;
		}
		if (BootY + 8 > MAC_HEIGHT) {
			BootScreenScroll();
			BootY -= 8;
		}
		gi = c - 32;
		for (row = 0; row < 8; ++row) {
			u8 bits = Font8x8[gi][row];
			if (bits == 0) {
				continue;
			}
			for (col = 0; col < 8; ++col) {
				if (bits & (0x80 >> col)) {
					int x = BootX + col;
					int y = BootY + row;
					BootScreen[y * MAC_ROW_BYTES + (x >> 3)] |=
						(u8)(0x80 >> (x & 7));
				}
			}
		}
		BootX += 8;
	}
	if (BootY + 8 > MAC_HEIGHT) {
		BootScreenScroll();
		BootY -= 8;
	}
}

void PS2Video_BootLogReset(void)
{
	memset(BootScreen, 0, sizeof(BootScreen)); /* 0 bits = white */
	BootX = 0;
	BootY = 0;
	if ((gsGlobal != NULL) && (TextureMem != NULL)) {
		PS2Video_DrawFrame(BootScreen);
	}
}

void PS2Video_BootLog(const char *text)
{
	if ((gsGlobal == NULL) || (TextureMem == NULL) || (text == NULL)) {
		return;
	}
	while (*text != 0) {
		BootPutChar(*text++);
	}
	PS2Video_DrawFrame(BootScreen);
}

static void DrawStatusLine(void)
{
	int len = strlen(StatusMessage);
	int i, row, col;
	int startX = 16;
	int startY = 8;
	u64 boxColor = GS_SETREG_RGBAQ(0, 0, 0, 128, 0);
	u64 textColor = GS_SETREG_RGBAQ(255, 255, 255, 0, 0);

	if (len == 0) return;
	gsKit_prim_sprite(gsGlobal, startX, startY,
		startX + len * 8 + 12, startY + 20, 2, boxColor);
	for (i = 0; i < len; ++i) {
		unsigned char c = (unsigned char)StatusMessage[i];
		int glyphIdx = (c >= 32 && c <= 127) ? (c - 32) : 0;
		for (row = 0; row < 8; ++row) {
			u8 bits = Font8x8[glyphIdx][row];
			for (col = 0; col < 8; ++col) {
				if (bits & (0x80 >> col)) {
					int x = startX + 6 + i * 8 + col;
					int y = startY + 6 + row;
					gsKit_prim_sprite(gsGlobal, x, y,
						x + 1, y + 1, 3, textColor);
				}
			}
		}
	}
}

static void DrawOSDBanner(void)
{
	int len = strlen(OsdMessage);
	int charW = 8;
	int charH = 8;
	int padX = 10;
	int padY = 6;
	int boxW = len * charW + (padX * 2);
	int boxH = charH + (padY * 2);
	int startX = (gsGlobal->Width - boxW) / 2;
	int startY = 24;
	int i, row, col;
	u64 boxColor = GS_SETREG_RGBAQ(16, 24, 32, 128, 0);
	u64 textColor = GS_SETREG_RGBAQ(255, 255, 255, 0, 0);

	/* Background box */
	gsKit_prim_sprite(gsGlobal,
		startX, startY,
		startX + boxW, startY + boxH,
		2, boxColor);

	/* Text glyphs */
	for (i = 0; i < len; ++i) {
		unsigned char c = (unsigned char)OsdMessage[i];
		int glyphIdx = (c >= 32 && c <= 127) ? (c - 32) : 0;
		int gx = startX + padX + (i * charW);
		int gy = startY + padY;

		for (row = 0; row < 8; ++row) {
			u8 bits = Font8x8[glyphIdx][row];
			if (bits == 0) continue;
			for (col = 0; col < 8; ++col) {
				if (bits & (0x80 >> col)) {
					gsKit_prim_sprite(gsGlobal,
						gx + col, gy + row,
						gx + col + 1, gy + row + 1,
						3, textColor);
				}
			}
		}
	}
}

void PS2Video_DrawFrame(const u8 *macFrameBuffer)
{
	const u8 *src = macFrameBuffer;
	int y;
	int x;

	if ((gsGlobal == NULL) || (TextureMem == NULL)
		|| (macFrameBuffer == NULL)) {
		return;
	}

	if (TextureIs16) {
		u16 *dst = (u16 *)TextureStorage;

		for (y = 0; y < MAC_HEIGHT; ++y) {
			for (x = 0; x < MAC_ROW_BYTES; ++x) {
				memcpy(dst, PixelLUT16[src[x]], 8 * sizeof(u16));
				dst += 8;
			}
			src += MAC_ROW_BYTES;
		}
	} else {
		u32 *dst = (u32 *)TextureStorage;

		for (y = 0; y < MAC_HEIGHT; ++y) {
			for (x = 0; x < MAC_ROW_BYTES; ++x) {
				memcpy(dst, PixelLUT[src[x]], 8 * sizeof(u32));
				dst += 8;
			}
			src += MAC_ROW_BYTES;
		}
	}

	gsKit_texture_upload(gsGlobal, &Texture);

	gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
	gsKit_prim_sprite_texture(gsGlobal, &Texture,
		DstX, DstY, 0.0f, 0.0f,
		DstX + DstW, DstY + DstH,
		(float)MAC_WIDTH, (float)MAC_HEIGHT,
		1, 0xFFFFFFFF);

	if (OsdFramesRemaining > 0) {
		DrawOSDBanner();
		--OsdFramesRemaining;
	}
	DrawStatusLine();

#if 1
	/* Temporary diagnostic: red frame proves a Mac frame was presented. */
	{
		u64 red = GS_SETREG_RGBAQ(255, 0, 0, 0x80, 0);
		int w = gsGlobal->Width;
		int h = gsGlobal->Height;

		/* Keep the diagnostic border well inside TV overscan. */
		gsKit_prim_sprite(gsGlobal, 24, 24, w - 24, 30, 2, red);
		gsKit_prim_sprite(gsGlobal, 24, h - 30, w - 24, h - 24, 2, red);
		gsKit_prim_sprite(gsGlobal, 24, 30, 30, h - 30, 2, red);
		gsKit_prim_sprite(gsGlobal, w - 30, 30, w - 24, h - 30, 2, red);
	}
#endif

	gsKit_queue_exec(gsGlobal);
	gsKit_sync_flip(gsGlobal);
}

void PS2Video_Quit(void)
{
	Texture.Mem = NULL;
	TextureMem = NULL;
	if (gsGlobal != NULL) {
		gsKit_deinit_global(gsGlobal);
		gsGlobal = NULL;
	}
}
