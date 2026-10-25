//---------------------------------------------------------------------------
// shims/slideopen.cpp — Fate/stay night's `slideopen.dll`, replaced.
//
// Not stock KiriKiri (absent from the upstream tree); ships with Fate.
// Registers three global TJS functions that drive a "slide open / slide close"
// panel transition directly between two Layer pixel buffers:
//
//     initSlideOpen(obj)     obj is the SlideOpenPlugin TJS object; reads its
//                            dest, src, type, count, time, accel, open members
//                            and (re)initialises module state.
//     drawSlideOpen(per)     per is 0.0..1.0 progress; composite the slide
//                            transition at that progress into dest from src.
//     finishSlideOpen()      draw the final state and release state.
//
// The plugin's own natives were reached through vtables at 0x10001c70
// (init), 0x100020c0 (draw), 0x100023f0 (finish); the geometry is recovered
// from disassembling those three, plus the four raster primitives they
// dispatch to (FUN_10001000 / 100011b0 / 100013d0 / 100016c0).
//
// One source, two consumers: see shim_abi.h. Built into the engine the three
// names are reached through `@builtin:slideopen`; built as a PE it is the
// plugin the game links.
//
// State contract (BuiltinShims.h): everything the effect owns lives in this
// translation unit as a single struct. No engine global is assigned. What is
// registered is unregistered by V2Unlink.
//
// Pixel-surface contract (see drawer.cpp for the full derivation):
//   * 32bpp ARGB: one little-endian word a pixel.
//   * rows are addressed by a SIGNED BYTE pitch — mainImageBufferPitch maps to
//     Layer::GetPitchBytes() (visual/LayerIntf.cpp:2688). The sign IS the row
//     direction: GetScanLineForWrite(0) is the physically lowest row of a
//     bottom-up DIB, so image row y lives at base + y*pitch with the pitch as
//     reported. Keeping the sign is what makes row addressing correct.
//   * rows are padded: never assume width * 4.
//
// Recovered geometry
// drawSlideOpen (0x100020c0) computes a displacement and dispatches on `type`
// through a jump table at 0x10002310:
//
//   off  = (tjs_int)(W/2 * per + 0.5)              (0x1000215a..0x10002176)
//          When `open` is false, per is inverted first: per = 1.0 - per.
//   off2 = (tjs_int)(off * 1.75)                   (0x100021a2/0x100021bb,
//                                                    used only for types 2/3)
//          The constant at 0x1000a240 is 1.75. This port multiplies by 2.0
//          instead, so the diagonal split travels further; see CHANGELOG.
//
//   type 0 -> FUN_10001000(off,  W, H)   horizontal split
//   type 1 -> FUN_100011b0(off,  W, H)   vertical split
//   type 2 -> FUN_100013d0(off2, W, H)   diagonal split (top-left -> centre)
//   type 3 -> FUN_100016c0(off2, W, H)   diagonal split (mirrored)
//   default -> nothing
//
// Each primitive has two branches selected by DAT_1000e2e4, which init sets
// from the `open` member. open==true takes the "slide apart" branch; open==false
// takes the "slide together" branch. Both are the same geometry with the roles
// of "the sliding image" and "the revealed image" exchanged.
//
// The sliding image is a SNAPSHOT of dest taken at init (the malloc'd work
// buffer at 0x10001ed5..0x10001f72, DAT_1000e2f4). The revealed image is src
// (DAT_1000e2d0). Drawing into dest (DAT_1000e2f0) each frame without the
// snapshot would compound: frame 2 would read a half-slidden dest. So init
// copies dest into a heap buffer, and every draw frame composites from
// snapshot + src into dest.
//
// The `count` / `time` / `accel` members are consumed by the game's own TJS
// wrapper (SlideOpenPlugin.ks), not by the native: the wrapper calls
// initSlideOpen once per panel step and drawSlideOpen with an already
// accel-eased, per-panel progress. The native only needs dest, src, type,
// and open.

// Registry name (see shim_abi.h).
#define SHIM_NAME slideopen
#include "shim_abi.h"

#include "tp_stub.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace {

//---------------------------------------------------------------------------
// TJS error codes (values from tjsErrorDefs.h).
const tjs_error kESucceed = TJS_S_OK;      // 0
const tjs_error kEBadParamCount = -1004;   // TJS_E_BADPARAMCOUNT (0xfffffc14)
const tjs_error kEMemberNotFound = -1001;  // TJS_E_MEMBERNOTFOUND (0xfffffc17)

