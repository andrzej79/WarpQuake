// WarpQuake - input: the game window's IDCMP messages turned into Quake key
// events and mouse deltas.
//
// The mouse comes, by default, from an input handler (priority 51, just in
// front of Intuition) that adds up the raw relative mouse counts and passes
// every event on.  IDCMP_MOUSEMOVE deltas are what Intuition makes of those
// counts: accelerated by the Input preferences, and only while the window's
// mouse queue has room - Intuition stops sending them to a window that has
// five unreplied, and the game only drains its port once a frame, which a
// mouse reporting faster than ~125 Hz can fill.  The raw counts are neither
// accelerated nor ever dropped.  in_rawmouse 0 goes back to IDCMP.

#ifndef __VBCC__
#define __reg(x)
#define __saveds
#endif

#include <exec/types.h>
#include <exec/interrupts.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include "quakegeneric.h"
#include "amiga_qg.h"

// NewMouse wheel rawkeys (no NDK header defines them).
#define RAWKEY_NM_WHEEL_UP 0x7A
#define RAWKEY_NM_WHEEL_DOWN 0x7B

// Positional: the rawkey is the key's place on the keyboard, so a keymap does
// not move WASD.  Quake wants lower-case ASCII for printable keys.  The
// keypad works as a PC keypad with Num Lock off.
static const unsigned char rawkeyToQuake[0x80] = {
  /* 0x00 */ '`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\\', 0, K_INS,
  /* 0x10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, K_END, K_DOWNARROW, K_PGDN,
  /* 0x20 */ 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', 0, 0, K_LEFTARROW, 0, K_RIGHTARROW,
  /* 0x30 */ '<', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, K_DEL, K_HOME, K_UPARROW, K_PGUP,
  /* 0x40 */ K_SPACE, K_BACKSPACE, K_TAB, K_ENTER, K_ENTER, K_ESCAPE, K_DEL, K_INS, K_PGUP, K_PGDN, '-', K_F11,
  /*      */ K_UPARROW, K_DOWNARROW, K_RIGHTARROW, K_LEFTARROW,
  /* 0x50 */ K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, '(', ')', '/', '*', '+', 0,
  /* 0x60 */ K_SHIFT, K_SHIFT, 0, K_CTRL, K_ALT, K_ALT, 0, 0, 0, 0, 0, 0, 0, 0, K_PAUSE, K_F12,
  /* 0x70 */ K_HOME, K_END, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

#define KEYQUEUE_SIZE 64   // power of two
static struct {
  unsigned char key;
  unsigned char down;
} keyQueue[KEYQUEUE_SIZE];
static unsigned keyHead, keyTail;
static int mouseDx, mouseDy;
static volatile BOOL windowActive = TRUE;

// the input handler's state: the counts it has added up since the last
// QG_GetMouseMove (read and cleared under Forbid: the handler runs in
// input.device's task)
static volatile LONG rawDx, rawDy;
static struct MsgPort *inputPort;
static struct IOStdReq *inputReq;
static struct Interrupt inputHandler;
static BOOL inputHandlerOn;
static int mouseRaw = 1;

static struct InputEvent *mouseHandler(__reg("a0") struct InputEvent *events, __reg("a1") APTR data)
{
  struct InputEvent *ev;

  (void)data;
  if(windowActive) {
    for(ev = events; ev != NULL; ev = ev->ie_NextEvent) {
      if(ev->ie_Class == IECLASS_RAWMOUSE && (ev->ie_Qualifier & IEQUALIFIER_RELATIVEMOUSE)) {
        rawDx += ev->ie_X;
        rawDy += ev->ie_Y;
      }
    }
  }
  return events;                        // everything goes on to Intuition
}

void qgInputHandlerStart(void)
{
  if(inputHandlerOn) {
    return;
  }
  inputPort = CreateMsgPort();
  if(inputPort == NULL) {
    return;
  }
  inputReq = (struct IOStdReq *)CreateIORequest(inputPort, sizeof(struct IOStdReq));
  if(inputReq == NULL || OpenDevice("input.device", 0, (struct IORequest *)inputReq, 0) != 0) {
    if(inputReq != NULL) {
      DeleteIORequest((struct IORequest *)inputReq);
      inputReq = NULL;
    }
    DeleteMsgPort(inputPort);
    inputPort = NULL;
    return;
  }
  inputHandler.is_Node.ln_Type = NT_INTERRUPT;
  inputHandler.is_Node.ln_Pri = 51;
  inputHandler.is_Node.ln_Name = "WarpQuake mouse";
  inputHandler.is_Data = NULL;
  inputHandler.is_Code = (void (*)())mouseHandler;
  inputReq->io_Command = IND_ADDHANDLER;
  inputReq->io_Data = &inputHandler;
  DoIO((struct IORequest *)inputReq);
  rawDx = rawDy = 0;
  inputHandlerOn = TRUE;
}

void qgInputHandlerStop(void)
{
  if(inputHandlerOn) {
    inputReq->io_Command = IND_REMHANDLER;
    inputReq->io_Data = &inputHandler;
    DoIO((struct IORequest *)inputReq);
    inputHandlerOn = FALSE;
  }
  if(inputReq != NULL) {
    CloseDevice((struct IORequest *)inputReq);
    DeleteIORequest((struct IORequest *)inputReq);
    inputReq = NULL;
  }
  if(inputPort != NULL) {
    DeleteMsgPort(inputPort);
    inputPort = NULL;
  }
}

void QG_SetMouseMode(int raw)
{
  mouseRaw = raw;
}

static void queueKey(int key, int down)
{
  if(key == 0 || keyHead - keyTail >= KEYQUEUE_SIZE) {
    return;
  }
  keyQueue[keyHead % KEYQUEUE_SIZE].key = (unsigned char)key;
  keyQueue[keyHead % KEYQUEUE_SIZE].down = (unsigned char)down;
  keyHead++;
}

void qgInputReset(void)
{
  keyHead = keyTail = 0;
  mouseDx = mouseDy = 0;
  windowActive = TRUE;
}

void QG_SendKeyEvents(void)
{
  struct IntuiMessage *msg;

  if(SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
    Sys_Quit();
  }
  if(qgWindow == NULL) {
    return;
  }

  while((msg = (struct IntuiMessage *)GetMsg(qgWindow->UserPort)) != NULL) {
    ULONG cls = msg->Class;
    UWORD code = msg->Code;
    WORD mx = msg->MouseX;
    WORD my = msg->MouseY;

    ReplyMsg((struct Message *)msg);

    switch(cls) {
    case IDCMP_RAWKEY:
      if((code & 0x7F) == RAWKEY_NM_WHEEL_UP || (code & 0x7F) == RAWKEY_NM_WHEEL_DOWN) {
        // a wheel click is one press, Quake wants a press and a release
        if(!(code & IECODE_UP_PREFIX)) {
          int key = (code == RAWKEY_NM_WHEEL_UP) ? K_MWHEELUP : K_MWHEELDOWN;
          queueKey(key, 1);
          queueKey(key, 0);
        }
      } else {
        queueKey(rawkeyToQuake[code & 0x7F], !(code & IECODE_UP_PREFIX));
      }
      break;

    case IDCMP_MOUSEBUTTONS:
      switch(code) {
      case SELECTDOWN: queueKey(K_MOUSE1, 1); break;
      case SELECTUP: queueKey(K_MOUSE1, 0); break;
      case MENUDOWN: queueKey(K_MOUSE2, 1); break;
      case MENUUP: queueKey(K_MOUSE2, 0); break;
      case MIDDLEDOWN: queueKey(K_MOUSE3, 1); break;
      case MIDDLEUP: queueKey(K_MOUSE3, 0); break;
      }
      break;

    case IDCMP_MOUSEMOVE:
      // IDCMP_DELTAMOVE: MouseX/Y are the motion, not a position
      if(windowActive) {
        mouseDx += mx;
        mouseDy += my;
      }
      break;

    case IDCMP_ACTIVEWINDOW:
      windowActive = TRUE;
      break;

    case IDCMP_INACTIVEWINDOW:
      windowActive = FALSE;
      mouseDx = mouseDy = 0;
      break;
    }
  }
}

int QG_GetKey(int *down, int *key)
{
  if(keyTail == keyHead) {
    return 0;
  }
  *key = keyQueue[keyTail % KEYQUEUE_SIZE].key;
  *down = keyQueue[keyTail % KEYQUEUE_SIZE].down;
  keyTail++;
  return 1;
}

void QG_GetMouseMove(int *x, int *y)
{
  if(mouseRaw && inputHandlerOn) {
    Forbid();
    *x = rawDx;
    *y = rawDy;
    rawDx = rawDy = 0;
    Permit();
    mouseDx = mouseDy = 0;
    return;
  }
  *x = mouseDx;
  *y = mouseDy;
  mouseDx = mouseDy = 0;
}

void QG_GetJoyAxes(float *axes)
{
  int i;

  for(i = 0; i < QUAKEGENERIC_JOY_MAX_AXES; i++) {
    axes[i] = 0.0f;
  }
}
