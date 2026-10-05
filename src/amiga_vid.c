// WarpQuake - video: an 8-bit CLUT or 16-bit RGB RTG screen at the
// resolution the engine renders, chosen at start (ASL requester, saved mode,
// or arguments).  A 16-bit screen gets RGB pixels from the engine (d_rgb.c),
// in its own format, and the damage/bonus flashes as a blend applied here,
// as the frame is copied to the screen.

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
static int pixFormat = QG_PIX_CLUT8;   // the screen's, as a QG_PIX_* value
static RGBFTYPE rgbFormat = RGBFB_CLUT; // ... and as P96 names it
static int pixBytes = 1;
static int wantDepth;                  // -bpp 8 / 16: only modes of that depth
static UWORD *blendBuffer;             // a blended frame, for the unlocked path

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

// The QG_PIX_* value of a P96 pixel format; -1 for one the engine cannot
// draw (24/32-bit and planar).
static int qgFormat(ULONG rgb)
{
  switch(rgb) {
  case RGBFB_CLUT:
    return QG_PIX_CLUT8;
  case RGBFB_R5G6B5:
    return QG_PIX_RGB565;
  case RGBFB_R5G5B5:
    return QG_PIX_RGB555;
  case RGBFB_R5G6B5PC:
    return QG_PIX_RGB565PC;
  case RGBFB_R5G5B5PC:
    return QG_PIX_RGB555PC;
  case RGBFB_B5G6R5PC:
    return QG_PIX_BGR565PC;
  case RGBFB_B5G5R5PC:
    return QG_PIX_BGR555PC;
  default:
    return -1;
  }
}

#define RGBFF_16BIT (RGBFF_R5G6B5 | RGBFF_R5G5B5 | RGBFF_R5G6B5PC | RGBFF_R5G5B5PC | RGBFF_B5G6R5PC | RGBFF_B5G5R5PC)

