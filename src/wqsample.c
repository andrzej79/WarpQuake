/**
 * On-machine sampling profiler - see wqsample.h.
 * A copy of warpPDFViewer/src/common/wpdfprof.c; tools/wprof.py reads its files.
 */

#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>

#include "wqsample.h"

#define TAG_CALIB 1
#define TAG_RUN   2
#define REC_LONGS (2 + WPROF_WINDOW)   // tag|state, tc_SPReg, the window
#define PERIOD_US 997                  // off 1 ms, not to beat with a 1 kHz source
#define PROF_PRI  20

static struct Task *mainTask = NULL;
static struct Task *profTask = NULL;
static ULONG *samples = NULL;
static ULONG maxN = 0;
static volatile ULONG count = 0;
static volatile ULONG tag = TAG_RUN;
static volatile BOOL stopReq = FALSE;
static BYTE doneSig = -1;
static volatile ULONG spin = 0;

/* The sampler.  Everything it needs is in the globals above; it opens its
   own timer, since a message port's signal belongs to the task that made it. */
static void profEntry(void)
{
  struct MsgPort *port = CreateMsgPort();
  struct timerequest *tr = NULL;

  if(port != NULL) {
    tr = (struct timerequest *)CreateIORequest(port, sizeof(struct timerequest));
  }
  if(tr != NULL && OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *)tr, 0) == 0) {
    while(!stopReq) {
      tr->tr_node.io_Command = TR_ADDREQUEST;
      tr->tr_time.tv_secs = 0;
      tr->tr_time.tv_micro = PERIOD_US;
      DoIO((struct IORequest *)tr);
      /* The main task is switched out - ready or waiting - so its context
         sits at tc_SPReg.  Forbid() keeps it there while it is copied. */
      Forbid();
      if(count < maxN) {
        ULONG *rec = samples + count * REC_LONGS;
        rec[0] = tag | ((ULONG)mainTask->tc_State << 8);
        rec[1] = (ULONG)mainTask->tc_SPReg;
        CopyMem(mainTask->tc_SPReg, &rec[2], WPROF_WINDOW * 4);
        count++;
      }
      Permit();
    }
    CloseDevice((struct IORequest *)tr);
  }
  if(tr != NULL) {
    DeleteIORequest((struct IORequest *)tr);
  }
  if(port != NULL) {
    DeleteMsgPort(port);
  }
  /* Signal from inside Forbid(): the task is gone before the main task runs
     again and can unload the code. */
  Forbid();
  Signal(mainTask, 1UL << doneSig);
}

/* The calibration target: a loop that stays inside this function while the
   sampler takes ms samples.  wprofCalibSpinEnd() marks where it ends - vbcc
   emits functions in source order. */
void wprofCalibSpin(ULONG n)
{
  ULONG target = count + n;

  while(count < target && count < maxN) {
    spin++;
  }
}

void wprofCalibSpinEnd(void)
{
}

BOOL wprofStart(ULONG maxSamples)
{
  mainTask = FindTask(NULL);
  samples = (ULONG *)AllocVec(maxSamples * REC_LONGS * 4, MEMF_ANY);
  if(samples == NULL) {
    return FALSE;
  }
  doneSig = AllocSignal(-1);
  if(doneSig < 0) {
    FreeVec(samples);
    samples = NULL;
    return FALSE;
  }
  maxN = maxSamples;
  count = 0;
  stopReq = FALSE;
  tag = TAG_RUN;
  profTask = CreateTask((CONST_STRPTR) "WarpQuake profiler", PROF_PRI, (APTR)profEntry, 4096);
  if(profTask == NULL) {
    FreeSignal(doneSig);
    FreeVec(samples);
    samples = NULL;
    return FALSE;
  }
  return TRUE;
}

void wprofCalibrate(ULONG ms)
{
  if(profTask == NULL) {
    return;
  }
  tag = TAG_CALIB;
  wprofCalibSpin(ms);
  tag = TAG_RUN;
}

void wprofStop(void)
{
  if(profTask == NULL) {
    return;
  }
  stopReq = TRUE;
  Wait(1UL << doneSig);
  FreeSignal(doneSig);
  profTask = NULL;
}

ULONG wprofCount(void)
{
  return count;
}

BOOL wprofSave(const char *path)
{
  FILE *f;
  ULONG hdr[6];
  BOOL ok;

  if(samples == NULL) {
    return FALSE;
  }
  hdr[0] = 0x57505246UL;      // 'WPRF'
  hdr[1] = 1;
  hdr[2] = WPROF_WINDOW;
  hdr[3] = count;
  hdr[4] = (ULONG)wprofCalibSpin;
  hdr[5] = (ULONG)wprofCalibSpinEnd;
  f = fopen(path, "wb");
  ok = (f != NULL) ? TRUE : FALSE;
  if(ok) {
    ok = (fwrite(hdr, sizeof(hdr), 1, f) == 1) ? TRUE : FALSE;
    if(ok && count > 0) {
      ok = (fwrite(samples, count * REC_LONGS * 4, 1, f) == 1) ? TRUE : FALSE;
    }
    fclose(f);
  }
  FreeVec(samples);
  samples = NULL;
  return ok;
}

void wprofFree(void)
{
  if(samples != NULL) {
    FreeVec(samples);
    samples = NULL;
  }
}
