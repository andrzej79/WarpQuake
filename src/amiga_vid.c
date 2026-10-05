// WarpQuake - video: an 8-bit CLUT RTG screen at the resolution the engine
// renders, chosen at start (ASL requester, saved mode, or arguments).

#ifndef __VBCC__
#define __reg(x)
#define __saveds
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/asl.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/asl.h>

/* Picasso96: the structures from the repo's P96 SDK, the vbcc call stubs from
   the compiler's own tree (see warpPDFViewer/src/viewer/pageview.c). */
#include <libraries/Picasso96.h>
#pragma stdargs-on
#include <clib/Picasso96_protos.h>
#pragma stdargs-off
#include <inline/Picasso96_protos.h>

#include "quakegeneric.h"
#include "amiga_qg.h"

// The engine's limits (MAXWIDTH/MAXHEIGHT in r_shared.h) and its smallest view.
#define MIN_WIDTH 320
#define MIN_HEIGHT 200
#define MAX_WIDTH 1280
#define MAX_HEIGHT 1024

#define MODE_FILE "PROGDIR:WarpQuake.mode"

struct GfxBase *GfxBase = NULL;
struct Library *P96Base = NULL;
struct Library *AslBase = NULL;
struct Window *qgWindow = NULL;

static struct Screen *qgScreen = NULL;
static UWORD *blankPointer = NULL;   // chip RAM, for SetPointer
static ULONG paletteTable[1 + 256 * 3 + 1];
static BOOL headless;                // -headless: no screen at all (vamos runs)
static ULONG modeId = INVALID_ID;
static int vidWidth = QUAKEGENERIC_RES_X, vidHeight = QUAKEGENERIC_RES_Y;
static int blitMode;
static char videoInfo[96];

static const char *argValue(const char *name)
{
  extern int com_argc;
  extern char **com_argv;
  int i;

  for(i = 1; i < com_argc; i++) {
    if(strcmp(com_argv[i], name) == 0) {
      return (i < com_argc - 1) ? com_argv[i + 1] : "";
    }
  }
  return NULL;
}

/*
===============================================================================
MODE CHOICE
===============================================================================
*/

// An RTG CLUT mode the engine can render at.
static BOOL modeUsable(ULONG id)
{
  ULONG w, h;

  if(id == INVALID_ID || !p96GetModeIDAttr(id, P96IDA_ISP96)) {
    return FALSE;
  }
  if(p96GetModeIDAttr(id, P96IDA_RGBFORMAT) != RGBFB_CLUT) {
    return FALSE;
  }
  w = p96GetModeIDAttr(id, P96IDA_WIDTH);
  h = p96GetModeIDAttr(id, P96IDA_HEIGHT);
  return (w >= MIN_WIDTH && w <= MAX_WIDTH && h >= MIN_HEIGHT && h <= MAX_HEIGHT) ? TRUE : FALSE;
}

static ULONG __saveds modeFilter(__reg("a0") struct Hook *hook, __reg("a2") APTR requester, __reg("a1") ULONG id)
{
  (void)hook;
  (void)requester;
  return modeUsable(id) ? TRUE : FALSE;
}

static ULONG bestMode(int w, int h)
{
  return p96BestModeIDTags(P96BIDTAG_NominalWidth, w, P96BIDTAG_NominalHeight, h, P96BIDTAG_Depth, 8,
                           P96BIDTAG_FormatsAllowed, RGBFF_CLUT, TAG_DONE);
}

static ULONG loadSavedMode(void)
{
  FILE *f = fopen(MODE_FILE, "r");
  ULONG id = INVALID_ID;
  char line[32];

  if(f != NULL) {
    if(fgets(line, sizeof(line), f) != NULL) {
      id = strtoul(line, NULL, 0);
    }
    fclose(f);
  }
  return id;
}

static void saveMode(ULONG id)
{
  FILE *f = fopen(MODE_FILE, "w");

  if(f != NULL) {
    fprintf(f, "0x%08lx\n", id);
    fclose(f);
  }
}

// The ASL screen-mode requester, showing only modes the engine can use.
// INVALID_ID if cancelled or ASL is missing.
static ULONG askMode(ULONG initial)
{
  struct Hook filterHook;
  struct ScreenModeRequester *req;
  ULONG id = INVALID_ID;

  AslBase = OpenLibrary("asl.library", 38);
  if(AslBase == NULL) {
    return INVALID_ID;
  }
  memset(&filterHook, 0, sizeof(filterHook));
  filterHook.h_Entry = (HOOKFUNC)modeFilter;

  req = (struct ScreenModeRequester *)AllocAslRequestTags(
    ASL_ScreenModeRequest, ASLSM_TitleText, (ULONG) "WarpQuake: screen mode (8-bit RTG)", ASLSM_InitialDisplayID,
    initial != INVALID_ID ? initial : 0, ASLSM_MinWidth, MIN_WIDTH, ASLSM_MaxWidth, MAX_WIDTH, ASLSM_MinHeight,
    MIN_HEIGHT, ASLSM_MaxHeight, MAX_HEIGHT, ASLSM_MinDepth, 8, ASLSM_MaxDepth, 8, ASLSM_FilterFunc,
    (ULONG)&filterHook, TAG_DONE);
  if(req != NULL) {
    if(AslRequest(req, NULL)) {
      id = req->sm_DisplayID;
    }
    FreeAslRequest(req);
  }
  CloseLibrary(AslBase);
  AslBase = NULL;
  return id;
}

