/*
	ps2_input.c

	USB keyboard, USB mouse and DualShock 2 input with hot-plug support.

	The keyboard is read in raw HID mode so the main program can map
	usage IDs to Macintosh key codes. The mouse is read in difference
	mode and its movement is accumulated into the shared 512x342 cursor.
*/

#include <tamtypes.h>
#include <kernel.h>
#include <timer.h>

#include <libpad.h>
#include <libkbd.h>
#include <libmouse.h>

#include <string.h>

#include "ps2_input.h"
#include "ps2_platform.h"

#define PAD_PORT 0
#define PAD_SLOT 0

#define MOUSE_WIDTH 512
#define MOUSE_HEIGHT 342

static char PadBuffer[256] __attribute__((aligned(64)));
static int PadInited = 0;
static int PadOpened = 0;
static int PadReady = 0;
static int PadModeSet = 0;
static unsigned short PadButtons = 0;
static unsigned short PadPrevButtons = 0;
static unsigned short PadEdge = 0;
static u8 PadLX = 128;
static u8 PadLY = 128;
static u8 PadRX = 128;
static u8 PadRY = 128;

static int KbdOpened = 0;
static u64 KbdRetryAt = 0;

static int MouseOpened = 0;
static u64 MouseRetryAt = 0;
static int MousePresent = 0;
static int MouseReadOk = 0;
static int MouseX = MOUSE_WIDTH / 2;
static int MouseY = MOUSE_HEIGHT / 2;
static int MouseButtons = 0;

static PS2KeyEvent KeyQueue[64];
static int KeyHead = 0;
static int KeyTail = 0;

static u64 LastKeyboardPollClock = 0;
static u64 LastDevicePollClock = 0;
static int InputCaps = 0;
static int KeyEventCount = 0;
static u8 LastKeyHid = 0;
static int MouseEventCount = 0;
static int MouseEnumCount = 0;
static u32 MouseEnumValue = 0;
static int MouseLastDx = 0;
static int MouseLastDy = 0;
static int MouseRemainderX = 0;
static int MouseRemainderY = 0;
static int MousePrevRawX = -1;
static int MousePrevRawY = -1;
static u32 MousePrevButtons = 0xFFFFFFFFu;
/* Read the keyboard quickly; poll mouse/pad at the PS2SDK frame cadence. */
#define KEYBOARD_POLL_INTERVAL_MS 8
#define DEVICE_POLL_INTERVAL_MS 16
#define MAX_KEY_EVENTS_PER_POLL 8

static void KeyPush(u8 hid, u8 down)
{
	int next = (KeyHead + 1) % (int)(sizeof(KeyQueue) / sizeof(KeyQueue[0]));

	if (next == KeyTail) {
		return;	/* queue full, drop the event */
	}
	KeyQueue[KeyHead].hid = hid;
	KeyQueue[KeyHead].down = down;
	KeyHead = next;
}

static void PollKeyboard(void)
{
	PS2KbdRawKey key;

	if (! KbdOpened) {
		u64 now = GetTimerSystemTime();
		int ret;

		if ((KbdRetryAt != 0) && (now < KbdRetryAt)) {
			return;
		}
		/* PS2KbdInit returns 0 when open("usbkbd:dev") fails;
		   only a positive return means the keyboard fd is usable. */
		ret = PS2KbdInit();
		if (ret > 0) {
			KbdOpened = 1;
			KbdRetryAt = 0;
			PS2KbdSetReadmode(PS2KBD_READMODE_RAW);
			PS2KbdSetBlockingMode(PS2KBD_NONBLOCKING);
			PS2KbdSetRepeatRate(0);
			PS2KbdSetLeds(0);
		} else {
			/* Avoid hammering IOP file I/O while waiting for hot-plug. */
			KbdRetryAt = now + MSec2TimerBusClock(1000);
			return;
		}
	}
	/* Drain queued press/release transitions each poll. A small bound keeps
	   a stuck/noisy device from monopolizing the emulation thread. */
	{
		int count;
		for (count = 0; count < MAX_KEY_EVENTS_PER_POLL; ++count) {
			int ret = PS2KbdReadRaw(&key);
			if (ret <= 0) {
				break;
			}
			LastKeyHid = key.key;
			if (key.state == PS2KBD_RAWKEY_DOWN) {
				if (KeyEventCount < 9999) ++KeyEventCount;
				KeyPush(key.key, 1);
			} else if (key.state == PS2KBD_RAWKEY_UP) {
				KeyPush(key.key, 0);
			}
		}
	}
}

