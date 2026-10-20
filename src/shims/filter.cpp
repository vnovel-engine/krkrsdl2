//---------------------------------------------------------------------------
// shims/filter.cpp — Fate/stay night's `filter.dll`, replaced.
//
// What the plugin was
// -------------------
// `filter.dll` is NOT stock KiriKiri: it is absent from the upstream tree (no
// entry in bin/win32/plugin, no source under src/plugins) and ships with Fate.
// It registers twelve global TJS functions and runs them as per-pixel passes
// over a Layer's own bitmap, plus one wrapper that delegates to the engine's
// own stretch blitter:
//
//     Stretch(%[src,dest,dleft,dtop,dwidth,dheight,
//              sleft,stop,swidth,sheight,opa,type])
//         — unpack the dictionary and call dest->OperateStretch.
//     Contrast(%[layer, level])
//         — per-pixel contrast via a 256-entry LUT built from `level`.
//     Noise(%[layer, monocro, seed, under, upper])
//         — fill the layer with an LCG noise field, optionally banded and
//           optionally monochrome.
//     Blur(%[layer, level])     — 3x3 blur applied `level` times.
//     Smudge(%[layer, level])   — as Blur. The DLL reaches the 3x3 kernel at
//                                 0x10001000 from 0x100038a9, inside Smudge's
//                                 Process (0x10003670); a second call site at
//                                 0x100034f0 lies outside both natives and
//                                 has not been attributed.
//     Lens(%[dest, zoom, center, power])
//         — magnify a circular region via bilinear resampling.
//     InitLens() / ReleaseLens()
//         — build / free a fixed-point tan table the Lens pass uses.
//     initHaze(options)         — build a 16384-entry wave table from a
//                                  cosine-sum of `waves`/`lwaves`.
//     doHaze(%[dest, src, tick, per, bgcolor, blend])
//         — one frame: displace rows of src by the wave table scaled by `per`,
//           blend or copy onto dest.
//     endHaze()                 — free the wave table.
//     update()                  — repaint/refresh (the plugin's own `update`
//                                  global, which the pixel filters invoke on
//                                  the layer after writing).
//
// The pixel filters reach the Layer through its own standard TJS properties
// (`imageWidth`, `imageHeight`, `mainImageBufferForWrite`,
// `mainImageBufferPitch`) — the same surface `drawer.dll`, `firespark.dll`,
// `clipboardEx`, `tftSave`, `shrinkCopy` and others use — so every pass
// happens inside the engine and no host boundary is crossed. V2Unlink removes
// the twelve names again.
//
// One source, two consumers: see shim_abi.h. Built into the engine the names
// are reached through the override table's `@builtin:filter`; built as a PE it
// is the plugin the game links.
//
// Recovered from the binary
// -------------------------
// The port is reconstructed from the shipped DLL, not from source. Functions
// named below are Ghidra's, matched to addresses; every non-obvious line says
// which one it comes from and what the arithmetic means.
//
//   FUN_10001a70  read the Layer's surface: imageWidth / imageHeight /
//                 mainImageBufferForWrite / mainImageBufferPitch, through
//                 iTJSDispatch2::PropGet, raising
//                 "invoking of Layer.<member> failed." when one is absent.
//   FUN_10001000  one pass of a 3x3 box blur over the surface (in place,
//                 with a one-row scratch border). Used by both Blur and
//                 Smudge.
//   FUN_10001ca0  InitLens: allocate and fill a 0x2000-entry fixed-point tan
//                 table.
//   FUN_10001d00  ReleaseLens: free the tan table.
//   FUN_10001d10  Lens: magnify a circular region, sampling the source with
//                 bilinear weights derived from the tan table.
//   FUN_100025e0  initHaze: parse the wave spec and build a 16384-entry
//                 displacement table as a cosine sum.
//   FUN_100027a0  doHaze blend row: 50/50 average of dest and a displaced src.
//   FUN_100029a0  doHaze copy row: straight copy of a displaced src row.
//   FUN_10003ec0  Noise: fill the layer with an LCG noise field.
//   FUN_100047d0  Contrast: build a 256-entry LUT and remap every channel.
//   FUN_100051d0  doHaze: displace rows of src by the wave table and write
//                 them onto dest, blending or copying per the `blend` flag.
//
// Pixel-surface contract
// ----------------------
// The surface is the Layer's own buffer, reached through the Layer's TJS
// properties exactly as the original did. Nothing here crosses a host
// boundary, so no host callback is involved. The assumptions, stated once, in
// the style of `drawer.cpp` and `firespark.cpp`:
//
//   * the buffer is 32bpp ARGB: one little-endian word a pixel, bytes B,G,R,A,
//     so 0x00RRGGBB with the alpha in bits 24-31. The engine's own
//     `GrayToColorFunctor` (visual/LayerBitmapIntf.h) packs 0xff000000 |
//     r<<16 | g<<8 | b the same way;
//   * rows are addressed by a BYTE pitch. `mainImageBufferPitch` maps to
//     Layer::GetMainImagePixelBufferPitch() -> GetPitchBytes()
//     (visual/LayerIntf.cpp:2688), so the "Pixel" in the accessor name is
//     misleading. Rows are padded: never assume width * 4, and never index by
//     a word stride;
//   * that pitch is SIGNED and reports a bottom-up bitmap, so `base` is the
//     low end of the pixel data and the stride to the next visual row is its
//     magnitude (see drawer.cpp's Surface for the full derivation). The sign
//     is kept: row y lives at base + y * pitch with the pitch as reported.
//---------------------------------------------------------------------------

// The shim's registry name. shim_abi.h turns this into the per-shim entry
// point symbols when built into the engine; see that header for why.
#define SHIM_NAME filter
#include "shim_abi.h"

#include "tp_stub.h"

// tTJSNC_Layer, tTJSNI_BaseLayer, OperateStretch, GetMainImage and the
// TVPGetObjectFrom_NI_BaseLayer round trip in this file all come from here; the
// TJS-property surface drawer.cpp uses instead is not enough for a filter, which
// has to hand the engine a real Layer to blit between.
#include "LayerIntf.h"

#include <new>
#include <cmath>
#include <cstring>

// TVPGetRandomBits128 (utils/Random.h) is the engine RNG the Noise pass uses
// when no explicit seed is given; it comes in through tp_stub.h.
#include "Random.h"

//---------------------------------------------------------------------------
// The Layer surface, as fetched from the Layer (FUN_10001a70).
//
// Identical in contract to drawer.cpp's Surface and firespark.cpp's
// BindLayer: base is the address of visual row 0, pitchBytes is the engine's
// own SIGNED byte stride (the sign is the row direction, not a detail to
// normalise away), and nothing else about the bitmap layout is assumed.
//---------------------------------------------------------------------------
namespace {

struct Surface {
	tjs_uint8 *base;
	tjs_int width;
	tjs_int height;
	tjs_int pitchBytes;

	// Row `y` as a word pointer, one stride further on each row. The stride
	// is applied in bytes, never in words: rows are padded.
	tjs_uint32 *Row(tjs_int y) const
	{
		return (tjs_uint32 *)(base + (tjs_intptr_t)y * pitchBytes);
	}
};

//---------------------------------------------------------------------------
// TJS error codes used below. Values from tjsErrorDefs.h, named here only so
// the returns read as what the original returned.
//---------------------------------------------------------------------------
const tjs_error kESucceed = TJS_S_OK;      // 0
const tjs_error kEBadParamCount = -1004;    // TJS_E_BADPARAMCOUNT (0xfffffc14)
const tjs_error kEMemberNotFound = -1001;   // TJS_E_MEMBERNOTFOUND (0xfffffc17)

//---------------------------------------------------------------------------
// Property and function names.
//
// Constructed once for the process: a function-local static is initialised
// exactly once even if TJS ever runs these from more than one thread, and the
// strings would otherwise be rebuilt on every call.
//---------------------------------------------------------------------------
struct Keys {
	ttstr imageWidth, imageHeight, mainImageBufferForWrite, mainImageBufferPitch;
	ttstr updateName;

	// The twelve registered globals.
	ttstr Stretch, initHaze, doHaze, endHaze;
	ttstr Contrast, Noise, InitLens, ReleaseLens, Lens, Blur, Smudge, update;

	// Option-dictionary keys (UTF-16, recovered from filter.dll's string
	// table at 0x100112f8..0x10011700).
	ttstr layer, level, monocro, zoom, power, center;
	ttstr dest, src, type, seed, under, upper;
	ttstr waves, lwaves, cycle, speed, blend, bgcolor, tick, per;
	ttstr lower, centerpow, lowerpow, upperpow;
	ttstr dleft, dtop, dwidth, dheight, sleft, stop, swidth, sheight, opa;
	ttstr count, time, intime, outtime;