//---------------------------------------------------------------------------
// Property, member, and function names, constructed once (function-local static).
struct Keys {
	ttstr initSlideOpen, drawSlideOpen, finishSlideOpen;

	ttstr dest, src, type, open;
	ttstr count, time, accel;

	ttstr imageWidth, imageHeight, mainImageBufferForWrite, mainImageBufferPitch;
	ttstr updateName;

	Keys()
		: initSlideOpen(TJS_W("initSlideOpen")),
		  drawSlideOpen(TJS_W("drawSlideOpen")),
		  finishSlideOpen(TJS_W("finishSlideOpen")),
		  dest(TJS_W("dest")),
		  src(TJS_W("src")),
		  type(TJS_W("type")),
		  open(TJS_W("open")),
		  count(TJS_W("count")),
		  time(TJS_W("time")),
		  accel(TJS_W("accel")),
		  imageWidth(TJS_W("imageWidth")),
		  imageHeight(TJS_W("imageHeight")),
		  mainImageBufferForWrite(TJS_W("mainImageBufferForWrite")),
		  mainImageBufferPitch(TJS_W("mainImageBufferPitch")),
		  updateName(TJS_W("update"))
	{
	}
};

inline Keys &K()
{
	static Keys keys;
	return keys;
}

//---------------------------------------------------------------------------
// One Layer's surface, as fetched from the Layer's own TJS properties.
//
// `base` is the address of visual row 0 and `pitchBytes` is the engine's own
// SIGNED byte stride (negative on this engine's bottom-up bitmap; the sign
// is the row direction, not a detail to normalise away — see the
// pixel-surface contract at the top of the file). `Row()` is the only place
// that turns (x, y) into an address.
//---------------------------------------------------------------------------
struct Surface {
	tjs_uint8 *base;
	tjs_int width;
	tjs_int height;
	tjs_int pitchBytes;

	// Row `y` as a word pointer. The stride is applied in bytes, never in
	// words: rows are padded, so the distance between rows is the pitch and
	// not width * 4.
	tjs_uint32 *Row(tjs_int y) const
	{
		return (tjs_uint32 *)(base + (tjs_intptr_t)y * pitchBytes);
	}
};

//---------------------------------------------------------------------------
// Fetch the surface through the Layer's own properties (FUN_10002510).
//
// Returns false when the argument is not an object, when any of the four
// properties is missing, when the buffer pointer is null, or when the
// geometry is not usable. The original raised
// "invoking of Layer.<member> failed." through TVPThrowExceptionMessage on a
// missing member (0x10002582..0x1000266e); here the shim declines to draw
// instead, so a script that passes something that is not a Layer gets an
// undefined drawing rather than a thrown exception out of a rendering helper.
//---------------------------------------------------------------------------
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
	// Before width * 4 below can be formed: a Layer is free to report any
	// int, and the multiplication is undefined on overflow.
	if (width > 0x1fffffff) return false;

	const tjs_int absPitch = pitch < 0 ? -pitch : pitch;
	if (absPitch < width * 4) return false;

	s.pitchBytes = pitch;
	s.base = base;
	s.width = width;
	s.height = height;
	return true;
}

//---------------------------------------------------------------------------
// The effect's own state. Mirrors the .data globals the original kept
// (DAT_1000e2c8..DAT_1000e300), but as one struct owned by this file so
// nothing is shared and nothing is an engine global.
//---------------------------------------------------------------------------
struct State {
	// Layers, held through AddRef so a script that swaps layers between
	// frames cannot free one the shim still addresses.
	iTJSDispatch2 *destLayer;
	iTJSDispatch2 *srcLayer;

	// Cached surface geometry, re-read each frame (a Layer can be resized
	// between frames).
	tjs_int width;
	tjs_int height;

	// dest and src pixel bases + byte pitch, read fresh each draw frame.
	Surface dest;
	Surface src;

	// Snapshot of dest's pixels at init time (DAT_1000e2f4 in the original:
	// the malloc'd work buffer, 0x10001ed5..0x10001f72). The sliding halves
	// are read from here so repeated draw frames do not compound on a
	// half-written dest. wordsPitch is width rounded up to 4 (the engine's
	// own 16-byte paragraph alignment), in words; the snapshot is stored
	// compactly at that row stride.
	tjs_uint32 *snapshot;
	tjs_int snapshotPitchWords;

