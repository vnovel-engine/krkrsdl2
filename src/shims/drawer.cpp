//---------------------------------------------------------------------------
// shims/drawer.cpp — Fate/stay night's `drawer.dll`, replaced.
//
// Not stock KiriKiri (absent from the upstream tree); ships with Fate.
// Registers three global TJS functions that rasterise into a Layer's own pixel
// buffer, reached through its TJS properties (imageWidth, imageHeight,
// mainImageBufferForWrite, mainImageBufferPitch):
//
//     drawLine(layer, x1, y1, x2, y2, color)              aliased line
//     drawAALine(layer, x1, y1, x2, y2, color)            anti-aliased line
//     drawAATriangle(layer, x1, y1, x2, y2, x3, y3, color) anti-aliased triangle
//
// One source, two consumers: see shim_abi.h. Built into the engine the three
// names are reached through `@builtin:drawer`; built as a PE it is the plugin
// the game links.
//
// Reconstructed from the shipped DLL (not from source). Ghidra function names
// and addresses are cited where a non-obvious line derives from one:
//
//   FUN_10001000  read the Layer surface through PropGet
//   FUN_10002c50  publish the surface to the module's clip globals
//   FUN_10002b90  clip a segment to the clip rect (0=inside, 1=clipped, -1=rejected)
//   FUN_10002c90  aliased line: Bresenham, flat colour, no blending
//   FUN_10002e90  anti-aliased line: two complementary-coverage pixels a step
//   FUN_100022b0  anti-aliased edge for the triangle: one pixel a step
//   FUN_10002840  anti-aliased triangle
//
// Pixel-surface contract (stated once, in the style of EngineSurface and
// VNovelTextPlane):
//
//   * 32bpp ARGB: one little-endian word a pixel (bytes B,G,R,A), 0x00RRGGBB
//     with alpha in bits 24-31. The original packs blended pixels with
//     `or ecx,0xffffff00` forcing bits 24-31 to 0xff (0x1000246e); the engine's
//     own GrayToColorFunctor packs 0xff000000 | r<<16 | g<<8 | b the same way.
//   * rows are addressed by a BYTE pitch. mainImageBufferPitch maps to
//     Layer::GetMainImagePixelBufferPitch() -> GetPitchBytes()
//     (visual/LayerIntf.cpp:2688). Rows are padded: never assume width * 4.
//   * that pitch is SIGNED and reports a bottom-up bitmap. GetScanLine(0) is
//     the physically lowest row; image row y lives at
//     (Height - y - 1) * PitchBytes + Bits (LayerBitmapImpl.cpp:528), so from
//     the base pointer row y is at base + y * pitch with pitch as reported.
//     Walking upward in memory walks down the image (the visual y direction).
//     The sign carries no information beyond "this is a bottom-up DIB", which
//     the base pointer already encodes.
//   * every write is clipped to [0,width) x [0,height). The original did NOT
//     always do this — see the triangle note, where an off-layer vertex walks
//     its scratch off the end in the shipped DLL.

// One source, two consumers: see shim_abi.h.

// The shim's registry name. shim_abi.h turns this into the per-shim entry point
// symbols when built into the engine; see that header for why.
#define SHIM_NAME drawer
#include "shim_abi.h"

#include "tp_stub.h"

#include <new>
#include <stdio.h>

namespace {

//---------------------------------------------------------------------------
// TJS error codes (values from tjsErrorDefs.h).
const tjs_error kESucceed = TJS_S_OK;      // 0
const tjs_error kEBadParamCount = -1004;   // TJS_E_BADPARAMCOUNT (0xfffffc14)

//---------------------------------------------------------------------------
// Property and function names, constructed once (function-local static).
struct Keys {
	ttstr imageWidth, imageHeight, mainImageBufferForWrite, mainImageBufferPitch;
	ttstr drawLine, drawAALine, drawAATriangle;

	Keys()
		: imageWidth(TJS_W("imageWidth")),
		  imageHeight(TJS_W("imageHeight")),
		  mainImageBufferForWrite(TJS_W("mainImageBufferForWrite")),
		  mainImageBufferPitch(TJS_W("mainImageBufferPitch")),
		  drawLine(TJS_W("drawLine")),
		  drawAALine(TJS_W("drawAALine")),
		  drawAATriangle(TJS_W("drawAATriangle"))
	{
	}
};

inline Keys &K()
{
	static Keys keys;
	return keys;
}

//---------------------------------------------------------------------------
// The layer surface: `base` is the address of visual row 0, `pitchBytes` is
// the engine's SIGNED byte stride (the sign is the row direction — see the
// pixel-surface contract at the top of the file). The two together are the
// whole contract; nothing else about the bitmap layout is assumed. Row() is
// the only place that turns (x, y) into an address.
struct Surface {
	tjs_uint8 *base;
	tjs_int width;
	tjs_int height;
	tjs_int pitchBytes;