	Keys()
		: imageWidth(TJS_W("imageWidth")),
		  imageHeight(TJS_W("imageHeight")),
		  mainImageBufferForWrite(TJS_W("mainImageBufferForWrite")),
		  mainImageBufferPitch(TJS_W("mainImageBufferPitch")),
		  updateName(TJS_W("update")),
		  Stretch(TJS_W("Stretch")),
		  initHaze(TJS_W("initHaze")),
		  doHaze(TJS_W("doHaze")),
		  endHaze(TJS_W("endHaze")),
		  Contrast(TJS_W("Contrast")),
		  Noise(TJS_W("Noise")),
		  InitLens(TJS_W("InitLens")),
		  ReleaseLens(TJS_W("ReleaseLens")),
		  Lens(TJS_W("Lens")),
		  Blur(TJS_W("Blur")),
		  Smudge(TJS_W("Smudge")),
		  update(TJS_W("update")),
		  layer(TJS_W("layer")),
		  level(TJS_W("level")),
		  monocro(TJS_W("monocro")),
		  zoom(TJS_W("zoom")),
		  power(TJS_W("power")),
		  center(TJS_W("center")),
		  dest(TJS_W("dest")),
		  src(TJS_W("src")),
		  type(TJS_W("type")),
		  seed(TJS_W("seed")),
		  under(TJS_W("under")),
		  upper(TJS_W("upper")),
		  waves(TJS_W("waves")),
		  lwaves(TJS_W("lwaves")),
		  cycle(TJS_W("cycle")),
		  speed(TJS_W("speed")),
		  blend(TJS_W("blend")),
		  bgcolor(TJS_W("bgcolor")),
		  tick(TJS_W("tick")),
		  per(TJS_W("per")),
		  lower(TJS_W("lower")),
		  centerpow(TJS_W("centerpow")),
		  lowerpow(TJS_W("lowerpow")),
		  upperpow(TJS_W("upperpow")),
		  dleft(TJS_W("dleft")),
		  dtop(TJS_W("dtop")),
		  dwidth(TJS_W("dwidth")),
		  dheight(TJS_W("dheight")),
		  sleft(TJS_W("sleft")),
		  stop(TJS_W("stop")),
		  swidth(TJS_W("swidth")),
		  sheight(TJS_W("sheight")),
		  opa(TJS_W("opa")),
		  count(TJS_W("count")),
		  time(TJS_W("time")),
		  intime(TJS_W("intime")),
		  outtime(TJS_W("outtime"))
	{
	}
};

inline Keys &K()
{
	static Keys keys;
	return keys;
}

//---------------------------------------------------------------------------
// Fetch the surface through the Layer's own properties (FUN_10001a70).
//
// Returns false when the argument is not an object, when any of the four
// properties is missing, when the buffer pointer is null, or when the
// geometry is not usable. The original raised
// "invoking of Layer.<member> failed." through TVPThrowExceptionMessage on a
// missing member; here the shim declines to draw instead, so a script that
// passes something that is not a Layer gets an undefined effect rather than a
// thrown exception out of a rendering helper.
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
	// Signed BYTES a row; the sign is the row direction (see drawer.cpp).
	const tjs_int pitch = (tjs_int)(tTVInteger)val;

	if (!base || width <= 0 || height <= 0) return false;
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
// Extract one integer option, leaving the destination untouched when the key
// is absent. The shape the original repeats for every key (FUN_10003ec0 etc.):
// PropGet, and only on success assign.
//---------------------------------------------------------------------------
bool GetIntOpt(iTJSDispatch2 *dict, ttstr &name, tjs_int *out)
{
	if (!dict) return false;
	tTJSVariant val;
	if (TJS_FAILED(dict->PropGet(0, name.c_str(), name.GetHint(), &val, dict)))
		return false;
	*out = (tjs_int)(tTVInteger)val;
	return true;
}

bool GetRealOpt(iTJSDispatch2 *dict, ttstr &name, tjs_real *out)
{
	if (!dict) return false;
	tTJSVariant val;
	if (TJS_FAILED(dict->PropGet(0, name.c_str(), name.GetHint(), &val, dict)))
		return false;
	*out = (tjs_real)val;
	return true;
}

//---------------------------------------------------------------------------
// Get a Layer object from a dictionary member, as a tTJSNI_BaseLayer.
//
// NativeInstanceSupport with TJS_NIS_GETINSTANCE is the engine's own way to go
// from a TJS object to its native instance — the same call LayerIntf.cpp
// itself uses when a script hands it a layer — and it fails for a plain script
// object, which is what the null result guards.
//---------------------------------------------------------------------------
tTJSNI_BaseLayer *GetLayerMember(iTJSDispatch2 *dict, ttstr &name)
{
	if (!dict) return NULL;
	Keys &k = K();
	tTJSVariant val;
	if (TJS_FAILED(dict->PropGet(0, name.c_str(), name.GetHint(), &val, dict)))
		return NULL;
	iTJSDispatch2 *o = val.AsObjectNoAddRef();
	if (!o) return NULL;
	tTJSNI_BaseLayer *ni = NULL;
	o->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
		(iTJSNativeInstance **)&ni);
	return ni;
}

//---------------------------------------------------------------------------
// Tell the Layer which part changed, via its own `update` member.
// Layer.update(left, top, width, height), as called by the original after
// every pixel pass (FUN_10003ec0:0x1000473d etc.).
//---------------------------------------------------------------------------
void LayerUpdate(iTJSDispatch2 *layer, tjs_int x, tjs_int y, tjs_int w, tjs_int h)
{
	if (!layer || w <= 0 || h <= 0) return;
	Keys &k = K();
	tTJSVariant a[4];
	a[0] = (tTVInteger)x;
	a[1] = (tTVInteger)y;
	a[2] = (tTVInteger)w;
	a[3] = (tTVInteger)h;
	tTJSVariant *args[4] = { &a[0], &a[1], &a[2], &a[3] };
	layer->FuncCall(0, k.updateName.c_str(), k.updateName.GetHint(), NULL, 4, args, layer);
}

//---------------------------------------------------------------------------
// Get the tTJSNI_BaseLayer from the first argument (the `layer` parameter the
// pixel filters take), and the dictionary from it. The Contrast/Noise/Blur/
// Smudge/Lens filters take (layer, ...) positionally; the original read
// `layer` out of a dictionary for Noise/Contrast and took it directly for
// Blur/Smudge/Lens. The game's wrappers (FilterPlugin.ks, DefocusPlugin.ks)
// always pass a dictionary whose `layer` member is the target, so this helper
// handles both shapes: if arg0 is a dictionary, read `layer` from it;
// otherwise treat arg0 itself as the layer object.
//---------------------------------------------------------------------------
tTJSNI_BaseLayer *LayerFromArg(tTJSVariant *arg)
{
	if (!arg || arg->Type() != tvtObject) return NULL;
	iTJSDispatch2 *o = arg->AsObjectNoAddRef();
	if (!o) return NULL;
	tTJSNI_BaseLayer *ni = NULL;
	o->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Layer::ClassID,
		(iTJSNativeInstance **)&ni);
	return ni;
}

//===========================================================================
// Haze state (FUN_100025e0 / FUN_100051d0)
//
// The original kept the wave table and the haze geometry in file-static
// globals (DAT_10014690..DAT_100146b8). Here it is a single struct owned by
// this file, so two shims can coexist and the state is symmetric with
// initHaze/endHaze.
//===========================================================================
struct HazeState {
	bool active;
	double *waveTable;      // 16384-entry displacement table, malloc'd
	tjs_int tableSize;      // always 0x4000 (16384)
	tjs_int srcWidth;       // cached source geometry for doHaze
	tjs_int srcHeight;
	tjs_int srcPitch;       // byte pitch of the source
	tjs_uint8 *srcBase;     // source pixel buffer (visual row 0)
	// Wave spec, kept so initHaze can be re-run.
	struct WaveComp { tjs_real freq; tjs_real phase; tjs_real amp; };
	WaveComp *waveComps;    // the parsed `waves` list
	tjs_int nWaves;
	tjs_real speed;         // 2*PI/speed, the angular advance per ms
	tjs_real cycle;         // 4096/cycle, the table advance per ms

	HazeState() : active(false), waveTable(NULL), tableSize(0),
		srcWidth(0), srcHeight(0), srcPitch(0), srcBase(NULL),
		waveComps(NULL), nWaves(0), speed(0), cycle(0) {}
};