// In order: -modeid, -width/-height, -asl (ask again), the saved mode, and
// on a first start the requester.  A chosen mode is saved for next time.
static ULONG chooseMode(void)
{
  const char *arg, *argH;
  ULONG id, saved;

  if((arg = argValue("-modeid")) != NULL) {
    return strtoul(arg, NULL, 0);
  }
  arg = argValue("-width");
  argH = argValue("-height");
  if(arg != NULL || argH != NULL) {
    int w = arg ? atoi(arg) : QUAKEGENERIC_RES_X;
    int h = argH ? atoi(argH) : (w * 3) / 4;
    return bestMode(w, h);
  }

  saved = loadSavedMode();
  if(argValue("-asl") == NULL && modeUsable(saved)) {
    return saved;
  }
  id = askMode(saved);
  if(modeUsable(id)) {
    saveMode(id);
    return id;
  }
  return bestMode(QUAKEGENERIC_RES_X, QUAKEGENERIC_RES_Y);
}

/*
===============================================================================
SCREEN
===============================================================================
*/

static void updateInfo(void)
{
  if(headless) {
    sprintf(videoInfo, "headless");
  } else {
    static const char *const names[] = {"WriteChunkyPixels", "lock+copy", "lock+MOVE16"};
    sprintf(videoInfo, "mode 0x%08lx, blit %s", modeId, names[blitMode]);
  }
}

void QG_Init(void)
{
  const char *arg;

  // The engine without a display: for vamos, which has no graphics.library,
  // and for checking renderer changes by frame checksums rather than by eye.
  headless = (argValue("-headless") != NULL);
  if(headless) {
    if((arg = argValue("-width")) != NULL) {
      vidWidth = atoi(arg);
    }
    if((arg = argValue("-height")) != NULL) {
      vidHeight = atoi(arg);
    }
    updateInfo();
    qgPrintf("Video: headless %dx%d\n", vidWidth, vidHeight);
    return;
  }

  GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 40);
  if(GfxBase == NULL) {
    Sys_Error("graphics.library V40 (AmigaOS 3.1) is required");
  }
  P96Base = OpenLibrary("Picasso96API.library", 2);
  if(P96Base == NULL) {
    Sys_Error("Picasso96API.library V2 is required (RTG only)");
  }

  modeId = chooseMode();
  if(!modeUsable(modeId)) {
    Sys_Error("No usable 8-bit RTG screen mode (0x%08lx); 320x200 to 1280x1024, try -asl", modeId);
  }
  vidWidth = (int)p96GetModeIDAttr(modeId, P96IDA_WIDTH);
  vidHeight = (int)p96GetModeIDAttr(modeId, P96IDA_HEIGHT);

  qgScreen = OpenScreenTags(NULL, SA_DisplayID, modeId, SA_Width, vidWidth, SA_Height, vidHeight, SA_Depth, 8,
                            SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Type, CUSTOMSCREEN, SA_Exclusive, TRUE,
                            SA_Draggable, FALSE, SA_Title, (ULONG) "WarpQuake", TAG_DONE);
  if(qgScreen == NULL) {
    Sys_Error("Cannot open a %dx%d 8-bit screen (mode 0x%08lx)", vidWidth, vidHeight, modeId);
  }

  qgWindow = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)qgScreen, WA_Left, 0, WA_Top, 0, WA_Width, vidWidth,
                            WA_Height, vidHeight, WA_Borderless, TRUE, WA_Backdrop, TRUE, WA_Activate, TRUE,
                            WA_RMBTrap, TRUE, WA_ReportMouse, TRUE, WA_NoCareRefresh, TRUE, WA_SimpleRefresh, TRUE,
                            WA_IDCMP,
                            IDCMP_RAWKEY | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS | IDCMP_DELTAMOVE |
                              IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW,
                            TAG_DONE);
  if(qgWindow == NULL) {
    Sys_Error("Cannot open the game window");
  }

  // A 1x1 transparent sprite: the pointer is hidden, the mouse still reports.
  blankPointer = AllocVec(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
  if(blankPointer != NULL) {
    SetPointer(qgWindow, blankPointer, 1, 1, 0, 0);
  }

  SetRast(qgWindow->RPort, 0);
  qgInputReset();
  updateInfo();
  qgPrintf("Video: mode 0x%08lx %dx%d\n", modeId, vidWidth, vidHeight);
}

