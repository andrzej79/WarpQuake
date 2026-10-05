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
// vid_null.c -- null video driver to aid porting efforts

#include "quakedef.h"
#include "d_local.h"

#include "quakegeneric.h"
#include "wq_prof.h"

viddef_t	vid;				// global video state

// warpQuake: sized at run time from the mode the platform layer opened
byte	*vid_buffer;
short	*zbuffer;
byte	*surfcache;
size_t	surfcache_size;

// 0: WriteChunkyPixels, 1: lock the screen and copy (see QG_SetBlitMode)
cvar_t	vid_blit = {"vid_blit", "0", true};
static int	blitmode = -1;

void	VID_SetPalette (unsigned char *palette)
{
	// quake generic
	QG_SetPalette(palette);
}

void	VID_ShiftPalette (unsigned char *palette)
{
	// quake generic
	QG_SetPalette(palette);
}

// Low-res modes are shown 4:3 (320x200 is not square-pixel, as in
// vid_dos.c); anything wider than 640 is taken as square pixels.  -aspect
// <h/w ratio of a pixel> overrides both.
static float PixelAspect (int width, int height)
{
	int	i = COM_CheckParm ("-aspect");

	if (i && i < com_argc-1)
		return Q_atof (com_argv[i+1]);
	if (width <= 640)
		return ((float)height / (float)width) * (320.0 / 240.0);
	return 1.0;
}

void	VID_Init (unsigned char *palette)
{
	int		width, height;

	Cvar_RegisterVariable (&vid_blit);

	// quake generic: the platform opens the display and says how big it is
	QG_Init();
	QG_GetVideoSize(&width, &height);
	if (width < 320 || height < 200 || width > MAXWIDTH || height > MAXHEIGHT)
		Sys_Error ("VID_Init: %dx%d is outside 320x200 .. %dx%d", width, height, MAXWIDTH, MAXHEIGHT);

	// zeroed: what malloc leaves there differs per build, and -crc sees it
	vid_buffer = calloc (width * height, 1);
	zbuffer = calloc (width * height, sizeof(short));
	if (!vid_buffer || !zbuffer)
		Sys_Error ("VID_Init: no memory for a %dx%d frame", width, height);

	vid.width = vid.conwidth = width;
	vid.height = vid.conheight = height;
	vid.maxwarpwidth = WARP_WIDTH;		// larger views warp at reduced size
	vid.maxwarpheight = WARP_HEIGHT;
	vid.aspect = PixelAspect (width, height);
	vid.numpages = 1;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
	vid.buffer = vid.conbuffer = vid_buffer;
	vid.rowbytes = vid.conrowbytes = width;
	
	d_pzbuffer = zbuffer;

	surfcache_size = D_SurfaceCacheForRes(width, height);
	surfcache = calloc(surfcache_size, 1);
	if (!surfcache)
		Sys_Error ("VID_Init: no memory for a %d KB surface cache", (int)(surfcache_size / 1024));
	D_InitCaches (surfcache, surfcache_size);

	QG_SetPalette(palette);
}

void	VID_Shutdown (void)
{
	free(surfcache);
	free(zbuffer);
	free(vid_buffer);
	surfcache = NULL;
	zbuffer = NULL;
	vid_buffer = NULL;
}

void	VID_Update (vrect_t *rects)
{
	if ((int)vid_blit.value != blitmode)
	{
		blitmode = (int)vid_blit.value;
		QG_SetBlitMode (blitmode);
	}

	{
		WQP_BEGIN (WQP_BLIT);
		// quake generic
		QG_DrawFrame(vid.buffer);
		WQP_END (WQP_BLIT);
	}
	WQC_ADD (WQC_BLIT_BYTES, vid.width * vid.height);
	if (wqp_crcon)
	{
		WQP_BEGIN (WQP_CRC);
		WQP_FrameCRC (vid.buffer, vid.width, vid.height, vid.rowbytes);
		WQP_END (WQP_CRC);
	}
}

/*
================
D_BeginDirectRect
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
}


/*
================
D_EndDirectRect
================
*/
void D_EndDirectRect (int x, int y, int width, int height)
{
}


