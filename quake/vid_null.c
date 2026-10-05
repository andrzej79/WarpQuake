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

static void VID_MenuDraw (void);
static void VID_MenuKey (int key);
static void VID_ModeList_f (void);
static void VID_SetMode_f (void);
static int	vm_count = -1;				// the video menu's mode count; -1: not read yet

/*
================
VID_SetBuffers

The frame, z-buffer and surface cache for the mode the platform has open:
at start, and again after a mode change (VID_ChangeMode), which frees the
old ones first.
================
*/
static void VID_SetBuffers (void)
{
	int		width, height;

	QG_GetVideoSize(&width, &height);
	if (width < 320 || height < 200 || width > MAXWIDTH || height > MAXHEIGHT)
		Sys_Error ("VID_Init: %dx%d is outside 320x200 .. %dx%d", width, height, MAXWIDTH, MAXHEIGHT);
	D_RGB_Init (QG_GetPixelFormat ());	// sets r_pixbytes

	free (surfcache);
	free (zbuffer);
	free (vid_buffer_mem);

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
}

void	VID_Init (unsigned char *palette)
{
	Cvar_RegisterVariable (&vid_blit);

	// quake generic: the platform opens the display and says how big it is
	QG_Init();
	VID_SetBuffers ();
	VID_SetPalette(palette);

	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;
	Cmd_AddCommand ("vid_modelist", VID_ModeList_f);
	Cmd_AddCommand ("vid_setmode", VID_SetMode_f);
}

/*
================
VID_ChangeMode

The video menu's: the platform reopens the display in another mode (8 or
16 bpp, any size), and everything sized or coloured for the old one is
made again.  Called from a key handler, between frames.
================
*/
static qboolean VID_ChangeMode (unsigned long id)
{
	int		i;

	if (id == QG_GetMode ())
		return true;
	if (!QG_SetMode (id))
	{
		Con_Printf ("Cannot open screen mode 0x%08lx\n", id);
		return false;
	}
	D_FlushCaches ();					// surfaces point into the old cache
	VID_SetBuffers ();
	VID_SetPalette (host_basepal);		// the CLUT, or the colour tables
	for (i = 0 ; i < NUM_CSHIFTS ; i++)
		cl.prev_cshifts[i].percent = -1;	// V_UpdatePalette: shifts again
	vid.recalc_refdef = 1;				// view size, scan tables, z rows
	scr_fullupdate = 0;
	Sbar_Changed ();
	Con_CheckResize ();
	Key_ClearStates ();					// the old window took the key-ups
	Con_Printf ("Screen mode %dx%d, %d bits\n", vid.width, vid.height, r_pixbytes * 8);
	return true;
}

/*
================
VID_ModeList_f, VID_SetMode_f

The same from the console (and scripts): "vid_modelist", and "vid_setmode
<mode>" with a mode as vid_modelist prints it, WIDTHxHEIGHTxBITS (e.g.
640x480x16), or its id (0x...).
================
*/
static void VID_ModeList_f (void)
{
	qgmode_t	modes[64];
	int			i, n = QG_ListModes (modes, 64);

	for (i = 0 ; i < n ; i++)
		Con_Printf ("%c %dx%dx%d  (0x%08lx)\n", modes[i].id == QG_GetMode () ? '*' : ' ',
				modes[i].width, modes[i].height, modes[i].bpp, modes[i].id);
	if (!n)
		Con_Printf ("no screen modes\n");
}

static void VID_SetMode_f (void)
{
	qgmode_t	modes[64];
	int			i, n, w, h, b;
	char		*a;

	if (Cmd_Argc () != 2)
	{
		Con_Printf ("vid_setmode <WIDTHxHEIGHTxBITS | 0xMODEID>: see vid_modelist\n");
		return;
	}
	a = Cmd_Argv (1);
	n = QG_ListModes (modes, 64);
	for (i = 0 ; i < n ; i++)
	{
		if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X'))
		{
			if (strtoul (a, NULL, 0) == modes[i].id)
				break;
		}
		else if (sscanf (a, "%dx%dx%d", &w, &h, &b) == 3
				&& w == modes[i].width && h == modes[i].height && b == modes[i].bpp)
			break;
	}
	if (i == n)
	{
		Con_Printf ("vid_setmode: no mode %s (see vid_modelist)\n", a);
		return;
	}
	VID_ChangeMode (modes[i].id);
	vm_count = -1;						// the menu's cursor follows
}

/*
===============================================================================
VIDEO MENU

Options / Video Options: the screen modes, 8-bit ones left and 16-bit ones
right.  Arrows choose, Enter switches; the mode is saved for the next start.
===============================================================================
*/

