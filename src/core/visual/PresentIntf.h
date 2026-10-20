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

/* Enables the line-placement context on publications (PlaneX/PlaneY in
 * tTVPTextPlane). OFF by default, because producing it costs a parent-chain
 * walk per publication and a host that only presents the latest surface has no
 * use for it.
 *
 * A host asks for it exactly when it intends to ACCUMULATE a page out of the
 * per-publication fragments (KiriKiri publishes one line box at a time; see the
 * field's note in Present.h). With it off, the engine publishes today's fields
 * only and the walk is skipped, so nothing pays for machinery nobody reads. */
extern void TVPSetTextPlaneSplice(bool enabled);
extern bool TVPIsTextPlaneSpliceEnabled();

/* Split view: compose the frame with text-bearing layers left out, so the
 * result is artwork only. OFF by default.
 *
 * A layer is "text-bearing" when its bitmap has received a glyph - stamped by
 * the one funnel every glyph passes through
 * (tTVPNativeBaseBitmap::SetHasText). Skipping those layers at composite time
 * is what makes the two halves of the split independent: the frame becomes
 * artwork-only, while the text arrives separately on the plane seam, and no
 * masking or subtraction is needed.
 *
 * Note what is skipped: the layer the glyphs are drawn INTO (in KAG, the
 * per-line floating layer), not its parent. A message layer's own background -
 * the text box artwork - is still composited, because no glyph is ever drawn
 * into that bitmap. */
extern void TVPSetSplitViewArtworkOnly(bool enabled);
extern bool TVPIsSplitViewArtworkOnly();

/* Announces a text-plane update. `pixels`/`width`/`height`/`pitch` describe the
 * engine's text surface, which is *not* copied and stays valid after the call
 * (unlike the present buffer) because it is the engine's own surface.
 *
 * `changed` is the region written since the previous call, in plane
 * coordinates; pass an empty rect when only `restart` or the origin changed.
 * `restart` says the plane was cleared and everything on it is new.
 *
 * `plane_x`/`plane_y` are where this surface sits in the window. They are
 * consulted only while splice mode is on (see TVPSetTextPlaneSplice); a caller
 * that has not enabled it may pass 0. */
extern void TVPPresentTextPlane(void * pixels, tjs_int width, tjs_int height,
	tjs_int pitch, const tTVPRect & changed, tjs_int origin_x, tjs_int origin_y,
	bool restart, tjs_int plane_x = 0, tjs_int plane_y = 0);

/*---------------------------------------------------------------------------*/
/* The text run stream, engine-internal side.
 *
 * See the long note on tTVPTextRun in Present.h. The caller is the one place
 * every layer text draw passes through (tTJSNI_BaseLayer::DrawText), so this
 * stays one observation point like the plane funnel above.
 */
/*---------------------------------------------------------------------------*/

/* True if a host is listening for text runs. The drawing code checks this so a
 * host that does not want them pays nothing - not even the parent-chain walk
 * that resolves WindowX/WindowY. */
extern bool TVPIsTextRunCallbackSet();

/* Announces one text run: the characters a draw call carried, where it was
 * asked to draw them, and with what style.
 *
 * `text`/`length` are UTF-16 code units and are NOT retained - the caller's
 * string may be reused the moment this returns, so Present.cpp copies before
 * handing anything to the host. */
extern void TVPPresentTextRun(const tjs_char * text, tjs_int length,
	tjs_int x, tjs_int y,
	tjs_uint32 color, tjs_int opacity, bool antialias,
	tjs_int shadow_level, tjs_uint32 shadow_color,
	tjs_int shadow_ofs_x, tjs_int shadow_ofs_y);

#endif /* __TVP_PRESENT_INTF_H__ */
