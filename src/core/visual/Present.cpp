/*---------------------------------------------------------------------------*/
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
/*---------------------------------------------------------------------------*/
/* Presentation seam - implementation.
 *
 * Holds the host's callbacks and translates between the engine's types and the
 * plain-C contract in Present.h. The translation happens only here, so the
 * boundary is crossed in exactly one place and the host side never sees a
 * tTVPRect or a ttstr.
 */
/*---------------------------------------------------------------------------*/

#include <stdlib.h>
#include <stdio.h>

#include "tjsCommHead.h"

#include "ComplexRect.h"

#include "Present.h"
#include "PresentIntf.h"

/*---------------------------------------------------------------------------*/
/* Host callbacks
 *
 * Written once before the engine starts and read thereafter. A host that wants
 * to change them mid-run must stop the engine first, which the libretro core
 * does on unload.
 */
/*---------------------------------------------------------------------------*/

static tTVPPresentCallback PresentCallback = NULL;
static void * PresentHostData = NULL;

static tTVPQuerySurfaceSizeCallback QuerySurfaceSizeCallback = NULL;
static void * QuerySurfaceSizeHostData = NULL;

static tTVPWaitForPresentCallback WaitForPresentCallback = NULL;
static void * WaitForPresentHostData = NULL;

static tTVPTextPlaneCallback TextPlaneCallback = NULL;
static void * TextPlaneHostData = NULL;

/*---------------------------------------------------------------------------*/

extern "C" void TVPSetPresentCallback(tTVPPresentCallback callback, void * hostdata)
{
	PresentCallback = callback;
	PresentHostData = hostdata;
}

extern "C" void TVPSetQuerySurfaceSizeCallback(tTVPQuerySurfaceSizeCallback callback,
	void * hostdata)
{
	QuerySurfaceSizeCallback = callback;
	QuerySurfaceSizeHostData = hostdata;
}

extern "C" void TVPSetWaitForPresentCallback(tTVPWaitForPresentCallback callback,
	void * hostdata)
{
	WaitForPresentCallback = callback;
	WaitForPresentHostData = hostdata;
}

/*---------------------------------------------------------------------------*/
/* Engine-internal entry points
 */
/*---------------------------------------------------------------------------*/

bool TVPIsPresentCallbackSet()
{
	return PresentCallback != NULL;
}

void TVPPresentComposite(void * pixels, tjs_int width, tjs_int height,
	tjs_int pitch, const tTVPRect & changed, bool full, tjs_uint64 serial)
{
	if(PresentCallback == NULL) return;

	/* Fill the host's description from the engine's. The pixel pointer is
	 * handed over as-is: the engine's bitmap is already BGRA with a padded
	 * pitch, which is exactly what the contract promises, so this is a struct
	 * fill and not a copy. The rect is converted from the engine's
	 * left/top/right/bottom form to the host's x/y/width/height form, which is
	 * the one piece of convention translation the boundary needs. */
	tTVPPresentBuffer buffer;
	buffer.Pixels       = pixels;
	buffer.Width        = (int)width;
	buffer.Height       = (int)height;
	buffer.Pitch        = (int)pitch;
	buffer.ChangedX     = (int)changed.left;
	buffer.ChangedY     = (int)changed.top;
	buffer.ChangedWidth = (int)(changed.right - changed.left);
	buffer.ChangedHeight= (int)(changed.bottom - changed.top);
	buffer.Full         = full;
	buffer.FrameSerial  = serial;
	buffer.HostData     = PresentHostData;

	PresentCallback(&buffer);
}

void TVPQueryPresentSurfaceSize(tjs_int * width, tjs_int * height)
{
	if(width == NULL || height == NULL) return;

	/* The host answers in plain ints; 0 for either means "you choose". */
	int w = 0, h = 0;
	if(QuerySurfaceSizeCallback != NULL)
		QuerySurfaceSizeCallback(&w, &h, QuerySurfaceSizeHostData);

	*width  = (tjs_int)w;
	*height = (tjs_int)h;
}

