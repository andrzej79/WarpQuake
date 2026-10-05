#ifndef WQSAMPLE_H
#define WQSAMPLE_H

/**
 * A sampling profiler that runs on the Amiga itself (WarpQuake: profsample, or -sample during a timedemo).
 *
 * vamos cannot say where a real 68060 spends its time: it emulates the OS in
 * Python and has no 060 timing.  This takes samples on the machine.  A task
 * above the main one wakes about every millisecond and copies the top of the
 * main task's saved context.  Exec keeps that at tc_SPReg while a task is
 * switched out, and it holds the interrupted PC.
 *
 * Where in that frame the PC sits (and whether an FPU frame comes first)
 * depends on the Exec version, so nothing here assumes it.  Each sample keeps
 * WPROF_WINDOW longwords raw.  A calibration phase first spins in a known
 * function, and the host script (tools/wprof.py) learns the PC's slot from
 * those samples, then maps the rest onto the vlink map of the same binary.
 */

#include <exec/types.h>

#define WPROF_WINDOW 64         // longwords copied from tc_SPReg per sample

BOOL wprofStart(ULONG maxSamples);
// Spins in wprofCalibSpin() for about ms milliseconds, samples tagged CALIB.
void wprofCalibrate(ULONG ms);
void wprofStop(void);
// Writes the samples, then frees them.  FALSE if the file cannot be written.
BOOL wprofSave(const char *path);
ULONG wprofCount(void);
// Drops unsaved samples (warpQuake: the exit path, after wprofStop()).
void wprofFree(void);

#endif