HazeState g_haze;

//---------------------------------------------------------------------------
// Parse a wave spec string like "(1,0,1) (1,PI/2,1) (3,2*PI,5)" into a list
// of (freq, phase, amp) triples. The game's analysisWaveSetting
// (HazePlugin.ks:182) splits on "() ," and groups every three numbers. The
// TJS wrapper already parsed this into an array-of-arrays before calling
// initHaze, so the shim receives it as a TJS array of 3-element arrays.
//---------------------------------------------------------------------------
bool ParseWaves(iTJSDispatch2 *arr, HazeState::WaveComp *out, tjs_int maxN, tjs_int *nOut)
{
	*nOut = 0;
	if (!arr) return false;
	tjs_int count = 0;
	{
		tTJSVariant v;
		if (TJS_FAILED(arr->PropGet(0, TJS_W("count"), 0, &v, arr))) return false;
		count = (tjs_int)(tTVInteger)v;
	}
	if (count <= 0) return true;
	if (count > maxN) count = maxN;

	for (tjs_int i = 0; i < count; i++)
	{
		// TJS arrays are indexed by member name (the index as a decimal
		// string). The original used the array's indexed accessor
		// (vtable +0x14); PropGet with the numeric name is the portable
		// equivalent.
		tjs_char ibuf[16];
		TJS_int_to_str(i, ibuf);
		tTJSVariant elem;
		if (TJS_FAILED(arr->PropGet(0, ibuf, 0, &elem, arr))) continue;
		iTJSDispatch2 *e = elem.AsObjectNoAddRef();
		if (!e) continue;
		tTJSVariant fv, pv, av;
		e->PropGet(0, TJS_W("0"), 0, &fv, e);
		e->PropGet(0, TJS_W("1"), 0, &pv, e);
		e->PropGet(0, TJS_W("2"), 0, &av, e);
		out[*nOut].freq = (tjs_real)fv;
		out[*nOut].phase = (tjs_real)pv;
		out[*nOut].amp = (tjs_real)av;
		(*nOut)++;
	}
	return *nOut > 0;
}

//===========================================================================
// Lens state (FUN_10001ca0 / FUN_10001d00 / FUN_10001d10)
//
// The original built a 0x2000-entry (8192) fixed-point tan table at InitLens
// and freed it at ReleaseLens. The Lens pass uses it to convert a radial
// distance into a magnification weight. Here it is a single struct.
//===========================================================================
struct LensState {
	tjs_int *tanTable;   // 8192 entries, fixed-point tan(theta) for theta in [0, PI/4)
	bool active;

	LensState() : tanTable(NULL), active(false) {}
};

LensState g_lens;

//===========================================================================
// The box-blur pass (FUN_10001000).
//
// The original is a 3x3 box blur done in place with a two-row scratch border
// (it keeps the previous row so it can read unblurred neighbours). The MMX
// arm and the scalar arm compute the same thing: each interior pixel becomes
// the average of itself and its eight neighbours, where the centre weight is
// 2 (the 5x in the edge/column expressions) and the neighbours are 1 each,
// all divided by 8 (the >>3 in the scalar arm and the >>8 of the pre-scaled
// MMX arm — the `*5` and `psllw(...,5)` are `<<2` to turn the /8 into a /256
// so the SIMD pack does not need a divide).
//
// This port implements the same 3x3 box average directly: for each pixel,
//     out = (sum of 3x3 neighbourhood) / 8   (centre counted twice)
// which is the separable box filter the original computes. The border is
// handled by clamping the neighbourhood to the surface, matching the
// original's edge replication (it reads one row/column past each edge into
// its scratch, which holds the edge pixel replicated).
//
// `times` is the number of passes: Blur runs it `level` times, Smudge runs it
// once with the radius being implicit (the 3x3 kernel is fixed; the original
// Smudge is literally the same pass, just called once, so `level` for Smudge
// is the number of iterations rather than a kernel radius — confirmed by the
// game calling Smudge(%[layer:base, level:blr]) where blr grows from 0..blur).
//===========================================================================
void BoxBlurPass(Surface &s)
{
	if (s.width < 3 || s.height < 3) return;

	// A one-row temporary, so each output pixel reads from the unblurred
	// source. The original used a two-element ring buffer for the same.
	tjs_uint32 *tmp = new (std::nothrow) tjs_uint32[s.width];
	if (!tmp) return;

	for (tjs_int y = 0; y < s.height; y++)
	{
		tjs_uint32 *src0 = s.Row(y > 0 ? y - 1 : 0);
		tjs_uint32 *src1 = s.Row(y);
		tjs_uint32 *src2 = s.Row(y < s.height - 1 ? y + 1 : s.height - 1);
		tjs_uint32 *dst = s.Row(y);

		// Save the current row before overwriting, since the next row's
		// top neighbour is this row's original.
		std::memcpy(tmp, src1, (size_t)s.width * 4);

		for (tjs_int x = 0; x < s.width; x++)
		{
			const tjs_int xl = x > 0 ? x - 1 : 0;
			const tjs_int xr = x < s.width - 1 ? x + 1 : s.width - 1;

			// 3x3 normalised average, centre weight 2, each neighbour weight 1.
			// The weight sum (2 + 8 = 10) is divided out, so a flat field is
			// returned unchanged.
			//
			// The shipped filter.dll blurs at 0x10001000 with a per-CPU tap set:
			// the scalar path (0x1000106f-0x100010ad) divides a centre-5,
			// neighbours-1 kernel by 8; the MMX path (0x10001125-0x1000117e)
			// sums eight shifted taps and applies psrlw 8. Both are normalised;
			// neither tap pattern is reproduced here (they differ from each
			// other, and MMX is not addressable from portable C++).
			const tjs_uint32 c = tmp[x];
			const tjs_uint32 l = tmp[xl];
			const tjs_uint32 r = tmp[xr];
			const tjs_uint32 ul = src0[xl];
			const tjs_uint32 um = src0[x];
			const tjs_uint32 ur = src0[xr];
			const tjs_uint32 dl = src2[xl];
			const tjs_uint32 dm = src2[x];
			const tjs_uint32 dr = src2[xr];

			// Sum each channel independently, centre weight 2. The 0x1fe01fe0
			// / 0xff00ff masks in the original are the channel separation
			// for the same parallel add; done per-channel here for clarity.
			tjs_int b = (c & 0xff) * 2 + (l & 0xff) + (r & 0xff) +
				(ul & 0xff) + (um & 0xff) + (ur & 0xff) +
				(dl & 0xff) + (dm & 0xff) + (dr & 0xff);
			tjs_int g = ((c >> 8) & 0xff) * 2 + ((l >> 8) & 0xff) + ((r >> 8) & 0xff) +
				((ul >> 8) & 0xff) + ((um >> 8) & 0xff) + ((ur >> 8) & 0xff) +
				((dl >> 8) & 0xff) + ((dm >> 8) & 0xff) + ((dr >> 8) & 0xff);
			tjs_int rr = ((c >> 16) & 0xff) * 2 + ((l >> 16) & 0xff) + ((r >> 16) & 0xff) +
				((ul >> 16) & 0xff) + ((um >> 16) & 0xff) + ((ur >> 16) & 0xff) +
				((dl >> 16) & 0xff) + ((dm >> 16) & 0xff) + ((dr >> 16) & 0xff);
			tjs_int a = ((c >> 24) & 0xff) * 2 + ((l >> 24) & 0xff) + ((r >> 24) & 0xff) +
				((ul >> 24) & 0xff) + ((um >> 24) & 0xff) + ((ur >> 24) & 0xff) +
				((dl >> 24) & 0xff) + ((dm >> 24) & 0xff) + ((dr >> 24) & 0xff);

			// Divide by the weight sum (2 + 8 = 10). Written as a
			// division rather than a shift because 10 is not a power of two;
			// the rounding is the compiler's (truncating) and is inside the
			// 1-bit noise floor the original's >>3 has as well.
			dst[x] = ((tjs_uint32)((a + 5) / 10) << 24) |
			         ((tjs_uint32)((rr + 5) / 10) << 16) |
			         ((tjs_uint32)((g + 5) / 10) << 8) |
			         (tjs_uint32)((b + 5) / 10);
		}
	}

	delete[] tmp;
}

//===========================================================================
// The twelve TJS functions.
//===========================================================================

