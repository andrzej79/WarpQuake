// wq_prof.c -- warpQuake instrumentation, see wq_prof.h

#include "quakedef.h"
#include "wq_prof.h"

unsigned long	wqp_ticks[WQP_COUNT];
unsigned long	wqp_calls[WQP_COUNT];
unsigned long	wqc_count[WQC_COUNT];
int				wqp_qcdepth;

qboolean		wqp_crcon;

// -crc: a CRC-32 over every frame shown.  With a fixed host_framerate a
// timedemo renders the same frames on any machine at any speed, so two
// builds that print the same CRC drew the same pixels.
static unsigned long	crcTable[256];
static unsigned long	crcAll;         // CRC of the frames' CRCs
static unsigned long	crcFrames;
static FILE				*crcFile;       // -crcfile: one line per frame

static qboolean	resetPending;
static qboolean	sampling;
static double	timerCostTicks;     // one QG_Ticks() call, in ticks

extern cvar_t	host_framerate;		// host.c

#define DEFAULT_LOG		"RAM:WarpQuake_prof.txt"
#define DEFAULT_SAMPLES	"RAM:WarpQuake.wprf"

// The report tree: depth is the indent.  A row with a "minus" list is
// derived: its own phase minus the listed ones (what is left inside it).
typedef struct
{
	int			depth;
	const char	*name;
	int			phase;
	int			minus[3];           // up to 3; -1 ends a shorter list
} wqp_row_t;

#define NONE	{-1}

static const wqp_row_t rows[] =
{
	{0, "input + console commands",	WQP_INPUT,		NONE},
	{0, "server",					WQP_SERVER,		NONE},
	{1, "QuakeC",					WQP_QC,			NONE},
	{0, "client (parse messages)",	WQP_CLIENT,		NONE},
	{0, "screen",					WQP_SCREEN,		NONE},
	{1, "3D view (R_RenderView)",	WQP_RENDER,		NONE},
	{2, "setup + mark leaves",		WQP_R_SETUP,	NONE},
	{2, "world BSP + edges",		WQP_R_WORLD,	NONE},
	{2, "brush models",				WQP_R_BMODELS,	NONE},
	{2, "scan edges (all)",			WQP_R_SCAN,		NONE},
	{3, "edge sort + span gen",		WQP_R_SCAN,		{WQP_D_SURFS, -1}},
	{3, "draw surfaces",			WQP_D_SURFS,	NONE},
	{4, "surface cache build",		WQP_D_CACHE,	NONE},
	{4, "spans, z, sky, water",		WQP_D_SURFS,	{WQP_D_CACHE, -1}},
	{2, "alias models + sprites",	WQP_R_ENTS,		NONE},
	{2, "view model",				WQP_R_VIEWMODEL, NONE},
	{2, "particles",				WQP_R_PARTICLES, NONE},
	{2, "underwater warp",			WQP_R_WARP,		NONE},
	{1, "2D: HUD, console, menu",	WQP_SCREEN,		{WQP_RENDER, WQP_BLIT, WQP_CRC}},
	{1, "blit to display",			WQP_BLIT,		NONE},
	{1, "frame CRC (-crc)",			WQP_CRC,		NONE},
	{0, "sound",					WQP_SOUND,		NONE},
	{0, "rest of the frame",		WQP_FRAME,		{WQP_INPUT, WQP_SERVER, WQP_CLIENT}},
};

static void CRCInit (void)
{
	unsigned long	c;
	int				i, k;

	for (i = 0 ; i < 256 ; i++)
	{
		c = i;
		for (k = 0 ; k < 8 ; k++)
			c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
		crcTable[i] = c;
	}
}

static unsigned long CRCUpdate (unsigned long crc, const unsigned char *p, int n)
{
	while (n-- > 0)
		crc = crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}

void WQP_FrameCRC (const unsigned char *buf, int width, int height, int rowbytes)
{
	unsigned long	crc = 0xFFFFFFFFUL;
	unsigned char	be[4];
	int				y;

	for (y = 0 ; y < height ; y++)
		crc = CRCUpdate (crc, buf + y * rowbytes, width);
	crc ^= 0xFFFFFFFFUL;

	be[0] = crc >> 24; be[1] = crc >> 16; be[2] = crc >> 8; be[3] = crc;
	crcAll = CRCUpdate (crcAll, be, 4);
	crcFrames++;
	if (crcFile)
		fprintf (crcFile, "%lu %08lx\n", crcFrames, crc);
}