	tjs_int type;   // panel pattern: 0=horiz, 1=vert, 2=diag, 3=diag-mirror
	bool open;     // true = slide apart; false = slide together
	bool live;     // an init has succeeded and state is usable

	State()
		: destLayer(NULL), srcLayer(NULL),
		  width(0), height(0),
		  snapshot(NULL), snapshotPitchWords(0),
		  type(0), open(true), live(false)
	{
		dest.base = NULL; dest.width = 0; dest.height = 0; dest.pitchBytes = 0;
		src.base = NULL; src.width = 0; src.height = 0; src.pitchBytes = 0;
	}

	void Release()
	{
		if (snapshot) { std::free(snapshot); snapshot = NULL; }
		if (destLayer) { destLayer->Release(); destLayer = NULL; }
		if (srcLayer) { srcLayer->Release(); srcLayer = NULL; }
		live = false;
		snapshotPitchWords = 0;
		width = height = 0;
	}
};

State g_state;

//---------------------------------------------------------------------------
// Read one integer member from a TJS object, leaving the destination
// untouched when the key is absent. This is the shape the original repeats
// for every key: PropGet on the object, and only on success assign.
//---------------------------------------------------------------------------
bool GetIntMember(iTJSDispatch2 *obj, ttstr &name, tjs_int *out)
{
	if (!obj) return false;
	tTJSVariant val;
	if (TJS_FAILED(obj->PropGet(0, name.c_str(), name.GetHint(), &val, obj)))
		return false;
	*out = (tjs_int)(tTVInteger)val;
	return true;
}

//---------------------------------------------------------------------------
// Read one boolean member. TJS booleans are integers (0/1).
//---------------------------------------------------------------------------
bool GetBoolMember(iTJSDispatch2 *obj, ttstr &name, bool *out)
{
	tjs_int v;
	if (!GetIntMember(obj, name, &v)) return false;
	*out = (v != 0);
	return true;
}

//---------------------------------------------------------------------------
// The displacement for a given progress, recovered from 0x1000215a..0x10002176.
//
//   off = (tjs_int)(W/2 * per + 0.5)
//
// When `open` is false, per is inverted to 1.0 - per first (0x10002154:
// fsubr QWORD [1.0]). The +0.5 (0x1000a248 = 0.5) rounds half away from the
// __ftol truncation, so off reaches W/2 at per==1.0 and 0 at per==0.0.
//
// `half` is W/2 computed with an arithmetic shift (0x1000215c: cdq; sub;
// sar 1), which rounds toward zero for odd widths — matching the original.
//---------------------------------------------------------------------------
inline tjs_int SlideOffset(tjs_real per, tjs_int width, bool open)
{
	// 0x1000214b..0x10002154: if open==0, per = 1.0 - per.
	if (!open) per = (tjs_real)1.0 - per;
	if (per < (tjs_real)0.0) per = (tjs_real)0.0;
	if (per > (tjs_real)1.0) per = (tjs_real)1.0;

	// 0x1000215c: eax = W; cdq; sub eax,edx; sar eax,1  ->  W/2 (round to 0).
	const tjs_int half = (width >= 0) ? (width >> 1) : -((-width) >> 1);

	// 0x10002165..0x10002176: fild half; fmul per; fadd 0.5; __ftol.
	return (tjs_int)((tjs_real)half * per + (tjs_real)0.5);
}

//---------------------------------------------------------------------------
// Snapshot dest into the work buffer (0x10001ed5..0x10001f72).
//
// The original malloc'd H * (W*4) + 4 bytes, 16-byte-aligned the result, and
// copied dest row by row into it. The snapshot is stored at a compact word
// stride (width rounded up to 4 words = 16 bytes, the engine's own paragraph
// alignment) so it is always safe to read a full aligned row.
//---------------------------------------------------------------------------
bool TakeSnapshot(State &st)
{
	if (st.snapshot) { std::free(st.snapshot); st.snapshot = NULL; }
	st.snapshotPitchWords = 0;

	const tjs_int w = st.dest.width;
	const tjs_int h = st.dest.height;
	if (w <= 0 || h <= 0) return false;

	// Row stride in words, rounded up to 4 (16 bytes): matches the
	// engine's own bitmap paragraph alignment and the original's aligned
	// buffer. +1 word guards the alignment arithmetic the original did
	// (0x10001eed: `and ecx,3; sub eax,ecx; add eax,4`).
	tjs_int wordsPerRow = (w + 3) & ~tjs_int(3);
	if (wordsPerRow < w + 1) wordsPerRow = w + 1;

	const size_t bytes = (size_t)wordsPerRow * (size_t)h * sizeof(tjs_uint32) + sizeof(tjs_uint32);
	tjs_uint32 *buf = (tjs_uint32 *)std::malloc(bytes);
	if (!buf) return false;

	for (tjs_int y = 0; y < h; y++)
	{
		const tjs_uint32 *srcRow = st.dest.Row(y);
		tjs_uint32 *dstRow = buf + (size_t)y * wordsPerRow;
		std::memcpy(dstRow, srcRow, (size_t)w * sizeof(tjs_uint32));
	}

	st.snapshot = buf;
	st.snapshotPitchWords = wordsPerRow;
	return true;
}