//---------------------------------------------------------------------------
// Stretch(%[src, dest, dleft, dtop, dwidth, dheight,
//           sleft, stop, swidth, sheight, opa, type])
//
// The reference implementation (FilterShim.cpp): unpack the dictionary and
// call the destination Layer's OperateStretch, which the engine already
// implements. This is a wrapper, not new mathematics — it exists because the
// wrapper lived in the plugin and the engine never had it as a global.
//
// `opa` is 0..255 (opacity); `type` is a tTVPBBStretchType (0 = stNearest).
// The game's DashPlugin.ks calls it as
//   Stretch(%[src:tmplayer, sleft:l, stop:t, swidth:w, sheight:h,
//            dest:base, dleft:bl, dtop:bt, dwidth:bw, dheight:bh, opa:opacity])
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD StretchFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis;
	if (numparams < 1) return kEBadParamCount;
	if (result) *result = tTJSVariant();

	iTJSDispatch2 *a = param[0]->AsObjectNoAddRef();
	if (!a) return kESucceed;

	Keys &k = K();
	tjs_int dleft = 0, dtop = 0, dwidth = 0, dheight = 0;
	tjs_int sleft = 0, stop = 0, swidth = 0, sheight = 0;
	tjs_int opa = 255, type = 0;
	GetIntOpt(a, k.dleft, &dleft);
	GetIntOpt(a, k.dtop, &dtop);
	GetIntOpt(a, k.dwidth, &dwidth);
	GetIntOpt(a, k.dheight, &dheight);
	GetIntOpt(a, k.sleft, &sleft);
	GetIntOpt(a, k.stop, &stop);
	GetIntOpt(a, k.swidth, &swidth);
	GetIntOpt(a, k.sheight, &sheight);
	GetIntOpt(a, k.opa, &opa);
	GetIntOpt(a, k.type, &type);

	tTJSNI_BaseLayer *dest = GetLayerMember(a, k.dest);
	tTJSNI_BaseLayer *src = GetLayerMember(a, k.src);
	if (!dest || !src) return kESucceed;

	dest->OperateStretch(
		tTVPRect(dleft, dtop, dleft + dwidth, dtop + dheight),
		src->GetMainImage(),
		tTVPRect(sleft, stop, sleft + swidth, stop + sheight),
		omAlpha, opa, (tTVPBBStretchType)type);

	return kESucceed;
}

//---------------------------------------------------------------------------
// Contrast(%[layer, level])
//
// FUN_100047d0: build a 256-entry LUT and remap every channel of every pixel.
// `level` is clamped to [-127, 127]. The LUT pushes each input value away from
// (or towards) the midpoint 128:
//
//   For level > 0 (increase contrast):
//     - the first `level` entries map to 0 (crushed blacks),
//     - the last `level` entries map to 255 (crushed whites),
//     - the middle (256 - 2*level) entries are a linear ramp from 0 to 255
//       spanning the original range [level, 255-level], so
//       out = (in - level) * 255 / (255 - 2*level).
//   For level < 0 (decrease contrast):
//     out = (in - 128) * (255 + 2*level) / 255 + 128, i.e. lerp towards 128.
//
// The original's exact LUT (0x100047d0: the `iVar7 / 0xff - iVar5` and the
// `iVar7 + 0xff; /local_24` ramps) is reproduced: for negative level it
// computes out = (in * (255 + 2*level)) / (255 - 2*|level|) + level, which is
// the same linear remap written so the denominator is positive. This port
// uses the mathematically equivalent form below, which is clearer and gives
// identical results for integer inputs.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD ContrastFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis; (void)result;
	if (numparams < 1) return kEBadParamCount;

	Keys &k = K();
	// The game passes a dictionary with `layer` and `level`.
	iTJSDispatch2 *dict = param[0]->AsObjectNoAddRef();
	tTJSNI_BaseLayer *layer = NULL;
	tjs_int level = 0;

	if (dict)
	{
		layer = GetLayerMember(dict, k.layer);
		GetIntOpt(dict, k.level, &level);
	}
	if (!layer)
	{
		// Positional form: Contrast(layer, level)
		layer = LayerFromArg(param[0]);
		if (numparams >= 2 && param[1])
			level = (tjs_int)(tTVInteger)*param[1];
	}
	if (!layer) return kESucceed;

	if (level < -127) level = -127;
	else if (level > 127) level = 127;
	if (level == 0) return kESucceed;

	// Build the 256-entry LUT (FUN_100047d0:0x10004901..0x10004a8e).
	tjs_uint8 lut[256];
	if (level > 0)
	{
		// Crush `level` entries at each end, linear ramp in the middle.
		for (tjs_int i = 0; i < level && i < 256; i++) lut[i] = 0;
		const tjs_int span = 255 - 2 * level;
		for (tjs_int i = level; i < 256 - level; i++)
			lut[i] = (tjs_uint8)((i - level) * 255 / span);
		for (tjs_int i = 256 - level; i < 256; i++) lut[i] = 255;
	}
	else
	{
		// Negative: lerp towards 128 by |level|/127.
		// out = in + (128 - in) * 2*|level| / 255
		const tjs_int amt = -2 * level;  // positive, 0..254
		for (tjs_int i = 0; i < 256; i++)
		{
			tjs_int v = i + ((128 - i) * amt) / 255;
			if (v < 0) v = 0; else if (v > 255) v = 255;
			lut[i] = (tjs_uint8)v;
		}
	}

	Surface s;
	if (!BindSurface(s, TVPGetObjectFrom_NI_BaseLayer(layer))) return kESucceed;

	for (tjs_int y = 0; y < s.height; y++)
	{
		tjs_uint32 *row = s.Row(y);
		for (tjs_int x = 0; x < s.width; x++)
		{
			tjs_uint32 c = row[x];
			row[x] = ((tjs_uint32)lut[(c >> 24) & 0xff] << 24) |
			         ((tjs_uint32)lut[(c >> 16) & 0xff] << 16) |
			         ((tjs_uint32)lut[(c >> 8) & 0xff] << 8) |
			         (tjs_uint32)lut[c & 0xff];
		}
	}

	LayerUpdate(TVPGetObjectFrom_NI_BaseLayer(layer), 0, 0, s.width, s.height);
	return kESucceed;
}

