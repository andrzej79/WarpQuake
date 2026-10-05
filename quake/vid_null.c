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
byte	*vid_buffer;			// 16-byte aligned: the MOVE16 blit needs it
static byte	*vid_buffer_mem;	// what calloc returned
short	*zbuffer;
byte	*surfcache;
size_t	surfcache_size;

// 0: WriteChunkyPixels, 1: lock + CopyMemQuick, 2: lock + MOVE16 (QG_SetBlitMode)
cvar_t	vid_blit = {"vid_blit", "0", true};
static int	blitmode = -1;

// On a 16-bit screen there is no palette to set: the colour tables are
// built from it instead (d_rgb.c), and V_UpdatePalette does its shifts there.
void	VID_SetPalette (unsigned char *palette)
{
	if (r_pixbytes == 2)
		D_RGB_ShiftPalette (palette, NULL);
	else
		QG_SetPalette(palette);
}

void	VID_ShiftPalette (unsigned char *palette)
{
	if (r_pixbytes == 2)
		return;			// V_UpdatePalette calls D_RGB_ShiftPalette itself
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
	D_RGB_Init (QG_GetPixelFormat ());	// sets r_pixbytes

	// zeroed: what malloc leaves there differs per build, and -crc sees it
	vid_buffer_mem = calloc (width * height * r_pixbytes + 15, 1);
	vid_buffer = (byte *)(((intptr_t)vid_buffer_mem + 15) & ~(intptr_t)15);
	zbuffer = calloc (width * height, sizeof(short));
	if (!vid_buffer_mem || !zbuffer)
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
	vid.rowbytes = vid.conrowbytes = width;	// in pixels, also at 16 bpp
	
	d_pzbuffer = zbuffer;

	surfcache_size = D_SurfaceCacheForRes(width, height) * r_pixbytes;
	surfcache = calloc(surfcache_size, 1);
	if (!surfcache)
		Sys_Error ("VID_Init: no memory for a %d KB surface cache", (int)(surfcache_size / 1024));
	D_InitCaches (surfcache, surfcache_size);

	VID_SetPalette(palette);
}

void	VID_Shutdown (void)
{
	free(surfcache);
	free(zbuffer);
	free(vid_buffer_mem);
	surfcache = NULL;
	zbuffer = NULL;
	vid_buffer = vid_buffer_mem = NULL;
}

void	VID_Update (vrect_t *rects)
{
	if ((int)vid_blit.value != blitmode)
	{
		blitmode = (int)vid_blit.value;
		QG_SetBlitMode (blitmode);
	}

	{
		int		y0 = vid.height, y1 = 0;
		vrect_t	*r;

		// warpQuake: only the rows SCR_UpdateScreen says changed (it passes
		// the whole screen, the top above the status bar, or just the 3D
		// view, keeping track of what it redrew)
		for (r = rects ; r ; r = r->pnext)
		{
			if (r->y < y0)
				y0 = r->y;
			if (r->y + r->height > y1)
				y1 = r->y + r->height;
		}
		// 16 bpp: the flash blend is applied as the frame is shown, so while
		// it lasts, and once more when it changes, every row is shown again
		// (the status bar too, which is not redrawn each frame)
		if (r_pixbytes == 2 && (d_rgbblendchanged || D_RGB_Blending ()))
		{
			y0 = 0;
			y1 = vid.height;
			d_rgbblendchanged = false;
		}
		if (y1 > y0)
		{
			WQP_BEGIN (WQP_BLIT);
			QG_DrawFrameRows (vid.buffer, y0, y1 - y0);
			WQP_END (WQP_BLIT);
			WQC_ADD (WQC_BLIT_BYTES, vid.width * r_pixbytes * (y1 - y0));
		}
	}
	if (wqp_crcon)
	{
		WQP_BEGIN (WQP_CRC);
		if (d_rgbtest)
		{
			// the low bytes only: the 8-bit frame, if the 16-bit drawers
			// match the 8-bit ones (d_rgb.c)
			static byte	*lowbytes;
			int			i;

			if (!lowbytes)
				lowbytes = malloc (vid.width * vid.height);
			for (i = 0 ; i < vid.width * vid.height ; i++)
				lowbytes[i] = (byte)((unsigned short *)vid.buffer)[i];
			WQP_FrameCRC (lowbytes, vid.width, vid.height, vid.width);
		}
		else
			WQP_FrameCRC (vid.buffer, vid.width * r_pixbytes, vid.height, vid.rowbytes * r_pixbytes);
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