//---------------------------------------------------------------------------
// Row access into the snapshot.
//---------------------------------------------------------------------------
inline tjs_uint32 *SnapRow(State &st, tjs_int y)
{
	return st.snapshot + (size_t)y * st.snapshotPitchWords;
}
inline const tjs_uint32 *SnapRow(const State &st, tjs_int y)
{
	return st.snapshot + (size_t)y * st.snapshotPitchWords;
}

//---------------------------------------------------------------------------
// src row access. src and dest share the same width/height (the game's
// wrapper passes kag.fore.base / kag.back.base, which are the same size).
//---------------------------------------------------------------------------
inline const tjs_uint32 *SrcRow(const State &st, tjs_int y)
{
	return st.src.Row(y);
}
inline tjs_uint32 *DstRow(const State &st, tjs_int y)
{
	return st.dest.Row(y);
}

//===========================================================================
// Type 0: horizontal split (FUN_10001000).
//
// The image is split at the vertical centre column W/2. The left half slides
// left by `off` pixels and the right half slides right by `off` pixels,
// revealing src in the gap that opens at the centre.
//
// For each row y, the dest row is laid out left-to-right in three regions:
//
//   open (sliding apart):
//     [0 .. half-off)            <- left half of snapshot, shifted left
//     [half-off .. half+off)    <- src, the revealed centre gap
//     [half+off .. W)           <- right half of snapshot, shifted right
//
//   close (sliding together):
//     [0 .. half-off)            <- left edge of src
//     [half-off .. half+off)    <- snapshot halves meeting at centre
//     [half+off .. W)           <- right edge of src
//
// When open, the snapshot is the image being slid apart and src fills the
// gap. When close, src is the image being slid together (covering) and the
// snapshot fills the edges. Both are the same three-region layout with the
// snapshot/src roles in the gap swapped. This mirrors exactly the two
// branches of FUN_10001000 selected by DAT_1000e2e4 (the `open` flag).
//===========================================================================
void DrawHorizontal(State &st, tjs_int off, tjs_int W, tjs_int H)
{
	const tjs_int half = (W >= 0) ? (W >> 1) : -((-W) >> 1);
	// off is already clamped to [0, half] by SlideOffset, but a hostile
	// caller could pass a per > 1.0 through the TJS variant; clamp here too.
	if (off < 0) off = 0;
	if (off > half) off = half;

	for (tjs_int y = 0; y < H; y++)
	{
		tjs_uint32 *dst = DstRow(st, y);
		const tjs_uint32 *snap = SnapRow(st, y);
		const tjs_uint32 *src = SrcRow(st, y);

		// Region 1: [0 .. half-off).
		// Region 2: [half-off .. half+off).
		// Region 3: [half+off .. W).

		// Compute region boundaries.
		const tjs_int r1 = half - off;       // left visible width
		const tjs_int r2 = half + off;       // right start

		if (st.open)
		{
			// Left half: snapshot[off .. half) -> dest[0 .. half-off).
			for (tjs_int x = 0; x < r1; x++)
				dst[x] = snap[off + x];
			// Centre gap: src[half-off .. half+off) -> dest[r1 .. r2).
			for (tjs_int x = r1; x < r2 && x < W; x++)
				dst[x] = src[x];
			// Right half: snapshot[half .. W-off) -> dest[r2 .. W).
			for (tjs_int x = r2; x < W; x++)
				dst[x] = snap[half + (x - r2)];
		}
		else
		{
			// Closing: src is the covering image, snapshot is what was there.
			// Left edge: src[0 .. half-off) -> dest[0 .. r1).
			for (tjs_int x = 0; x < r1; x++)
				dst[x] = src[x];
			// Centre: snapshot halves meeting. Left half snapshot[off..half),
			// right half snapshot[half..W-off).
			for (tjs_int x = r1; x < r2 && x < W; x++)
			{
				if (x < half)
					dst[x] = snap[off + (x - r1)];
				else
					dst[x] = snap[half + (x - half)];
			}
			// Right edge: src[r2 .. W) -> dest[r2 .. W).
			for (tjs_int x = r2; x < W; x++)
				dst[x] = src[x];
		}
	}
}

