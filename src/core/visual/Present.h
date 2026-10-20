/*---------------------------------------------------------------------------*/
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
/*---------------------------------------------------------------------------*/
/* The presentation seam - host-facing contract.
 *
 * The engine composites layers into a bitmap and then has to get that bitmap
 * onto the screen. On Windows that was DirectDraw; here it is whatever the host
 * provides. This header is the whole of what a host has to know, and it is
 * written in plain C: no engine headers, no host headers, no SDL, no rect
 * class. A host includes this file and nothing else from the engine.
 *
 * Why a new seam rather than the existing one
 * -------------------------------------------
 * The previous arrangement passed an SDL_Surface* across the boundary and had
 * the libretro core read that struct through hardcoded byte offsets. Three
 * problems followed from that:
 *
 *   - SDL's struct layout became part of our ABI. A different SDL build, or a
 *     different platform's padding, silently reinterprets width as height.
 *   - The core had to force SDL_VIDEODRIVER=dummy so that a surface existed at
 *     all, which meant the engine rendered into a dummy-driver window and the
 *     core scraped the result back out of it.
 *   - The upload was a partial SDL_UpdateTexture of a dirty sub-rectangle.
 *     Measured, forcing a full upload recovers some but not all of the missing
 *     content, so this path contributed to the loss rather than accounting for
 *     all of it.
 *
 * The replacement is a plain description of a pixel buffer plus callbacks. The
 * host owns the destination; the engine only says what it produced. The same
 * seam serves the SDL window build, the libretro core, and a batch test that
 * just writes PNGs.
 *
 * Threading
 * ---------
 * The present callback is invoked from the engine's own thread, at the end of a
 * composite. The buffer's pixel pointer stays valid only until the callback
 * returns; a host that needs the pixels later must copy them, not retain the
 * pointer.
 */
/*---------------------------------------------------------------------------*/

#ifndef __TVP_PRESENT_H__
#define __TVP_PRESENT_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*/
/* Pixel format
 *
 * BGRA byte order in memory, 8 bits per channel, no padding between pixels,
 * rows top-down. This is what the engine's bitmaps hold, and what SDL's
 * SDL_PIXELFORMAT_ARGB8888 looks like as bytes on a little-endian machine, so
 * both sides can use it without a conversion pass.
 *
 * Stated here rather than left implicit: the previous seam's format was
 * whatever SDL's surface happened to be and the core had to assume. A host that
 * wants another layout converts inside its own callback, where it can see both
 * sides.
 */
/*---------------------------------------------------------------------------*/

typedef struct tTVPPresentBuffer
{
	/* Pixel data: BGRA, `Pitch` bytes per row, `Height` rows, top-down.
	 * Valid only for the duration of the callback. */
	const void * Pixels;

	/* Geometry, in pixels. */
	int Width;
	int Height;

	/* Bytes from the start of one row to the start of the next. Never assume
	 * Width * 4: the engine pads rows to a 4-pixel boundary, so a 317-pixel
	 * modal has a pitch of 320 * 4. A host that assumes otherwise shears the
	 * image. */
	/* Bytes per row, and it KEEPS ITS SIGN: the engine's bitmaps are
	 * bottom-up, so a negative value is the row direction and not an error.
	 * A host must use it as given rather than taking its magnitude. */
	int Pitch;

	/* The region that changed since the previous present, in buffer
	 * coordinates, top-left origin, width/height rather than an exclusive
	 * right/bottom so a C host needs no convention note. A host may ignore it
	 * and consume the whole buffer.
	 *
	 * This is the union of the regions presented since the last call, so it may
	 * cover unchanged pixels, and it is never empty when the callback fires. */
	int ChangedX;
	int ChangedY;
	int ChangedWidth;
	int ChangedHeight;

	/* When true, treat the entire buffer as changed and ignore the fields
	 * above. Set on the first present, after a resize, and whenever the union
	 * would cover the whole buffer anyway. */
	bool Full;

	/* Increments once per present, so a host can tell "the engine presented
	 * again with identical pixels" from "the engine did not present". Wraps;
	 * compare by subtraction. */
	uint64_t FrameSerial;

	/* The value passed to TVPSetPresentCallback, returned unchanged. */
	void * HostData;
} tTVPPresentBuffer;