static void PollMouse(void)
{
	PS2MouseData data;
	u32 mice;

	if (! MouseOpened) {
		u64 now = GetTimerSystemTime();
		int ret;

		if ((MouseRetryAt != 0) && (now < MouseRetryAt)) {
			MousePresent = 0;
			return;
		}
		ret = PS2MouseInit();
		if (ret > 0) {
			MouseOpened = 1;
			MouseRetryAt = 0;
			/* The PS2SDK sample uses DIFFERENCE mode by default and
			   preserves fractional movement across reads. */
			PS2MouseSetReadMode(PS2MOUSE_READMODE_DIFF);
			PS2MouseSetThres(65536);
			PS2MouseSetAccel(1.0f);
		} else {
			MousePresent = 0;
			MouseRetryAt = now + MSec2TimerBusClock(1000);
			return;
		}
	}

	/* Follow the PS2SDK sample: enumerate and read once per input frame. */
	mice = PS2MouseEnum();
	MouseEnumValue = mice;
	if (MouseEnumCount < 9999) ++MouseEnumCount;
	/* ENUM may briefly be zero while reports remain available; keep
	   reading so a transient enumeration gap does not freeze the pointer. */
	if (mice != 0) {
		MousePresent = 1;
	}
	memset(&data, 0, sizeof(data));
	MouseReadOk = (PS2MouseRead(&data) >= 0);
	if (! MouseReadOk) {
		return;
	}
	MouseLastDx = data.x;
	MouseLastDy = data.y;
	if ((data.x != 0) || (data.y != 0)
		|| (data.buttons != MousePrevButtons)) {
		MousePresent = 1;
		if (MouseEventCount < 9999) ++MouseEventCount;
	}
	MousePrevRawX = data.x;
	MousePrevRawY = data.y;
	MousePrevButtons = data.buttons;
	/* Keep sub-three-count movement instead of truncating each report. */
	MouseRemainderX += data.x;
	MouseRemainderY += data.y;
	{
		int scaledX = MouseRemainderX / 3;
		int scaledY = MouseRemainderY / 3;
		MouseRemainderX -= scaledX * 3;
		MouseRemainderY -= scaledY * 3;
		MouseX += scaledX;
		MouseY += scaledY;
	}

	if (MouseX < 0) {
		MouseX = 0;
	} else if (MouseX >= MOUSE_WIDTH) {
		MouseX = MOUSE_WIDTH - 1;
	}
	if (MouseY < 0) {
		MouseY = 0;
	} else if (MouseY >= MOUSE_HEIGHT) {
		MouseY = MOUSE_HEIGHT - 1;
	}

	MouseButtons = (data.buttons & PS2MOUSE_BTN1) ? 1 : 0;
}

static void PollPad(void)
{
	struct padButtonStatus buttons;
	int state;
	unsigned short now;
	int ret;

	if (! PadInited) {
		(void) padInit(0);
		PadInited = 1;
	}

	if (! PadOpened) {
		PadOpened = (padPortOpen(PAD_PORT, PAD_SLOT, PadBuffer) != 0);
		if (! PadOpened) {
			return;
		}
	}

	state = padGetState(PAD_PORT, PAD_SLOT);
	if ((state == PAD_STATE_STABLE) || (state == PAD_STATE_FINDCTP1)) {
		if (! PadModeSet) {
			padSetMainMode(PAD_PORT, PAD_SLOT, PAD_MMODE_DUALSHOCK,
				PAD_MMODE_LOCK);
			PadModeSet = 1;
		}

		memset(&buttons, 0, sizeof(buttons));
		ret = padRead(PAD_PORT, PAD_SLOT, &buttons);
		if (ret != 0) {
			PadReady = 1;
			now = (unsigned short)(0xFFFF ^ buttons.btns);
			PadButtons = now;
			PadEdge |= (unsigned short)(now & ~PadPrevButtons);
			PadPrevButtons = now;

			PadLX = buttons.ljoy_h;
			PadLY = buttons.ljoy_v;
			PadRX = buttons.rjoy_h;
			PadRY = buttons.rjoy_v;
		}
	} else if (state == PAD_STATE_DISCONN) {
		PadReady = 0;
		PadButtons = 0;
		PadPrevButtons = 0;
		PadModeSet = 0;
	}
}

