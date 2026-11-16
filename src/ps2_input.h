#ifndef PS2_INPUT_H
#define PS2_INPUT_H

#include <tamtypes.h>

typedef struct PS2KeyEvent {
	u8 hid;
	u8 down;
} PS2KeyEvent;

typedef struct PS2PadState {
	int connected;
	unsigned short buttons;	/* active high: a set bit means pressed */
	unsigned short pressed;	/* edge since the last call to GetPad */
	u8 lx;
	u8 ly;
	u8 rx;
	u8 ry;
} PS2PadState;

void PS2Input_Init(void);
void PS2Input_Poll(void);

/* Accessors.  GetKey returns 1 when an event was dequeued. */
int PS2Input_GetKey(PS2KeyEvent *event);
void PS2Input_GetPad(PS2PadState *state);

/* Diagnostic state for the on-screen input status. */
int PS2Input_KeyboardReady(void);
int PS2Input_MouseReady(void);
int PS2Input_MousePresent(void);
int PS2Input_KeyEventCount(void);
u8 PS2Input_LastKeyHid(void);
int PS2Input_MouseEventCount(void);
int PS2Input_MouseEnumCount(void);
u32 PS2Input_MouseEnumValue(void);
int PS2Input_MouseReadOk(void);
void PS2Input_LastMouseDelta(int *dx, int *dy);
int PS2Input_PadReady(void);

/* USB mouse.  present is set to 0 when no mouse is connected. */
void PS2Input_GetMouse(int *present, int *x, int *y, int *buttons);
void PS2Input_SetMousePos(int x, int y);

void PS2Input_Quit(void);

#endif /* PS2_INPUT_H */