// An RTG mode the engine can render at: CLUT or 16-bit RGB (only the one
// -bpp asks for, if given).
static BOOL modeUsable(ULONG id)
{
  ULONG w, h;
  int f;

  if(id == INVALID_ID || !p96GetModeIDAttr(id, P96IDA_ISP96)) {
    return FALSE;
  }
  f = qgFormat(p96GetModeIDAttr(id, P96IDA_RGBFORMAT));
  if(f < 0 || (wantDepth == 8 && f != QG_PIX_CLUT8) || (wantDepth == 16 && f == QG_PIX_CLUT8)) {
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
  if(wantDepth == 16) {
    return p96BestModeIDTags(P96BIDTAG_NominalWidth, w, P96BIDTAG_NominalHeight, h, P96BIDTAG_Depth, 16,
                             P96BIDTAG_FormatsAllowed, RGBFF_16BIT, TAG_DONE);
  }
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
    ASL_ScreenModeRequest, ASLSM_TitleText, (ULONG) "WarpQuake: screen mode (8 or 16-bit RTG)", ASLSM_InitialDisplayID,
    initial != INVALID_ID ? initial : 0, ASLSM_MinWidth, MIN_WIDTH, ASLSM_MaxWidth, MAX_WIDTH, ASLSM_MinHeight,
    MIN_HEIGHT, ASLSM_MaxHeight, MAX_HEIGHT, ASLSM_MinDepth, wantDepth == 16 ? 15 : 8, ASLSM_MaxDepth,
    wantDepth == 8 ? 8 : 16, ASLSM_FilterFunc,
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
// -bpp 8 or 16 limits all of them to that depth; a saved mode of the other
// depth then stands for its size (-bpp 16 alone: the saved 8-bit mode's
// resolution, in 16 bits).
static ULONG chooseMode(void)
{
  const char *arg, *argH;
  ULONG id, saved;

  if((arg = argValue("-bpp")) != NULL) {
    wantDepth = (atoi(arg) >= 15) ? 16 : 8;
  }

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
  if(argValue("-asl") == NULL && wantDepth != 0 && saved != INVALID_ID && p96GetModeIDAttr(saved, P96IDA_ISP96)) {
    id = bestMode((int)p96GetModeIDAttr(saved, P96IDA_WIDTH), (int)p96GetModeIDAttr(saved, P96IDA_HEIGHT));
    if(modeUsable(id)) {
      return id;
    }
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
    sprintf(videoInfo, "mode 0x%08lx, %d bpp, blit %s", modeId, pixBytes * 8,
            (pixBytes == 2 && blitMode == 0) ? "WritePixelArray" : names[blitMode]);
  }
}

// Headless (vamos checks of the engine's mode change): four made-up modes,
// ids HEADLESS_MODE + index; switching only changes the size and format.
#define HEADLESS_MODE 0x100
static const qgmode_t headlessModes[] = {
  {HEADLESS_MODE + 0, 320, 200, 8},
  {HEADLESS_MODE + 1, 640, 400, 8},
  {HEADLESS_MODE + 2, 320, 200, 16},
  {HEADLESS_MODE + 3, 640, 400, 16},
};
#define NUM_HEADLESS_MODES ((int)(sizeof(headlessModes) / sizeof(headlessModes[0])))

// The screen and its game window in mode id; the mode's size and format
// become the engine's.  FALSE, with nothing left open, if either fails.
static BOOL openDisplay(ULONG id)
{
  vidWidth = (int)p96GetModeIDAttr(id, P96IDA_WIDTH);
  vidHeight = (int)p96GetModeIDAttr(id, P96IDA_HEIGHT);
  rgbFormat = (RGBFTYPE)p96GetModeIDAttr(id, P96IDA_RGBFORMAT);
  pixFormat = qgFormat(rgbFormat);
  pixBytes = (pixFormat == QG_PIX_CLUT8) ? 1 : 2;

  qgScreen = OpenScreenTags(NULL, SA_DisplayID, id, SA_Width, vidWidth, SA_Height, vidHeight, SA_Depth, pixBytes * 8,
                            SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Type, CUSTOMSCREEN, SA_Exclusive, TRUE,
                            SA_Draggable, FALSE, SA_Title, (ULONG) "WarpQuake", TAG_DONE);
  if(qgScreen == NULL) {
    return FALSE;
  }
  qgWindow = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)qgScreen, WA_Left, 0, WA_Top, 0, WA_Width, vidWidth,
                            WA_Height, vidHeight, WA_Borderless, TRUE, WA_Backdrop, TRUE, WA_Activate, TRUE,
                            WA_RMBTrap, TRUE, WA_ReportMouse, TRUE, WA_NoCareRefresh, TRUE, WA_SimpleRefresh, TRUE,
                            WA_IDCMP,
                            IDCMP_RAWKEY | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS | IDCMP_DELTAMOVE |
                              IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW,
                            TAG_DONE);
  if(qgWindow == NULL) {
    CloseScreen(qgScreen);
    qgScreen = NULL;
    return FALSE;
  }
  if(blankPointer != NULL) {
    SetPointer(qgWindow, blankPointer, 1, 1, 0, 0);
  }
  SetRast(qgWindow->RPort, 0);
  modeId = id;
  updateInfo();
  return TRUE;
}

static void closeDisplay(void)
{
  if(qgWindow != NULL) {
    ClearPointer(qgWindow);
    CloseWindow(qgWindow);
    qgWindow = NULL;
  }
  if(qgScreen != NULL) {
    CloseScreen(qgScreen);
    qgScreen = NULL;
  }
  if(blendBuffer != NULL) {             // sized for the old mode
    FreeVec(blendBuffer);
    blendBuffer = NULL;
  }
}