void PS2Input_Init(void)
{
	/*
		Keep boot independent of the USB input modules.  libpad, the USB
		keyboard and the USB mouse are all brought up lazily by the first
		PS2Input_Poll() from the emulation loop.  This prevents a missing
		or slow USB HID device from blocking video/ROM boot.
	*/
	KeyHead = 0;
	KeyTail = 0;
	KeyEventCount = 0;
	LastKeyHid = 0;
	MouseEventCount = 0;
	MouseEnumCount = 0;
	MouseEnumValue = 0;
	MouseReadOk = 0;
	MouseLastDx = 0;
	MouseLastDy = 0;
	MouseRemainderX = 0;
	MouseRemainderY = 0;
	MousePrevRawX = -1;
	MousePrevRawY = -1;
	MousePrevButtons = 0xFFFFFFFFu;
	KbdOpened = 0;
	KbdRetryAt = 0;
	MouseOpened = 0;
	MouseRetryAt = 0;
	MousePresent = 0;
	MouseX = MOUSE_WIDTH / 2;
	MouseY = MOUSE_HEIGHT / 2;
	MouseButtons = 0;
	LastKeyboardPollClock = 0;
	LastDevicePollClock = 0;
	InputCaps = PS2_InputCaps();
}

void PS2Input_Poll(void)
{
	u64 now = GetTimerSystemTime();

	if (InputCaps & 2) {
		if ((LastKeyboardPollClock == 0) ||
			((now - LastKeyboardPollClock) >=
				MSec2TimerBusClock(KEYBOARD_POLL_INTERVAL_MS))) {
			LastKeyboardPollClock = now;
			PollKeyboard();
		}
	}

	if ((InputCaps & (1 | 4)) &&
		((LastDevicePollClock == 0) ||
			((now - LastDevicePollClock) >=
				MSec2TimerBusClock(DEVICE_POLL_INTERVAL_MS)))) {
		LastDevicePollClock = now;
		if (InputCaps & 4) {
			PollMouse();
		}
		if (InputCaps & 1) {
			PollPad();
		}
	}
}

int PS2Input_KeyboardReady(void)
{
	return KbdOpened;
}

int PS2Input_MouseReady(void)
{
	return MouseOpened;
}

int PS2Input_MousePresent(void)
{
	return MousePresent;
}

int PS2Input_KeyEventCount(void)
{
	return KeyEventCount;
}

u8 PS2Input_LastKeyHid(void)
{
	return LastKeyHid;
}

int PS2Input_MouseEventCount(void)
{
	return MouseEventCount;
}

int PS2Input_MouseEnumCount(void)
{
	return MouseEnumCount;
}

u32 PS2Input_MouseEnumValue(void)
{
	return MouseEnumValue;
}

int PS2Input_MouseReadOk(void)
{
	return MouseReadOk;
}

void PS2Input_LastMouseDelta(int *dx, int *dy)
{
	*dx = MouseLastDx;
	*dy = MouseLastDy;
}

int PS2Input_PadReady(void)
{
	return PadOpened && PadReady;
}

int PS2Input_GetKey(PS2KeyEvent *event)
{
	if (KeyTail == KeyHead) {
		return 0;
	}
	*event = KeyQueue[KeyTail];
	KeyTail = (KeyTail + 1) %
		(int)(sizeof(KeyQueue) / sizeof(KeyQueue[0]));
	return 1;
}

void PS2Input_GetPad(PS2PadState *state)
{
	state->connected = PadOpened && PadReady;
	state->buttons = PadButtons;
	state->pressed = PadEdge;
	state->lx = PadLX;
	state->ly = PadLY;
	state->rx = PadRX;
	state->ry = PadRY;
	PadEdge = 0;
}

void PS2Input_GetMouse(int *present, int *x, int *y, int *buttons)
{
	*present = MousePresent;
	*x = MouseX;
	*y = MouseY;
	*buttons = MouseButtons;
}

void PS2Input_SetMousePos(int x, int y)
{
	if (x < 0) x = 0;
	if (x >= MOUSE_WIDTH) x = MOUSE_WIDTH - 1;
	if (y < 0) y = 0;
	if (y >= MOUSE_HEIGHT) y = MOUSE_HEIGHT - 1;
	MouseX = x;
	MouseY = y;
}

void PS2Input_Quit(void)
{
	if (KbdOpened) {
		/* Closing the USB BDM keyboard fd may hang on the PS2 IOP.
		   Keep it open until reset, just like the storage handles. */
		KbdOpened = 0;
	}
	if (PadOpened) {
		padPortClose(PAD_PORT, PAD_SLOT);
		PadOpened = 0;
	}
	PadReady = 0;
	MouseOpened = 0;
	MousePresent = 0;
}