extern void M_Print (int cx, int cy, char *str);
extern void M_PrintWhite (int cx, int cy, char *str);
extern void M_DrawCharacter (int cx, int line, int num);
extern void M_DrawPic (int x, int y, qpic_t *pic);
extern void M_Menu_Options_f (void);

#define VM_MAXMODES		64
#define VM_ROWS			13				// lines per column on screen

static qgmode_t	vm_modes[VM_MAXMODES];
static int		vm_col[2][VM_MAXMODES];	// indexes into vm_modes, 8 / 16 bpp
static int		vm_num[2];
static int		vm_cursorcol, vm_cursorrow, vm_top[2];

// The mode list, and the cursor on the mode now open.
static void VID_MenuModes (void)
{
	int		i, c;

	vm_count = QG_ListModes (vm_modes, VM_MAXMODES);
	vm_num[0] = vm_num[1] = 0;
	vm_cursorcol = vm_cursorrow = 0;
	for (i = 0 ; i < vm_count ; i++)
	{
		c = (vm_modes[i].bpp == 16);
		if (vm_modes[i].id == QG_GetMode ())
		{
			vm_cursorcol = c;
			vm_cursorrow = vm_num[c];
		}
		vm_col[c][vm_num[c]++] = i;
	}
	if (vm_num[vm_cursorcol] == 0)
		vm_cursorcol = !vm_cursorcol;
}

static void VID_MenuDraw (void)
{
	qpic_t	*p;
	char	buf[48];
	int		c, r, x, y;
	unsigned long	cur = QG_GetMode ();

	if (vm_count < 0)
		VID_MenuModes ();

	p = Draw_CachePic ("gfx/vidmodes.lmp");
	M_DrawPic ((320 - p->width) / 2, 4, p);

	if (vm_count == 0)
	{
		M_Print (8, 48, "No screen modes to choose from");
		M_Print (8, 56, "(the display is headless)");
		return;
	}

	for (c = 0 ; c < 2 ; c++)
	{
		x = 32 + c * 160;
		M_PrintWhite (x, 36, c ? "16 bits" : "8 bits");
		if (vm_num[c] == 0)
			M_Print (x, 48, "(none)");
		// keep the cursor's row in view
		if (c == vm_cursorcol)
		{
			if (vm_cursorrow < vm_top[c])
				vm_top[c] = vm_cursorrow;
			if (vm_cursorrow >= vm_top[c] + VM_ROWS)
				vm_top[c] = vm_cursorrow - VM_ROWS + 1;
		}
		for (r = vm_top[c] ; r < vm_num[c] && r < vm_top[c] + VM_ROWS ; r++)
		{
			qgmode_t	*m = &vm_modes[vm_col[c][r]];

			y = 48 + (r - vm_top[c]) * 8;
			sprintf (buf, "%dx%d", m->width, m->height);
			if (m->id == cur)
				M_PrintWhite (x, y, buf);	// the mode now open
			else
				M_Print (x, y, buf);
			if (c == vm_cursorcol && r == vm_cursorrow)
				M_DrawCharacter (x - 12, y, 12 + ((int)(realtime * 4) & 1));
		}
		if (vm_top[c] > 0)
			M_Print (x + 88, 48, "\x8d");		// more above / below
		if (vm_num[c] > vm_top[c] + VM_ROWS)
			M_Print (x + 88, 48 + (VM_ROWS - 1) * 8, "\x8f");
	}

	sprintf (buf, "Now: %dx%d, %d bits", vid.width, vid.height, r_pixbytes * 8);
	M_PrintWhite (8, 160, buf);
	M_Print (8, 176, "Enter: switch       Esc: back");
}

static void VID_MenuKey (int key)
{
	int		n;

	if (vm_count <= 0)
	{
		if (key == K_ESCAPE)
			M_Menu_Options_f ();
		return;
	}
	n = vm_num[vm_cursorcol];

	switch (key)
	{
	case K_ESCAPE:
		S_LocalSound ("misc/menu1.wav");
		M_Menu_Options_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("misc/menu1.wav");
		vm_cursorrow = (vm_cursorrow > 0) ? vm_cursorrow - 1 : n - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound ("misc/menu1.wav");
		vm_cursorrow = (vm_cursorrow < n - 1) ? vm_cursorrow + 1 : 0;
		break;

	case K_LEFTARROW:
	case K_RIGHTARROW:
		if (vm_num[!vm_cursorcol])
		{
			S_LocalSound ("misc/menu1.wav");
			vm_cursorcol = !vm_cursorcol;
			if (vm_cursorrow >= vm_num[vm_cursorcol])
				vm_cursorrow = vm_num[vm_cursorcol] - 1;
		}
		break;

	case K_ENTER:
		S_LocalSound ("misc/menu2.wav");
		VID_ChangeMode (vm_modes[vm_col[vm_cursorcol][vm_cursorrow]].id);
		break;
	}
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