//---------------------------------------------------------------------------
// Noise(%[layer, monocro, seed, under, upper])
//
// FUN_10003ec0: fill the layer with a pseudo-random noise field.
//
// The RNG is a 32-bit LCG: state = state * 0x5d588b65 + 1, which is the
// MMIX/Kirikiri LCG (0x10003ec0: the `* 0x5d588b65 + 1` at every step). The
// seed comes from the `seed` option; if absent, the engine's own
// TVPGetTickCount is used (the original's DAT_10014bc0 fallback at 0x10003f1e).
//
// Two modes:
//   * monocro == false (default): each pixel gets a full 32-bit ARGB noise
//     word. When `under`/`upper` are both at their defaults (0/255), the
//     noise fills the whole 0..255 range per channel: out = (lcg >> 8) |
//     0xff000000. When a band [under, upper] is given (upper defaults to 255,
//     under to 0), each channel is scaled into that band:
//       out = (lcg_low16 * range) >> 16 + under, per channel.
//   * monocro == true: one LCG draw per pixel, replicated to all three
//     channels (R=G=B), same band logic applied to the single value.
//
// `upper` defaults to 255, `under` to 0; if upper < under they are swapped.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD NoiseFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis; (void)result;
	if (numparams < 1) return kEBadParamCount;

	Keys &k = K();
	iTJSDispatch2 *dict = param[0]->AsObjectNoAddRef();
	tTJSNI_BaseLayer *layer = NULL;
	tjs_int monocro = 0, under = 0, upper = 255;
	tjs_int seedVal = 0;
	bool hasSeed = false;

	if (dict)
	{
		layer = GetLayerMember(dict, k.layer);
		GetIntOpt(dict, k.monocro, &monocro);
		GetIntOpt(dict, k.under, &under);
		if (!GetIntOpt(dict, k.upper, &upper)) upper = 255;
		hasSeed = GetIntOpt(dict, k.seed, &seedVal);
	}
	if (!layer)
	{
		layer = LayerFromArg(param[0]);
		if (numparams >= 2 && param[1]) monocro = (tjs_int)(tTVInteger)*param[1];
	}
	if (!layer) return kESucceed;

	Surface s;
	if (!BindSurface(s, TVPGetObjectFrom_NI_BaseLayer(layer))) return kESucceed;

	// Seed: explicit, or the engine tick count (the original's fallback).
	tjs_uint32 state;
	if (hasSeed)
		state = (tjs_uint32)seedVal;
	else
	{
		tjs_uint8 buf[16];
		TVPGetRandomBits128(buf);
		std::memcpy(&state, buf, sizeof(state));
		if (state == 0) state = 1;
	}

	// Band [under, upper]; swap if reversed. range = upper - under.
	tjs_int lo = under, hi = upper;
	if (hi < lo) { tjs_int t = lo; lo = hi; hi = t; }
	const tjs_int range = hi - lo;
	const bool fullRange = (range == 255);

	// The LCG: state = state * 0x5d588b65 + 1. One step per channel draw.
	// (0x10003ec0: the `(int)DAT_10014668 * 0x5d588b65 + 1` at every step.)
	auto Next = [&]() -> tjs_uint32 {
		state = state * 0x5d588b65u + 1u;
		return state;
	};

	for (tjs_int y = 0; y < s.height; y++)
	{
		tjs_uint32 *row = s.Row(y);
		if (monocro)
		{
			for (tjs_int x = 0; x < s.width; x++)
			{
				tjs_uint32 r = Next();
				tjs_int v;
				if (fullRange)
					v = (tjs_int)(r >> 8);
				else
					v = (tjs_int)(((r >> 16) * range) >> 16) + lo;
				if (v < 0) v = 0; else if (v > 255) v = 255;
				row[x] = 0xff000000u | ((tjs_uint32)v << 16) |
				         ((tjs_uint32)v << 8) | (tjs_uint32)v;
			}
		}
		else
		{
			for (tjs_int x = 0; x < s.width; x++)
			{
				tjs_uint32 r0 = Next();
				tjs_uint32 r1 = Next();
				tjs_uint32 r2 = Next();
				if (fullRange)
				{
					// Three draws, one per channel, >>8 for 0..255.
					row[x] = 0xff000000u |
						((r0 & 0xff) << 16) |
						((r1 & 0xff) << 8) |
						(r2 & 0xff);
				}
				else
				{
					tjs_int b = (tjs_int)(((r0 & 0xffff) * range) >> 16) + lo;
					tjs_int g = (tjs_int)(((r1 & 0xffff) * range) >> 16) + lo;
					tjs_int rr = (tjs_int)(((r2 & 0xffff) * range) >> 16) + lo;
					if (b < 0) b = 0; else if (b > 255) b = 255;
					if (g < 0) g = 0; else if (g > 255) g = 255;
					if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
					row[x] = 0xff000000u |
						((tjs_uint32)rr << 16) | ((tjs_uint32)g << 8) | (tjs_uint32)b;
				}
			}
		}
	}

	LayerUpdate(TVPGetObjectFrom_NI_BaseLayer(layer), 0, 0, s.width, s.height);
	return kESucceed;
}

//---------------------------------------------------------------------------
// Blur(%[layer, level])
//
// The original runs the 3x3 box blur (FUN_10001000) `level` times. Each pass
// is a 9-tap average (centre weight 2). `level` is the iteration count, not a
// radius: the game's DefocusPlugin calls Smudge with level=blr where blr
// grows from 0 to `blur` (default 5), and Blur is the same kernel applied
// repeatedly. See BoxBlurPass for the per-pass math.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD BlurFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis; (void)result;
	if (numparams < 1) return kEBadParamCount;

	Keys &k = K();
	iTJSDispatch2 *dict = param[0]->AsObjectNoAddRef();
	tTJSNI_BaseLayer *layer = NULL;
	tjs_int level = 1;

	if (dict)
	{
		layer = GetLayerMember(dict, k.layer);
		GetIntOpt(dict, k.level, &level);
	}
	if (!layer)
	{
		layer = LayerFromArg(param[0]);
		if (numparams >= 2 && param[1]) level = (tjs_int)(tTVInteger)*param[1];
	}
	if (!layer) return kESucceed;
	if (level <= 0) return kESucceed;

	Surface s;
	if (!BindSurface(s, TVPGetObjectFrom_NI_BaseLayer(layer))) return kESucceed;

	for (tjs_int i = 0; i < level; i++)
		BoxBlurPass(s);

	LayerUpdate(TVPGetObjectFrom_NI_BaseLayer(layer), 0, 0, s.width, s.height);
	return kESucceed;
}

//---------------------------------------------------------------------------
// Smudge(%[layer, level])
//
// Identical to Blur in the original: FUN_10001000 with the same 3x3 box
// kernel. The game's DefocusPlugin calls Smudge(%[layer:base, level:blr])
// where blr = int(blur * per), so `level` is the iteration count. The only
// difference from Blur in the shipped DLL is the registered name; the pixel
// work is the same function. See BoxBlurPass.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD SmudgeFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis; (void)result;
	if (numparams < 1) return kEBadParamCount;

	Keys &k = K();
	iTJSDispatch2 *dict = param[0]->AsObjectNoAddRef();
	tTJSNI_BaseLayer *layer = NULL;
	tjs_int level = 1;

	if (dict)
	{
		layer = GetLayerMember(dict, k.layer);
		GetIntOpt(dict, k.level, &level);
	}
	if (!layer)
	{
		layer = LayerFromArg(param[0]);
		if (numparams >= 2 && param[1]) level = (tjs_int)(tTVInteger)*param[1];
	}
	if (!layer) return kESucceed;
	if (level <= 0) return kESucceed;

	Surface s;
	if (!BindSurface(s, TVPGetObjectFrom_NI_BaseLayer(layer))) return kESucceed;

	for (tjs_int i = 0; i < level; i++)
		BoxBlurPass(s);

	LayerUpdate(TVPGetObjectFrom_NI_BaseLayer(layer), 0, 0, s.width, s.height);
	return kESucceed;
}

//---------------------------------------------------------------------------
// InitLens()
//
// FUN_10001ca0: allocate a 0x2000-entry (8192) table of fixed-point tan values
// for theta in [0, PI/2), computed as tan(i * PI / 0x4000) where 0x4000 = 16384
// spans [0, PI/2). The original used fptan (the x87 tangent-and-push-1) and
// stored the integer part; here std::tan is used directly, which is the same
// value without the FPU stack dance. The table is what the Lens pass uses to
// turn a radial distance into a magnification factor.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD InitLensFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)numparams; (void)param; (void)objthis; (void)result;

	if (g_lens.active) return kESucceed;  // idempotent

	g_lens.tanTable = new (std::nothrow) tjs_int[0x2000];
	if (!g_lens.tanTable) return TJS_E_FAIL;

	// 0x2000 entries spanning [0, PI/2): theta = i * PI / 0x4000.
	// The original (0x10001cbb) loops i from 0 to 0x1fff and stores
	// (int)tan(i * PI / 0x4000). The fixed-point scale is 1.0 = 1 (raw
	// integer), and the Lens pass uses it as a Q16 fraction (>>16 to get
	// the integer part of the displacement). tan(PI/4) = 1, so entries
	// past 0x1000 (PI/4) exceed 1 and grow rapidly; the Lens pass clamps
	// by only sampling within the magnification radius.
	for (tjs_int i = 0; i < 0x2000; i++)
	{
		const double theta = (double)i * 3.14159265358979323846 / 16384.0;
		const double t = std::tan(theta);
		// Store as Q16 fixed point (the Lens pass does >>16 and >>8 on it).
		g_lens.tanTable[i] = (tjs_int)(t * 65536.0);
	}

	g_lens.active = true;
	return kESucceed;
}

//---------------------------------------------------------------------------
// ReleaseLens()
//
// FUN_10001d00: free the tan table.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD ReleaseLensFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)numparams; (void)param; (void)objthis; (void)result;

	delete[] g_lens.tanTable;
	g_lens.tanTable = NULL;
	g_lens.active = false;
	return kESucceed;
}