/*---------------------------------------------------------------------------*/
/* Host callbacks
 *
 * The present callback runs once per composite, on the engine thread, with a
 * buffer valid until it returns. Return true; the value is reserved.
 */
/*---------------------------------------------------------------------------*/

typedef bool (*tTVPPresentCallback)(const tTVPPresentBuffer * buffer);

/* Installs the host's present callback. Passing NULL detaches, which the engine
 * reads as "present nowhere": the composite still runs, so layer bookkeeping
 * and timing are unaffected, and a headless test can drive the engine with no
 * host at all. Installed before the engine starts, not synchronised against a
 * running composite. */
void TVPSetPresentCallback(tTVPPresentCallback callback, void * hostdata);

/* The engine asks the host how large the surface should be before it builds its
 * primary layer. A host that leaves both values 0 means "you choose" and the
 * engine uses its own default. The libretro core uses this to hand the engine
 * the frontend's expected resolution, so the game's own System.screenWidth and
 * System.screenHeight are right from the first frame rather than corrected
 * after it. */
typedef void (*tTVPQuerySurfaceSizeCallback)(int * width, int * height,
	void * hostdata);

void TVPSetQuerySurfaceSizeCallback(tTVPQuerySurfaceSizeCallback callback,
	void * hostdata);

/* The engine calls this once a composite is finished and it has nothing to do
 * until the host consumes a frame. A host with a real frame clock blocks here;
 * the libretro core blocks on its own frame semaphore. A NULL callback means
 * "do not block", so the engine runs as fast as it can - what a batch test
 * wants, and what the previous slider-style vsync wait degenerated into on a
 * dummy video driver. */
typedef void (*tTVPWaitForPresentCallback)(void * hostdata);

void TVPSetWaitForPresentCallback(tTVPWaitForPresentCallback callback,
	void * hostdata);

/*---------------------------------------------------------------------------*/
/* The text plane
 *
 * A second surface, published alongside the frame, holding only what the engine
 * drew as *text* - the message box's sentences - with the scene underneath
 * excluded. The engine still composites its frame as before; this is an
 * additional output, not a replacement.
 *
 * Why it exists: text is otherwise indistinguishable from artwork once the
 * engine has flattened the frame, so a host cannot reflow it, restyle it, or
 * show it as a page. Three host features need that separation:
 *
 *   - a scrollable reader page instead of text over artwork;
 *   - control over how text composites (position, opacity, veil) beyond what
 *     the game chose;
 *   - full-screen text when the scene shows no artwork at all.
 *
 * The plane is a *page*, not a frame: it persists across presents and grows as
 * sentences are written, while the frame is recomposited each time. A host may
 * retain the pixels (unlike the present buffer, which is valid only for the
 * duration of its callback) and read them again at will; the engine reuses the
 * same allocation and says when it restarts.
 *
 * A host that does not ask for it never sees it: with no callback installed the
 * engine's text goes to the frame exactly as before.
 */
/*---------------------------------------------------------------------------*/