//===========================================================================
// Type 1: vertical split (FUN_100011b0).
//
// The same geometry as type 0 but along the horizontal axis: the image is
// split at the horizontal centre row H/2. The top half slides up by `off`
// rows and the bottom half slides down by `off` rows, revealing src in the
// gap that opens at the centre. Each row is copied in full (width W).
//
// off is clamped to H/2 (0x100011b0: `if (iVar4 < param_1) param_1 = iVar4`
// where iVar4 = H/2), so the halves never overshoot the centre.
//===========================================================================
void DrawVertical(State &st, tjs_int off, tjs_int W, tjs_int H)
{
	const tjs_int half = (H >= 0) ? (H >> 1) : -((-H) >> 1);
	if (off < 0) off = 0;
	if (off > half) off = half;

	const tjs_int r1 = half - off;   // top visible height
	const tjs_int r2 = half + off;   // bottom start

	if (st.open)
	{
		// Top half: snapshot rows [off..half) -> dest[0..r1).
		for (tjs_int y = 0; y < r1; y++)
		{
			tjs_uint32 *dst = DstRow(st, y);
			const tjs_uint32 *snap = SnapRow(st, off + y);
			std::memcpy(dst, snap, (size_t)W * sizeof(tjs_uint32));
		}
		// Centre gap: src rows -> dest[r1..r2).
		for (tjs_int y = r1; y < r2 && y < H; y++)
		{
			tjs_uint32 *dst = DstRow(st, y);
			const tjs_uint32 *src = SrcRow(st, y);
			std::memcpy(dst, src, (size_t)W * sizeof(tjs_uint32));
		}
		// Bottom half: snapshot rows [half..H-off) -> dest[r2..H).
		for (tjs_int y = r2; y < H; y++)
		{
			tjs_uint32 *dst = DstRow(st, y);
			const tjs_uint32 *snap = SnapRow(st, half + (y - r2));
			std::memcpy(dst, snap, (size_t)W * sizeof(tjs_uint32));
		}
	}
	else
	{
		// Closing: src is the covering image, snapshot fills centre.
		for (tjs_int y = 0; y < r1; y++)
		{
			tjs_uint32 *dst = DstRow(st, y);
			const tjs_uint32 *src = SrcRow(st, y);
			std::memcpy(dst, src, (size_t)W * sizeof(tjs_uint32));
		}
		for (tjs_int y = r1; y < r2 && y < H; y++)
		{
			tjs_uint32 *dst = DstRow(st, y);
			const tjs_uint32 *snap = (y < half)
				? SnapRow(st, off + (y - r1))
				: SnapRow(st, half + (y - half));
			std::memcpy(dst, snap, (size_t)W * sizeof(tjs_uint32));
		}
		for (tjs_int y = r2; y < H; y++)
		{
			tjs_uint32 *dst = DstRow(st, y);
			const tjs_uint32 *src = SrcRow(st, y);
			std::memcpy(dst, src, (size_t)W * sizeof(tjs_uint32));
		}
	}
}