// Also the atexit() cleanup: safe to call twice, and with nothing open.
void QG_Quit(void)
{
  if(qgWindow != NULL) {
    ClearPointer(qgWindow);
    CloseWindow(qgWindow);
    qgWindow = NULL;
  }
  if(blankPointer != NULL) {
    FreeVec(blankPointer);
    blankPointer = NULL;
  }
  if(qgScreen != NULL) {
    CloseScreen(qgScreen);
    qgScreen = NULL;
  }
  if(P96Base != NULL) {
    CloseLibrary(P96Base);
    P96Base = NULL;
  }
  if(GfxBase != NULL) {
    CloseLibrary((struct Library *)GfxBase);
    GfxBase = NULL;
  }
}

void QG_GetVideoSize(int *width, int *height)
{
  *width = vidWidth;
  *height = vidHeight;
}

const char *QG_VideoInfo(void)
{
  return videoInfo;
}

void QG_SetBlitMode(int mode)
{
  blitMode = (mode >= 0 && mode <= 2) ? mode : 0;
  updateInfo();
}

/*
===============================================================================
BLIT
===============================================================================
*/

// Straight into the screen's memory, row by row: CopyMemQuick, or with
// move16 MOVE16 bursts, which the uncached RTG memory is built for.  Falls
// back to WriteChunkyPixels if the bitmap cannot be locked or is not CLUT, and
// from MOVE16 to CopyMemQuick if anything is not 16-byte aligned.
static BOOL blitLocked(const UBYTE *src, int y0, int rows, BOOL move16)
{
  struct BitMap *bm = qgWindow->RPort->BitMap;
  struct RenderInfo ri;
  LONG lock;
  UBYTE *dst;
  int y;

  lock = p96LockBitMap(bm, (UBYTE *)&ri, sizeof(ri));
  if(lock == 0) {
    return FALSE;
  }
  if(ri.RGBFormat != RGBFB_CLUT || ri.Memory == NULL) {
    p96UnlockBitMap(bm, lock);
    return FALSE;
  }
  dst = (UBYTE *)ri.Memory + y0 * ri.BytesPerRow;
  src += y0 * vidWidth;
  if(move16 && (((ULONG)src | (ULONG)dst | (ULONG)vidWidth | (ULONG)ri.BytesPerRow) & 15) == 0) {
    qgMove16Rows(src, dst, vidWidth, rows, ri.BytesPerRow);
  } else if(((ULONG)src & 3) == 0 && ((ULONG)dst & 3) == 0 && (vidWidth & 3) == 0 && (ri.BytesPerRow & 3) == 0) {
    for(y = 0; y < rows; y++) {
      CopyMemQuick((APTR)src, dst, vidWidth);
      src += vidWidth;
      dst += ri.BytesPerRow;
    }
  } else {
    for(y = 0; y < rows; y++) {
      CopyMem((APTR)src, dst, vidWidth);
      src += vidWidth;
      dst += ri.BytesPerRow;
    }
  }
  p96UnlockBitMap(bm, lock);
  return TRUE;
}

// Rows y .. y+rows-1 only: the engine says which part of the frame changed
// (a status bar that did not change is not redrawn, so not copied either).
void QG_DrawFrameRows(void *pixels, int y, int rows)
{
  if(qgWindow == NULL) {
    return;
  }
  if(y < 0) {
    rows += y;
    y = 0;
  }
  if(y + rows > vidHeight) {
    rows = vidHeight - y;
  }
  if(rows <= 0) {
    return;
  }
  if(blitMode != 0 && blitLocked((const UBYTE *)pixels, y, rows, blitMode == 2)) {
    return;
  }
  WriteChunkyPixels(qgWindow->RPort, 0, y, vidWidth - 1, y + rows - 1, (UBYTE *)pixels + y * vidWidth, vidWidth);
}

void QG_DrawFrame(void *pixels)
{
  QG_DrawFrameRows(pixels, 0, vidHeight);
}

void QG_SetPalette(unsigned char palette[768])
{
  int i;

  if(qgScreen == NULL) {
    return;
  }
  paletteTable[0] = (256UL << 16) | 0;
  for(i = 0; i < 256 * 3; i++) {
    paletteTable[1 + i] = (ULONG)palette[i] * 0x01010101UL;
  }
  paletteTable[1 + 256 * 3] = 0;
  LoadRGB32(&qgScreen->ViewPort, paletteTable);
}