void QG_Init(void)
{
  const char *arg;
  int i;

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
    if((arg = argValue("-bpp")) != NULL && atoi(arg) >= 15) {
      pixFormat = QG_PIX_RGB565;       // what the Warp's RTG offers
      pixBytes = 2;
    }
    // the made-up mode it matches, if any (QG_ListModes)
    for(i = 0; i < NUM_HEADLESS_MODES; i++) {
      if(headlessModes[i].width == vidWidth && headlessModes[i].height == vidHeight &&
         headlessModes[i].bpp == pixBytes * 8) {
        modeId = headlessModes[i].id;
      }
    }
    updateInfo();
    qgPrintf("Video: headless %dx%d, %d bpp\n", vidWidth, vidHeight, pixBytes * 8);
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
    Sys_Error("No usable %sRTG screen mode (0x%08lx); 320x200 to 1280x1024, try -asl",
              wantDepth == 16 ? "16-bit " : (wantDepth == 8 ? "8-bit " : ""), modeId);
  }

  // A 1x1 transparent sprite: the pointer is hidden, the mouse still reports.
  blankPointer = AllocVec(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);

  if(!openDisplay(modeId)) {
    Sys_Error("Cannot open a %ldx%ld screen in mode 0x%08lx", p96GetModeIDAttr(modeId, P96IDA_WIDTH),
              p96GetModeIDAttr(modeId, P96IDA_HEIGHT), modeId);
  }
  qgInputReset();
  qgInputHandlerStart();
  qgPrintf("Video: mode 0x%08lx %dx%d, %d bpp\n", modeId, vidWidth, vidHeight, pixBytes * 8);
}