	// Row `y` as a word pointer, one positive stride further on each row. The
	// stride is applied in bytes, never in words: rows are padded, so the
	// distance between rows is the pitch and not width * 4.
	tjs_uint32 *Row(tjs_int y) const
	{
		/* Signed pitch: larger y is a lower address. See BindSurface. */
		return (tjs_uint32 *)(base + (tjs_intptr_t)y * pitchBytes);
	}
};

//---------------------------------------------------------------------------
// Fetch the surface through the Layer's own properties (FUN_10001000).
// Returns false on a non-object, a missing property, a null buffer, or unusable
// geometry. The original raised "invoking of Layer.<member> failed." on a
// missing member; here the shim declines to draw instead, so a non-Layer gets
// an undefined drawing rather than a thrown exception.
bool BindSurface(Surface &s, iTJSDispatch2 *layer)
{
	if (!layer) return false;

	Keys &k = K();
	tTJSVariant val;

	if (TJS_FAILED(layer->PropGet(0, k.imageWidth.c_str(), k.imageWidth.GetHint(), &val, layer)))
		return false;
	const tjs_int width = (tjs_int)(tTVInteger)val;

	if (TJS_FAILED(layer->PropGet(0, k.imageHeight.c_str(), k.imageHeight.GetHint(), &val, layer)))
		return false;
	const tjs_int height = (tjs_int)(tTVInteger)val;

	if (TJS_FAILED(layer->PropGet(0, k.mainImageBufferForWrite.c_str(), k.mainImageBufferForWrite.GetHint(), &val, layer)))
		return false;
	// The engine hands the pointer over as a tTVInteger
	// (LayerIntf.cpp:10379: (tTVInteger)reinterpret_cast<tjs_intptr_t>(...)),
	// so it comes back through the same two steps.
	tjs_uint8 *base = (tjs_uint8 *)(tjs_intptr_t)(tTVInteger)val;

	if (TJS_FAILED(layer->PropGet(0, k.mainImageBufferPitch.c_str(), k.mainImageBufferPitch.GetHint(), &val, layer)))
		return false;
	// Signed BYTES a row; see the pixel-surface contract at the top.
	const tjs_int pitch = (tjs_int)(tTVInteger)val;

	if (!base || width <= 0 || height <= 0) return false;
	// Before width * 4 below can be formed: a Layer is free to report any int,
	// and the multiplication is undefined on overflow.
	if (width > 0x1fffffff) return false;

	// A row of 32bpp pixels needs at least width * 4 bytes of stride, and the
	// engine always provides it (the pitch is the row width rounded up to a
	// 16-byte paragraph, LayerBitmapImpl / BitmapInfomation). A Layer reporting
	// less is describing a buffer this code cannot address safely, so decline
	// rather than run off the end of an allocation the engine owns.
	const tjs_int absPitch = pitch < 0 ? -pitch : pitch;
	if (absPitch < width * 4) return false;

	// The stride keeps its sign: image row y lives at base + y * pitch with
	// pitch as reported (an 800x600 Layer reports -3200). Taking only the
	// magnitude writes every row the wrong way and is silent — the call
	// returns success and draws nothing. The shipped plugin folded the sign
	// into its row offsets the same way (FUN_10002840, 0x10002dd9).
	s.pitchBytes = pitch;
	s.base = base;
	s.width = width;
	s.height = height;
	return true;
}

//---------------------------------------------------------------------------
// Bounds check and write. Every write goes through PutPixel or BlendPixel,
// which is what makes the clipping claim at the top of the file true.
inline bool InBounds(const Surface &s, tjs_int x, tjs_int y)
{
	return x >= 0 && y >= 0 && x < s.width && y < s.height;
}

inline void PutPixel(const Surface &s, tjs_int x, tjs_int y, tjs_uint32 color)
{
	if (!InBounds(s, x, y)) return;
	s.Row(y)[x] = color;
}

//---------------------------------------------------------------------------
// 16.16 fixed point, spelled out rather than left to the compiler.
//
// The original scales with `shl` and truncates with `sar` (e.g. 0x10002319 for
// the scale and 0x10002407 for the truncation), and those machine operations are
// well defined: the shift of a negative value keeps its sign and rounds towards
// minus infinity. C++ promises neither for `<<` / `>>` on a signed value — a
// left shift of a negative is undefined, and a right shift is only
// implementation-defined — and a coordinate here can be negative, because the
// triangle does not clip its vertices. So both operations are written as the
// arithmetic they mean, which is also what the sanitizers check.
//---------------------------------------------------------------------------
inline tjs_int64 Fix16FromInt(tjs_int v)      // v << 16, the well-defined way
{
	return (tjs_int64)v * 65536;
}

inline tjs_int Fix16High(tjs_int64 v)          // v >> 16, arithmetic (floor)
{
	// Shifting the unsigned image right is a logical shift, which for a negative
	// value is exactly floor(v / 65536); the narrowing cast then keeps the low
	// 32 bits, which is the value the original's `sar` leaves in a register.
	return (tjs_int)((tjs_uint64)v >> 16);
}

inline tjs_int Fix16Frac(tjs_int64 v)          // v's low 16 bits
{
	// The original's `and eax,0xffff` on the scaled coordinate. Done through the
	// unsigned image for the same reason as above: the low bits of a negative
	// value are two's complement, which C++ only guarantees in the unsigned type.
	return (tjs_int)((tjs_uint64)v & 0xffff);
}

//---------------------------------------------------------------------------
// The blend the original performs for one anti-aliased pixel.
//
// Per channel, arithmetically (0x10002421 onwards for red, and the same shape
// for green at 0x10002431 and blue at 0x10002435):
//
//     out = ((dst - src) * frac) >> 16) + src
//
// with `frac` the low 16 bits of the pixel's 16.16 coordinate and `>>` an
// arithmetic shift (the original's `sar`). So frac = 0 leaves the destination
// exactly as it was and frac = 0xffff is effectively the source: a lerp from
// destination to source, which is exactly the coverage weight of an
// anti-aliased cell.
//
// Implemented directly: the engine's compositors take a bitmap/rect pair, not
// a per-pixel weight.
//
// Alpha is forced opaque: the decompiled expression ORs 0xffffff00 into the red
// channel before the two left shifts that assemble the word (0x1000243c), which
// leaves 0xff in bits 24-31 of the result. Anti-aliased pixels are therefore
// always written with alpha 255, whatever the caller's colour alpha was. The
// flat-colour writers (the aliased line and the triangle's interior fill) do
// NOT do this — the original stores the script's word unchanged (0x10002d45,
// 0x100029c9) — so the two disagree about alpha exactly as they did in the
// shipped DLL. That is reproduced deliberately; it is a property of the plugin,
// and "fixing" it would change what the scripts see.
//---------------------------------------------------------------------------
inline tjs_int LerpChannel(tjs_int dst, tjs_int src, tjs_int frac)
{
	// (dst - src) * frac fits int32 easily: |dst - src| <= 255, frac < 65536.
	// The shift is arithmetic, as the original's `sar` is; every compiler this
	// engine targets does that for a signed value.
	return src + (((dst - src) * frac) >> 16);
}

inline tjs_uint32 BlendARGB(tjs_uint32 dst, tjs_uint32 src, tjs_int frac)
{
	const tjs_int r = LerpChannel((tjs_int)((dst >> 16) & 0xff), (tjs_int)((src >> 16) & 0xff), frac);
	const tjs_int g = LerpChannel((tjs_int)((dst >> 8) & 0xff), (tjs_int)((src >> 8) & 0xff), frac);
	const tjs_int b = LerpChannel((tjs_int)(dst & 0xff), (tjs_int)(src & 0xff), frac);
	return 0xff000000u | ((tjs_uint32)r << 16) | ((tjs_uint32)g << 8) | (tjs_uint32)b;
}

inline void BlendPixel(const Surface &s, tjs_int x, tjs_int y, tjs_uint32 color, tjs_int frac)
{
	if (!InBounds(s, x, y)) return;
	tjs_uint32 *p = &s.Row(y)[x];
	*p = BlendARGB(*p, color, frac);
}

//---------------------------------------------------------------------------
// Clip a segment to the Layer rect (FUN_10002b90 over FUN_100029f0 /
// FUN_10002a40).
//
// The original encodes each endpoint as a four-bit mask — bit 0 "x < left",
// bit 1 "x > right", bit 2 "y < top", bit 3 "y > bottom" (FUN_100029f0,
// 0x10002c50 publishes left/top = 0 and right = width-1, bottom = height-1 into
// the clip globals) — and then:
//
//   * both endpoints inside  -> 0, coordinates untouched, caller draws;
//   * both outside on the same side (mask1 & mask2) -> -1, caller skips;
//   * otherwise it moves each outside endpoint onto the crossed edge, solving
//     the parametric equation along the segment, e.g. for the left edge
//     x = left:  y = (left - x1) * (y2 - y1) / (x2 - x1) + y1  (FUN_10002a40),
//     keeping the endpoint only if the crossing lands within the rect, else -1.
//
// Reproduced in that shape rather than as a clamp on the writes, because the
// anti-aliased line computes its weights from the clipped endpoints:
// re-anchoring them changes the sub-pixel phase, and therefore the exact
// coverage, of every pixel of a line that pokes outside the Layer. Writes are
// clipped as well.
//
// Divergence worth naming: the original's crossing arithmetic is 32-bit and
// truncating, and it divides by (x2 - x1) / (y2 - y1) unguarded. The
// axis-aligned cases that would divide by zero are unreachable, because a
// vertical segment puts both endpoints in the same x-band (so mask1 & mask2 is
// non-zero and the function has already returned), but the products are
// evaluated in 64 bits here so a pathological coordinate cannot wrap.
//---------------------------------------------------------------------------
const int kClipLeft = 1, kClipRight = 2, kClipTop = 4, kClipBottom = 8;

inline int ClipMask(const Surface &s, tjs_int x, tjs_int y)
{
	int m = 0;
	if (x < 0) m |= kClipLeft;
	else if (x > s.width - 1) m |= kClipRight;
	if (y < 0) m |= kClipTop;
	else if (y > s.height - 1) m |= kClipBottom;
	return m;
}

// Move one endpoint onto the edge `mask` says it is beyond, if the crossing
// lands inside the rect. Returns false when the segment misses the rect
// entirely, in which case the caller draws nothing.
bool ClipEdge(int mask, tjs_int &x1, tjs_int &y1, tjs_int x2, tjs_int y2, const Surface &s)
{
	const tjs_int right = s.width - 1, bottom = s.height - 1;

	if (mask & kClipLeft)
	{
		if (x2 == x1) return false;
		const tjs_int y = (tjs_int)((((tjs_int64)0 - x1) * (tjs_int64)(y2 - y1)) / (x2 - x1)) + y1;
		if (y < 0 || y > bottom) return false;
		x1 = 0; y1 = y;
		return true;
	}
	if (mask & kClipRight)
	{
		if (x2 == x1) return false;
		const tjs_int y = (tjs_int)((((tjs_int64)right - x1) * (tjs_int64)(y2 - y1)) / (x2 - x1)) + y1;
		if (y < 0 || y > bottom) return false;
		x1 = right; y1 = y;
		return true;
	}
	if (mask & kClipTop)
	{
		if (y2 == y1) return false;
		const tjs_int x = (tjs_int)((((tjs_int64)0 - y1) * (tjs_int64)(x2 - x1)) / (y2 - y1)) + x1;
		if (x < 0 || x > right) return false;
		x1 = x; y1 = 0;
		return true;
	}
	if (mask & kClipBottom)
	{
		if (y2 == y1) return false;
		const tjs_int x = (tjs_int)((((tjs_int64)bottom - y1) * (tjs_int64)(x2 - x1)) / (y2 - y1)) + x1;
		if (x < 0 || x > right) return false;
		x1 = x; y1 = bottom;
		return true;
	}
	return true;
}

bool ClipSegment(const Surface &s, tjs_int &x1, tjs_int &y1, tjs_int &x2, tjs_int &y2)
{
	const int m1 = ClipMask(s, x1, y1);
	const int m2 = ClipMask(s, x2, y2);
	if ((m1 == 0) && (m2 == 0)) return true;    // both inside: nothing to do
	if ((m1 & m2) != 0) return false;           // both beyond the same edge
	if ((m1 != 0) && !ClipEdge(m1, x1, y1, x2, y2, s)) return false;
	if ((m2 != 0) && !ClipEdge(m2, x2, y2, x1, y1, s)) return false;
	return true;
}

//---------------------------------------------------------------------------
// Aliased line (FUN_10002c90).
//
// Bresenham on the major axis, one pixel a step, flat colour: no blending and
// no alpha work at all — the word the script passed is stored as-is
// (0x10002d45, and the same in the second half of the function).
//
// The original walks from both endpoints towards the middle — two pixels an
// iteration — then draws the odd leftover pixel (the `(major + 1) % 2` test
// at 0x10002d9b). The union of the two half-walks is the same pixel set as
// the single forward walk below. The error term is the original's:
// initialised to -major, closed by 2*minor each step, re-opened by -2*major
// on a minor step (0x10002d01).
//---------------------------------------------------------------------------
void RasterAliasedLine(const Surface &s, tjs_int x1, tjs_int y1, tjs_int x2, tjs_int y2, tjs_uint32 color)
{
	const tjs_int dx = x2 - x1, dy = y2 - y1;
	const tjs_int adx = dx < 0 ? -dx : dx;
	const tjs_int ady = dy < 0 ? -dy : dy;
	const tjs_int major = adx >= ady ? adx : ady;
	// Step directions: the original derives each as
	// ((target <= start) - 1 & 2) - 1 (0x10002c9b..0x10002ca6), i.e. +1 when
	// start < target and -1 otherwise.
	const tjs_int sx = dx < 0 ? -1 : 1;
	const tjs_int sy = dy < 0 ? -1 : 1;

	tjs_int err = -major;
	tjs_int x = x1, y = y1;

	if (adx >= ady)
	{
		for (tjs_int i = 0; i <= major; i++)
		{
			PutPixel(s, x, y, color);
			err += 2 * ady;
			if (err >= 0) { y += sy; err -= 2 * major; }
			x += sx;
		}
	}
	else
	{
		for (tjs_int i = 0; i <= major; i++)
		{
			PutPixel(s, x, y, color);
			err += 2 * adx;
			if (err >= 0) { x += sx; err -= 2 * major; }
			y += sy;
		}
	}
}

//---------------------------------------------------------------------------
// Anti-aliased line (FUN_10002e90).
//
// X major (|dx| >= |dy|), which is the original's second half and the one the
// disassembly at 0x10002f74 shows directly: the column steps by +-1 (0x10002f12:
// `sar edi,0x1f; or edi,1`), the row advances by the integer slope
// (y2 - y1) / |dx| in 16.16 (0x10002f03: `idiv` by the step count), and each
// step writes TWO pixels, one word a step apart — the row's own word and the one
// at +pitchPx:
//
//     word = pitchPx * row + column
//     pixel[word]             = blend(..., frac)
//     pixel[word + pitchPx]   = blend(..., 1 - frac)
//
// (0x10002fdc and 0x10003042 are the two stores of Y major — adjacent words,
// so adjacent columns, the row fixed and the two weights complementary; the X
// major arm is the same shape a pitch apart. A small detail that falls out of
// the same code: `or edi,0xffffff00` just before each store, 0x10002fa7, is the
// alpha forcing described at BlendARGB.)
//
// with `frac` the minor coordinate's low 16 bits. Complementary weights — frac
// and 1 - frac — are what makes the pair the two cells of one coverage
// footprint.
//
// The step direction for the major axis is the original's sign idiom
// (0x10002f12: `sar edi,0x1f; or edi,1`), and the minor step is the integer
// slope in 16.16 obtained with a signed divide (0x10002f03: `idiv`) truncated
// towards zero.
//
// The loop runs |major| times, not |major| + 1, so the AA line does not paint
// the far endpoint (0x10002f21). A degenerate segment is forced to one step
// (0x10002efb) so a zero-length line still plots its single covered cell. The
// step arithmetic is 64-bit here where the original is 32-bit, so a coordinate
// beyond +-32767 cannot overflow the 16.16 fixed point; within a screen-sized
// Layer the values are identical.
//---------------------------------------------------------------------------
void RasterAALine(const Surface &s, tjs_int x1, tjs_int y1, tjs_int x2, tjs_int y2, tjs_uint32 color)
{
	const tjs_int dx = x2 - x1, dy = y2 - y1;
	const tjs_int adx = dx < 0 ? -dx : dx;
	const tjs_int ady = dy < 0 ? -dy : dy;

	if (adx >= ady)
	{
		const tjs_int count = adx != 0 ? adx : 1;
		const tjs_int sx = dx < 0 ? -1 : 1;
		const tjs_int64 step = (Fix16FromInt(dy)) / count;   // idiv: truncates
		tjs_int64 fix = Fix16FromInt(y1);
		tjs_int x = x1;
		for (tjs_int i = 0; i < count; i++)
		{
			const tjs_int y = Fix16High(fix);                  // sar: floors
			const tjs_int frac = Fix16Frac(fix);
			BlendPixel(s, x, y, color, frac);
			BlendPixel(s, x, y + 1, color, 0xffff - frac);
			fix += step;
			x += sx;
		}
	}
	else
	{
		const tjs_int count = ady != 0 ? ady : 1;
		const tjs_int sy = dy < 0 ? -1 : 1;
		const tjs_int64 step = Fix16FromInt(dx) / count;
		tjs_int64 fix = Fix16FromInt(x1);
		tjs_int y = y1;
		for (tjs_int i = 0; i < count; i++)
		{
			const tjs_int x = Fix16High(fix);
			const tjs_int frac = Fix16Frac(fix);
			BlendPixel(s, x, y, color, frac);
			BlendPixel(s, x + 1, y, color, 0xffff - frac);
			fix += step;
			y += sy;
		}
	}
}

//---------------------------------------------------------------------------
// One anti-aliased EDGE of the triangle (FUN_100022b0).
//
// Same footprint as the AA line — two cells along the minor axis with weights
// frac and 1 - frac — but only ONE of the two is written per step: the OUTSIDE
// cell, the one on the far side of the edge from the centroid. The other cell is
// the interior one, and it is not painted here: its column is recorded in the
// triangle's scratch so the span fill at the end of RasterAATriangle covers it,
// which is what keeps the interior flat and the boundary anti-aliased.
//
// WHICH side that is does not change during the walk: it is a property of the
// edge and the centroid alone. The original computes it before its loop, as an
// interpolation of the edge at the centre point's other coordinate:
//
//   x major (|dx| >= |dy|), slope = (y2 - y1) / |dx| in 16.16:
//       yAtCx = y1 + |cx - x1| * slope        (0x10002364: `sub eax,edi` takes
//                                              the magnitude, then `imul`)
//       cy < yAtCx  -> the edge is below the centre, so the interior is above:
//                      paint (x, y+1) with 1 - frac, record row y, column x
//       otherwise   -> paint (x, y)   with frac,     record row y+1, column x
//                      (0x10002389 and 0x100024c6 are the two loops)
//
//   y major, slope = (x2 - x1) / |dy| in 16.16:
//       xAtCy = x1 + |cy - y1| * slope        (0x10002364's mirror)
//       xAtCy < cx  -> the edge is left of the centre, so the interior is right:
//                      paint (x, y)   with frac,     record row y, column x+1
//       otherwise   -> paint (x+1, y) with 1 - frac, record row y, column x
//
// The recorded column is the interior cell's, which is what bounds the span fill
// on that row. The row recorded for an x-major edge is the interior row, which is
// y or y+1 — the row the edge did not paint. Both are clipped by Rec::Col, so an
// edge that strays outside the Layer cannot record out of range.
//
// The side-selection predicate is loop-invariant (depends only on the call's
// arguments); MSVC emitted it as two separate loops per arm.
//---------------------------------------------------------------------------
void RasterAAEdge(const Surface &s, tjs_int x1, tjs_int y1, tjs_int x2, tjs_int y2,
	tjs_int cx, tjs_int cy, tjs_uint32 color, tjs_int *spanMin, tjs_int *spanMax)
{
	const tjs_int dx = x2 - x1, dy = y2 - y1;
	const tjs_int adx = dx < 0 ? -dx : dx;
	const tjs_int ady = dy < 0 ? -dy : dy;

	// Record the interior cell's column for one row of the scratch. The arrays
	// are seeded by the caller and narrowed here: min keeps the smallest column,
	// max the largest (0x10002475 / 0x10002481: `if (bound < x) bound = x` and
	// `if (x < bound) bound = x`).
	struct Rec {
		static void Col(const Surface &s, tjs_int *spanMin, tjs_int *spanMax, tjs_int row, tjs_int col)
		{
			if (row < 0 || row >= s.height) return;
			if (col < spanMin[row]) spanMin[row] = col;
			if (col > spanMax[row]) spanMax[row] = col;
		}
	};

	if (adx >= ady)
	{
		// X major: the column steps by +-1 (the original's `or ... ,1` sign at
		// 0x1000234d), the row advances by the slope, and the candidate cells are
		// the row's own and the row below it.
		const tjs_int count = adx != 0 ? adx : 1;
		const tjs_int sx = dx < 0 ? -1 : 1;
		const tjs_int64 step = Fix16FromInt(dy) / count;
		// The edge's row at the centre's column, truncated as the original's
		// `imul` / `sar` pair does.
		const tjs_int yAtCx = y1 + Fix16High((tjs_int64)(cx - x1 < 0 ? x1 - cx : cx - x1) * step);

		tjs_int64 fix = Fix16FromInt(y1);
		tjs_int x = x1;
		if (cy < yAtCx)
		{
			// Edge below the centre: interior above, so paint the lower cell and
			// record the upper row.
			for (tjs_int i = 0; i < count; i++)
			{
				const tjs_int y = Fix16High(fix);
				const tjs_int frac = Fix16Frac(fix);
				BlendPixel(s, x, y + 1, color, 0xffff - frac);
				Rec::Col(s, spanMin, spanMax, y, x);
				fix += step;
				x += sx;
			}
		}
		else
		{
			for (tjs_int i = 0; i < count; i++)
			{
				const tjs_int y = Fix16High(fix);
				const tjs_int frac = Fix16Frac(fix);
				BlendPixel(s, x, y, color, frac);
				Rec::Col(s, spanMin, spanMax, y + 1, x);
				fix += step;
				x += sx;
			}
		}
	}
	else
	{
		// Y major: the row steps, the column advances by the slope, and the
		// candidate cells are the column's own and the next column.
		const tjs_int count = ady != 0 ? ady : 1;
		const tjs_int sy = dy < 0 ? -1 : 1;
		const tjs_int64 step = Fix16FromInt(dx) / count;
		const tjs_int xAtCy = x1 + Fix16High((tjs_int64)(cy - y1 < 0 ? y1 - cy : cy - y1) * step);

		tjs_int64 fix = Fix16FromInt(x1);
		tjs_int y = y1;
		if (xAtCy < cx)
		{
			// Edge left of the centre: interior to the right, so paint the left
			// cell and record the column to its right.
			for (tjs_int i = 0; i < count; i++)
			{
				const tjs_int x = Fix16High(fix);
				const tjs_int frac = Fix16Frac(fix);
				BlendPixel(s, x, y, color, frac);
				Rec::Col(s, spanMin, spanMax, y, x + 1);
				fix += step;
				y += sy;
			}
		}
		else
		{
			for (tjs_int i = 0; i < count; i++)
			{
				const tjs_int x = Fix16High(fix);
				const tjs_int frac = Fix16Frac(fix);
				BlendPixel(s, x + 1, y, color, 0xffff - frac);
				Rec::Col(s, spanMin, spanMax, y, x);
				fix += step;
				y += sy;
			}
		}
	}
}

//---------------------------------------------------------------------------
// Anti-aliased triangle (FUN_10002840).
//
// The shape of the original, in its order:
//
//   1. find the row band of the three vertices and seed a per-row scratch over
//      it with an empty column span — the two arrays the function reads from its
//      own state at +0 and +4, the min one seeded to the layer width minus one
//      and the max one to zero (0x100028a2..0x100028bb);
//   2. compute the centroid trivially: (x1+x2+x3)/3 and (y1+y2+y3)/3, truncated
//      — the `0x55555556; imul` pairs at 0x100028cd and 0x100028e9 are the
//      compiler's divide-by-three (0x10002865: `iVar7 = (param_2 + param_4 +
//      param_6) / 3`);
//   3. run the three edges towards the centroid:
//        (x1,y1)->(x2,y2), (x2,y2)->(x3,y3), (x3,y3)->(x1,y1)
//      (0x10002916, 0x10002938, 0x1000295a), each with the centroid as its
//      centre point, so each paints only the side of the boundary that faces the
//      interior and records the interior column for that row;
//   4. fill each row of the band between the recorded bounds with the flat
//      colour — including the bounds themselves (0x100029c9: `rep stosd` from
//      the min column for (max - min + 1) words), stepping the row pointer by
//      the pitch and clipping the bounds to [0, width-1] (0x100029ab clamps the
//      low bound up, 0x100029b5 the high one down).
//
// Divergences from the original:
//   * the scratch is seeded strictly empty here (min = width, max = -1) rather
//     than width-1 / 0. The original's seeds are equivalent for every column
//     in [0, width-1] except the last one: a triangle touching the right edge
//     would leave the last column's minimum at width-1 and lose that column's
//     fill. No correct triangle is affected by seeding strictly.
//   * the row band is clipped to the Layer before anything is written. The
//     original does not clip it, so a vertex outside the Layer makes its
//     scratch seeding start outside the array — undefined behaviour in the
//     shipped DLL, not reproduced here.
//
// The scratch is two arrays of one int a row; the fill loop indexes them by
// row, so this port sizes them by the Layer's height.
//---------------------------------------------------------------------------
bool RasterAATriangle(const Surface &s, tjs_int x1, tjs_int y1, tjs_int x2, tjs_int y2,
	tjs_int x3, tjs_int y3, tjs_uint32 color)
{
	// The row band of the three vertices, clipped to the Layer. A triangle
	// entirely outside draws nothing.
	tjs_int yTop = y1 < y2 ? y1 : y2;
	if (y3 < yTop) yTop = y3;
	tjs_int yBottom = y1 > y2 ? y1 : y2;
	if (y3 > yBottom) yBottom = y3;
	if (yTop < 0) yTop = 0;
	if (yBottom > s.height - 1) yBottom = s.height - 1;
	if (yTop > yBottom) return true;

	tjs_int *spanMin = new (std::nothrow) tjs_int[s.height];
	tjs_int *spanMax = new (std::nothrow) tjs_int[s.height];
	if (!spanMin || !spanMax)
	{
		delete[] spanMin;
		delete[] spanMax;
		return false;
	}

	// Seed every row, not just the band: an edge can record a row at the band's
	// edge that the band clip did not include, and narrowing against an
	// uninitialised int would be reading indeterminate memory. The band alone is
	// what the fill loop reads, so this costs one pass and no behaviour.
	for (tjs_int row = 0; row < s.height; row++)
	{
		spanMin[row] = s.width;   // wider than any column: no lower bound yet
		spanMax[row] = -1;        // left of any column: no upper bound yet
	}

	// The centroid, truncated towards zero as the original's divides do.
	const tjs_int cx = (x1 + x2 + x3) / 3;
	const tjs_int cy = (y1 + y2 + y3) / 3;

	RasterAAEdge(s, x1, y1, x2, y2, cx, cy, color, spanMin, spanMax);
	RasterAAEdge(s, x2, y2, x3, y3, cx, cy, color, spanMin, spanMax);
	RasterAAEdge(s, x3, y3, x1, y1, cx, cy, color, spanMin, spanMax);

	// Interior: every row's span, flat colour, both bounds inclusive.
	for (tjs_int row = yTop; row <= yBottom; row++)
	{
		tjs_int a = spanMin[row];
		tjs_int b = spanMax[row];
		if (a < 0) a = 0;
		if (b > s.width - 1) b = s.width - 1;
		if (a > b) continue;   // nothing recorded, or clamped to nothing
		tjs_uint32 *dst = s.Row(row);
		for (tjs_int x = a; x <= b; x++) dst[x] = color;
	}

	delete[] spanMin;
	delete[] spanMax;
	return true;
}

//---------------------------------------------------------------------------
// The three TJS functions.
//
// Argument order is the original's: the Layer first, then the coordinates, then
// the colour (all integers; the original converts each parameter with
// tTJSVariant::operator tjs_int, the symbol it resolves at 0x1000a1d4).
//
// Parameter counts are checked before anything is read. The original checks
// drawLine for at least six parameters (0x10001596: `cmp numparams, 6; jge`)
// but only checks drawAALine and drawAATriangle for a non-zero count
// (0x10001726, 0x100018b6), which would leave their later parameter reads
// undefined on a short call. Requiring the full count is stricter and cannot
// reject a correct caller.
//
// A Layer that does not answer the four surface properties returns without
// drawing (see BindSurface). Drawing failures do not throw: the scripts call
// these for their effect, and the effect of a missing surface is "nothing drawn".
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD DrawLineFunc(tTJSVariant * /*result*/, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 * /*objthis*/)
{
	if (numparams < 6) return kEBadParamCount;
	if (!param || !param[0] || param[0]->Type() != tvtObject) return kESucceed;

	tjs_int x1 = (tjs_int)*param[1];
	tjs_int y1 = (tjs_int)*param[2];
	tjs_int x2 = (tjs_int)*param[3];
	tjs_int y2 = (tjs_int)*param[4];
	const tjs_uint32 color = (tjs_uint32)(tjs_int)*param[5];

	Surface s;
	if (!BindSurface(s, param[0]->AsObjectNoAddRef())) return kESucceed;
	if (!ClipSegment(s, x1, y1, x2, y2)) return kESucceed;

	RasterAliasedLine(s, x1, y1, x2, y2, color);
	return kESucceed;
}

tjs_error TJS_INTF_METHOD DrawAALineFunc(tTJSVariant * /*result*/, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 * /*objthis*/)
{
	if (numparams < 6) return kEBadParamCount;
	if (!param || !param[0] || param[0]->Type() != tvtObject) return kESucceed;

	tjs_int x1 = (tjs_int)*param[1];
	tjs_int y1 = (tjs_int)*param[2];
	tjs_int x2 = (tjs_int)*param[3];
	tjs_int y2 = (tjs_int)*param[4];
	const tjs_uint32 color = (tjs_uint32)(tjs_int)*param[5];

	Surface s;
	if (!BindSurface(s, param[0]->AsObjectNoAddRef())) return kESucceed;

	// Same clip as the aliased line: the original runs both entry points through
	// FUN_10002c50 / FUN_10002b90 before drawing (0x10001847, 0x10001860), and
	// so the AA weights are computed from the clipped endpoints.
	if (!ClipSegment(s, x1, y1, x2, y2)) return kESucceed;

	RasterAALine(s, x1, y1, x2, y2, color);
	return kESucceed;
}

tjs_error TJS_INTF_METHOD DrawAATriangleFunc(tTJSVariant * /*result*/, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 * /*objthis*/)
{
	if (numparams < 8) return kEBadParamCount;
	if (!param || !param[0] || param[0]->Type() != tvtObject) return kESucceed;

	const tjs_int x1 = (tjs_int)*param[1];
	const tjs_int y1 = (tjs_int)*param[2];
	const tjs_int x2 = (tjs_int)*param[3];
	const tjs_int y2 = (tjs_int)*param[4];
	const tjs_int x3 = (tjs_int)*param[5];
	const tjs_int y3 = (tjs_int)*param[6];
	const tjs_uint32 color = (tjs_uint32)(tjs_int)*param[7];

	Surface s;
	if (!BindSurface(s, param[0]->AsObjectNoAddRef())) return kESucceed;

	// No segment clip here: the original clips nothing for the triangle and the
	// vertex coordinates go straight into the span band. The band is clipped
	// inside RasterAATriangle, and every write is clipped with it, so a vertex
	// outside the Layer loses the parts of the triangle that fall outside rather
	// than corrupting memory the way the original can.
	if (!RasterAATriangle(s, x1, y1, x2, y2, x3, y3, color)) return TJS_E_FAIL;
	return kESucceed;
}

} // namespace