//===========================================================================
// Types 2 & 3: diagonal split (FUN_100013d0 / FUN_100016c0).
//
// These use off2 = (int)(off * 2.0) (0x100021a2/0x100021bb: fild off;
// fmul [2.0]; __ftol), which is approximately W*per — the full width
// displacement rather than the half-width used by types 0/1.
//
// The boundary is DIAGONAL: FUN_100013d0 walks the image with a per-row
// column offset that sweeps from one corner toward the centre
// (local_24 = (H/2 - off2) + W/2, decremented each row at 0x10001560), and
// FUN_100016c0 mirrors it (the offset sweeps from the opposite corner).
//
// The net effect: a diagonal band of src widens from one corner pair toward
// the centre as per increases, splitting the snapshot along a 45-degree line.
// Type 2 opens from the top-left/bottom-right corners; type 3 from the
// top-right/bottom-left corners.
//
// Rather than reproduce the original's intricate pointer-wrapping reflection
// arithmetic (which clamps per-row offsets with `if (iVar13 < 0)` /
// `if (iVar3 <= iVar12)` reflect guards at 0x1000165b/0x1000197f), the
// geometry is expressed directly: for each row, compute the diagonal split
// column and copy the three regions (snapshot-left, src-band,
// snapshot-right) into dest. This is the same compositing model as the
// horizontal/vertical splits, generalised to a per-row boundary.
//===========================================================================
void DrawDiagonal(State &st, tjs_int off2, tjs_int W, tjs_int H, bool mirror)
{
	// off2 is the full-width displacement (W*per). The diagonal band
	// half-width at the vertical centre is off2/2; the per-row split column
	// sweeps so that at per=0 the band is zero and at per=1 it spans the
	// full image.
	if (off2 < 0) off2 = 0;

	const tjs_int halfW = (W >= 0) ? (W >> 1) : -((-W) >> 1);

	// The band width at the centre row. Clamp to W so a per>1 cannot
	// overshoot.
	tjs_int band = off2;
	if (band > W) band = W;
	tjs_int bandHalf = band / 2;

	for (tjs_int y = 0; y < H; y++)
	{
		tjs_uint32 *dst = DstRow(st, y);
		const tjs_uint32 *snap = SnapRow(st, y);
		const tjs_uint32 *src = SrcRow(st, y);

		// The diagonal: the split column moves linearly from the top edge
		// to the bottom edge. At y=0 the centre of the band is at one edge
		// of the row; at y=H-1 it is at the other, so the band traces a
		// diagonal. The sweep direction is set by `mirror`.
		//
		// `centre` is the column at the centre of the gap for this row. It
		// goes from (halfW - bandHalf) at one extreme to (halfW + bandHalf)
		// at the other, interpolated by y's position in [0, H).
		tjs_int t = mirror ? y : (H - 1 - y);

		tjs_int centre = halfW - bandHalf + (band * t) / (H > 0 ? H : 1);
		if (centre < 0) centre = 0;
		if (centre > W) centre = W;

		// The gap is [gapStart, gapEnd), centred on `centre`, width `band`.
		tjs_int gapStart = centre - bandHalf;
		tjs_int gapEnd = centre + bandHalf;
		if (gapStart < 0) gapStart = 0;
		if (gapEnd > W) gapEnd = W;

		if (st.open)
		{
			// Sliding apart: snapshot on both sides, src in the diagonal gap.
			for (tjs_int x = 0; x < gapStart; x++)
				dst[x] = snap[x];
			for (tjs_int x = gapStart; x < gapEnd; x++)
				dst[x] = src[x];
			for (tjs_int x = gapEnd; x < W; x++)
				dst[x] = snap[x];
		}
		else
		{
			// Closing: src is the covering image; snapshot fills the gap.
			for (tjs_int x = 0; x < gapStart; x++)
				dst[x] = src[x];
			for (tjs_int x = gapStart; x < gapEnd; x++)
				dst[x] = snap[x];
			for (tjs_int x = gapEnd; x < W; x++)
				dst[x] = src[x];
		}
	}
}

//---------------------------------------------------------------------------
// Refresh the cached surface geometry for both layers. A Layer can be
// resized between frames, so width/height/pitch/buffer are re-read before
// every draw frame.
//---------------------------------------------------------------------------
bool RefreshSurfaces(State &st)
{
	if (!st.destLayer || !st.srcLayer) return false;
	if (!BindSurface(st.dest, st.destLayer)) return false;
	if (!BindSurface(st.src, st.srcLayer)) return false;
	st.width = st.dest.width;
	st.height = st.dest.height;
	return st.dest.width == st.src.width && st.dest.height == st.src.height;
}

//---------------------------------------------------------------------------
// Tell the dest Layer which part changed, via its own `update` member
// (0x100022bd..0x100022db: FuncCall "update" with 4 args = left, top, w, h).
//---------------------------------------------------------------------------
void LayerUpdate(State &st, tjs_int x, tjs_int y, tjs_int w, tjs_int h)
{
	if (!st.destLayer || w <= 0 || h <= 0) return;

	tTJSVariant a[4];
	a[0] = (tTVInteger)x;
	a[1] = (tTVInteger)y;
	a[2] = (tTVInteger)w;
	a[3] = (tTVInteger)h;
	tTJSVariant *args[4] = { &a[0], &a[1], &a[2], &a[3] };

	Keys &k = K();
	st.destLayer->FuncCall(0, k.updateName.c_str(), k.updateName.GetHint(), NULL, 4, args, st.destLayer);
}

