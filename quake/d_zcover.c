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
// d_zcover.c -- warpQuake: where the world's z-buffer is needed this frame
//
// The world writes a z value for every pixel of the 3D view (97 KB a frame
// at 320x200), but only alias models, sprites and particles ever read one -
// typically a small part of the screen.  On a Warp the z-buffer alone is
// bigger than the FPGA's 96 KB L2, and every line it touches is a fill from
// DDR3.  So before the world is drawn, this bounds, row by row, the screen
// area that anything reading z can touch, and D_DrawZSpans writes only that.
//
// Nothing is approximated: the z values that are written are the same ones,
// and the readers only look inside the bounds.  A bound that is too small
// shows up as a -crc mismatch against r_zcover 0, which writes every row in
// full as before.

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"
#include "wq_prof.h"

// per screen row: the z-buffer is written for d_zcov_x0 <= x < d_zcov_x1
short	d_zcov_x0[MAXHEIGHT];
short	d_zcov_x1[MAXHEIGHT];

cvar_t	r_zcover = {"r_zcover", "1"};

extern particle_t	*active_particles;	// r_part.c

static int	viewx0, viewy0, viewx1, viewy1;	// the 3D view, inclusive

static void AddRect (int x0, int y0, int x1, int y1)
{
	int	y;

	if (x0 < viewx0) x0 = viewx0;
	if (x1 > viewx1) x1 = viewx1;
	if (y0 < viewy0) y0 = viewy0;
	if (y1 > viewy1) y1 = viewy1;
	if (x0 > x1 || y0 > y1)
		return;

	{
		// pointers, not indexes: vbcc reloaded both tables' addresses for
		// every row of the indexed version
		short	*p0 = &d_zcov_x0[y0], *p1 = &d_zcov_x1[y0];
		short	sx0 = (short)x0, sx1 = (short)(x1 + 1);

		for (y = y1 - y0 ; y >= 0 ; y--, p0++, p1++)
		{
			if (sx0 < *p0)
				*p0 = sx0;
			if (sx1 > *p1)
				*p1 = sx1;
		}
	}
}

// A point in view space: x right, y up, z forward.  A macro: vbcc does not
// inline, and as a function this was a third of the pass.
#define TO_VIEW(org, out) \
	do { \
		float	lx = (org)[0] - r_origin[0], ly = (org)[1] - r_origin[1], lz = (org)[2] - r_origin[2]; \
		(out)[0] = lx * vright[0] + ly * vright[1] + lz * vright[2]; \
		(out)[1] = lx * vup[0] + ly * vup[1] + lz * vup[2]; \
		(out)[2] = lx * vpn[0] + ly * vpn[1] + lz * vpn[2]; \
	} while (0)

static void ToView (vec3_t org, vec3_t out)
{
	TO_VIEW (org, out);
}

// True if the world-space box mins..maxs lies wholly outside one of the four
// side planes of the view (R_SetupFrame's view_clipplanes): its corner
// farthest along the plane's normal is still behind it - by 4 units, so a
// model grazing the screen edge is never lost to rounding.
static qboolean BoxOffScreen (vec3_t mins, vec3_t maxs)
{
	int		i;
	float	*n;

	for (i = 0 ; i < 4 ; i++)
	{
		n = view_clipplanes[i].normal;
		if ((n[0] >= 0 ? maxs[0] : mins[0]) * n[0]
		  + (n[1] >= 0 ? maxs[1] : mins[1]) * n[1]
		  + (n[2] >= 0 ? maxs[2] : mins[2]) * n[2] - view_clipplanes[i].dist < -4.0)
			return true;
	}
	return false;
}

static void CornerBounds (vec3_t corners[8], vec3_t mins, vec3_t maxs)
{
	int		i;
	float	*c;

	VectorCopy (corners[0], mins);
	VectorCopy (corners[0], maxs);
	// a pointer walk: vbcc multiplied out every corners[i][k] index
	for (i = 1, c = corners[1] ; i < 8 ; i++, c += 3)
	{
		if (c[0] < mins[0]) mins[0] = c[0];
		if (c[0] > maxs[0]) maxs[0] = c[0];
		if (c[1] < mins[1]) mins[1] = c[1];
		if (c[1] > maxs[1]) maxs[1] = c[1];
		if (c[2] < mins[2]) mins[2] = c[2];
		if (c[2] > maxs[2]) maxs[2] = c[2];
	}
}