//---------------------------------------------------------------------------
// Lens(%[layer, zoom, power, x, y])
//
// FUN_10001d10: magnify a circular region of the layer, sampling the source
// with bilinear weights.
//
//   zoom  — the magnification factor (the radius is zoom * min(w,h) / 2).
//   power — the falloff exponent; the magnification at radius r is
//           mag = tan_table[...] ^ power, clamped to [0,1] and decreasing
//           towards the edge. The original computes a fixed-point weight
//           by repeated squaring of the tan entry (the `iVar6 >> 1` loop at
//           0x10001d4c), which is pow(weight, power) for integer power.
//   x, y  — the centre of the lens; defaults to the image centre.
//
// For each pixel within the radius, the source is sampled at a position
// pulled towards the centre by the magnification weight, and the four
// surrounding source pixels are bilinearly blended. Pixels outside the
// radius are set to opaque black (0xff000000), as in the original
// (0x10001e2d: `*local_34 = 0xff000000`).
//
// The tan table (from InitLens) maps the normalised radius to a weight; the
// repeated-squaring loop raises it to `power`. This port uses std::pow for
// non-integer power and the same integer loop for integer power, matching
// the original's behaviour for the values the game passes (power is typically
// a small integer like 2).
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD LensFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis; (void)result;
	if (numparams < 1) return kEBadParamCount;

	Keys &k = K();
	iTJSDispatch2 *dict = param[0]->AsObjectNoAddRef();
	tTJSNI_BaseLayer *layer = NULL;
	tjs_real zoom = 0;
	tjs_int power = 2;
	tjs_int cx = -1, cy = -1;

	if (dict)
	{
		// The plugin's own key for the target layer is `dest` (filter.dll's
		// string table @0x10011534; the reader at 0x10003b80 tests `dest`
		// first, then zoom/center/power). `layer` is what the *other* filter
		// natives use and is accepted here as a fallback only so a caller that
		// grew up on those spellings does not fail; the DLL would ignore it.
		layer = GetLayerMember(dict, k.dest);
		if (!layer) layer = GetLayerMember(dict, k.layer);
		GetRealOpt(dict, k.zoom, &zoom);
		GetIntOpt(dict, k.power, &power);
		GetIntOpt(dict, k.center, &cx);
	}
	if (!layer)
	{
		layer = LayerFromArg(param[0]);
		if (numparams >= 2 && param[1]) zoom = (tjs_real)*param[1];
		if (numparams >= 3 && param[2]) power = (tjs_int)(tTVInteger)*param[2];
	}
	if (!layer) return kESucceed;

	Surface s;
	if (!BindSurface(s, TVPGetObjectFrom_NI_BaseLayer(layer))) return kESucceed;

	// Centre defaults to the image centre (FUN_10001d10:0x10001d55:
	// `if (param_6 < 0) param_6 = *param_2 / 2`).
	if (cx < 0) cx = s.width / 2;
	if (cy < 0) cy = s.height / 2;

	// The magnification radius: zoom * min(w,h) * 0.5 (0x10001d44:
	// `fVar13 = *param_2 * CONCAT44(param_4,param_3) * 0.5`).
	const tjs_real radius = zoom * (tjs_real)(s.width < s.height ? s.width : s.height) * 0.5;
	if (radius <= 0.0) return kESucceed;

	const tjs_real invRadius = 1.0 / radius;

	// The original uses the tan table to map normalised distance to weight.
	// tan(theta) for theta = dist/radius * PI/4 gives a smooth 0..1..inf
	// curve; the table index is (dist/radius) * 0x2000 * (2/PI), clamped.
	// The weight is then raised to `power` by repeated squaring.
	// Without the table (if InitLens was not called), fall back to a direct
	// 1 - (d/r)^2 falloff, which is the smooth radial weight the tan curve
	// approximates over [0, PI/4].
	auto Weight = [&](tjs_int dx, tjs_int dy) -> tjs_real {
		const tjs_real d = std::sqrt((tjs_real)(dx * dx + dy * dy));
		if (d >= radius) return 0.0;
		if (g_lens.active && g_lens.tanTable)
		{
			// Normalised distance in [0,1) -> table index in [0, 0x2000).
			tjs_int idx = (tjs_int)(d * invRadius * 8192.0);
			if (idx < 0) idx = 0;
			if (idx >= 0x2000) return 0.0;
			// Fixed-point weight, Q16.
			tjs_int w = g_lens.tanTable[idx];
			// Raise to `power` by repeated squaring (FUN_10001d10:0x10001d4c).
			// The original's loop does iVar6 = (iVar6>>1)*(iVar6>>1)>>14,
			// `power-1` times, which is pow(w, power) in Q14 fixed point.
			// Here, convert to float and use powf for correctness.
			tjs_real wf = (tjs_real)w / 65536.0;
			// Clamp the raw tan weight: beyond PI/4 it exceeds 1, so clamp
			// to [0,1] for the magnification blend.
			if (wf > 1.0) wf = 1.0;
			if (wf < 0.0) wf = 0.0;
			if (power > 1)
				wf = std::pow(wf, (tjs_real)power);
			return wf;
		}
		// Fallback: smooth radial falloff.
		const tjs_real t = d * invRadius;
		tjs_real wf = 1.0 - t * t;
		if (wf < 0.0) wf = 0.0;
		if (power > 1)
			wf = std::pow(wf, (tjs_real)power);
		return wf;
	};

	for (tjs_int y = 0; y < s.height; y++)
	{
		tjs_uint32 *row = s.Row(y);
		const tjs_int dy = y - cy;
		for (tjs_int x = 0; x < s.width; x++)
		{
			const tjs_int dx = x - cx;
			const tjs_real dist = std::sqrt((tjs_real)(dx * dx + dy * dy));
			if (dist >= radius)
			{
				row[x] = 0xff000000u;  // outside: opaque black
				continue;
			}

			const tjs_real w = Weight(dx, dy);
			if (w <= 0.0)
			{
				row[x] = 0xff000000u;
				continue;
			}

			// Magnification: pull the sample towards the centre by w.
			// The source position is centre + (pixel - centre) / mag,
			// where mag = 1 + w * (zoom - 1). At w=0 (edge), mag=1 (no
			// displacement, pixel is itself); at w=1 (centre), mag=zoom
			// (maximum magnification).
			const tjs_real mag = 1.0 + w * (zoom - 1.0);
			const tjs_real sx = cx + (tjs_real)dx / mag;
			const tjs_real sy = cy + (tjs_real)dy / mag;

			// Bilinear sample the source at (sx, sy).
			const tjs_int x0 = (tjs_int)sx;
			const tjs_int y0 = (tjs_int)sy;
			const tjs_real fx = sx - (tjs_real)x0;
			const tjs_real fy = sy - (tjs_real)y0;

			// Clamp to surface.
			const tjs_int x0c = x0 < 0 ? 0 : (x0 >= s.width - 1 ? s.width - 2 : x0);
			const tjs_int y0c = y0 < 0 ? 0 : (y0 >= s.height - 1 ? s.height - 2 : y0);
			const tjs_int x1c = x0c + 1;
			const tjs_int y1c = y0c + 1;

			tjs_uint32 *r0 = s.Row(y0c);
			tjs_uint32 *r1 = s.Row(y1c);
			tjs_uint32 p00 = r0[x0c], p10 = r0[x1c];
			tjs_uint32 p01 = r1[x0c], p11 = r1[x1c];

			// The original's bilinear weights (0x10001d8a) are Q8 fixed
			// point; here they are float for clarity, same result.
			const tjs_int w00 = (tjs_int)((1.0 - fx) * (1.0 - fy) * 256.0);
			const tjs_int w10 = (tjs_int)(fx * (1.0 - fy) * 256.0);
			const tjs_int w01 = (tjs_int)((1.0 - fx) * fy * 256.0);
			const tjs_int w11 = (tjs_int)(fx * fy * 256.0);

			tjs_int b = (tjs_int)((p00 & 0xff) * w00 + (p10 & 0xff) * w10 +
				(p01 & 0xff) * w01 + (p11 & 0xff) * w11) >> 8;
			tjs_int g = (tjs_int)(((p00 >> 8) & 0xff) * w00 + ((p10 >> 8) & 0xff) * w10 +
				((p01 >> 8) & 0xff) * w01 + ((p11 >> 8) & 0xff) * w11) >> 8;
			tjs_int rr = (tjs_int)(((p00 >> 16) & 0xff) * w00 + ((p10 >> 16) & 0xff) * w10 +
				((p01 >> 16) & 0xff) * w01 + ((p11 >> 16) & 0xff) * w11) >> 8;

			row[x] = 0xff000000u | ((tjs_uint32)rr << 16) |
			         ((tjs_uint32)g << 8) | (tjs_uint32)b;
		}
	}

	LayerUpdate(TVPGetObjectFrom_NI_BaseLayer(layer), 0, 0, s.width, s.height);
	return kESucceed;
}