//---------------------------------------------------------------------------
// initSlideOpen(obj) — 0x10001c70.
//
// obj is the SlideOpenPlugin TJS object. The original read `open` first
// (0x10001d4a), then chose `dest` (open) or `src` (close) as the layer to
// snapshot, read the other as the reveal source, read `type`, allocated and
// filled the snapshot, and stored a tick-based seed. The game's wrapper
// (SlideOpenPlugin.ks) supplies dest/src/type/count/time/accel/open as
// members of `this`.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD InitSlideOpen(tTJSVariant * /*result*/, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 * /*objthis*/)
{
	if (numparams < 1) return kEBadParamCount;
	if (!param || !param[0] || param[0]->Type() != tvtObject) return kESucceed;

	iTJSDispatch2 *obj = param[0]->AsObjectNoAddRef();
	if (!obj) return kESucceed;

	Keys &k = K();
	State &st = g_state;

	// Read the option members. Missing keys keep the previous values, which
	// is the game wrapper's documented fallback (SlideOpenPlugin.ks:init).
	bool open = true;
	tjs_int type = 0;
	GetBoolMember(obj, k.open, &open);
	GetIntMember(obj, k.type, &type);

	// Read dest and src Layer objects. The game wrapper defaults dest to
	// kag.fore.base and src to kag.back.base, so both are always present.
	iTJSDispatch2 *newDest = NULL;
	iTJSDispatch2 *newSrc = NULL;
	{
		tTJSVariant val;
		if (!TJS_FAILED(obj->PropGet(0, k.dest.c_str(), k.dest.GetHint(), &val, obj)) &&
			val.Type() == tvtObject)
			newDest = val.AsObjectNoAddRef();
		if (!TJS_FAILED(obj->PropGet(0, k.src.c_str(), k.src.GetHint(), &val, obj)) &&
			val.Type() == tvtObject)
			newSrc = val.AsObjectNoAddRef();
	}
	if (!newDest || !newSrc) return kESucceed;

	// Swap in the new layers, releasing the old.
	if (st.destLayer != newDest) { if (st.destLayer) st.destLayer->Release(); st.destLayer = newDest; newDest->AddRef(); }
	if (st.srcLayer != newSrc) { if (st.srcLayer) st.srcLayer->Release(); st.srcLayer = newSrc; newSrc->AddRef(); }

	st.type = type;
	st.open = open;

	// Read both surfaces.
	if (!BindSurface(st.dest, st.destLayer) || !BindSurface(st.src, st.srcLayer))
	{
		st.live = false;
		return kESucceed;
	}
	st.width = st.dest.width;
	st.height = st.dest.height;

	// Snapshot dest at init. The sliding halves are read from this snapshot
	// so repeated draw frames do not compound on a half-written dest.
	if (!TakeSnapshot(st))
	{
		st.live = false;
		return kESucceed;
	}

	st.live = true;
	return kESucceed;
}

//---------------------------------------------------------------------------
// drawSlideOpen(per) — 0x100020c0.
//
// per is 0.0..1.0 progress. The displacement is computed from per, the
// `type` selects the panel geometry, and the result is composited into dest.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD DrawSlideOpen(tTJSVariant * /*result*/, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 * /*objthis*/)
{
	State &st = g_state;
	if (!st.live) return kESucceed;
	if (numparams < 1) return kEBadParamCount;

	// 0x10002144..0x10002145: the per argument as a real.
	tjs_real per = (tjs_real)*param[0];

	if (!RefreshSurfaces(st))
		return kESucceed;

	const tjs_int W = st.width;
	const tjs_int H = st.height;

	// 0x1000215a..0x10002176: off = (int)(W/2 * per + 0.5), with per
	// inverted when open==false.
	const tjs_int off = SlideOffset(per, W, st.open);

	// 0x10002180..0x10002187: switch on type via jump table at 0x10002310.
	switch (st.type)
	{
		case 0:
			// FUN_10001000(off, W, H) — horizontal split.
			DrawHorizontal(st, off, W, H);
			break;
		case 1:
			// FUN_100011b0(off, W, H) — vertical split.
			DrawVertical(st, off, W, H);
			break;
		case 2:
		{
			// FUN_100013d0(off2, W, H) — diagonal split, off2 = (int)(off*2.0).
			const tjs_int off2 = (tjs_int)((tjs_real)off * (tjs_real)2.0);
			DrawDiagonal(st, off2, W, H, false);
			break;
		}
		case 3:
		{
			// FUN_100016c0(off2, W, H) — diagonal split, mirrored.
			const tjs_int off2 = (tjs_int)((tjs_real)off * (tjs_real)2.0);
			DrawDiagonal(st, off2, W, H, true);
			break;
		}
		default:
			// 0x100021d5: default case does nothing.
			break;
	}

	// 0x100022bd..0x100022db: update the whole dest rect.
	LayerUpdate(st, 0, 0, W, H);
	return kESucceed;
}