//---------------------------------------------------------------------------
// V2Link / V2Unlink — the plugin ABI (shim_abi.h). Registration is symmetric
// (V2Unlink removes what V2Link added). extern "C" is stated explicitly: in the
// built-in build SHIM_EXPORT is empty, so without it the symbols would be
// C++-mangled and would not match BuiltinShims.cpp's
// `extern "C" int shim_drawer_V2Link(...)`.
extern "C" {

SHIM_EXPORT SHIM_HIDDEN int V2Link(iTVPFunctionExporter *exporter)
{
	(void)exporter;

	tTJSNativeClassMethod *line = TJSCreateNativeClassMethod(&DrawLineFunc);
	tTJSNativeClassMethod *aaline = TJSCreateNativeClassMethod(&DrawAALineFunc);
	tTJSNativeClassMethod *triangle = TJSCreateNativeClassMethod(&DrawAATriangleFunc);
	if (!line || !aaline || !triangle)
	{
		if (line) line->Release();
		if (aaline) aaline->Release();
		if (triangle) triangle->Release();
		return TJS_E_FAIL;
	}

	bool ok = true;
	Keys &k = K();
	ok = TVPRegisterGlobalObject(k.drawLine.c_str(), line) && ok;
	ok = TVPRegisterGlobalObject(k.drawAALine.c_str(), aaline) && ok;
	ok = TVPRegisterGlobalObject(k.drawAATriangle.c_str(), triangle) && ok;

	// TVPRegisterGlobalObject keeps its own reference in the global object, so
	// the creator's reference is dropped here.
	line->Release();
	aaline->Release();
	triangle->Release();

	return ok ? SHIM_TJS_S_OK : TJS_E_FAIL;
}

SHIM_EXPORT SHIM_HIDDEN int V2Unlink(void)
{
	Keys &k = K();
	TVPRemoveGlobalObject(k.drawLine.c_str());
	TVPRemoveGlobalObject(k.drawAALine.c_str());
	TVPRemoveGlobalObject(k.drawAATriangle.c_str());
	return SHIM_TJS_S_OK;
}

} // extern "C"