// Also the atexit() cleanup: safe to call twice, and with nothing open.
void QG_Quit(void)
{
  qgInputHandlerStop();
  closeDisplay();
  if(blankPointer != NULL) {
    FreeVec(blankPointer);
    blankPointer = NULL;
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

/*
===============================================================================
RUN-TIME MODE CHANGE (the video menu)
===============================================================================
*/

int QG_ListModes(qgmode_t *modes, int max)
{
  ULONG id = INVALID_ID;
  int saved = wantDepth, n = 0, i, j;

  if(headless) {
    for(n = 0; n < NUM_HEADLESS_MODES && n < max; n++) {
      modes[n] = headlessModes[n];
    }
    return n;
  }
  wantDepth = 0;                        // both depths, whatever -bpp said
  while(n < max && (id = NextDisplayInfo(id)) != INVALID_ID) {
    if(modeUsable(id)) {
      modes[n].id = id;
      modes[n].width = (int)p96GetModeIDAttr(id, P96IDA_WIDTH);
      modes[n].height = (int)p96GetModeIDAttr(id, P96IDA_HEIGHT);
      modes[n].bpp = (qgFormat(p96GetModeIDAttr(id, P96IDA_RGBFORMAT)) == QG_PIX_CLUT8) ? 8 : 16;
      n++;
    }
  }
  wantDepth = saved;
  // by depth, width, height, then id (insertion sort: a few dozen modes)
  for(i = 1; i < n; i++) {
    qgmode_t m = modes[i];
    long key = ((long)m.bpp << 24) | ((long)m.width << 12) | m.height;

    for(j = i; j > 0; j--) {
      long k = ((long)modes[j - 1].bpp << 24) | ((long)modes[j - 1].width << 12) | modes[j - 1].height;

      if(k < key || (k == key && modes[j - 1].id < m.id)) {
        break;
      }
      modes[j] = modes[j - 1];
    }
    modes[j] = m;
  }
  // One entry per size and depth: the RTG board can list the same one twice
  // (on the Warp, 320x200 also comes as a 0xfff0xxxx mode).  The open mode
  // wins, else the lower id (the board's own, there).
  for(i = j = 0; i < n; i++) {
    if(j > 0 && modes[j - 1].bpp == modes[i].bpp && modes[j - 1].width == modes[i].width &&
       modes[j - 1].height == modes[i].height) {
      if(modes[i].id == modeId) {
        modes[j - 1] = modes[i];
      }
      continue;
    }
    modes[j++] = modes[i];
  }
  return j;
}

unsigned long QG_GetMode(void)
{
  return modeId;
}

int QG_SetMode(unsigned long id)
{
  ULONG old = modeId;
  int saved = wantDepth;
  BOOL usable;

  if(headless) {
    if(id < HEADLESS_MODE || id >= HEADLESS_MODE + NUM_HEADLESS_MODES) {
      return 0;
    }
    vidWidth = headlessModes[id - HEADLESS_MODE].width;
    vidHeight = headlessModes[id - HEADLESS_MODE].height;
    pixBytes = headlessModes[id - HEADLESS_MODE].bpp / 8;
    pixFormat = (pixBytes == 2) ? QG_PIX_RGB565 : QG_PIX_CLUT8;
    modeId = id;
    updateInfo();
    return 1;
  }
  if(qgScreen == NULL) {
    return 0;
  }
  wantDepth = 0;
  usable = modeUsable(id);
  wantDepth = saved;
  if(!usable) {
    return 0;
  }
  closeDisplay();
  if(openDisplay(id)) {
    saveMode(id);
    qgPrintf("Video: mode 0x%08lx %dx%d, %d bpp\n", modeId, vidWidth, vidHeight, pixBytes * 8);
    return 1;
  }
  if(!openDisplay(old)) {
    Sys_Error("Cannot open mode 0x%08lx, nor reopen 0x%08lx", id, old);
  }
  return 0;
}

int QG_GetPixelFormat(void)
{
  return pixFormat;
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

/*
 * The flash blend (16 bpp): each pixel moved toward the blend colour by
 * alpha, per channel, in the screen's own format.  Big-endian formats go
 * through qgBlend16 (blend16.s: one multiply a pixel, alpha in 1/32ths).  The
 * C below is for PC formats, byte-swapped around it: red and blue blended
 * together and green on its own, so that no field's product runs into the
 * next (alpha in 1/64ths, 1/32ths for 5-5-5, where red starts a bit lower).
 */
static ULONG blendRBMask, blendGMask;  // the fields of the format
static ULONG blendRB, blendG;          // the blend colour's fields * alpha
static ULONG blendInv;                 // (1 << blendShift) - alpha
static int blendShift;                 // 6, or 5 for 5-5-5 formats
static BOOL blendOn, blendSwap;
static ULONG blendParams[3];           // qgBlend16's: mask, spread(c) * a, 32 - a

void QG_SetBlend(int r, int g, int b, int alpha)
{
  ULONG c, a;
  BOOL is555 = (pixFormat == QG_PIX_RGB555 || pixFormat == QG_PIX_RGB555PC || pixFormat == QG_PIX_BGR555PC);
  int t;

  blendShift = is555 ? 5 : 6;
  a = ((ULONG)alpha << blendShift) >> 8;
  blendOn = (pixBytes == 2 && a != 0);
  if(!blendOn) {
    return;
  }
  if(pixFormat == QG_PIX_BGR565PC || pixFormat == QG_PIX_BGR555PC) {
    t = r;
    r = b;
    b = t;
  }
  if(is555) {
    c = ((ULONG)(r >> 3) << 10) | ((ULONG)(g >> 3) << 5) | (ULONG)(b >> 3);
    blendRBMask = 0x7c1f;
    blendGMask = 0x03e0;
  } else {
    c = ((ULONG)(r >> 3) << 11) | ((ULONG)(g >> 2) << 5) | (ULONG)(b >> 3);
    blendRBMask = 0xf81f;
    blendGMask = 0x07e0;
  }
  blendSwap = (pixFormat >= QG_PIX_RGB565PC);
  blendRB = (c & blendRBMask) * a;
  blendG = (c & blendGMask) * a;
  blendInv = (1UL << blendShift) - a;

  // qgBlend16: alpha in 1/32ths whatever the format
  a = ((ULONG)alpha << 5) >> 8;
  blendParams[0] = is555 ? 0x03e07c1fUL : 0x07e0f81fUL;
  blendParams[1] = ((c | (c << 16)) & blendParams[0]) * a;
  blendParams[2] = 32 - a;
}

// n pixels from src to dst, blended
static void blendPixels(const UWORD *src, UWORD *dst, int n)
{
  ULONG rbMask = blendRBMask, gMask = blendGMask, rb0 = blendRB, g0 = blendG, inv = blendInv;
  int shift = blendShift;

  if(!blendSwap) {
    qgBlend16(src, dst, (ULONG)n, blendParams);
    return;
  }
  while(n-- > 0) {
    ULONG p = *src++;
    p = ((p << 8) | (p >> 8)) & 0xffff;
    p = ((((p & rbMask) * inv + rb0) >> shift) & rbMask) | ((((p & gMask) * inv + g0) >> shift) & gMask);
    *dst++ = (UWORD)((p << 8) | (p >> 8));
  }
}

// Straight into the screen's memory, row by row: CopyMemQuick, or with
// move16 MOVE16 bursts, which the uncached RTG memory is built for (or
// blended, at 16 bpp while a flash lasts).  FALSE if the bitmap cannot be
// locked or is not in the mode's format: the caller falls back to the OS
// call.  From MOVE16 to CopyMemQuick if anything is not 16-byte aligned.
static BOOL blitLocked(const UBYTE *src, int y0, int rows, BOOL move16)
{
  struct BitMap *bm = qgWindow->RPort->BitMap;
  struct RenderInfo ri;
  LONG lock;
  UBYTE *dst;
  ULONG rowBytes = (ULONG)vidWidth * pixBytes;
  int y;

  lock = p96LockBitMap(bm, (UBYTE *)&ri, sizeof(ri));
  if(lock == 0) {
    return FALSE;
  }
  if(ri.RGBFormat != rgbFormat || ri.Memory == NULL) {
    p96UnlockBitMap(bm, lock);
    return FALSE;
  }
  dst = (UBYTE *)ri.Memory + y0 * ri.BytesPerRow;
  src += y0 * rowBytes;
  if(blendOn) {
    for(y = 0; y < rows; y++) {
      blendPixels((const UWORD *)src, (UWORD *)dst, vidWidth);
      src += rowBytes;
      dst += ri.BytesPerRow;
    }
  } else if(move16 && (((ULONG)src | (ULONG)dst | rowBytes | (ULONG)ri.BytesPerRow) & 15) == 0) {
    qgMove16Rows(src, dst, rowBytes, rows, ri.BytesPerRow);
  } else if(((ULONG)src & 3) == 0 && ((ULONG)dst & 3) == 0 && (rowBytes & 3) == 0 && (ri.BytesPerRow & 3) == 0) {
    for(y = 0; y < rows; y++) {
      CopyMemQuick((APTR)src, dst, rowBytes);
      src += rowBytes;
      dst += ri.BytesPerRow;
    }
  } else {
    for(y = 0; y < rows; y++) {
      CopyMem((APTR)src, dst, rowBytes);
      src += rowBytes;
      dst += ri.BytesPerRow;
    }
  }
  p96UnlockBitMap(bm, lock);
  return TRUE;
}

// 16 bpp through P96 (vid_blit 0, or no lock): the frame as a RenderInfo in
// the screen's format, blended first into blendBuffer while a flash lasts.
static void blitPixelArray(const UBYTE *src, int y0, int rows)
{
  struct RenderInfo ri;
  ULONG rowBytes = (ULONG)vidWidth * 2;

  if(blendOn) {
    if(blendBuffer == NULL) {
      blendBuffer = AllocVec(rowBytes * vidHeight, MEMF_ANY);
    }
    if(blendBuffer != NULL) {
      blendPixels((const UWORD *)(src + y0 * rowBytes), blendBuffer + y0 * vidWidth, vidWidth * rows);
      src = (const UBYTE *)blendBuffer;
    }
  }
  memset(&ri, 0, sizeof(ri));
  ri.Memory = (APTR)src;
  ri.BytesPerRow = (WORD)rowBytes;
  ri.RGBFormat = rgbFormat;
  p96WritePixelArray(&ri, 0, y0, qgWindow->RPort, 0, y0, vidWidth, rows);
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
  if(pixBytes == 2) {
    blitPixelArray((const UBYTE *)pixels, y, rows);
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

  if(qgScreen == NULL || pixBytes != 1) {
    return;
  }
  paletteTable[0] = (256UL << 16) | 0;
  for(i = 0; i < 256 * 3; i++) {
    paletteTable[1 + i] = (ULONG)palette[i] * 0x01010101UL;
  }
  paletteTable[1 + 256 * 3] = 0;
  LoadRGB32(&qgScreen->ViewPort, paletteTable);
}