// The screen rectangle of a box given by its 8 corners in world space (bit 0,
// 1, 2 of the index choose max over min per axis), clipped at the alias near
// plane (geometry closer is clipped away when drawn).  False if it is all
// behind that plane.
static qboolean BoxScreenRect (vec3_t corners[8], int *x0, int *y0, int *x1, int *y1)
{
	vec3_t		view[20];
	qboolean	clipped[8];
	float		zi, u, v, frac, minu, minv, maxu, maxv;
	int			i, j, bit, numv;

	for (i = 0 ; i < 8 ; i++)
	{
		TO_VIEW (corners[i], view[i]);
		clipped[i] = view[i][2] < ALIAS_Z_CLIP_PLANE;
	}

	// where the box's edges (corners one bit apart) cross the near plane
	numv = 8;
	for (i = 0 ; i < 8 ; i++)
	{
		for (bit = 1 ; bit < 8 ; bit <<= 1)
		{
			j = i ^ bit;
			if (j < i || clipped[i] == clipped[j])
				continue;
			frac = (ALIAS_Z_CLIP_PLANE - view[i][2]) / (view[j][2] - view[i][2]);
			view[numv][0] = view[i][0] + (view[j][0] - view[i][0]) * frac;
			view[numv][1] = view[i][1] + (view[j][1] - view[i][1]) * frac;
			view[numv][2] = ALIAS_Z_CLIP_PLANE;
			numv++;
		}
	}

	minu = minv = 1e30;
	maxu = maxv = -1e30;
	for (i = 0 ; i < numv ; i++)
	{
		if (i < 8 && clipped[i])
			continue;
		zi = 1.0 / view[i][2];
		u = xcenter + xscale * view[i][0] * zi;
		v = ycenter - yscale * view[i][1] * zi;	// screen y grows downward
		if (u < minu) minu = u;
		if (u > maxu) maxu = u;
		if (v < minv) minv = v;
		if (v > maxv) maxv = v;
	}
	if (minu > maxu)
		return false;

	// clamp in float first: a point near the near plane projects far out
	if (minu < -1e6) minu = -1e6;
	if (maxu > 1e6) maxu = 1e6;
	if (minv < -1e6) minv = -1e6;
	if (maxv > 1e6) maxv = 1e6;
	*x0 = (int)minu - 2;	// 2 pixels of slack for the rasterizer's rounding
	*x1 = (int)maxu + 2;
	*y0 = (int)minv - 2;
	*y1 = (int)maxv + 2;
	return true;
}

// Static entities never move or turn, so their world-space corners are
// computed once (AngleVectors is software sin/cos on the 68060) and only
// projected per frame.  Keyed by entity, frame and origin, so a new map or a
// changed frame recomputes.
typedef struct
{
	entity_t	*e;
	int			frame;
	vec3_t		origin;
	vec3_t		corners[8];
	vec3_t		mins, maxs;		// their world-space bounds, for culling
} cornercache_t;

static cornercache_t	staticcorners[MAX_STATIC_ENTITIES];

static void AddAlias (entity_t *e, int staticindex)
{
	vec3_t	dyn[8], dynmins, dynmaxs;
	vec3_t	*corners;
	float	*mins, *maxs;
	int		x0, y0, x1, y1;

	if (staticindex >= 0)
	{
		cornercache_t	*c = &staticcorners[staticindex];

		if (c->e != e || c->frame != e->frame || !VectorCompare (c->origin, e->origin))
		{
			R_AliasWorldCorners (e, c->corners);
			CornerBounds (c->corners, c->mins, c->maxs);
			c->e = e;
			c->frame = e->frame;
			VectorCopy (e->origin, c->origin);
		}
		corners = c->corners;
		mins = c->mins;
		maxs = c->maxs;
	}
	else
	{
		R_AliasWorldCorners (e, dyn);
		CornerBounds (dyn, dynmins, dynmaxs);
		corners = dyn;
		mins = dynmins;
		maxs = dynmaxs;
	}

	if (BoxOffScreen (mins, maxs))
		return;			// not drawn: R_AliasCheckBBox rejects it too

	if (BoxScreenRect (corners, &x0, &y0, &x1, &y1))
		AddRect (x0, y0, x1, y1);
}

// A sprite: the box around a sphere of its largest frame, projected at its
// nearest and farthest depth.  Close to the eye the bound is not worth
// computing: the whole view.
static void AddSprite (entity_t *e)
{
	msprite_t	*psprite = e->model->cache.data;
	float		r, zn, zf, x, y, u0, u1, v0, v1;
	vec3_t		c;

	r = (float)(psprite->maxwidth + psprite->maxheight);
	ToView (e->origin, c);
	if (c[2] + r < 1.0)
		return;			// behind the eye
	if (c[2] - r < 1.0)
	{
		AddRect (viewx0, viewy0, viewx1, viewy1);
		return;
	}
	zn = c[2] - r;
	zf = c[2] + r;
	x = c[0] - r;
	u0 = xcenter + xscale * x / ((x < 0) ? zn : zf);
	x = c[0] + r;
	u1 = xcenter + xscale * x / ((x > 0) ? zn : zf);
	y = c[1] + r;		// screen y grows downward
	v0 = ycenter - yscale * y / ((y > 0) ? zn : zf);
	y = c[1] - r;
	v1 = ycenter - yscale * y / ((y < 0) ? zn : zf);
	AddRect ((int)u0 - 2, (int)v0 - 2, (int)u1 + 2, (int)v1 + 2);
}