static void Reset (void)
{
	memset (wqp_ticks, 0, sizeof(wqp_ticks));
	memset (wqp_calls, 0, sizeof(wqp_calls));
	memset (wqc_count, 0, sizeof(wqc_count));
	wqp_qcdepth = 0;
	crcAll = 0xFFFFFFFFUL;
	crcFrames = 0;
	if (crcFile)
		rewind (crcFile);
}

// What one timer read costs, so the report can say how much it disturbs.
static void CalibrateTimer (void)
{
	unsigned long	t0, t1;
	int				i;

	t0 = QG_Ticks ();
	for (i = 0 ; i < 1000 ; i++)
		QG_Ticks ();
	t1 = QG_Ticks ();
	timerCostTicks = (double)(t1 - t0) / 1000.0;
}

static double PhaseTicks (const wqp_row_t *r)
{
	double	t;
	int		i;

	t = (double)wqp_ticks[r->phase];
	for (i = 0 ; i < 3 && r->minus[i] >= 0 ; i++)
		t -= (double)wqp_ticks[r->minus[i]];
	// "rest of the frame" also leaves out the screen and sound
	if (r->phase == WQP_FRAME)
		t -= (double)wqp_ticks[WQP_SCREEN] + (double)wqp_ticks[WQP_SOUND];
	return t;
}

static void Out (FILE *f, char *fmt, ...)
{
	va_list		argptr;
	char		text[256];

	va_start (argptr, fmt);
	vsnprintf (text, sizeof(text), fmt, argptr);
	va_end (argptr);
	Con_Printf ("%s", text);
	if (f)
		fputs (text, f);
}

static void Report (const char *logpath)
{
	FILE			*f = NULL;
	unsigned long	frames;
	double			rate, frameMs, msPerTick, t, pixels;
	unsigned long	reads;
	int				i;
	const wqp_row_t	*r;

	frames = wqp_calls[WQP_FRAME];
	if (!frames)
	{
		Con_Printf ("prof: no frames measured\n");
		return;
	}
	if (logpath)
		f = fopen (logpath, "w");

	rate = (double)QG_TickRate ();
	msPerTick = 1000.0 / rate;
	frameMs = (double)wqp_ticks[WQP_FRAME] * msPerTick / frames;
	// the 3D view, not the screen: the status bar is not rendered in 3D
	pixels = (double)r_refdef.vrect.width * r_refdef.vrect.height;

	Out (f, "---- warpQuake profile: %lu frames, %.2f ms/frame (%.1f fps) ----\n",
			frames, frameMs, 1000.0 / frameMs);
	Out (f, "%dx%d (3D view %dx%d), %s\n", vid.width, vid.height,
			r_refdef.vrect.width, r_refdef.vrect.height, QG_VideoInfo ());
	Out (f, "%-30s %8s %6s %7s\n", "phase", "ms/frm", "%", "calls");
	for (r = rows ; r < rows + sizeof(rows)/sizeof(rows[0]) ; r++)
	{
		char	name[40];

		t = PhaseTicks (r) * msPerTick / frames;
		sprintf (name, "%*s%s", r->depth * 2, "", r->name);
		if (r->minus[0] >= 0 || r->phase == WQP_FRAME)
			Out (f, "%-30s %8.2f %5.1f%%\n", name, t, 100.0 * t / frameMs);
		else
			Out (f, "%-30s %8.2f %5.1f%% %7.1f\n", name, t, 100.0 * t / frameMs,
					(double)wqp_calls[r->phase] / frames);
	}

	Out (f, "-- work per frame\n");
	Out (f, "world span pixels   %8.0f  (%.2fx the 3D view; 1 B each + 1 B texel read)\n",
			(double)wqc_count[WQC_SPAN_PIXELS] / frames, (double)wqc_count[WQC_SPAN_PIXELS] / frames / pixels);
	Out (f, "z-buffer words      %8.0f  (2 B each)\n", (double)wqc_count[WQC_ZSPAN_PIXELS] / frames);
	Out (f, "water/lava pixels   %8.0f\n", (double)wqc_count[WQC_TURB_PIXELS] / frames);
	Out (f, "alias span pixels   %8.0f  (tested against z; not all are written)\n",
			(double)wqc_count[WQC_POLY_PIXELS] / frames);
	Out (f, "surface cache       %8.1f blocks, %.1f KB built\n",
			(double)wqc_count[WQC_CACHE_BUILDS] / frames, wqc_count[WQC_CACHE_BYTES] / 1024.0 / frames);
	Out (f, "blit                %8.1f KB\n", wqc_count[WQC_BLIT_BYTES] / 1024.0 / frames);

	reads = 0;
	for (i = 0 ; i < WQP_COUNT ; i++)
		reads += 2 * wqp_calls[i];
	Out (f, "-- timer: %.0f Hz, %.2f us/read, %.1f reads/frame = %.3f ms/frame (%.2f%%) of overhead\n",
			rate, timerCostTicks * 1000.0 * msPerTick, (double)reads / frames,
			timerCostTicks * reads / frames * msPerTick, 100.0 * timerCostTicks * reads / frames * msPerTick / frameMs);

	if (wqp_crcon)
		Out (f, "-- frame CRC-32: %08lx over %lu frames%s\n", crcAll ^ 0xFFFFFFFFUL, crcFrames,
				host_framerate.value > 0 ? "" : " (host_framerate is 0: not reproducible)");

	if (f)
	{
		fclose (f);
		Con_Printf ("prof: written to %s\n", logpath);
	}
}

