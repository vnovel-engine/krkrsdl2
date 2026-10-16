/*---------------------------------------------------------------------------*/
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
/*---------------------------------------------------------------------------*/
/* Presentation seam - engine-internal half.
 *
 * Present.h is the host-facing contract and is deliberately free of engine
 * types. This header is the other side of the same seam: the entry points the
 * engine's own drawing code calls to reach the host, expressed in engine types.
 * The translation happens in Present.cpp, so the boundary is crossed in exactly
 * one place.
 */
/*---------------------------------------------------------------------------*/

#ifndef __TVP_PRESENT_INTF_H__
#define __TVP_PRESENT_INTF_H__

#include "tjsCommHead.h"

#include "ComplexRect.h"

#include "Present.h"

/*---------------------------------------------------------------------------*/
/* True if a host has installed a present callback. The drawing code uses this
 * to skip assembling a buffer nobody will read; the composite itself still runs
 * either way, so layer bookkeeping and timing do not depend on whether a host
 * is attached. */
extern bool TVPIsPresentCallbackSet();

/* Hands a completed composite to the host.
 *
 * `pixels` points at the engine's live pixel data and is not copied: the
 * description is filled in place and the pointer stays valid for the duration
 * of the host callback only. `changed` is the union of the regions composited
 * since the previous call; `full` says the host should treat the whole buffer
 * as changed; `serial` is a monotonic frame counter.
 *
 * A no-op when no host is attached. */
extern void TVPPresentComposite(void * pixels, tjs_int width, tjs_int height,
	tjs_int pitch, const tTVPRect & changed, bool full, tjs_uint64 serial);

/* Asks the host how large the surface should be before the engine sizes its
 * primary layer. Leaves both outputs 0 when the host has no opinion or none is
 * attached, which the caller reads as "engine's choice". */
extern void TVPQueryPresentSurfaceSize(tjs_int * width, tjs_int * height);

/* Blocks until the host has consumed a frame, or returns immediately when no
 * host asked to pace the engine. */
extern void TVPWaitForPresent();

/*---------------------------------------------------------------------------*/
/* The text plane, engine-internal side.
 *
 * See the long note on tTVPTextPlane in Present.h for what this is for. These
 * are what the drawing code calls; Present.cpp crosses the boundary.
 */
/*---------------------------------------------------------------------------*/

/* True if a host is listening for the text plane. The drawing code checks this
 * before doing the work of separating text from the frame, so a host that does
 * not want the feature pays nothing. */
extern bool TVPIsTextPlaneCallbackSet();

/* Announces a text-plane update. `pixels`/`width`/`height`/`pitch` describe the
 * engine's text surface, which is *not* copied and stays valid after the call
 * (unlike the present buffer) because it is the engine's own surface.
 *
 * `changed` is the region written since the previous call, in plane
 * coordinates; pass an empty rect when only `restart` or the origin changed.
 * `restart` says the plane was cleared and everything on it is new. */
extern void TVPPresentTextPlane(void * pixels, tjs_int width, tjs_int height,
	tjs_int pitch, const tTVPRect & changed, tjs_int origin_x, tjs_int origin_y,
	bool restart);

#endif /* __TVP_PRESENT_INTF_H__ */