void TVPWaitForPresent()
{
	if(WaitForPresentCallback == NULL) return;
	WaitForPresentCallback(WaitForPresentHostData);
}

/*---------------------------------------------------------------------------*/
/* The text plane
 */
/*---------------------------------------------------------------------------*/

extern "C" void TVPSetTextPlaneCallback(tTVPTextPlaneCallback callback, void * hostdata)
{
	TextPlaneCallback = callback;
	TextPlaneHostData = hostdata;
}

bool TVPIsTextPlaneCallbackSet()
{
	return TextPlaneCallback != NULL;
}

/* Off unless a host asks (see PresentIntf.h). Not cleared by detaching the
 * callback: a host that reattaches has already stated whether it splices, and
 * the flag costs one branch when nothing is listening anyway. */
static bool TextPlaneSplice = false;

/* C++ linkage, matching PresentIntf.h - the engine's own side of the seam. The
 * CORE reaches this through krkr_set_text_plane_splice in the C ABI
 * (src/engine_kirikiri_cabi.cpp), not by name, so no extern "C" is needed. */
void TVPSetTextPlaneSplice(bool enabled)
{
	TextPlaneSplice = enabled;
}

bool TVPIsTextPlaneSpliceEnabled()
{
	return TextPlaneSplice;
}

/* Off unless a host asks (see PresentIntf.h). */
static bool SplitViewArtworkOnly = false;

void TVPSetSplitViewArtworkOnly(bool enabled)
{
	SplitViewArtworkOnly = enabled;
}

bool TVPIsSplitViewArtworkOnly()
{
	return SplitViewArtworkOnly;
}

void TVPPresentTextPlane(void * pixels, tjs_int width, tjs_int height,
	tjs_int pitch, const tTVPRect & changed, tjs_int origin_x, tjs_int origin_y,
	bool restart, tjs_int plane_x, tjs_int plane_y)
{
	if(TextPlaneCallback == NULL) return;
	/* pitch retains its sign: a negative value is the engine's bottom-up
	 * row direction, not an error, and rejecting it dropped every
	 * publication. Only a zero stride is unusable. */
	if(pixels == NULL || width <= 0 || height <= 0 || pitch == 0) return;

	/* A monotonic counter, as for frames, so a host can tell "the engine
	 * updated the plane with identical pixels" from "the engine did not
	 * update". Kept here rather than in the drawing code so every caller
	 * shares one sequence. */
	static tjs_uint64 plane_serial = 0;

	tTVPTextPlane plane;
	plane.Pixels        = pixels;
	plane.Width         = (int)width;
	plane.Height        = (int)height;
	plane.Pitch         = (int)pitch;  /* signed; see the guard above */
	/* Converted from the engine's left/top/right/bottom form, as for the frame:
	 * the one piece of convention translation this boundary needs. An empty
	 * engine rect (right <= left or bottom <= top) becomes width/height 0, which
	 * the contract defines as "no pixels changed". */
	plane.ChangedX      = (int)changed.left;
	plane.ChangedY      = (int)changed.top;
	plane.ChangedWidth  = (int)(changed.right - changed.left);
	plane.ChangedHeight = (int)(changed.bottom - changed.top);
	if(plane.ChangedWidth  < 0) plane.ChangedWidth  = 0;
	if(plane.ChangedHeight < 0) plane.ChangedHeight = 0;
	plane.OriginX       = (int)origin_x;
	plane.OriginY       = (int)origin_y;
	/* Splice mode only: with it off the walk that produced these was skipped,
	 * so the caller passed 0 and no host can be relying on them. */
	plane.PlaneX        = TextPlaneSplice ? (int)plane_x : 0;
	plane.PlaneY        = TextPlaneSplice ? (int)plane_y : 0;
	plane.Restart       = restart;
	plane.PlaneSerial   = ++plane_serial;
	plane.HostData      = TextPlaneHostData;

	TextPlaneCallback(&plane);
}

/*---------------------------------------------------------------------------*/
/* The text run stream
 *
 * Same shape as the plane above: one host callback, one monotonic serial, one
 * place where the engine's types stop (the contract struct is all plain C).
 */