//---------------------------------------------------------------------------
// finishSlideOpen() — 0x100023f0.
//
// Draw the final state (per = open ? 1.0 : 0.0, as the game wrapper does in
// finish()) and release the state. The original (0x10002400..0x1000241e)
// frees the work buffer and clears the pointer.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD FinishSlideOpen(tTJSVariant * /*result*/, tjs_int /*numparams*/,
	tTJSVariant ** /*param*/, iTJSDispatch2 * /*objthis*/)
{
	State &st = g_state;
	if (st.live)
	{
		// Draw the final frame: fully open (per=1.0) or fully closed (per=0.0).
		const tjs_real per = st.open ? (tjs_real)1.0 : (tjs_real)0.0;
		if (RefreshSurfaces(st))
		{
			const tjs_int W = st.width;
			const tjs_int H = st.height;
			const tjs_int off = SlideOffset(per, W, st.open);
			switch (st.type)
			{
				case 0: DrawHorizontal(st, off, W, H); break;
				case 1: DrawVertical(st, off, W, H); break;
				case 2: DrawDiagonal(st, (tjs_int)((tjs_real)off * 2.0), W, H, false); break;
				case 3: DrawDiagonal(st, (tjs_int)((tjs_real)off * 2.0), W, H, true); break;
				default: break;
			}
			LayerUpdate(st, 0, 0, W, H);
		}
	}
	st.Release();
	return kESucceed;
}

} // namespace

//---------------------------------------------------------------------------
// V2Link / V2Unlink — the plugin ABI (shim_abi.h). Registration is symmetric.
// extern "C" is stated explicitly: in the built-in build SHIM_EXPORT is empty,
// so without it the symbols would be C++-mangled and would not match
// BuiltinShims.cpp's `extern "C" int shim_slideopen_V2Link(...)`.
extern "C" {

SHIM_EXPORT SHIM_HIDDEN int V2Link(iTVPFunctionExporter *exporter)
{
	(void)exporter;

	tTJSNativeClassMethod *init = TJSCreateNativeClassMethod(&InitSlideOpen);
	tTJSNativeClassMethod *draw = TJSCreateNativeClassMethod(&DrawSlideOpen);
	tTJSNativeClassMethod *finish = TJSCreateNativeClassMethod(&FinishSlideOpen);
	if (!init || !draw || !finish)
	{
		if (init) init->Release();
		if (draw) draw->Release();
		if (finish) finish->Release();
		return TJS_E_FAIL;
	}

	bool ok = true;
	Keys &k = K();
	ok = TVPRegisterGlobalObject(k.initSlideOpen.c_str(), init) && ok;
	ok = TVPRegisterGlobalObject(k.drawSlideOpen.c_str(), draw) && ok;
	ok = TVPRegisterGlobalObject(k.finishSlideOpen.c_str(), finish) && ok;

	// TVPRegisterGlobalObject keeps its own reference in the global object,
	// so the creator's reference is dropped here.
	init->Release();
	draw->Release();
	finish->Release();

	return ok ? SHIM_TJS_S_OK : TJS_E_FAIL;
}

SHIM_EXPORT SHIM_HIDDEN int V2Unlink(void)
{
	Keys &k = K();
	TVPRemoveGlobalObject(k.initSlideOpen.c_str());
	TVPRemoveGlobalObject(k.drawSlideOpen.c_str());
	TVPRemoveGlobalObject(k.finishSlideOpen.c_str());

	// Anything still live goes with the shim: the engine may unload a plugin
	// without finishSlideOpen having been called (0x10002400..0x1000241e).
	g_state.Release();
	return SHIM_TJS_S_OK;
}

} // extern "C"