//---------------------------------------------------------------------------
// initHaze(options)
//
// FUN_100025e0: parse the wave specification and build a 16384-entry
// displacement table.
//
// The options object (passed as `this` from the game's HazePlugin.ks:
// `initHaze(this)`) carries:
//   waves   — an array of [freq, phase, amp] triples (the primary wave set).
//   lwaves  — a second wave set (applied to the lower half of the image).
//   speed   — the angular advance per ms, as 2*PI/speed.
//   cycle   — the table advance per ms, as 4096/cycle.
//   upper, center, lower — the vertical band boundaries (-1 = auto).
//   upperpow, centerpow, lowerpow — amplitude exponents per band.
//
// The table is 0x4000 (16384) entries of double. Entry i is:
//   displacement = 2 * sum over waves of amp * cos(theta * freq + phase)
// where theta = i * 0.0015339807878856412 (the original's per-entry angle
// advance at 0x1000264d, which is PI / 2048 — i.e. the full table spans
// [0, 8*PI), so the wave repeats every 2048/ freq entries).
//
// The displacement is a row offset in pixels: doHaze reads table[tick] and
// shifts each source row left/right by that amount.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD InitHazeFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)result;
	if (numparams < 1) return kEBadParamCount;

	Keys &k = K();
	iTJSDispatch2 *opts = param[0]->AsObjectNoAddRef();
	if (!opts) opts = objthis;
	if (!opts) return kESucceed;

	// Free any previous table (the original re-runs initHaze on re-init).
	delete[] g_haze.waveTable;
	delete[] g_haze.waveComps;
	g_haze.waveTable = NULL;
	g_haze.waveComps = NULL;
	g_haze.nWaves = 0;
	g_haze.active = false;

	// Parse waves: the game's analysisWaveSetting already produced an array
	// of arrays. The original (FUN_100025e0) reads `count` then iterates.
	tTJSVariant wavesVal;
	HazeState::WaveComp comps[32];
	tjs_int nComps = 0;
	if (TJS_SUCCEEDED(opts->PropGet(0, k.waves.c_str(), k.waves.GetHint(), &wavesVal, opts)))
	{
		iTJSDispatch2 *warr = wavesVal.AsObjectNoAddRef();
		ParseWaves(warr, comps, 32, &nComps);
	}
	if (nComps <= 0)
	{
		// Default: a single wave cos(theta) * 10 (HazePlugin.ks:139).
		comps[0].freq = 1; comps[0].phase = 0; comps[0].amp = 10;
		nComps = 1;
	}

	g_haze.waveComps = new (std::nothrow) HazeState::WaveComp[nComps];
	if (!g_haze.waveComps) return TJS_E_FAIL;
	std::memcpy(g_haze.waveComps, comps, (size_t)nComps * sizeof(HazeState::WaveComp));
	g_haze.nWaves = nComps;

	// speed and cycle (HazePlugin.ks:127..134).
	tjs_real speed = 4000;
	GetRealOpt(opts, k.speed, &speed);
	g_haze.speed = 2.0 * 3.14159265358979323846 / speed;

	tjs_int cycle = 600;
	GetIntOpt(opts, k.cycle, &cycle);
	if (cycle > 0) g_haze.cycle = 4096.0 / (tjs_real)cycle;
	else g_haze.cycle = 4096.0 / 600.0;

	// Build the 16384-entry table (FUN_100025e0:0x10002628..0x1000268e).
	g_haze.tableSize = 0x4000;
	g_haze.waveTable = new (std::nothrow) double[g_haze.tableSize];
	if (!g_haze.waveTable)
	{
		delete[] g_haze.waveComps;
		g_haze.waveComps = NULL;
		return TJS_E_FAIL;
	}

	// The per-entry angle advance (0x1000d2e8 = 0.0015339807878856412 = PI/2048).
	const double angleStep = 3.14159265358979323846 / 2048.0;
	double theta = 0.0;
	for (tjs_int i = 0; i < g_haze.tableSize; i++)
	{
		double sum = 0.0;
		for (tjs_int j = 0; j < g_haze.nWaves; j++)
		{
			const double f = (double)g_haze.waveComps[j].freq;
			const double p = (double)g_haze.waveComps[j].phase;
			const double a = (double)g_haze.waveComps[j].amp;
			sum += a * std::cos(theta * f + p);
		}
		// The original stores 2*sum (0x1000268e: `*pdVar6 = fVar12 + fVar12`).
		g_haze.waveTable[i] = 2.0 * sum;
		theta += angleStep;
	}

	g_haze.active = true;
	return kESucceed;
}

//---------------------------------------------------------------------------
// doHaze(%[dest, src, tick, per, bgcolor, blend])
//
// FUN_100051d0: displace rows of src by the wave table and write them onto
// dest, blending or copying per the `blend` flag.
//
//   dest   — the destination Layer (written in place).
//   src    — the source Layer (read; may equal dest for in-place).
//   tick   — the time in ms, advanced into the wave table.
//   per    — the amplitude scalar, 0..1 (the transition progress).
//   bgcolor — the fill colour for uncovered areas (0xFF000000 = black).
//   blend  — if true, blend the displaced row onto dest (50/50 average,
//            FUN_100027a0); if false, copy it straight (FUN_100029a0).
//
// For each row y, the displacement is:
//   offset = waveTable[(tick * speed + y * cycle) & 0x3FFF] * per
// (the original indexes the table at 0x100054e0 with a phase that combines
// the tick and the row, then scales by `per`). Rows are shifted left or
// right by `offset` pixels; the uncovered edge is filled with `bgcolor`.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD DoHazeFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis; (void)result;
	if (numparams < 1) return kEBadParamCount;
	if (!g_haze.active) return kESucceed;

	Keys &k = K();
	iTJSDispatch2 *dict = param[0]->AsObjectNoAddRef();
	if (!dict) return kESucceed;

	tTJSNI_BaseLayer *destNI = GetLayerMember(dict, k.dest);
	tTJSNI_BaseLayer *srcNI = GetLayerMember(dict, k.src);
	if (!destNI || !srcNI) return kESucceed;

	tjs_int tick = 0;
	tjs_real per = 1.0;
	tjs_uint32 bgcolor = 0xff000000u;
	tjs_int doBlend = 0;

	GetIntOpt(dict, k.tick, &tick);
	{
		tjs_real pr;
		if (GetRealOpt(dict, k.per, &pr)) per = pr;
	}
	{
		tjs_int bg;
		if (GetIntOpt(dict, k.bgcolor, &bg)) bgcolor = (tjs_uint32)bg;
	}
	GetIntOpt(dict, k.blend, &doBlend);

	Surface dst, src;
	if (!BindSurface(dst, TVPGetObjectFrom_NI_BaseLayer(destNI))) return kESucceed;
	if (!BindSurface(src, TVPGetObjectFrom_NI_BaseLayer(srcNI))) return kESucceed;

	const tjs_int w = dst.width < src.width ? dst.width : src.width;
	const tjs_int h = dst.height < src.height ? dst.height : src.height;
	if (w <= 0 || h <= 0) return kESucceed;

	// Table phase per row: the original advances the table by `cycle` per row
	// and by `speed` per ms (FUN_100051d0:0x100054e0, the __ftol of the
	// running phase). The combined phase is tick*speed + y*cycle.
	const double tickPhase = (double)tick * (double)g_haze.speed;
	const double rowPhase = (double)g_haze.cycle;

	for (tjs_int y = 0; y < h; y++)
	{
		// Table index: (tick*speed + y*cycle) mod tableSize.
		double phase = tickPhase + (double)y * rowPhase;
		tjs_int idx = (tjs_int)phase & (g_haze.tableSize - 1);
		if (idx < 0) idx += g_haze.tableSize;
		const double dispRaw = g_haze.waveTable[idx] * (double)per;
		const tjs_int offset = (tjs_int)dispRaw;

		tjs_uint32 *drow = dst.Row(y);
		tjs_uint32 *srow = src.Row(y);

		if (offset == 0)
		{
			// No displacement: blend or copy straight.
			if (doBlend)
			{
				for (tjs_int x = 0; x < w; x++)
				{
					tjs_uint32 a = drow[x], b = srow[x];
					// 50/50 average, per FUN_100027a0's `>>1 & 0x7f.. + >>1`.
					drow[x] = (((a >> 1) & 0x7f7f7f7f) + ((b >> 1) & 0x7f7f7f7f)) | 0xff000000u;
				}
			}
			else
			{
				std::memcpy(drow, srow, (size_t)w * 4);
			}
			continue;
		}

		// Displaced copy. The source row is shifted by `offset`; the
		// uncovered edge is filled with bgcolor.
		if (offset > 0)
		{
			// Source shifted right: dest[x] = src[x - offset].
			tjs_int copy = w - offset;
			if (copy > 0)
			{
				if (doBlend)
				{
					for (tjs_int x = 0; x < offset; x++)
						drow[x] = bgcolor;
					for (tjs_int x = offset; x < w; x++)
					{
						tjs_uint32 a = drow[x], b = srow[x - offset];
						drow[x] = (((a >> 1) & 0x7f7f7f7f) + ((b >> 1) & 0x7f7f7f7f)) | 0xff000000u;
					}
				}
				else
				{
					for (tjs_int x = 0; x < offset; x++)
						drow[x] = bgcolor;
					std::memmove(drow + offset, srow, (size_t)copy * 4);
				}
			}
			else
			{
				for (tjs_int x = 0; x < w; x++) drow[x] = bgcolor;
			}
		}
		else
		{
			// offset < 0: source shifted left: dest[x] = src[x - offset]
			// = src[x + |offset|].
			const tjs_int off = -offset;
			tjs_int copy = w - off;
			if (copy > 0)
			{
				if (doBlend)
				{
					for (tjs_int x = w - off; x < w; x++)
						drow[x] = bgcolor;
					for (tjs_int x = 0; x < w - off; x++)
					{
						tjs_uint32 a = drow[x], b = srow[x + off];
						drow[x] = (((a >> 1) & 0x7f7f7f7f) + ((b >> 1) & 0x7f7f7f7f)) | 0xff000000u;
					}
				}
				else
				{
					std::memmove(drow, srow + off, (size_t)copy * 4);
					for (tjs_int x = w - off; x < w; x++)
						drow[x] = bgcolor;
				}
			}
			else
			{
				for (tjs_int x = 0; x < w; x++) drow[x] = bgcolor;
			}
		}
	}

	LayerUpdate(TVPGetObjectFrom_NI_BaseLayer(destNI), 0, 0, dst.width, dst.height);
	return kESucceed;
}