/*---------------------------------------------------------------------------*/

static tTVPTextRunCallback TextRunCallback = NULL;
static void * TextRunHostData = NULL;

extern "C" void TVPSetTextRunCallback(tTVPTextRunCallback callback, void * hostdata)
{
	TextRunCallback = callback;
	TextRunHostData = hostdata;
}

bool TVPIsTextRunCallbackSet()
{
	return TextRunCallback != NULL;
}

void TVPPresentTextRun(const tjs_char * text, tjs_int length,
	tjs_int x, tjs_int y,
	tjs_uint32 color, tjs_int opacity, bool antialias,
	tjs_int shadow_level, tjs_uint32 shadow_color,
	tjs_int shadow_ofs_x, tjs_int shadow_ofs_y)
{
	if(TextRunCallback == NULL) return;
	if(text == NULL || length <= 0) return;

	/* A monotonic counter, as for frames and the plane. */
	static tjs_uint64 run_serial = 0;

	tTVPTextRun run;
	/* NOT retained: the caller's string may be reused the moment this returns,
	 * and the contract says so. Present.cpp hands the host the same pointer for
	 * the duration of the callback only, which is the documented window. The
	 * cast is the boundary's one type translation - the engine's tjs_char is
	 * utf-16 and the contract states UTF-16 code units. */
	run.Text          = (const uint16_t *)text;
	run.TextLength    = (int)length;
	run.X             = (int)x;
	run.Y             = (int)y;
	run.Color         = (uint32_t)color;
	run.Opacity       = (int)opacity;
	run.Antialias     = antialias;
	run.ShadowLevel   = (int)shadow_level;
	run.ShadowColor   = (uint32_t)shadow_color;
	run.ShadowOffsetX = (int)shadow_ofs_x;
	run.ShadowOffsetY = (int)shadow_ofs_y;
	run.RunSerial     = ++run_serial;
	run.HostData      = TextRunHostData;

	TextRunCallback(&run);
}

/*---------------------------------------------------------------------------*/
/* User font options
 *
 * The host (the libretro core) resolves the core options and publishes them
 * through the bridge; these three entry points are how the engine's own code
 * reads them, and the ONLY host declarations this file adds. Everything below
 * returns the identity when no host is attached, so a standalone engine build
 * (its own main(), no core) behaves exactly as it did before the feature and no
 * engine call site needs to test for the host.
 *
 * Unlike the callbacks above these are not installed: they are values, read at
 * the point of use. The face is read once, when fonts are registered; the size
 * and the tint are read per glyph, so a frontend that changes one mid-run takes
 * effect without a restart.
 */
/*---------------------------------------------------------------------------*/

/* __LIBRETRO__ is the one C marker this whole tree tests for "inside the vnovel
 * core" — the core, the two ONScripter engines and this engine's CMake build all
 * define it (see the CMakeLists note beside the KRKRSDL2_LIBRETRO option). The
 * fallbacks below are for a STANDALONE engine build, which has no core to
 * resolve these against. */
#ifdef __LIBRETRO__
extern "C" {
/* The bridge's helpers (see src/engine_bridge.h). Declared here rather than by
 * including a core header: this file is compiled into the engine module, which
 * must not depend on the core's sources, and these two functions are the whole
 * of what it needs. */
extern int vnovel_font_scaled_size(int size);
extern unsigned int vnovel_font_tint_rgb(unsigned int rgb);
}
#else
static inline int vnovel_font_scaled_size(int size) { return size; }
static inline unsigned int vnovel_font_tint_rgb(unsigned int rgb) { return rgb; }
#endif

tjs_int TVPGetUserFontScaledHeight(tjs_int height)
{
	return (tjs_int)vnovel_font_scaled_size((int)height);
}

tjs_uint32 TVPGetUserFontTint(tjs_uint32 color)
{
	/* The tint helper works on 0xRRGGBB, which is what the glyph funnels carry
	 * (alpha rides in the blend's opacity, not in the colour). */
	return (tjs_uint32)vnovel_font_tint_rgb((unsigned int)color);
}
