// WarpQuake - entry point and the system side of the platform layer:
// timer, base directory, mkdir, fatal errors and cleanup.

#ifndef __VBCC__
#define __reg(x)
#define __saveds
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/timer.h>

#include "quakegeneric.h"
#include "amiga_qg.h"
#include "wqsample.h"

// The software renderer recurses (R_RecursiveWorldNode) and vbcc gives every
// nested block its own stack slot: the default 4 KB would not last a frame.
size_t __stack = 512 * 1024;
static const char stackCookie[] = "$STACK: 524288";
#define VERSION_STRING "WarpQuake 0.2 (4.10.2026)"
static const char versionTag[] = "$VER: " VERSION_STRING;

struct IntuitionBase *IntuitionBase = NULL;
struct Device *TimerBase = NULL;

static struct MsgPort *timerPort = NULL;
static struct timerequest *timerReq = NULL;
static double eclockFreq;
static double eclockBase;
static BOOL fromWorkbench;

/*
===============================================================================
TIMER - E-clock, opened once, read without I/O (ReadEClock is a plain call)
===============================================================================
*/

static BOOL timerOpen(void)
{
  struct EClockVal ev;

  timerPort = CreateMsgPort();
  if(timerPort == NULL) {
    return FALSE;
  }
  timerReq = (struct timerequest *)CreateIORequest(timerPort, sizeof(struct timerequest));
  if(timerReq == NULL || OpenDevice(TIMERNAME, UNIT_ECLOCK, (struct IORequest *)timerReq, 0) != 0) {
    return FALSE;
  }
  TimerBase = timerReq->tr_node.io_Device;
  eclockFreq = (double)ReadEClock(&ev);
  // Seconds since start: keeps the double's 52-bit mantissa for the fraction.
  eclockBase = (double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo;
  return TRUE;
}

static void timerClose(void)
{
  if(TimerBase != NULL) {
    CloseDevice((struct IORequest *)timerReq);
    TimerBase = NULL;
  }
  if(timerReq != NULL) {
    DeleteIORequest((struct IORequest *)timerReq);
    timerReq = NULL;
  }
  if(timerPort != NULL) {
    DeleteMsgPort(timerPort);
    timerPort = NULL;
  }
}

double QG_FloatTime(void)
{
  struct EClockVal ev;

  ReadEClock(&ev);
  return ((double)ev.ev_hi * 4294967296.0 + (double)ev.ev_lo - eclockBase) / eclockFreq;
}

// The raw low word: wq_prof.c only ever subtracts two close readings.
unsigned long QG_Ticks(void)
{
  struct EClockVal ev;

  ReadEClock(&ev);
  return ev.ev_lo;
}

unsigned long QG_TickRate(void)
{
  return (unsigned long)eclockFreq;
}

/*
===============================================================================
SYSTEM
===============================================================================
*/

const char *QG_BaseDir(void)
{
  BPTR lock;

  // common.c joins "PROGDIR:" + "id1" without a slash (see COM_InitFilesystem).
  // No PROGDIR: (vamos has none): the current directory, also slash-free.
  lock = Lock((STRPTR) "PROGDIR:", ACCESS_READ);
  if(lock == 0) {
    return "";
  }
  UnLock(lock);
  return "PROGDIR:";
}

void QG_Mkdir(const char *path)
{
  BPTR lock = CreateDir((STRPTR)path);

  if(lock != 0) {
    UnLock(lock);
  }
}

void qgPrintf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

void QG_Error(const char *msg)
{
  // The screen first: a requester or a Shell line behind a game screen is invisible.
  QG_Quit();
  printf("WarpQuake error: %s\n", msg);
  if(fromWorkbench && IntuitionBase != NULL) {
    struct EasyStruct es = {sizeof(struct EasyStruct), 0, (UBYTE *)"WarpQuake", (UBYTE *)"%s", (UBYTE *)"Quit"};
    EasyRequest(NULL, &es, NULL, (ULONG)msg);
  }
}

/*
===============================================================================
SAMPLING PROFILER - wqsample.c, driven by wq_prof.c
===============================================================================
*/

int QG_SampleStart(void)
{
  // ~1 kHz, 66 longwords a sample: 90 s is 23 MB; less if that is not free
  if(!wprofStart(90000) && !wprofStart(20000)) {
    return 0;
  }
  wprofCalibrate(200);
  return 1;
}

unsigned long QG_SampleStop(const char *path)
{
  ULONG n;

  wprofStop();
  n = wprofCount();
  return wprofSave(path) ? n : 0;
}

static void cleanup(void)
{
  // The sampler task first: it must not outlive the code it runs in.
  wprofStop();
  wprofFree();
  QG_Quit();
  timerClose();
  if(IntuitionBase != NULL) {
    CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = NULL;
  }
}

int main(int argc, char *argv[])
{
  static char *wbArgv[] = {"WarpQuake", NULL};
  double last, now;

  (void)stackCookie;
  (void)versionTag;

  // Workbench start: argc 0 and argv is the WBStartup message.
  if(argc == 0) {
    fromWorkbench = TRUE;
    argc = 1;
    argv = wbArgv;
  }

  atexit(cleanup);
  IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
  if(IntuitionBase == NULL) {
    printf("WarpQuake needs AmigaOS 3.0 or newer\n");
    return 20;
  }
  if(!timerOpen()) {
    printf("WarpQuake: cannot open timer.device\n");
    return 20;
  }

  printf(VERSION_STRING "\n");
  QG_Create(argc, argv);

  last = QG_FloatTime();
  for(;;) {
    now = QG_FloatTime();
    QG_Tick(now - last);
    last = now;
  }
  return 0;
}