static const char *ArgValue (const char *name, const char *def)
{
	int	i = COM_CheckParm ((char *)name);

	if (i && i < com_argc - 1)
		return com_argv[i+1];
	return def;
}

static void SampleStart (void)
{
	if (sampling)
		return;
	sampling = QG_SampleStart ();
	Con_Printf (sampling ? "profsample: sampling\n" : "profsample: could not start\n");
}

static void SampleStop (void)
{
	const char		*path;
	unsigned long	n;

	if (!sampling)
		return;
	sampling = false;
	path = ArgValue ("-samplefile", DEFAULT_SAMPLES);
	n = QG_SampleStop (path);
	Con_Printf ("profsample: %lu samples written to %s\n", n, path);
}

static void Prof_f (void)
{
	Report (Cmd_Argc () > 1 ? Cmd_Argv (1) : NULL);
}

static void ProfReset_f (void)
{
	resetPending = true;
}

static void ProfSample_f (void)
{
	if (Cmd_Argc () > 1 && !Q_strcmp (Cmd_Argv (1), "stop"))
		SampleStop ();
	else
		SampleStart ();
}

void WQP_Init (void)
{
	Cmd_AddCommand ("prof", Prof_f);
	Cmd_AddCommand ("profreset", ProfReset_f);
	Cmd_AddCommand ("profsample", ProfSample_f);
	CalibrateTimer ();
	CRCInit ();
	wqp_crcon = COM_CheckParm ("-crc") != 0;
	if (wqp_crcon && COM_CheckParm ("-crcfile") && COM_CheckParm ("-crcfile") < com_argc - 1)
		crcFile = fopen (com_argv[COM_CheckParm ("-crcfile") + 1], "w");
	Reset ();
}

void WQP_FrameStart (void)
{
	if (resetPending)
	{
		resetPending = false;
		Reset ();
	}
}

void WQP_TimedemoCommand (void)
{
	// _Host_Frame calls rand() on every pass, filtered-out ones included,
	// so how many calls came before depends on the machine's speed.  The
	// particles draw from it: reseed, or no two runs render alike.
	if (wqp_crcon)
		srand (0);
}

void WQP_TimedemoStart (void)
{
	// mid-frame: the reset waits for the next frame to begin
	resetPending = true;
	if (COM_CheckParm ("-sample"))
		SampleStart ();
}

void WQP_TimedemoEnd (void)
{
	SampleStop ();
	Report (ArgValue ("-proflog", DEFAULT_LOG));
	if (crcFile)
	{
		fclose (crcFile);
		crcFile = NULL;
	}
}
