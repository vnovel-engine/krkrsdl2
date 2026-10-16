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

/* Menu visibility. An edge, not a frame: the script sets MenuItem.visible and
 * this forwards it once. Deliberately NOT gated on anything - a host with no
 * menu simply gets a call it ignores - and no-op when no callback is installed,
 * so a headless run is unaffected. */
static tTVPMenuVisibleCallback MenuVisibleCallback = NULL;
static void * MenuVisibleHostData = NULL;

extern "C" void TVPSetMenuVisibleCallback(tTVPMenuVisibleCallback callback, void * hostdata)
{
	MenuVisibleCallback = callback;
	MenuVisibleHostData = hostdata;
}

/* Called from the platform MenuItem's SetVisible. Reached on the engine thread,
 * from script, so it must not block. */
extern "C" void TVPNotifyMenuVisible(bool visible)
{
	if (MenuVisibleCallback)
		MenuVisibleCallback(visible, MenuVisibleHostData);
}

bool TVPIsTextPlaneCallbackSet()
{
	return TextPlaneCallback != NULL;
}

void TVPPresentTextPlane(void * pixels, tjs_int width, tjs_int height,
	tjs_int pitch, const tTVPRect & changed, tjs_int origin_x, tjs_int origin_y,
	bool restart)
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
	plane.Restart       = restart;
	plane.PlaneSerial   = ++plane_serial;
	plane.HostData      = TextPlaneHostData;

	TextPlaneCallback(&plane);
}