typedef struct tTVPTextPlane
{
	/* Pixel data: BGRA, `Pitch` bytes per row, `Height` rows.
	 *
	 * WHICH ROW `Pixels` POINTS AT DEPENDS ON `Pitch`'s SIGN, and getting this
	 * wrong reads past the end of the allocation. The engine's bitmaps are
	 * bottom-up, so `Pitch` is negative and `Pixels` is the LAST row - the rows
	 * before it live at LOWER addresses. A host must therefore step rows by the
	 * signed pitch, never copy `|Pitch| * Height` bytes forward from `Pixels`:
	 * for a 808x48 plane that reads ~155 KB forward from a pointer that is
	 * already at the end of a 152 KB allocation. (This is exactly the bug the
	 * core's own copy had, found with a core dump inside memcpy.)
	 *
	 * So: for row y, the bytes are `(const unsigned char *)Pixels + y * Pitch`.
	 *
	 * Unlike tTVPPresentBuffer::Pixels this stays valid after the callback
	 * returns: it is the engine's own text surface, and the engine will overwrite
	 * it in place as more text is written. A host that needs a snapshot of a
	 * particular state must copy it. */
	const void * Pixels;

	int Width;
	int Height;

	/* Bytes per row, and it KEEPS ITS SIGN - see Pixels above. Never assume
	 * Width * 4: the engine pads rows. */
	int Pitch;

	/* The region the engine changed since the previous call, in plane
	 * coordinates. Empty (width or height 0) when only `Restart` or the
	 * viewport changed. */
	int ChangedX;
	int ChangedY;
	int ChangedWidth;
	int ChangedHeight;

	/* Where the host should place the plane's visible origin, in plane
	 * coordinates. The engine sets this to its own text origin so a host that
	 * does not implement scrolling can composite the plane 1:1 at the position
	 * the game intended. A scrolling host ignores it and chooses its own.
	 *
	 * NOTE the space: this is a position WITHIN the surface described by
	 * Pixels/Width/Height. It is a pen/destination position, not the surface's
	 * own placement on screen - for that see PlaneX/PlaneY below. */
	int OriginX;
	int OriginY;

	/* Where the SURFACE described by Pixels/Width/Height sits in the window,
	 * in window (primary-layer) pixels. Read-only context: the host never tells
	 * the engine where it drew, it is only told.
	 *
	 * The two engines describe different areas, and this field is what makes a
	 * host able to treat them the same way:
	 *
	 *   - ONScripter/PONScripter publish their whole text surface, which is
	 *     screen-sized, so it IS the page: PlaneX/PlaneY are 0 and OriginX/Y is
	 *     already window-absolute.
	 *   - KiriKiri publishes the line box text is currently accumulating into
	 *     (a 616x40 line-height bitmap, measured), which is a fragment of the
	 *     window: OriginX/Y is the pen position INSIDE that box, and
	 *     PlaneX/PlaneY says where the box itself is.
	 *
	 * So one formula places any publication, on any engine: the surface's own
	 * pixel (px, py) sits at window
	 *
	 *     (PlaneX + px, PlaneY + py)
	 *
	 * which is what a host accumulating a page uses - it blits the CHANGED rect
	 * out of the surface to (PlaneX + ChangedX, PlaneY + ChangedY). (On
	 * KiriKiri OriginX/Y happens to equal ChangedX/Y, because the pen position
	 * and the changed glyph rect are the same thing there; on ONS/PON the two
	 * differ and the CHANGED rect is the one a splice uses.) A host that only
	 * ever presents the latest publication can ignore this field entirely, as
	 * it already ignores the origin. */
	int PlaneX;
	int PlaneY;

	/* True when the engine began a new page: everything previously drawn on the
	 * plane is gone (a scene change, a `reset`, a new chapter). A host showing a
	 * scrollable page returns to the top; one compositing onto the frame can
	 * treat the whole plane as changed. */
	bool Restart;

	/* Increments once per update to the plane. Wraps; compare by subtraction. */
	uint64_t PlaneSerial;

	/* The value passed to TVPSetTextPlaneCallback, returned unchanged. */
	void * HostData;
} tTVPTextPlane;

typedef bool (*tTVPTextPlaneCallback)(const tTVPTextPlane * plane);

/* Installs the host's text-plane callback. Passing NULL detaches, and the
 * engine then draws text into its frame as it always did. Installed before the
 * engine starts, not synchronised against a running composite.
 *
 * Installing this is what enables capture: the engine only maintains the
 * separate surface when a host is listening, so an uninstalled callback costs
 * nothing. */
void TVPSetTextPlaneCallback(tTVPTextPlaneCallback callback, void * hostdata);

