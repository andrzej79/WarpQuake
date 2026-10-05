/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef __QUAKEGENERIC__
#define __QUAKEGENERIC__

#include "quakekeys.h"

#define QUAKEGENERIC_RES_X 320
#define QUAKEGENERIC_RES_Y 200

#define QUAKEGENERIC_JOY_MAX_AXES 6
#define QUAKEGENERIC_JOY_AXIS_X 0
#define QUAKEGENERIC_JOY_AXIS_Y 1
#define QUAKEGENERIC_JOY_AXIS_Z 2
#define QUAKEGENERIC_JOY_AXIS_R 3
#define QUAKEGENERIC_JOY_AXIS_U 4
#define QUAKEGENERIC_JOY_AXIS_V 5


// provided functions
void QG_Tick(double duration);
void QG_Create(int argc, char *argv[]);

// user must implement these
void QG_Init(void);
void QG_Quit(void);
void QG_DrawFrame(void *pixels);
void QG_SetPalette(unsigned char palette[768]);
int QG_GetKey(int *down, int *key);
void QG_GetMouseMove(int *x, int *y);
void QG_GetJoyAxes(float *axes);

// warpQuake: the rest of the OS the engine needs, so that no AmigaOS header
// is ever included next to quakedef.h (exec's inline macros and Quake's
// identifiers do not mix).
const char *QG_BaseDir(void);           // where id1/ lives
double QG_FloatTime(void);              // seconds, monotonic
void QG_SendKeyEvents(void);            // drain the window's messages
void QG_Mkdir(const char *path);
void QG_Error(const char *msg);         // report a fatal error, then exit(1)

// v1: the display mode is chosen at run time
void QG_GetVideoSize(int *width, int *height);  // valid after QG_Init()
void QG_SetBlitMode(int mode);          // vid_blit: 0 WriteChunkyPixels, 1 lock + copy
const char *QG_VideoInfo(void);         // mode and blit path, for reports

// v1: instrumentation (wq_prof.c)
unsigned long QG_Ticks(void);           // free-running counter, wraps
unsigned long QG_TickRate(void);        // its frequency in Hz
int QG_SampleStart(void);               // the sampling profiler; 0 if it cannot run
unsigned long QG_SampleStop(const char *path);  // saves; returns the sample count

#endif // __QUAKEGENERIC__