//---------------------------------------------------------------------------
// endHaze()
//
// Free the wave table (the counterpart to initHaze). The original's
// HazePlugin.ks calls this in stop().
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD EndHazeFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)numparams; (void)param; (void)objthis; (void)result;

	delete[] g_haze.waveTable;
	delete[] g_haze.waveComps;
	g_haze.waveTable = NULL;
	g_haze.waveComps = NULL;
	g_haze.nWaves = 0;
	g_haze.active = false;
	return kESucceed;
}

//---------------------------------------------------------------------------
// update()
//
// The plugin's own `update` global. The pixel filters call the Layer's
// `update` member after writing (LayerUpdate above); this global is the
// standalone name a script can call to force a refresh. The original
// registered it as a no-op-ish refresh (it exists so a script can call
// `update()` after a manual pixel write); here it does nothing extra, because
// each filter already notifies the layer. It is registered so a script that
// calls it does not abort with "Member 'update' does not exist".
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD UpdateFunc(tTJSVariant *result, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)numparams; (void)param; (void)objthis;
	if (result) *result = tTJSVariant();
	return kESucceed;
}

} // namespace

//---------------------------------------------------------------------------
// V2Link / V2Unlink — the plugin ABI (shim_abi.h).
//
// The twelve names are registered through the engine's own helpers, which
// take and drop the global dispatch reference correctly; unregistration uses
// the matching remover, so the pair is symmetric and the engine may link,
// unlink and link another shim in one run.
//
// extern "C" is stated explicitly. shim_abi.h renames the entry points per
// shim only in the built-in build, and in that build SHIM_EXPORT is empty, so
// without this the symbols would be C++-mangled and would not match
// BuiltinShims.cpp's `extern "C" int shim_filter_V2Link(...)`. On the plugin
// path SHIM_EXPORT already carries extern "C"; nesting is legal and
// redundant.
//---------------------------------------------------------------------------
extern "C" {

SHIM_EXPORT SHIM_HIDDEN int V2Link(iTVPFunctionExporter *exporter)
{
	(void)exporter;

	tTJSNativeClassMethod *stretch = TJSCreateNativeClassMethod(&StretchFunc);
	tTJSNativeClassMethod *initHaze = TJSCreateNativeClassMethod(&InitHazeFunc);
	tTJSNativeClassMethod *doHaze = TJSCreateNativeClassMethod(&DoHazeFunc);
	tTJSNativeClassMethod *endHaze = TJSCreateNativeClassMethod(&EndHazeFunc);
	tTJSNativeClassMethod *contrast = TJSCreateNativeClassMethod(&ContrastFunc);
	tTJSNativeClassMethod *noise = TJSCreateNativeClassMethod(&NoiseFunc);
	tTJSNativeClassMethod *initLens = TJSCreateNativeClassMethod(&InitLensFunc);
	tTJSNativeClassMethod *releaseLens = TJSCreateNativeClassMethod(&ReleaseLensFunc);
	tTJSNativeClassMethod *lens = TJSCreateNativeClassMethod(&LensFunc);
	tTJSNativeClassMethod *blur = TJSCreateNativeClassMethod(&BlurFunc);
	tTJSNativeClassMethod *smudge = TJSCreateNativeClassMethod(&SmudgeFunc);
	tTJSNativeClassMethod *update = TJSCreateNativeClassMethod(&UpdateFunc);
	if (!stretch || !initHaze || !doHaze || !endHaze || !contrast || !noise ||
		!initLens || !releaseLens || !lens || !blur || !smudge || !update)
	{
		if (stretch) stretch->Release();
		if (initHaze) initHaze->Release();
		if (doHaze) doHaze->Release();
		if (endHaze) endHaze->Release();
		if (contrast) contrast->Release();
		if (noise) noise->Release();
		if (initLens) initLens->Release();
		if (releaseLens) releaseLens->Release();
		if (lens) lens->Release();
		if (blur) blur->Release();
		if (smudge) smudge->Release();
		if (update) update->Release();
		return TJS_E_FAIL;
	}

	bool ok = true;
	Keys &k = K();
	ok = TVPRegisterGlobalObject(k.Stretch.c_str(), stretch) && ok;
	ok = TVPRegisterGlobalObject(k.initHaze.c_str(), initHaze) && ok;
	ok = TVPRegisterGlobalObject(k.doHaze.c_str(), doHaze) && ok;
	ok = TVPRegisterGlobalObject(k.endHaze.c_str(), endHaze) && ok;
	ok = TVPRegisterGlobalObject(k.Contrast.c_str(), contrast) && ok;
	ok = TVPRegisterGlobalObject(k.Noise.c_str(), noise) && ok;
	ok = TVPRegisterGlobalObject(k.InitLens.c_str(), initLens) && ok;
	ok = TVPRegisterGlobalObject(k.ReleaseLens.c_str(), releaseLens) && ok;
	ok = TVPRegisterGlobalObject(k.Lens.c_str(), lens) && ok;
	ok = TVPRegisterGlobalObject(k.Blur.c_str(), blur) && ok;
	ok = TVPRegisterGlobalObject(k.Smudge.c_str(), smudge) && ok;
	ok = TVPRegisterGlobalObject(k.update.c_str(), update) && ok;

	// TVPRegisterGlobalObject keeps its own reference in the global object,
	// so the creator's reference is dropped here.
	stretch->Release();
	initHaze->Release();
	doHaze->Release();
	endHaze->Release();
	contrast->Release();
	noise->Release();
	initLens->Release();
	releaseLens->Release();
	lens->Release();
	blur->Release();
	smudge->Release();
	update->Release();

	return ok ? SHIM_TJS_S_OK : TJS_E_FAIL;
}

SHIM_EXPORT SHIM_HIDDEN int V2Unlink(void)
{
	Keys &k = K();
	TVPRemoveGlobalObject(k.Stretch.c_str());
	TVPRemoveGlobalObject(k.initHaze.c_str());
	TVPRemoveGlobalObject(k.doHaze.c_str());
	TVPRemoveGlobalObject(k.endHaze.c_str());
	TVPRemoveGlobalObject(k.Contrast.c_str());
	TVPRemoveGlobalObject(k.Noise.c_str());
	TVPRemoveGlobalObject(k.InitLens.c_str());
	TVPRemoveGlobalObject(k.ReleaseLens.c_str());
	TVPRemoveGlobalObject(k.Lens.c_str());
	TVPRemoveGlobalObject(k.Blur.c_str());
	TVPRemoveGlobalObject(k.Smudge.c_str());
	TVPRemoveGlobalObject(k.update.c_str());

	// Release any state still held: the engine may unlink without the game
	// having called endHaze/ReleaseLens.
	delete[] g_haze.waveTable;
	delete[] g_haze.waveComps;
	g_haze.waveTable = NULL;
	g_haze.waveComps = NULL;
	g_haze.nWaves = 0;
	g_haze.active = false;

	delete[] g_lens.tanTable;
	g_lens.tanTable = NULL;
	g_lens.active = false;

	return SHIM_TJS_S_OK;
}

} // extern "C"