/*---------------------------------------------------------------------------*/
/* The text run stream
 *
 * The plane above is PIXELS: a picture of the text, which a host can relocate
 * but not restyle or reflow. This is the other half of the same separation -
 * the text the engine was asked to draw, before it became pixels.
 *
 * Why it is a separate stream rather than a field on the plane: they are
 * different things, produced at different points, and a host may want one
 * without the other. The plane is per-bitmap (it is whatever surface glyphs
 * landed on); this is per *call*, and the call is where the engine still knows
 * the characters, the position it was told to draw them at, and the style it
 * was given. Nothing downstream of that call can recover any of it.
 *
 * KiriKiri/KAG is the motivating case and shows why this is worth having: KAG
 * draws one character per call (`ll.drawText(dx, dy, ch, ...)`, MessageLayer.tjs)
 * into a per-line bitmap, so the run stream IS the script's text with layout
 * already resolved - while the plane is a line box that a host must accumulate
 * and cannot read as text at all.
 *
 * Shape of the data, and what it deliberately does NOT say: a run is what one
 * draw call carried. It is not a line, a page, or an utterance - those are
 * groupings a host builds from positions and order (the engine has no such
 * notion to report). Likewise there is no "this run is the shadow of that one":
 * a call carries its own shadow parameters, and the engine renders both from
 * them.
 */
/*---------------------------------------------------------------------------*/

typedef struct tTVPTextRun
{
	/* The characters this call was asked to draw, as UTF-16 code units.
	 *
	 * NOT NUL-terminated in general, and NOT valid after the callback returns
	 * (it points into a string the engine may reuse). Use TextLength; a host
	 * that intends to keep the text copies it.
	 *
	 * Usually one character on KiriKiri/KAG, but not always - a ruby run or a
	 * pre-composed string arrives whole - so a host must not assume length 1. */
	const uint16_t * Text;
	int TextLength;

	/* Where the text was drawn, in LAYER-LOCAL pixels: exactly the x/y the draw
	 * call was given. Kept as given rather than normalised, because the meaning
	 * depends on the engine's own text metrics (a KAG glyph's y is the top of
	 * the glyph, not a baseline) and inventing a convention here would be a
	 * guess the host cannot check against.
	 *
	 * There is deliberately NO window-resolved position. The point of this
	 * stream is to re-lay-out the text (reflow, totalfit, Knuth-Plass), and a
	 * consumer that does that derives its own placement - the engine's, already
	 * resolved through the layer tree, is at best a hint about where the engine
	 * decided to put things. Reporting it would also force a parent-chain walk
	 * per glyph, which is the one part of this path that costs anything. A
	 * consumer that wants "where did the engine put it" has the plane, which is
	 * that answer already rendered. */
	int X;
	int Y;

	/* Style, as given to the draw call. Color is 0xRRGGBB (the engine's alpha
	 * rides in Opacity, not here). ShadowLevel 0 means no shadow - the shadow
	 * fields are then meaningless. */
	uint32_t Color;
	int Opacity;
	bool Antialias;
	int ShadowLevel;
	uint32_t ShadowColor;
	int ShadowOffsetX;
	int ShadowOffsetY;

	/* Increments once per run, so a host can tell "the engine drew the same text
	 * again" from "the engine did not draw". Wraps; compare by subtraction. */
	uint64_t RunSerial;

	/* The value passed to TVPSetTextRunCallback, returned unchanged. */
	void * HostData;
} tTVPTextRun;

typedef bool (*tTVPTextRunCallback)(const tTVPTextRun * run);

/* Installs the host's text-run callback. Passing NULL detaches, and the engine
 * then only draws the text as it always did. Installed before the engine
 * starts, not synchronised against a running draw.
 *
 * Like the plane callback, installing this is what enables the work: with no
 * host listening the engine does not even resolve the window position, so an
 * uninstalled callback costs nothing. */
void TVPSetTextRunCallback(tTVPTextRunCallback callback, void * hostdata);

/*---------------------------------------------------------------------------*/
/* The engine-internal half of this seam lives in PresentIntf.h, which is where
 * the drawing code reaches these callbacks from. It is a separate file so that
 * this one stays free of engine types. */
/*---------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif /* __TVP_PRESENT_H__ */