// An alias model or a sprite, anything else ignored (brush models write z
// through the edge list, they never read it).
static void AddEntity (entity_t *e, int staticindex)
{
	if (!e->model)
		return;
	if (e->model->type == mod_alias)
		AddAlias (e, staticindex);
	else if (e->model->type == mod_sprite)
		AddSprite (e);
}

// A static entity (torches, flames...) reaches cl_visedicts only during the
// world's BSP walk (R_StoreEfrags), after this has run.  That walk takes it
// when one of its leaves is in the PVS and in the view; the PVS part is
// already known here (R_MarkLeaves), and is the safe superset.
static int		visstatics[MAX_STATIC_ENTITIES];	// the statics in the PVS ...
static int		numvisstatics;
static int		staticvisframe = -1;				// ... as of this PVS
static int		staticnum = -1;
static model_t	*staticworld;

static qboolean StaticVisible (entity_t *e)
{
	efrag_t	*ef;

	for (ef = e->efrag ; ef ; ef = ef->entnext)
		if (ef->leaf->visframe == r_visframecount)
			return true;
	return false;
}

// A particle: projected by D_ProjectParticle, which D_DrawParticle then
// reuses (so this costs the frame almost nothing), plus the largest block
// D_DrawParticle draws from that point: up to d_pix_max wide, and that many
// rows times 1 << d_y_aspect_shift.
static void AddParticle (particle_t *p)
{
	D_ProjectParticle (p);
	if (p->wq_u < 0)
		return;
	AddRect (p->wq_u - 1, p->wq_v - 1, p->wq_u + d_pix_max + 1,
			 p->wq_v + (d_pix_max << d_y_aspect_shift) + 1);
}

/*
=============
D_ZCoverBuild

After R_SetupFrame (the view is known), before R_EdgeDrawing.
=============
*/
void D_ZCoverBuild (void)
{
	int			i, y;
	unsigned long	n;
	particle_t	*p;

	viewx0 = r_refdef.vrect.x;
	viewy0 = r_refdef.vrect.y;
	viewx1 = viewx0 + r_refdef.vrect.width - 1;
	viewy1 = viewy0 + r_refdef.vrect.height - 1;

	if (!r_zcover.value)
	{
		for (y = viewy0 ; y <= viewy1 ; y++)
		{
			d_zcov_x0[y] = viewx0;
			d_zcov_x1[y] = viewx1 + 1;
		}
		return;
	}

	for (y = viewy0 ; y <= viewy1 ; y++)
	{
		d_zcov_x0[y] = viewx1 + 1;	// empty
		d_zcov_x1[y] = viewx0;
	}

	// everything R_DrawEntitiesOnList might draw (the player's own model
	// too, which it skips unless chasing: a bound too many costs little)
	for (i = 0 ; i < cl_numvisedicts ; i++)
		AddEntity (cl_visedicts[i], -1);

	// which statics are in the PVS changes only with the PVS
	// (r_visframecount): walking every static's efrag chain each frame
	// was a third of this pass
	if (staticvisframe != r_visframecount || staticnum != cl.num_statics
		|| staticworld != cl.worldmodel)
	{
		staticvisframe = r_visframecount;
		staticnum = cl.num_statics;
		staticworld = cl.worldmodel;
		numvisstatics = 0;
		for (i = 0 ; i < cl.num_statics && i < MAX_STATIC_ENTITIES ; i++)
			if (StaticVisible (&cl_static_entities[i]))
				visstatics[numvisstatics++] = i;
	}
	for (i = 0 ; i < numvisstatics ; i++)
		AddEntity (&cl_static_entities[visstatics[i]], visstatics[i]);

	AddEntity (&cl.viewent, -1);		// the weapon

	// the particles' view vectors, exactly as R_DrawParticles sets them
	VectorScale (vright, xscaleshrink, r_pright);
	VectorScale (vup, yscaleshrink, r_pup);
	VectorCopy (vpn, r_ppn);
	n = 0;
	for (p = active_particles ; p ; p = p->next)
	{
		AddParticle (p);
		n++;
	}
	WQC_ADD (WQC_PARTICLES, n);
	if (n > wqc_particlepeak)
		wqc_particlepeak = n;
}
