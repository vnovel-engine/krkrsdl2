//---------------------------------------------------------------------------
// shims/extrans.cpp — Fate/stay night's Extended Transition Plug-in, in-engine.
//
// What the plugin was
// -------------------
// extrans.dll exports only V2Link/V2Unlink (fate-decompiled/exports/extrans.dll.md
// §1) and does not add TJS globals or classes. What it registers are transition
// handler *providers* with the engine, through TVPAddTransHandlerProvider, and
// removes them symmetrically through TVPRemoveTransHandlerProvider in V2Unlink
// (decompiled V2Link at 0x10001000: store exporter, then five register calls;
// V2Unlink at 0x10001030 mirrors them).
//
// Scope: why this file exists, and all seven names
// -------------------------------------------------
// overrides.txt currently lists extrans.dll as an engine-simulated no-op and
// PluginStubs.tjs answers Quake/Rain/Zoom/MotionBlur/FallenLeaf. Neither covers
// what the game actually asks for. The shipping scripts request transitions by
// *name*:
//
//   * `mosaic` — scenario/マクロ.ks:566-572 defines @prick/@prickT, whose body is
//     `@trans layer=base children=true method=mosaic time=... maxsize=...`.
//     16 call sites in the scenario files (`@prickT time=200 maxsize=20`,
//     `@prick time=300 maxsize=40`, ...), plus @prick itself.
//   * `wave` — マクロ.ks:927-939 defines @wave/@waveT, whose body is
//     `@transex * method=wave`. 8 call sites (`@wave time=400 wavetype=2`, ...).
//   * `ripple` — the reel aborts at `Cannot find transition handler ripple`,
//     so this name is live even though it has no static call site in the
//     scenario corpus; the @trans tag resolves it dynamically.
//
// ...and the name does not resolve. KAGLayer.tjs:189-233 forwards `method` to
// Layer.beginTransition, which reaches tTJSNI_BaseLayer::StartTransition, whose
// first action is TVPFindTransHandlerProvider(name) (LayerIntf.cpp:6781). The
// registry holds only the engine's own crossfade/universal/scroll
// (TransIntf.cpp:1196-1213), so the lookup throws TVPCannotFindTransHander
// ("Cannot find transition handler %1", src/core/msg/sdl2/MsgLoad.cpp:162).
// Nothing in TJS catches it around the trans path, so it reaches Conductor's
// timerCallback catch (system/Conductor.tjs:163) and aborts the scene. So the
// no-op disposition is wrong for these names: without them the scenes die at
// the transition.
//
// This file ports all SEVEN handlers the upstream extrans.dll registers
// (Main.cpp:32-36 calls five Register* functions; one of them,
// RegisterRotateTransHandlerProvider, spawns three providers —
// rotatetrans.cpp:487-493 — so seven total):
//
//   mosaic, wave, turn, ripple, rotatezoom, rotatevanish, rotateswap
//
// Each is a faithful port of the upstream handler: mosaic from mosaic.cpp,
// wave from wave.cpp, turn from turn.cpp (+ turntrans_table.cpp), ripple from
// ripple.cpp, and rotatezoom/rotatevanish/rotateswap from rotatetrans.cpp +
// rotatebase.cpp. The binary's name strings are verified in .rdata at the
// addresses tabulated in EXTRANS-HANDLERS.md (the `)turn` artifact was a
// Ghidra alignment error; the real name is `turn`).
//
// Two consumers (see shim_abi.h)
// ------------------------------
// One source text builds as a standalone Windows PE plugin *and* compiled into
// the engine behind KRKRSDL2_BUILTIN_SHIMS. A shim may not include engine
// headers: those exist on only one of those two paths. So everything this file
// needs from the engine comes across the ABI — the transition interfaces below
// are declared locally, and the pixel primitives are resolved by name through
// the exporter, which is precisely what the original plugin did (its
// tp_stub.cpp resolved every engine function through the same exporter, and
// common.h/mosaic.cpp reached the pixel routines through that stub).
//
// State contract (BuiltinShims.h): all state is file-static in this translation
// unit, and everything V2Link registers, V2Unlink removes.
//---------------------------------------------------------------------------

#define SHIM_NAME extrans
#include "shim_abi.h"

#include <stdint.h>
#include <math.h>
#include <string.h>

//---------------------------------------------------------------------------
// The TJS ABI types, declared locally
//---------------------------------------------------------------------------
// A shim cannot include tjsTypes.h (it must build on both paths), so the few
// types that cross the boundary are restated here. They must match
// external/krkrz/tjs2/tjsTypes.h exactly or the vtable this file builds will not
// line up with the one the engine calls through.
typedef int32_t tjs_int32;                     // tjsTypes.h:124/133
typedef uint32_t tjs_uint32;                   // tjsTypes.h:125/134
typedef int64_t tjs_int64;                     // tjsTypes.h:126/135
typedef uint64_t tjs_uint64;
typedef int tjs_int;                           // tjsTypes.h:65/154
typedef unsigned int tjs_uint;                 // tjsTypes.h:66/155
typedef tjs_int32 tjs_error;                   // tjsTypes.h:303

// tjs_char is 16-bit on every platform, but it is spelled two different ways:
// MSVC's wchar_t, and (on GCC with a 32-bit wchar_t) char16_t. tjsTypes.h picks
// between them on __WCHAR_MAX__ (tjsTypes.h:69-73, 141-148), and TJS_W follows
// the same split (tjsTypes.h:100, 282-284). GetName hands a string of this type
// to the engine, so the choice has to match the host build exactly.
#if defined(_WIN32) && !defined(__GNUC__)
typedef wchar_t tjs_char;
#  define SHIM_TJS_W(X) L##X
#elif defined(__WCHAR_MAX__) && (__WCHAR_MAX__ != 0x7fff && __WCHAR_MAX__ != 0xffff)
typedef char16_t tjs_char;
#  define SHIM_TJS_W(X) u##X
#else
typedef wchar_t tjs_char;
#  define SHIM_TJS_W(X) L##X
#endif

// Interface-method calling convention. __cdecl on Windows 32-bit, where the
// plugin ABI is 32-bit and the convention is load-bearing; nothing on GCC
// (tjsTypes.h:264-271).
#ifdef _WIN32
#  define SHIM_INTF_METHOD __cdecl
#else
#  define SHIM_INTF_METHOD
#endif

// The return codes the handlers use (tjsErrorDefs.h:35-39, 54-56). Only these
// cross the boundary; the file never sees an engine header.
#define SHIM_TJS_S_TRUE 1
#define SHIM_TJS_S_FALSE 2
#define SHIM_TJS_E_FAIL (-1)
#define SHIM_TJS_E_MEMBERNOTFOUND (-1001)
#define SHIM_TJS_FAILED(x) ((x) < 0)
#define SHIM_TJS_SUCCEEDED(x) (!SHIM_TJS_FAILED(x))

//---------------------------------------------------------------------------
// The transition interfaces, declared locally
//---------------------------------------------------------------------------
// Member order is the vtable layout and is not ours to choose. Every class
// below is transcribed from external/krkrz/visual/transhandler.h, which is
// byte-identical to upstream's src/core/visual/transhandler.h.

// transhandler.h:195-217 — the rectangle/source/destination bundle handed to
// iTVPDivisibleTransHandler::Process. All pixel access is by BYTE pitch: the
// surfaces are `pitch` bytes per row and rows are padded, never `width * 4`.
// The engine's own layer accessor is a trap here — it is *named*
// GetMainImagePixelBufferPitch but returns byte pitch (LayerIntf.cpp:2688-2691
// forwards to GetPitchBytes()) — so this file multiplies row indices by the
// byte pitch everywhere and never assumes 4 bytes per pixel for addressing.
struct tTVPDivisibleData
{
	tjs_int Left;                          // processing rectangle left
	tjs_int Top;                           // processing rectangle top
	tjs_int Width;                         // processing rectangle width
	tjs_int Height;                        // processing rectangle height
	struct iTVPScanLineProvider *Dest;     // destination image
	tjs_int DestLeft;
	tjs_int DestTop;
	struct iTVPScanLineProvider *Src1;     // source 1 (self layer image)
	tjs_int Src1Left;
	tjs_int Src1Top;
	struct iTVPScanLineProvider *Src2;     // source 2 (other layer image)
	tjs_int Src2Left;
	tjs_int Src2Top;
};
// Upstream wrapped this in `#pragma pack(push, 4)` for Windows
// (transhandler.h:197-198, 218). Not needed here: every member is 4 or 8 bytes
// and each pointer already lands 8-byte aligned under both pack(4) and the
// default, so the layout is identical either way.

// transhandler.h:86-106 — one image, one scanline at a time.
class iTVPScanLineProvider
{
public:
	virtual tjs_error SHIM_INTF_METHOD AddRef() = 0;
	virtual tjs_error SHIM_INTF_METHOD Release() = 0;
	virtual tjs_error SHIM_INTF_METHOD GetWidth(tjs_int *width) = 0;
	virtual tjs_error SHIM_INTF_METHOD GetHeight(tjs_int *height) = 0;
	virtual tjs_error SHIM_INTF_METHOD GetPixelFormat(tjs_int *bpp) = 0;
	virtual tjs_error SHIM_INTF_METHOD GetPitchBytes(tjs_int *pitch) = 0;
	virtual tjs_error SHIM_INTF_METHOD GetScanLine(tjs_int line,
		const void **scanline) = 0;
	virtual tjs_error SHIM_INTF_METHOD GetScanLineForWrite(tjs_int line,
		void **scanline) = 0;
};

// transhandler.h:116-140 — the transition's option set, as handed in by the
// `@trans` tag's attributes.
class iTVPSimpleOptionProvider
{
public:
	virtual tjs_error SHIM_INTF_METHOD AddRef() = 0;
	virtual tjs_error SHIM_INTF_METHOD Release() = 0;
	virtual tjs_error SHIM_INTF_METHOD GetAsNumber(const tjs_char *name,
		tjs_int64 *value) = 0;
	virtual tjs_error SHIM_INTF_METHOD GetAsString(const tjs_char *name,
		const tjs_char **out) = 0;
	// GetValue(const tjs_char *, tTJSVariant *) sits here in the real header
	// (transhandler.h:132-134). It is deliberately omitted: reading it would
	// require tTJSVariant, an engine class this file cannot see. Omitting the
	// slot is safe *because the engine never calls a method on this interface
	// through this declaration* — the object is built by the engine and this
	// file only ever calls GetAsNumber, which is declared before the gap, so it
	// keeps its correct index (transhandler.h:123-125). The two slots after the
	// gap (Reserved2, GetDispatchObject) are declared for completeness and are
	// never invoked here.
	//
	// GetAsNumber is the right accessor for these options: the engine's own
	// built-in providers read time/vague/from/stay through it
	// (TransIntf.cpp:528, 766-772, 1042-1053), and it reports an absent or void
	// option as TJS_E_MEMBERNOTFOUND (TransIntf.cpp:57-70), which is exactly the
	// "not specified" test the upstream modifiers rely on.
	virtual tjs_error SHIM_INTF_METHOD Reserved2() = 0;
	virtual tjs_error SHIM_INTF_METHOD GetDispatchObject(void **dsp) = 0;
};

// transhandler.h:149 — only passed through, never called here.
class iTVPSimpleImageProvider;

// transhandler.h:27 — ttSimple uses one layer only, ttExchange blends two.
enum tTVPTransType { ttSimple, ttExchange };

// transhandler.h:46 — tutDivisible/tutDivisibleFade are the region-divisible
// kinds, which is the only kind this file produces.
enum tTVPTransUpdateType { tutDivisibleFade, tutDivisible, tutGiveUpdate };

// drawable.h:20-40 — the values this file tests. ltAlpha is 2, ltAddAlpha is 12;
// wave picks its blend primitive from this (wave.cpp:238-246).
enum tTVPLayerType
{
	ltOpaque = 1,
	ltAlpha = 2,
	ltAddAlpha = 12
};

// transhandler.h:224-234 — base of every handler.
class iTVPBaseTransHandler
{
public:
	virtual tjs_error SHIM_INTF_METHOD AddRef() = 0;
	virtual tjs_error SHIM_INTF_METHOD Release() = 0;
	virtual tjs_error SHIM_INTF_METHOD SetOption(
		iTVPSimpleOptionProvider *options) = 0;
};

// transhandler.h:242-281 — the handler shape both transitions use.
class iTVPDivisibleTransHandler : public iTVPBaseTransHandler
{
public:
	virtual tjs_error SHIM_INTF_METHOD StartProcess(tjs_uint64 tick) = 0;
	virtual tjs_error SHIM_INTF_METHOD EndProcess() = 0;
	virtual tjs_error SHIM_INTF_METHOD Process(tTVPDivisibleData *data) = 0;
	virtual tjs_error SHIM_INTF_METHOD MakeFinalImage(
		iTVPScanLineProvider **dest,
		iTVPScanLineProvider *src1,
		iTVPScanLineProvider *src2) = 0;
};

// transhandler.h:306-330 — what gets registered, and what the engine keeps.
class iTVPTransHandlerProvider
{
public:
	virtual tjs_error SHIM_INTF_METHOD AddRef() = 0;
	virtual tjs_error SHIM_INTF_METHOD Release() = 0;
	virtual tjs_error SHIM_INTF_METHOD GetName(const tjs_char **name) = 0;
	virtual tjs_error SHIM_INTF_METHOD StartTransition(
		iTVPSimpleOptionProvider *options,
		iTVPSimpleImageProvider *imagepro,
		tTVPLayerType layertype,
		tjs_uint src1w, tjs_uint src1h,
		tjs_uint src2w, tjs_uint src2h,
		tTVPTransType *type,
		tTVPTransUpdateType *updatetype,
		iTVPBaseTransHandler **handler) = 0;
};

//---------------------------------------------------------------------------
// The exporter, completed
//---------------------------------------------------------------------------
// shim_abi.h forward-declares iTVPFunctionExporter without a body, on purpose:
// the pointer crosses the boundary and nothing else about it belongs to a shim.
// Completing it here is legitimate — the ABI is the engine's, and the only
// member this file calls is the narrow-string query, which is what the engine's
// own generated plugin stubs use (src/core/base/sdl2/makestub.py:689-690, 709).
// Declaration order and convention must match PluginImpl.h:44-50.
struct iTVPFunctionExporter
{
	virtual bool SHIM_INTF_METHOD QueryFunctions(const tjs_char **name,
		void **function, tjs_uint count) = 0;
	virtual bool SHIM_INTF_METHOD QueryFunctionsByNarrowString(const char **name,
		void **function, tjs_uint count) = 0;
};

//---------------------------------------------------------------------------
// Engine pixel primitives, resolved by name
//---------------------------------------------------------------------------
// These are the engine's own portable pixel operations, not something this file
// reimplements. Their export keys are the exact mangled strings from the
// engine's function table (verified by decompressing `compressed_functable` in
// src/core/base/sdl2/FuncStubs.cpp:2753); the original plugin looked its
// functions up the same way, through tp_stub. Resolving by name is also what
// keeps this shim portable: it names an operation, not a symbol in one build.
typedef void (SHIM_INTF_METHOD *tFillARGB)(tjs_uint32 *dest, tjs_int len,
	tjs_uint32 value);
typedef void (SHIM_INTF_METHOD *tConstAlphaBlendSD)(tjs_uint32 *dest,
	const tjs_uint32 *src1, const tjs_uint32 *src2, tjs_int len, tjs_int opa);
typedef void (SHIM_INTF_METHOD *tAddTransHandlerProvider)(
	iTVPTransHandlerProvider *pro);
typedef void (SHIM_INTF_METHOD *tRemoveTransHandlerProvider)(
	iTVPTransHandlerProvider *pro);
// Stretch-copy: destlen pixels of src sampled at srcstart with srcstep (both
// 16.16 fixed-point). Used by the rotate handlers' "zoom only" path
// (rotatebase.cpp:154-164). Signature matches TVPStretchCopy in
// deps/krkrsdl2/.../tvpgl.h:343.
typedef void (SHIM_INTF_METHOD *tStretchCopy)(tjs_uint32 *dest, tjs_int destlen,
	const tjs_uint32 *src, tjs_int srcstart, tjs_int srcstep);
// Linear affine copy: destlen pixels of src sampled along the line (sx,sy) in
// 16.16 fixed-point with step (stepx,stepy); srcpitch is in BYTES.
// (rotatebase.cpp:166-176, turn.cpp:465-467.) Signature matches TVPLinTransCopy
// in tvpgl.h:349. srcpitch's byte unit is the same convention the rest of this
// file already follows for surface pitches.
typedef void (SHIM_INTF_METHOD *tLinTransCopy)(tjs_uint32 *dest, tjs_int destlen,
	const tjs_uint32 *src, tjs_int sx, tjs_int sy, tjs_int stepx, tjs_int stepy,
	tjs_int srcpitch);

static tFillARGB g_fill_argb = 0;
static tConstAlphaBlendSD g_blend_sd = 0;
static tConstAlphaBlendSD g_blend_sd_d = 0;
static tConstAlphaBlendSD g_blend_sd_a = 0;
static tAddTransHandlerProvider g_add_provider = 0;
static tRemoveTransHandlerProvider g_remove_provider = 0;
static tStretchCopy g_stretch_copy = 0;
static tLinTransCopy g_lintrans_copy = 0;

static void *ShimResolve(iTVPFunctionExporter *exporter, const char *name)
{
	void *fn = 0;
	const char *names[1];
	names[0] = name;
	if (!exporter)
		return 0;
	if (!exporter->QueryFunctionsByNarrowString(names, &fn, 1))
		return 0;
	return fn;
}

//---------------------------------------------------------------------------
// Small helpers shared by all seven handlers
//---------------------------------------------------------------------------

// Clip a segment [l,r) to [cl,cr). False when nothing remains.
// Ported from extrans/common.h:16-24, which all upstream handlers include.
static bool ShimClip(tjs_int &l, tjs_int &r, tjs_int cl, tjs_int cr)
{
	if (l < cl) l = cl;
	if (r > cr) r = cr;
	if (l >= r) return false;
	return true;
}

// Blend two ARGB pixels, opa = 0..255 (0 = a, 255 = b).
// Ported from extrans/common.h:22-33. This is a *scalar* blend of one pixel:
// both upstream handlers use it to pick a single representative colour (the
// centre pixel of a mosaic block, or the background colour of a wave), which is
// why it is not one of the surface primitives resolved above — those blend a
// run of pixels and cannot express "blend these two pixels into one colour".
static tjs_uint32 ShimBlend(tjs_uint32 a, tjs_uint32 b, tjs_int opa)
{
	tjs_uint32 ret;
	tjs_uint32 tmp;
	tmp = a & 0x000000ffu;
	ret = 0x000000ffu & (tmp + (((b & 0x000000ffu) - tmp) * opa >> 8));
	tmp = a & 0x0000ff00u;
	ret |= 0x0000ff00u & (tmp + (((b & 0x0000ff00u) - tmp) * opa >> 8));
	tmp = a & 0x00ff0000u;
	ret |= 0x00ff0000u & (tmp + (((b & 0x00ff0000u) - tmp) * opa >> 8));
	tmp = a >> 24;
	ret |= (0x000000ffu & (tmp + (((b >> 24) - tmp) * opa >> 8))) << 24;
	return ret;
}

// Read an integer option; false when absent or void.
static bool ShimOption(iTVPSimpleOptionProvider *options, const tjs_char *name,
	tjs_int64 *out)
{
	if (!options)
		return false;
	return SHIM_TJS_SUCCEEDED(options->GetAsNumber(name, out));
}

// Fill `ylen` rows of `xlen` pixels starting at `dest`, advancing by
// `destpitch` BYTES per row (the surfaces are padded — never width * 4).
//
// Upstream expanded this as a macro with a switch over xlen 2..7 that stored the
// pixels directly and only fell back to TVPFillARGB for other widths
// (mosaic.cpp:206-283); the direct stores existed purely to skip the call
// overhead of the stub pointer. Using the engine's own fill for every width
// instead is the whole point of resolving TVPFillARGB: same result, one
// primitive, no hand-rolled per-pixel store loop.
static void ShimFillRows(unsigned char *dest, tjs_int destpitch, tjs_int xlen,
	tjs_int ylen, tjs_uint32 color)
{
	for (tjs_int y = 0; y < ylen; y++)
	{
		g_fill_argb((tjs_uint32 *)dest, xlen, color);
		dest += destpitch;
	}
}

//---------------------------------------------------------------------------
// `mosaic` — rectangular mosaic transition
//---------------------------------------------------------------------------
// Ported from extrans/mosaic.cpp. Note the upstream caveat that it cannot carry
// per-pixel alpha: it writes opaque blocks, so it is only correct on a layer
// whose type is ltOpaque (mosaic.cpp:14-17). Fate drives it from @prick with
// `layer=base`, which is opaque.
class tShimMosaicTransHandler : public iTVPDivisibleTransHandler
{
	tjs_int RefCount;

	tjs_uint64 StartTick;      // tick the transition started at
	tjs_uint64 Time;           // duration, ms
	tjs_uint64 HalfTime;       // Time / 2
	tjs_uint64 CurTime;        // elapsed
	tjs_int Width;
	tjs_int Height;
	tjs_int CurOfsX;           // block-grid offset
	tjs_int CurOfsY;
	tjs_int MaxBlockSize;      // `maxsize` option
	tjs_int CurBlockSize;      // current block size
	tjs_int BlendRatio;        // 0..255
	bool First;

public:
	tShimMosaicTransHandler(tjs_uint64 time, tjs_int width, tjs_int height,
		tjs_int maxblocksize)
	{
		RefCount = 1;
		Width = width;
		Height = height;
		Time = time;
		HalfTime = time / 2;
		MaxBlockSize = maxblocksize;
		CurOfsX = CurOfsY = 0;
		CurBlockSize = 0;
		BlendRatio = 0;
		First = true;
	}

	// Upstream declares this (mosaic.cpp:57-59). It is not an ABI complication:
	// a derived class's destructor is appended after the inherited virtual
	// slots, so the base's layout the engine calls through is unchanged. Without
	// it, `delete this` through the base's Release() is undefined.
	virtual ~tShimMosaicTransHandler() {}

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1)
			delete this;
		else
			RefCount--;
		return SHIM_TJS_S_OK;
	}

	// Nothing to do per process step: all options were consumed by
	// StartTransition. (mosaic.cpp:91-98.)
	virtual tjs_error SHIM_INTF_METHOD SetOption(iTVPSimpleOptionProvider *)
	{
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD StartProcess(tjs_uint64 tick);
	virtual tjs_error SHIM_INTF_METHOD EndProcess();
	virtual tjs_error SHIM_INTF_METHOD Process(tTVPDivisibleData *data);

	// The final image is always the destination. (mosaic.cpp:100-105.)
	virtual tjs_error SHIM_INTF_METHOD MakeFinalImage(
		iTVPScanLineProvider **dest,
		iTVPScanLineProvider *,
		iTVPScanLineProvider *src2)
	{
		*dest = src2;
		return SHIM_TJS_S_OK;
	}
};

// mosaic.cpp:107-146.
tjs_error SHIM_INTF_METHOD tShimMosaicTransHandler::StartProcess(tjs_uint64 tick)
{
	if (First)
	{
		First = false;
		StartTick = tick;
	}

	// Current block size grows to MaxBlockSize at half time, then shrinks back —
	// the blocks open out and close again. (mosaic.cpp:126-133.)
	tjs_int64 t = (tjs_int64)(CurTime = tick - StartTick);
	if (CurTime > Time) CurTime = Time;
	if (t >= (tjs_int64)HalfTime) t = (tjs_int64)Time - t;
	if (t < 0) t = 0;
	CurBlockSize = (tjs_int)((MaxBlockSize - 2) * t / (tjs_int64)HalfTime + 2);

	// Cross-fade of the block colours tracks elapsed time linearly.
	// (mosaic.cpp:135-137.)
	BlendRatio = (tjs_int)(CurTime * 255 / Time);
	if (BlendRatio > 255) BlendRatio = 255;

	// Shift the block grid so a block centre lands on the image centre.
	// (mosaic.cpp:139-146.)
	tjs_int x = Width / 2;
	tjs_int y = Height / 2;
	x /= CurBlockSize;
	y /= CurBlockSize;
	x *= CurBlockSize;
	y *= CurBlockSize;
	CurOfsX = (Width - CurBlockSize) / 2 - x;
	CurOfsY = (Height - CurBlockSize) / 2 - y;
	if (CurOfsX > 0) CurOfsX -= CurBlockSize;
	if (CurOfsY > 0) CurOfsY -= CurBlockSize;

	return SHIM_TJS_S_TRUE;
}

// mosaic.cpp:148-155: the transition is over once the blend has run its course.
tjs_error SHIM_INTF_METHOD tShimMosaicTransHandler::EndProcess()
{
	if (BlendRatio == 255) return SHIM_TJS_S_FALSE;
	return SHIM_TJS_S_TRUE;
}

// mosaic.cpp:157-371. Fills the requested rectangle with blocks; each block
// takes the blended colour of the two sources at its centre, and blocks that
// straddle the requested rectangle are clipped to it.
tjs_error SHIM_INTF_METHOD tShimMosaicTransHandler::Process(tTVPDivisibleData *data)
{
	unsigned char *dest;
	tjs_int destpitch;
	const unsigned char *src1;
	tjs_int src1pitch;
	const unsigned char *src2;
	tjs_int src2pitch;

	if (SHIM_TJS_FAILED(data->Dest->GetScanLineForWrite(0, (void **)&dest)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src1->GetScanLine(0, (const void **)&src1)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src2->GetScanLine(0, (const void **)&src2)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Dest->GetPitchBytes(&destpitch)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src1->GetPitchBytes(&src1pitch)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src2->GetPitchBytes(&src2pitch)))
		return SHIM_TJS_E_FAIL;

	// Image coordinates are top-left-of-image; the requested rectangle and the
	// destination rectangle differ only by this offset. Pitch is in bytes, so
	// every row step below multiplies by the byte pitch.
	tjs_int destxofs = data->DestLeft - data->Left;
	tjs_int destyofs = data->DestTop - data->Top;

	const tjs_int bs = CurBlockSize;
	const tjs_int bs2 = bs >> 1;

	// Block range covering the requested rectangle. (mosaic.cpp:201-204.)
	tjs_int startx = (data->Left - CurOfsX) / bs;
	tjs_int starty = (data->Top - CurOfsY) / bs;
	tjs_int endx = (data->Left + data->Width - 1 - CurOfsX) / bs;
	tjs_int endy = (data->Top + data->Height - 1 - CurOfsY) / bs;

	// One block: blend the centre pixel of the two sources, then fill the part
	// of the block that survives clipping to the requested rectangle.
	// (mosaic.cpp:288-311.)
	struct BlockFiller
	{
		tShimMosaicTransHandler *self;
		tTVPDivisibleData *data;
		unsigned char *dest;
		const unsigned char *src1;
		const unsigned char *src2;
		tjs_int src1pitch;
		tjs_int src2pitch;
		tjs_int destpitch;
		tjs_int destxofs;
		tjs_int destyofs;
		tjs_int bs;

		void operator()(tjs_int bx, tjs_int by)
		{
			tjs_int l = bx * bs + self->CurOfsX;
			tjs_int t = by * bs + self->CurOfsY;
			tjs_int r = l + bs;
			tjs_int b = t + bs;
			if (!ShimClip(l, r, data->Left, data->Left + data->Width) ||
				!ShimClip(t, b, data->Top, data->Top + data->Height))
				return;

			// Centre pixel, clamped to the image (mosaic.cpp:295-299).
			tjs_int mx = bx * bs + self->CurOfsX + (bs >> 1);
			tjs_int my = by * bs + self->CurOfsY + (bs >> 1);
			if (mx < 0) mx = 0;
			if (my < 0) my = 0;
			if (mx >= self->Width) mx = self->Width - 1;
			if (my >= self->Height) my = self->Height - 1;

			// Byte pitch: the row pointers are advanced by bytes, then the
			// pixel is picked by 32-bit index within the row.
			const tjs_uint32 *row1 =
				(const tjs_uint32 *)(src1 + (int64_t)my * src1pitch);
			const tjs_uint32 *row2 =
				(const tjs_uint32 *)(src2 + (int64_t)my * src2pitch);
			tjs_uint32 colour =
				ShimBlend(row1[mx], row2[mx], self->BlendRatio);

			unsigned char *dst =
				dest + (int64_t)(t + destyofs) * destpitch;
			dst = (unsigned char *)((tjs_uint32 *)dst + l + destxofs);
			ShimFillRows(dst, destpitch, r - l, b - t, colour);
		}
	};

	BlockFiller fill;
	fill.self = this;
	fill.data = data;
	fill.dest = dest;
	fill.src1 = src1;
	fill.src2 = src2;
	fill.src1pitch = src1pitch;
	fill.src2pitch = src2pitch;
	fill.destpitch = destpitch;
	fill.destxofs = destxofs;
	fill.destyofs = destyofs;
	fill.bs = bs;

	// Top row: every block may straddle the rectangle. (mosaic.cpp:316-322.)
	tjs_int y = starty;
	for (tjs_int x = startx; x <= endx; x++)
		fill(x, y);
	y++;

	// Middle rows: the left and right blocks straddle, the rest cannot.
	// (mosaic.cpp:324-361.) A run of whole blocks per row is one fill each.
	for (; y < endy; y++)
	{
		fill(startx, y);

		const tjs_int t = y * bs + CurOfsY;
		const tjs_int my = t + bs2;
		const tjs_uint32 *row1 =
			(const tjs_uint32 *)(src1 + (int64_t)my * src1pitch);
		const tjs_uint32 *row2 =
			(const tjs_uint32 *)(src2 + (int64_t)my * src2pitch);
		unsigned char *dstrow =
			dest + (int64_t)(t + destyofs) * destpitch;

		for (tjs_int x = startx + 1; x < endx; x++)
		{
			// Unclipped block: l and t are the block's own origin.
			const tjs_int l = x * bs + CurOfsX;
			const tjs_int mx = l + bs2;
			tjs_uint32 colour = ShimBlend(row1[mx], row2[mx], BlendRatio);
			unsigned char *dst =
				(unsigned char *)((tjs_uint32 *)dstrow + l + destxofs);
			ShimFillRows(dst, destpitch, bs, bs, colour);
		}

		fill(endx, y);
	}

	// Bottom row. (mosaic.cpp:363-370.)
	if (y <= endy)
	{
		for (tjs_int x = startx; x <= endx; x++)
			fill(x, y);
	}

	return SHIM_TJS_S_OK;
}

// mosaic.cpp:378-455.
class tShimMosaicTransHandlerProvider : public iTVPTransHandlerProvider
{
	tjs_int RefCount;

public:
	tShimMosaicTransHandlerProvider() { RefCount = 1; }
	virtual ~tShimMosaicTransHandlerProvider() {}   // mosaic.cpp:383-384

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1)
			delete this;
		else
			RefCount--;
		return SHIM_TJS_S_OK;
	}

	// mosaic.cpp:404-411 — the TJS name the scripts ask for.
	virtual tjs_error SHIM_INTF_METHOD GetName(const tjs_char **name)
	{
		if (name)
		{
			*name = SHIM_TJS_W("mosaic");
			return SHIM_TJS_S_OK;
		}
		return SHIM_TJS_E_FAIL;
	}

	virtual tjs_error SHIM_INTF_METHOD StartTransition(
		iTVPSimpleOptionProvider *options,
		iTVPSimpleImageProvider *,
		tTVPLayerType,
		tjs_uint src1w, tjs_uint src1h,
		tjs_uint src2w, tjs_uint src2h,
		tTVPTransType *type,
		tTVPTransUpdateType *updatetype,
		iTVPBaseTransHandler **handler)
	{
		// Both upstream and the engine's contract are exchange transitions,
		// processed per rectangle. (mosaic.cpp:423-425.)
		if (type) *type = ttExchange;
		if (updatetype) *updatetype = tutDivisible;
		if (!handler) return SHIM_TJS_E_FAIL;
		if (!options) return SHIM_TJS_E_FAIL;

		// The two sources must be the same size. (mosaic.cpp:428-429.)
		if (src1w != src2w || src1h != src2h)
			return SHIM_TJS_E_FAIL;

		tjs_int64 time;
		tjs_int64 maxblocksize = 30;   // default, mosaic.cpp:437

		// `time` is mandatory: without it the transition has no duration.
		// (mosaic.cpp:439-444.)
		if (!ShimOption(options, SHIM_TJS_W("time"), &time))
			return SHIM_TJS_E_FAIL;
		if (time < 2) time = 2;        // too small a value misbehaves

		ShimOption(options, SHIM_TJS_W("maxsize"), &maxblocksize);

		*handler = new tShimMosaicTransHandler((tjs_uint64)time,
			(tjs_int)src1w, (tjs_int)src1h, (tjs_int)maxblocksize);
		return SHIM_TJS_S_OK;
	}
};

//---------------------------------------------------------------------------
// `wave` — raster-scroll (sine) transition
//---------------------------------------------------------------------------
// Ported from extrans/wave.cpp. Each scanline is shifted horizontally by a sine
// of its vertical position, the two sources are cross-faded along it, and the
// gap the shift opens at one edge is filled with a background colour.
class tShimWaveTransHandler : public iTVPDivisibleTransHandler
{
	tjs_int RefCount;

	tjs_uint64 StartTick;
	tjs_uint64 HalfTime;
	tjs_uint64 Time;
	tTVPLayerType LayerType;
	tjs_int Width;
	tjs_int Height;
	tjs_int MaxH;              // `maxh`
	double MaxOmega;           // `maxomega`
	tjs_int CurH;
	double CurOmega;
	double CurRadStart;
	tjs_int64 CurTime;
	tjs_int BlendRatio;
	tjs_uint32 BGColor1;       // `bgcolor1`
	tjs_uint32 BGColor2;       // `bgcolor2`
	tjs_uint32 CurBGColor;
	tjs_int WaveType;          // `wavetype`: 0 = fine at both ends, 1/2 = one end
	bool First;

public:
	tShimWaveTransHandler(tjs_uint64 time, tTVPLayerType layertype,
		tjs_int width, tjs_int height, tjs_int maxh, double maxomega,
		tjs_uint32 bgcolor1, tjs_uint32 bgcolor2, tjs_int wavetype)
	{
		RefCount = 1;
		LayerType = layertype;
		Width = width;
		Height = height;
		Time = time;
		HalfTime = time / 2;
		MaxH = maxh;
		MaxOmega = maxomega;
		BGColor1 = bgcolor1;
		BGColor2 = bgcolor2;
		WaveType = wavetype;
		CurH = 0;
		CurOmega = 0;
		CurRadStart = 0;
		CurTime = 0;
		BlendRatio = 0;
		CurBGColor = 0;
		First = true;
	}

	virtual ~tShimWaveTransHandler() {}   // wave.cpp:65-67

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1)
			delete this;
		else
			RefCount--;
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD SetOption(iTVPSimpleOptionProvider *)
	{
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD StartProcess(tjs_uint64 tick);
	virtual tjs_error SHIM_INTF_METHOD EndProcess();
	virtual tjs_error SHIM_INTF_METHOD Process(tTVPDivisibleData *data);

	virtual tjs_error SHIM_INTF_METHOD MakeFinalImage(
		iTVPScanLineProvider **dest,
		iTVPScanLineProvider *,
		iTVPScanLineProvider *src2)
	{
		*dest = src2;
		return SHIM_TJS_S_OK;
	}
};

// wave.cpp:115-162.
tjs_error SHIM_INTF_METHOD tShimWaveTransHandler::StartProcess(tjs_uint64 tick)
{
	if (First)
	{
		First = false;
		StartTick = tick;
	}

	tjs_int64 t = (tjs_int64)(CurTime = tick - StartTick);
	// Upstream compares a signed CurTime against the unsigned Time here
	// (wave.cpp:131-132); the cast keeps the comparison signed so the `t < 0`
	// guard below still means what it says.
	if (CurTime > (tjs_int64)Time) CurTime = (tjs_int64)Time;
	if (t >= (tjs_int64)HalfTime) t = (tjs_int64)Time - t;
	if (t < 0) t = 0;

	// Amplitude eases in and out over the transition; the angular frequency
	// follows `wavetype`. (wave.cpp:136-151.) Note Height / 2 is integer
	// division upstream, and stays so here.
	double tt = sin((3.14159265358979 / 2.0) * t / (tjs_int64)HalfTime);
	CurH = (tjs_int)(tt * MaxH);
	switch (WaveType)
	{
	case 0: CurOmega = MaxOmega * tt; break;
	case 1: CurOmega = MaxOmega * ((tjs_int64)Time - CurTime) / (tjs_int64)Time; break;
	case 2: CurOmega = MaxOmega * CurTime / (tjs_int64)Time; break;
	}
	CurRadStart = -CurOmega * (Height / 2);

	BlendRatio = (tjs_int)(CurTime * 255 / Time);
	if (BlendRatio > 255) BlendRatio = 255;

	// Background colour also cross-fades. (wave.cpp:156-157.)
	CurBGColor = ShimBlend(BGColor1, BGColor2, BlendRatio);

	return SHIM_TJS_S_TRUE;
}

// wave.cpp:164-171.
tjs_error SHIM_INTF_METHOD tShimWaveTransHandler::EndProcess()
{
	if (BlendRatio == 255) return SHIM_TJS_S_FALSE;
	return SHIM_TJS_S_TRUE;
}

// wave.cpp:173-261. Per scanline of the requested rectangle: fill the gap the
// shift opens with the background colour, then cross-fade the two sources along
// the shifted span.
tjs_error SHIM_INTF_METHOD tShimWaveTransHandler::Process(tTVPDivisibleData *data)
{
	double rad = data->Top * CurOmega + CurRadStart;

	for (tjs_int n = 0; n < data->Height; n++, rad += CurOmega)
	{
		tjs_int d = (tjs_int)(sin(rad) * CurH);   // horizontal shift

		tjs_uint32 *dest;
		const tjs_uint32 *src1;
		const tjs_uint32 *src2;
		if (SHIM_TJS_FAILED(data->Dest->GetScanLineForWrite(data->DestTop + n,
				(void **)&dest)))
			return SHIM_TJS_E_FAIL;
		if (SHIM_TJS_FAILED(data->Src1->GetScanLine(data->Top + n,
				(const void **)&src1)))
			return SHIM_TJS_E_FAIL;
		if (SHIM_TJS_FAILED(data->Src2->GetScanLine(data->Top + n,
				(const void **)&src2)))
			return SHIM_TJS_E_FAIL;

		tjs_int l, r;

		// Gap at the left, filled with the background colour. (wave.cpp:213-220.)
		if (d > 0)
		{
			l = 0;
			r = d;
			if (ShimClip(l, r, data->Left, data->Left + data->Width))
				g_fill_argb(dest + l + data->DestLeft - data->Left, r - l,
					CurBGColor);
		}

		// Gap at the right. (wave.cpp:222-229.)
		if (d < 0)
		{
			l = d + Width;
			r = Width;
			if (ShimClip(l, r, data->Left, data->Left + data->Width))
				g_fill_argb(dest + l + data->DestLeft - data->Left, r - l,
					CurBGColor);
		}

		// The shifted span: src1 and src2 blended at BlendRatio.
		// (wave.cpp:231-256.) The blend primitive is chosen by the destination
		// layer's type — a destination with alpha needs a different algorithm,
		// which is exactly what the _d / _a suffixes on the engine's blends are
		// for.
		l = d;
		r = Width + d;
		if (ShimClip(l, r, data->Left, data->Left + data->Width))
		{
			tjs_uint32 *out = dest + l + data->DestLeft - data->Left;
			const tjs_uint32 *a = src1 + l - d;
			const tjs_uint32 *b = src2 + l - d;
			if (LayerType == ltAlpha)
				g_blend_sd_d(out, a, b, r - l, BlendRatio);
			else if (LayerType == ltAddAlpha)
				g_blend_sd_a(out, a, b, r - l, BlendRatio);
			else
				g_blend_sd(out, a, b, r - l, BlendRatio);
		}
	}

	return SHIM_TJS_S_OK;
}

// wave.cpp:267-358.
class tShimWaveTransHandlerProvider : public iTVPTransHandlerProvider
{
	tjs_int RefCount;

public:
	tShimWaveTransHandlerProvider() { RefCount = 1; }
	virtual ~tShimWaveTransHandlerProvider() {}     // wave.cpp:271-272

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1)
			delete this;
		else
			RefCount--;
		return SHIM_TJS_S_OK;
	}

	// wave.cpp:293-300.
	virtual tjs_error SHIM_INTF_METHOD GetName(const tjs_char **name)
	{
		if (name)
		{
			*name = SHIM_TJS_W("wave");
			return SHIM_TJS_S_OK;
		}
		return SHIM_TJS_E_FAIL;
	}

	virtual tjs_error SHIM_INTF_METHOD StartTransition(
		iTVPSimpleOptionProvider *options,
		iTVPSimpleImageProvider *,
		tTVPLayerType layertype,
		tjs_uint src1w, tjs_uint src1h,
		tjs_uint src2w, tjs_uint src2h,
		tTVPTransType *type,
		tTVPTransUpdateType *updatetype,
		iTVPBaseTransHandler **handler)
	{
		// wave.cpp:312-314.
		if (type) *type = ttExchange;
		if (updatetype) *updatetype = tutDivisible;
		if (!handler) return SHIM_TJS_E_FAIL;
		if (!options) return SHIM_TJS_E_FAIL;
		if (src1w != src2w || src1h != src2h)
			return SHIM_TJS_E_FAIL;

		// Defaults, wave.cpp:326-330.
		tjs_int64 time;
		tjs_int64 maxh = 50;
		tjs_int64 bgcolor1 = 0;
		tjs_int64 bgcolor2 = 0;
		tjs_int64 wavetype = 0;

		// `time` mandatory (wave.cpp:332-337).
		if (!ShimOption(options, SHIM_TJS_W("time"), &time))
			return SHIM_TJS_E_FAIL;
		if (time < 2) time = 2;

		ShimOption(options, SHIM_TJS_W("maxh"), &maxh);
		ShimOption(options, SHIM_TJS_W("bgcolor1"), &bgcolor1);
		ShimOption(options, SHIM_TJS_W("bgcolor2"), &bgcolor2);
		ShimOption(options, SHIM_TJS_W("wavetype"), &wavetype);

		// `maxomega` is upstream's only real-valued option (default 0.2,
		// wave.cpp:327) and is read through the integer accessor here, because
		// the variant accessor needs tTJSVariant (see the note on
		// iTVPSimpleOptionProvider above). Fate's scripts never pass it — every
		// @wave/@waveT call site sets only `time` and `wavetype` — so the
		// default is what the game gets, unchanged. A caller that does pass a
		// fractional value would have it truncated; that is stated rather than
		// silently guessed at.
		tjs_int64 maxomega_int = 0;
		double maxomega = 0.2;
		if (ShimOption(options, SHIM_TJS_W("maxomega"), &maxomega_int))
			maxomega = (double)maxomega_int;

		*handler = new tShimWaveTransHandler((tjs_uint64)time, layertype,
			(tjs_int)src1w, (tjs_int)src1h, (tjs_int)maxh, maxomega,
			(tjs_uint32)bgcolor1, (tjs_uint32)bgcolor2, (tjs_int)wavetype);
		return SHIM_TJS_S_OK;
	}
};

//--------------------------------------------------------------------------
// `ripple` — ripple (wave-displacement) transition
//--------------------------------------------------------------------------
// Ported from extrans/ripple.cpp (lines 1-1559). A displacement-map ripple:
// each pixel is shifted by a vector read from a precomputed table indexed by
// the pixel's direction-and-distance from the ripple centre, then the two
// sources are cross-faded at the shifted location. The displacement table is
// regenerated whenever (centre, image size, ripple width, roundness, maxdrift)
// change, and up to four are cached (ripple.cpp:290-383).
//
// Upstream ships MMX/EMMX assembly variants of the inner transform
// (ripple.cpp:435-881); this port keeps only the scalar C versions
// (TVPRippleTransform_c_f/_c_b at ripple.cpp:406-430, and the four border-safe
// variants _f_a_e/_f_d_e/_b_a_e/_b_d_e at ripple.cpp:926-988). The engine
// resolves TVPGetCPUType through tp_stub, which this shim cannot do without
// engine headers; the scalar path is the fallback upstream selects when no MMX
// is present (ripple.cpp:891-892), so the result is bit-identical to upstream
// on a CPU without MMX, and identical in observable output on one with MMX
// (the assembly computes the same per-pixel blend).

// ripple.cpp:33-36. Direction resolution within one quadrant.
#define SHIM_RIPPLE_DIR_PREC 32
// ripple.cpp:36-37. Sub-pixel drift resolution.
#define SHIM_RIPPLE_DRIFT_PREC 4

// ripple.cpp:39-41. Upstream defines M_PI only when missing; math.h on every
// target this shim builds for already provides it, so the guard is kept for
// fidelity but resolves to the system definition.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ripple.cpp:147-154. The ripple waveform: a shaped sine whose square gives
// the displacement amplitude. Faithful reproduction, including the typo in the
// constant (upstream's 3.14159263589793238462 is used verbatim via M_PI above
// only when the system does not define it; when it does, the system value is
// the one upstream actually compiled with).
static inline float ShimRippleWaveForm(float rad)
{
	float s = (sinf(rad) + sinf(rad * 2 - 2) * 0.2f) / 1.19f;
	s *= s;
	return s;
}

// ripple.cpp:53-145. The displacement/drift tables for one configuration.
// Cached and refcounted exactly as upstream caches it.
class tShimRippleTable
{
	tjs_int RefCount;
	tjs_int Width, Height;
	tjs_int CenterX, CenterY;
	tjs_int RippleWidth;
	float Roundness;
	tjs_int MaxDrift;
	tjs_int MapWidth, MapHeight;
	tjs_uint16 *DisplaceMap;   // [y*MapWidth+x] -> (dist*DIR_PREC + dir)
	tjs_uint16 *DriftMap;      // [drift][phase][dir] (and mirrored) -> offset

public:
	tjs_int GetWidth() const { return Width; }
	tjs_int GetHeight() const { return Height; }
	tjs_int GetCenterX() const { return CenterX; }
	tjs_int GetCenterY() const { return CenterY; }
	tjs_int GetRippleWidth() const { return RippleWidth; }
	float GetRoundness() const { return Roundness; }
	tjs_int GetMaxDrift() const { return MaxDrift; }
	tjs_int GetMapWidth() const { return MapWidth; }
	tjs_int GetMapHeight() const { return MapHeight; }

	tShimRippleTable(tjs_int width, tjs_int height, tjs_int centerx,
		tjs_int centery, tjs_int ripplewidth, float roundness,
		tjs_int maxdrift)
	{
		RefCount = 1;
		DisplaceMap = 0;
		DriftMap = 0;
		Width = width;
		Height = height;
		CenterX = centerx;
		CenterY = centery;
		RippleWidth = ripplewidth;
		Roundness = roundness;
		MaxDrift = maxdrift;
		MakeTable();
	}

	~tShimRippleTable() { Clear(); }

	void AddRef() { RefCount++; }
	void Release()
	{
		if (RefCount == 1) delete this;
		else RefCount--;
	}

	// ripple.cpp:129-132. Index into the one-quadrant displacement map.
	const tjs_uint16 *GetDisplaceMap(tjs_int x, tjs_int y) const
	{
		return DisplaceMap + x + y * MapWidth;
	}

	// ripple.cpp:135-139. The drift map is laid out as
	// [drift][RippleWidth * 2 * DIR_PREC], addressed by phase*DIR_PREC + dir.
	// The *2 mirror is so the per-pixel inner loop can mask with & rather than
	// % (ripple.cpp:200-204).
	const tjs_uint16 *GetDriftMap(tjs_int drift, tjs_int phase)
	{
		return DriftMap + drift * RippleWidth * (2 * SHIM_RIPPLE_DIR_PREC) +
			phase * SHIM_RIPPLE_DIR_PREC;
	}

private:
	void MakeTable();
	void Clear();
};

// ripple.cpp:156-276. Builds DisplaceMap (one quadrant) and DriftMap (the
// fixed-point displacement lookup). All math is reproduced verbatim, including
// the fixed-point formats annotated in the upstream comments (8.10, 8.11, 1.11).
void tShimRippleTable::MakeTable()
{
	tjs_int32 *rippleform = 0;
	tjs_int32 *cos_table = 0;
	tjs_int32 *sin_table = 0;

	try
	{
		// ripple.cpp:167-170. MapWidth/MapHeight = the larger of the two
		// quadrants split by the centre, because the map covers one quadrant.
		MapWidth = CenterX < (Width >> 1) ? Width - CenterX : CenterX;
		MapHeight = CenterY < (Height >> 1) ? Height - CenterY : CenterY;

		DisplaceMap = new tjs_uint16[MapWidth * MapHeight];

		// ripple.cpp:177-196. One quadrant; the other three are reached by
		// sign flips in the transform functions.
		tjs_uint16 *rmp = DisplaceMap;
		tjs_int ripplemask = RippleWidth - 1;
		tjs_int x, y;
		for (y = 0; y < MapHeight; y++)
		{
			float yy = ((float)y + 0.5f) * Roundness;
			float fac = 1.0f / yy;
			for (x = 0; x < MapWidth; x++)
			{
				float xx = (float)x + 0.5f;
				tjs_int dir = (tjs_int)(atanf(xx * fac) *
					((1.0f / (float)(M_PI / 2.0)) * SHIM_RIPPLE_DIR_PREC));
				tjs_int dist = (int)sqrtf(xx * xx + yy * yy) & ripplemask;
				*(rmp++) = (tjs_uint16)((dist * SHIM_RIPPLE_DIR_PREC) + dir);
			}
		}

		// ripple.cpp:203-204. *2 so the inner loop masks with & not %.
		DriftMap = new tjs_uint16[MaxDrift * SHIM_RIPPLE_DRIFT_PREC *
			RippleWidth * 2 * SHIM_RIPPLE_DIR_PREC];

		// ripple.cpp:208-222. Waveform sample table, scaled to 8.11 fixed.
		float rcp_rw = 1.0f / (float)RippleWidth;
		rippleform = new tjs_int32[RippleWidth];
		tjs_int w;
		for (w = 0; w < RippleWidth; w++)
		{
			float rad = (float)w * rcp_rw * (float)(M_PI * -2.0);
			float s = ShimRippleWaveForm(rad);
			if (s < -1.0f) s = -1.0f;
			if (s > 1.0f) s = 1.0f;
			s *= 2048.0f;
			rippleform[w] = (tjs_int32)(s < 0 ? s - 0.5f : s + 0.5f);
		}

		// ripple.cpp:225-236. Direction tables, 8.11 fixed.
		cos_table = new tjs_int32[SHIM_RIPPLE_DIR_PREC];
		sin_table = new tjs_int32[SHIM_RIPPLE_DIR_PREC];
		for (w = 0; w < SHIM_RIPPLE_DIR_PREC; w++)
		{
			float fdir = (float)(M_PI * 0.5) - (((float)w + 0.5f) *
				((1.0f / (float)SHIM_RIPPLE_DIR_PREC) * (float)(M_PI / 2.0)));
			float v;
			v = cosf(fdir) * 2048.0f;
			cos_table[w] = (tjs_int32)(v < 0 ? v - 0.5f : v + 0.5f);
			v = sinf(fdir) * 2048.0f;
			sin_table[w] = (tjs_int32)(v < 0 ? v - 0.5f : v + 0.5f);
		}

		// ripple.cpp:240-262. Combine the tables into the per-(drift,phase,dir)
		// displacement, as a packed (dx,dy) byte pair.
		tjs_int drift, dir;
		tjs_int ripplewidth_step = RippleWidth * SHIM_RIPPLE_DIR_PREC;
		for (drift = 0; drift < MaxDrift * SHIM_RIPPLE_DRIFT_PREC; drift++)
		{
			tjs_int32 fdrift = (drift << 10) / SHIM_RIPPLE_DRIFT_PREC; // 8.10
			tjs_uint16 *dmp = DriftMap +
				drift * RippleWidth * (2 * SHIM_RIPPLE_DIR_PREC);
			for (w = 0; w < RippleWidth; w++)
			{
				tjs_int32 fd = rippleform[w] * fdrift >> 10; // 8.11
				for (dir = 0; dir < SHIM_RIPPLE_DIR_PREC; dir++)
				{
					tjs_int32 xd = cos_table[dir] * fd >> 11; // 8.11
					tjs_int32 yd = sin_table[dir] * fd >> 11; // 8.11
					tjs_uint16 val = (tjs_uint16)(
						((int)(char)(int)(xd >> 11) << 8) +
						(int)(char)(int)(yd >> 11));
					dmp[w * SHIM_RIPPLE_DIR_PREC + dir] =
					dmp[w * SHIM_RIPPLE_DIR_PREC + ripplewidth_step + dir] = val;
				}
			}
		}
	}
	catch (...)
	{
		Clear();
		delete[] rippleform;
		delete[] sin_table;
		delete[] cos_table;
		throw;
	}
	delete[] rippleform;
	delete[] sin_table;
	delete[] cos_table;
}

// ripple.cpp:278-282.
void tShimRippleTable::Clear()
{
	delete[] DisplaceMap; DisplaceMap = 0;
	delete[] DriftMap; DriftMap = 0;
}

// ripple.cpp:290-383. A four-entry LRU cache of tables, keyed on the full
// parameter tuple. The cache is process-global and cleared on unlink, exactly
// as upstream clears it in UnregisterRippleTransHandlerProvider.
#define SHIM_RIPPLE_TABLE_MAX_CACHE 4
static tShimRippleTable *g_ripple_table_cache[SHIM_RIPPLE_TABLE_MAX_CACHE] =
	{ 0, 0, 0, 0 };

// ripple.cpp:295-360.
static tShimRippleTable *ShimGetRippleTable(tjs_int width, tjs_int height,
	tjs_int centerx, tjs_int centery, tjs_int ripplewidth, float roundness,
	tjs_int maxdrift)
{
	tjs_int i;
	for (i = 0; i < SHIM_RIPPLE_TABLE_MAX_CACHE; i++)
	{
		tShimRippleTable *table = g_ripple_table_cache[i];
		if (!table) continue;
		if (table->GetWidth() == width && table->GetHeight() == height &&
			table->GetCenterX() == centerx && table->GetCenterY() == centery &&
			table->GetRippleWidth() == ripplewidth &&
			table->GetRoundness() == roundness &&
			table->GetMaxDrift() == maxdrift)
		{
			if (i != 0)
			{
				memmove(g_ripple_table_cache + 1, g_ripple_table_cache,
					i * sizeof(tShimRippleTable *));
				g_ripple_table_cache[0] = table;
			}
			table->AddRef();
			return table;
		}
	}

	if (g_ripple_table_cache[SHIM_RIPPLE_TABLE_MAX_CACHE - 1])
	{
		tShimRippleTable *table =
			g_ripple_table_cache[SHIM_RIPPLE_TABLE_MAX_CACHE - 1];
		g_ripple_table_cache[SHIM_RIPPLE_TABLE_MAX_CACHE - 1] = 0;
		table->Release();
	}

	tShimRippleTable *table = new tShimRippleTable(width, height, centerx,
		centery, ripplewidth, roundness, maxdrift);

	memmove(g_ripple_table_cache + 1, g_ripple_table_cache,
		(SHIM_RIPPLE_TABLE_MAX_CACHE - 1) * sizeof(tShimRippleTable *));
	g_ripple_table_cache[0] = table;
	table->AddRef();
	return table;
}

// ripple.cpp:362-383.
static void ShimClearRippleTableCache()
{
	for (tjs_int i = 0; i < SHIM_RIPPLE_TABLE_MAX_CACHE; i++)
	{
		tShimRippleTable *table = g_ripple_table_cache[i];
		g_ripple_table_cache[i] = 0;
		if (table) table->Release();
	}
}

// ripple.cpp:398-404. The per-pixel blend: cross-fade src1/src2 at `ratio`,
// keeping the 0x00ff00ff / 0x0000ff00 lane split upstream uses so the two
// half-word products can be done without crossing byte boundaries.
#define SHIM_RIPPLE_BLEND \
	{ \
		tjs_uint32 s1, s2, s1_; \
		s1 = *(const tjs_uint32*)(src1 + ofs); \
		s2 = *(const tjs_uint32*)(src2 + ofs); \
		s1_ = s1 & 0xff00ff; \
		s1_ = (s1_ + (((s2 & 0xff00ff) - s1_) * ratio >> 8)) & 0xff00ff; \
		s2 &= 0xff00; s1 &= 0xff00; \
		dest[i] = s1_ | ((s1 + ((s2 - s1) * ratio >> 8)) & 0xff00); \
	}

// ripple.cpp:406-417. Forward scan, no border folding.
static void ShimRippleTransform_c_f(const tjs_uint16 *displacemap,
	const tjs_uint16 *driftmap, tjs_uint32 *dest, tjs_int num, tjs_int pitch,
	const tjs_uint8 *src1, const tjs_uint8 *src2, tjs_int ratio)
{
	for (int i = 0; i < num; i++)
	{
		tjs_int n = driftmap[displacemap[i]];
		tjs_int ofs = (int)((i - (int)(char)(n >> 8)) * sizeof(tjs_uint32)) +
			(int)(char)(n) * pitch;
		SHIM_RIPPLE_BLEND
	}
}

// ripple.cpp:419-430. Backward scan, no border folding.
static void ShimRippleTransform_c_b(const tjs_uint16 *displacemap,
	const tjs_uint16 *driftmap, tjs_uint32 *dest, tjs_int num, tjs_int pitch,
	const tjs_uint8 *src1, const tjs_uint8 *src2, tjs_int ratio)
{
	for (int i = 0; i < num; i++)
	{
		tjs_int n = driftmap[*(displacemap--)];
		tjs_int ofs = (int)((i + (int)(char)(n >> 8)) * sizeof(tjs_uint32)) +
			(int)(char)(n) * pitch;
		SHIM_RIPPLE_BLEND
	}
}

// ripple.cpp:917-924. Fold coordinates that fall outside [0,srcwidth) x
// [0,srcheight) back by reflection, so the border scanlines never read past
// the source buffers.
#define SHIM_RIPPLE_TURN_BORDER \
	{ \
		if (x < 0) x = -x; \
		if (y < 0) y = -y; \
		if (x >= srcwidth) x = srcwidth - 1 - (x - srcwidth); \
		if (y >= srcheight) y = srcheight - 1 - (y - srcheight); \
	}
#define SHIM_RIPPLE_CALC_OFS \
	tjs_int ofs = x * (tjs_int)sizeof(tjs_uint32) + y * pitch;

// ripple.cpp:926-940. Forward, y-ascending, border-folded.
static void ShimRippleTransform_f_a_e(const tjs_uint16 *displacemap,
	const tjs_uint16 *driftmap, tjs_uint32 *dest, tjs_int num, tjs_int pitch,
	const tjs_uint8 *src1, const tjs_uint8 *src2, tjs_int srcx, tjs_int srcy,
	tjs_int srcwidth, tjs_int srcheight, tjs_int ratio)
{
	for (int i = 0; i < num; i++)
	{
		tjs_int n = driftmap[displacemap[i]];
		tjs_int x = srcx + i - (int)(char)(n >> 8);
		tjs_int y = srcy + (int)(char)n;
		SHIM_RIPPLE_TURN_BORDER
		SHIM_RIPPLE_CALC_OFS
		SHIM_RIPPLE_BLEND
	}
}

// ripple.cpp:942-956. Forward, y-descending, border-folded.
static void ShimRippleTransform_f_d_e(const tjs_uint16 *displacemap,
	const tjs_uint16 *driftmap, tjs_uint32 *dest, tjs_int num, tjs_int pitch,
	const tjs_uint8 *src1, const tjs_uint8 *src2, tjs_int srcx, tjs_int srcy,
	tjs_int srcwidth, tjs_int srcheight, tjs_int ratio)
{
	for (int i = 0; i < num; i++)
	{
		tjs_int n = driftmap[displacemap[i]];
		tjs_int x = srcx + i - (int)(char)(n >> 8);
		tjs_int y = srcy - (int)(char)n;
		SHIM_RIPPLE_TURN_BORDER
		SHIM_RIPPLE_CALC_OFS
		SHIM_RIPPLE_BLEND
	}
}

// ripple.cpp:958-972. Backward, y-ascending, border-folded.
static void ShimRippleTransform_b_a_e(const tjs_uint16 *displacemap,
	const tjs_uint16 *driftmap, tjs_uint32 *dest, tjs_int num, tjs_int pitch,
	const tjs_uint8 *src1, const tjs_uint8 *src2, tjs_int srcx, tjs_int srcy,
	tjs_int srcwidth, tjs_int srcheight, tjs_int ratio)
{
	for (int i = 0; i < num; i++)
	{
		tjs_int n = driftmap[*(displacemap--)];
		tjs_int x = srcx + i + (int)(char)(n >> 8);
		tjs_int y = srcy + (int)(char)n;
		SHIM_RIPPLE_TURN_BORDER
		SHIM_RIPPLE_CALC_OFS
		SHIM_RIPPLE_BLEND
	}
}

// ripple.cpp:974-988. Backward, y-descending, border-folded.
static void ShimRippleTransform_b_d_e(const tjs_uint16 *displacemap,
	const tjs_uint16 *driftmap, tjs_uint32 *dest, tjs_int num, tjs_int pitch,
	const tjs_uint8 *src1, const tjs_uint8 *src2, tjs_int srcx, tjs_int srcy,
	tjs_int srcwidth, tjs_int srcheight, tjs_int ratio)
{
	for (int i = 0; i < num; i++)
	{
		tjs_int n = driftmap[*(displacemap--)];
		tjs_int x = srcx + i + (int)(char)(n >> 8);
		tjs_int y = srcy - (int)(char)n;
		SHIM_RIPPLE_TURN_BORDER
		SHIM_RIPPLE_CALC_OFS
		SHIM_RIPPLE_BLEND
	}
}

#undef SHIM_RIPPLE_CALC_OFS
#undef SHIM_RIPPLE_TURN_BORDER
#undef SHIM_RIPPLE_BLEND

// ripple.cpp:1007-1128.
class tShimRippleTransHandler : public iTVPDivisibleTransHandler
{
	tjs_int RefCount;

	tjs_uint64 StartTick;
	tjs_uint64 Time;
	tjs_int Width;
	tjs_int Height;
	tjs_int64 CurTime;
	tjs_int BlendRatio;
	tjs_int Phase;
	tjs_int Drift;
	bool First;
	tjs_int DriftCarePixels;
	tjs_int CenterX;
	tjs_int CenterY;
	tjs_int RippleWidth;
	float Roundness;
	float Speed;
	tjs_int MaxDrift;
	const tjs_uint16 *CurDriftMap;
	tShimRippleTable *Table;

public:
	tShimRippleTransHandler(tjs_uint64 time, tjs_int width, tjs_int height,
		tjs_int centerx, tjs_int centery, tjs_int ripplewidth,
		float roundness, float speed, tjs_int maxdrift)
	{
		RefCount = 1;
		Width = width;
		Height = height;
		Time = time;
		CenterX = centerx;
		CenterY = centery;
		RippleWidth = ripplewidth;
		Roundness = roundness;
		Speed = speed;
		First = true;
		MaxDrift = maxdrift;
		Table = ShimGetRippleTable(Width, Height, CenterX, CenterY,
			RippleWidth, Roundness, MaxDrift);
	}

	virtual ~tShimRippleTransHandler() { Table->Release(); }

	virtual tjs_error SHIM_INTF_METHOD AddRef() { RefCount++; return SHIM_TJS_S_OK; }
	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1) delete this;
		else RefCount--;
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD SetOption(iTVPSimpleOptionProvider *)
	{
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD StartProcess(tjs_uint64 tick);
	virtual tjs_error SHIM_INTF_METHOD EndProcess();
	virtual tjs_error SHIM_INTF_METHOD Process(tTVPDivisibleData *data);

	virtual tjs_error SHIM_INTF_METHOD MakeFinalImage(
		iTVPScanLineProvider **dest, iTVPScanLineProvider *,
		iTVPScanLineProvider *src2)
	{
		*dest = src2;
		return SHIM_TJS_S_OK;
	}
};

// ripple.cpp:1130-1178.
tjs_error SHIM_INTF_METHOD tShimRippleTransHandler::StartProcess(tjs_uint64 tick)
{
	if (First)
	{
		First = false;
		StartTick = tick;
	}

	CurTime = (tjs_int64)(tick - StartTick);

	BlendRatio = (tjs_int)(CurTime * 255 / (tjs_int64)Time);
	if (BlendRatio > 255) BlendRatio = 255;

	// ripple.cpp:1155-1159. Phase wraps mod RippleWidth; the -1 makes the wave
	// travel outward (later phases read earlier table rows).
	Phase = (int)(Speed * ((1.0 / (M_PI * 2.0)) * (1.0 / 1000.0)) * CurTime *
		RippleWidth) % RippleWidth;
	if (Phase < 0) Phase = 0;
	Phase = RippleWidth - Phase - 1;

	// ripple.cpp:1162-1168. Drift eases in and out with a half-sine, quantised
	// to the drift sub-pixel resolution.
	float s = sinf(M_PI * CurTime / (tjs_int64)Time);
	Drift = (int)(s * MaxDrift * SHIM_RIPPLE_DRIFT_PREC);
	if (Drift < 0) Drift = 0;
	if (Drift >= MaxDrift * SHIM_RIPPLE_DRIFT_PREC)
		Drift = MaxDrift * SHIM_RIPPLE_DRIFT_PREC - 1;

	DriftCarePixels = (int)(Drift / SHIM_RIPPLE_DRIFT_PREC) + 1;
	if (DriftCarePixels & 1) DriftCarePixels++; // align to even

	CurDriftMap = Table->GetDriftMap(Drift, Phase);

	return SHIM_TJS_S_TRUE;
}

// ripple.cpp:1180-1187.
tjs_error SHIM_INTF_METHOD tShimRippleTransHandler::EndProcess()
{
	if (BlendRatio == 255) return SHIM_TJS_S_FALSE;
	return SHIM_TJS_S_TRUE;
}

// ripple.cpp:1189-1410. Each scanline is split into up to four segments:
// left-of-centre / right-of-centre, and within each, a border-folded edge band
// (where the displacement could read past the source) and a fast interior band
// (where it cannot). The four transform variants above cover the combinations
// of forward/backward scan direction and y-ascending/descending.
tjs_error SHIM_INTF_METHOD tShimRippleTransHandler::Process(
	tTVPDivisibleData *data)
{
	tjs_int destxofs = data->DestLeft - data->Left;

	tjs_uint8 *dest;
	tjs_int destpitch;
	const tjs_uint8 *src1;
	tjs_int src1pitch;
	const tjs_uint8 *src2;
	tjs_int src2pitch;
	if (SHIM_TJS_FAILED(data->Dest->GetScanLineForWrite(data->DestTop,
			(void **)&dest)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src1->GetScanLine(0, (const void **)&src1)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src2->GetScanLine(0, (const void **)&src2)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Dest->GetPitchBytes(&destpitch)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src1->GetPitchBytes(&src1pitch)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src2->GetPitchBytes(&src2pitch)))
		return SHIM_TJS_E_FAIL;

	// ripple.cpp:1221. Both sources must share pitch.
	if (src1pitch != src2pitch) return SHIM_TJS_E_FAIL;

	tjs_int h = data->Height;
	tjs_int y = data->Top;
	while (h--)
	{
		tjs_int l, r;

		if (y < DriftCarePixels || y >= Height - DriftCarePixels)
		{
			// ripple.cpp:1230-1281. Top/bottom border: full-width fold.
			l = 0; r = CenterX;
			if (ShimClip(l, r, data->Left, data->Left + data->Width))
			{
				if (y < CenterY)
					ShimRippleTransform_b_a_e(
						Table->GetDisplaceMap(CenterX - l - 1, CenterY - y - 1),
						CurDriftMap,
						(tjs_uint32 *)dest + l + destxofs, r - l, src1pitch,
						src1, src2, l, y, Width, Height, BlendRatio);
				else
					ShimRippleTransform_b_d_e(
						Table->GetDisplaceMap(CenterX - l - 1, y - CenterY),
						CurDriftMap,
						(tjs_uint32 *)dest + l + destxofs, r - l, src1pitch,
						src1, src2, l, y, Width, Height, BlendRatio);
			}

			l = CenterX; r = Width;
			if (ShimClip(l, r, data->Left, data->Left + data->Width))
			{
				if (y < CenterY)
					ShimRippleTransform_f_a_e(
						Table->GetDisplaceMap(l - CenterX, CenterY - y - 1),
						CurDriftMap,
						(tjs_uint32 *)dest + l + destxofs, r - l, src1pitch,
						src1, src2, l, y, Width, Height, BlendRatio);
				else
					ShimRippleTransform_f_d_e(
						Table->GetDisplaceMap(l - CenterX, y - CenterY),
						CurDriftMap,
						(tjs_uint32 *)dest + l + destxofs, r - l, src1pitch,
						src1, src2, l, y, Width, Height, BlendRatio);
			}
		}
		else
		{
			// ripple.cpp:1284-1402. Interior: left/right edge bands fold, the
			// middle uses the fast no-fold scan.
			l = 0; r = CenterX;
			if (ShimClip(l, r, data->Left, data->Left + data->Width))
			{
				tjs_int ll, rr;
				ll = 0; rr = DriftCarePixels;
				if (ShimClip(ll, rr, l, r))
				{
					if (y < CenterY)
						ShimRippleTransform_b_a_e(
							Table->GetDisplaceMap(CenterX - ll - 1,
								CenterY - y - 1),
							CurDriftMap,
							(tjs_uint32 *)dest + ll + destxofs, rr - ll,
							src1pitch, src1, src2, ll, y, Width, Height,
							BlendRatio);
					else
						ShimRippleTransform_b_d_e(
							Table->GetDisplaceMap(CenterX - ll - 1,
								y - CenterY),
							CurDriftMap,
							(tjs_uint32 *)dest + ll + destxofs, rr - ll,
							src1pitch, src1, src2, ll, y, Width, Height,
							BlendRatio);
				}

				ll = DriftCarePixels; rr = r;
				if (ShimClip(ll, rr, l, r))
				{
					if (y < CenterY)
						ShimRippleTransform_c_b(
							Table->GetDisplaceMap(CenterX - ll - 1,
								CenterY - y - 1),
							CurDriftMap,
							(tjs_uint32 *)dest + ll + destxofs, rr - ll,
							src1pitch,
							(const tjs_uint8 *)((const tjs_uint32 *)
								(src1 + y * src1pitch) + ll),
							(const tjs_uint8 *)((const tjs_uint32 *)
								(src2 + y * src2pitch) + ll),
							BlendRatio);
					else
						ShimRippleTransform_c_b(
							Table->GetDisplaceMap(CenterX - ll - 1,
								y - CenterY),
							CurDriftMap,
							(tjs_uint32 *)dest + ll + destxofs, rr - ll,
							-src1pitch,
							(const tjs_uint8 *)((const tjs_uint32 *)
								(src1 + y * src1pitch) + ll),
							(const tjs_uint8 *)((const tjs_uint32 *)
								(src2 + y * src2pitch) + ll),
							BlendRatio);
				}
			}

			l = CenterX; r = Width;
			if (ShimClip(l, r, data->Left, data->Left + data->Width))
			{
				tjs_int ll, rr;
				ll = l; rr = Width - DriftCarePixels;
				if (ShimClip(ll, rr, l, r))
				{
					if (y < CenterY)
						ShimRippleTransform_c_f(
							Table->GetDisplaceMap(ll - CenterX,
								CenterY - y - 1),
							CurDriftMap,
							(tjs_uint32 *)dest + ll + destxofs, rr - ll,
							src1pitch,
							(const tjs_uint8 *)((const tjs_uint32 *)
								(src1 + y * src1pitch) + ll),
							(const tjs_uint8 *)((const tjs_uint32 *)
								(src2 + y * src2pitch) + ll),
							BlendRatio);
					else
						ShimRippleTransform_c_f(
							Table->GetDisplaceMap(ll - CenterX, y - CenterY),
							CurDriftMap,
							(tjs_uint32 *)dest + ll + destxofs, rr - ll,
							-src1pitch,
							(const tjs_uint8 *)((const tjs_uint32 *)
								(src1 + y * src1pitch) + ll),
							(const tjs_uint8 *)((const tjs_uint32 *)
								(src2 + y * src2pitch) + ll),
							BlendRatio);
				}

				ll = Width - DriftCarePixels; rr = r;
				if (ShimClip(ll, rr, l, r))
				{
					if (y < CenterY)
						ShimRippleTransform_f_a_e(
							Table->GetDisplaceMap(ll - CenterX,
								CenterY - y - 1),
							CurDriftMap,
							(tjs_uint32 *)dest + ll + destxofs, rr - ll,
							src1pitch, src1, src2, ll, y, Width, Height,
							BlendRatio);
					else
						ShimRippleTransform_f_d_e(
							Table->GetDisplaceMap(ll - CenterX, y - CenterY),
							CurDriftMap,
							(tjs_uint32 *)dest + ll + destxofs, rr - ll,
							src1pitch, src1, src2, ll, y, Width, Height,
							BlendRatio);
				}
			}
		}

		dest += destpitch;
		y++;
	}

	return SHIM_TJS_S_OK;
}

// ripple.cpp:1418-1536.
class tShimRippleTransHandlerProvider : public iTVPTransHandlerProvider
{
	tjs_int RefCount;
public:
	tShimRippleTransHandlerProvider() { RefCount = 1; }
	virtual ~tShimRippleTransHandlerProvider() {}

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}
	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1) delete this;
		else RefCount--;
		return SHIM_TJS_S_OK;
	}

	// ripple.cpp:1448.
	virtual tjs_error SHIM_INTF_METHOD GetName(const tjs_char **name)
	{
		if (name)
		{
			*name = SHIM_TJS_W("ripple");
			return SHIM_TJS_S_OK;
		}
		return SHIM_TJS_E_FAIL;
	}

	virtual tjs_error SHIM_INTF_METHOD StartTransition(
		iTVPSimpleOptionProvider *options,
		iTVPSimpleImageProvider *,
		tTVPLayerType,
		tjs_uint src1w, tjs_uint src1h,
		tjs_uint src2w, tjs_uint src2h,
		tTVPTransType *type,
		tTVPTransUpdateType *updatetype,
		iTVPBaseTransHandler **handler)
	{
		if (type) *type = ttExchange;
		if (updatetype) *updatetype = tutDivisible;
		if (!handler) return SHIM_TJS_E_FAIL;
		if (!options) return SHIM_TJS_E_FAIL;
		if (src1w != src2w || src1h != src2h)
			return SHIM_TJS_E_FAIL;

		// ripple.cpp:1478-1482. Defaults.
		tjs_int64 time;
		tjs_int centerx = src1w >> 1, centery = src1h >> 1;
		tjs_int ripplewidth = 128;
		// roundness/speed/maxdrift are real-valued upstream, read through
		// GetValue (tTJSVariant). GetAsNumber returns an int64, so a fractional
		// value is truncated — same caveat as wave's maxomega. Fate never
		// passes any of these (no ripple call site exists in the corpus), so
		// the defaults are what the game would get.
		float roundness = 1.0f;
		float speed = 6;
		tjs_int maxdrift = 24;

		if (!ShimOption(options, SHIM_TJS_W("time"), &time))
			return SHIM_TJS_E_FAIL;
		if (time < 2) time = 2;

		tjs_int64 tmp;
		if (ShimOption(options, SHIM_TJS_W("centerx"), &tmp))
			centerx = (tjs_int)tmp;
		if (ShimOption(options, SHIM_TJS_W("centery"), &tmp))
			centery = (tjs_int)tmp;

		// ripple.cpp:1498-1500. Centre must be inside the image.
		if (centerx < 0 || centery < 0 ||
			(tjs_uint)centerx >= src1w || (tjs_uint)centery >= src1h)
			return SHIM_TJS_E_FAIL;

		if (ShimOption(options, SHIM_TJS_W("rwidth"), &tmp))
			ripplewidth = (tjs_int)tmp;

		// ripple.cpp:1506-1508. Only power-of-two widths fit the mask.
		if (ripplewidth != 16 && ripplewidth != 32 && ripplewidth != 64 &&
			ripplewidth != 128)
			return SHIM_TJS_E_FAIL;

		if (ShimOption(options, SHIM_TJS_W("roundness"), &tmp))
			roundness = (float)(double)tmp;
		if (roundness <= 0.0f)
			return SHIM_TJS_E_FAIL;

		if (ShimOption(options, SHIM_TJS_W("speed"), &tmp))
			speed = (float)(double)tmp;

		if (ShimOption(options, SHIM_TJS_W("maxdrift"), &tmp))
			maxdrift = (tjs_int)tmp;
		// ripple.cpp:1522-1523.
		if (maxdrift < 0 || maxdrift >= 128)
			return SHIM_TJS_E_FAIL;
		// ripple.cpp:1525-1526.
		if ((tjs_uint)maxdrift >= src1w || (tjs_uint)maxdrift >= src1h)
			return SHIM_TJS_E_FAIL;

		*handler = new tShimRippleTransHandler((tjs_uint64)time,
			(tjs_int)src1w, (tjs_int)src1h, centerx, centery,
			ripplewidth, roundness, speed, maxdrift);
		return SHIM_TJS_S_OK;
	}
};


//--------------------------------------------------------------------------
// `turn` — tile-flip transition
//--------------------------------------------------------------------------
// Ported from extrans/turn.cpp (lines 1-591) with the generated deformation
// table from extrans/turntrans_table.cpp (lines 1-4360). The image is divided
// into 64x64 tiles; each tile flips from src1 to src2 along its diagonal, the
// flip phased so the bottom-left tiles turn first and the top-right last. The
// per-tile, per-scanline deformation is a fixed lookup in TurnTransParams.
//
// The table below is upstream-generated (turntrans_table.cpp:2 "このファイルは
// mkturntranstable.pl により作成されました" — "this file was created by
// mkturntranstable.pl"). It is reproduced verbatim, only reformatted from the
// upstream one-entry-per-line layout to four-per-line to keep the file
// manageable; every value is byte-identical to the upstream source. The
// tTurnTransParams struct layout (turntrans_table.h:8-20) is reproduced
// locally below so no engine header is needed.
//
// turn.cpp uses TVPFillARGB, memcpy, and TVPLinTransCopy (the latter only in
// the no-gloss fast path, turn.cpp:465-467). TVPFillARGB and TVPLinTransCopy
// are resolved by name through the exporter, as the other handlers do; memcpy
// is the standard library.

// turntrans_table.h:8-20. pack(4) upstream; the struct is 8 ints = 32 bytes,
// already 4-byte aligned, so the pragma is a no-op on every target and is
// omitted.
struct tShimTurnTransParams
{
	tjs_int start;   // destination start x within the 64px tile
	tjs_int len;     // visible run length
	tjs_int sx;      // source start x (* 65536)
	tjs_int sy;      // source start y (* 65536)
	tjs_int ex;      // source end x (* 65536)
	tjs_int ey;      // source end y (* 65536)
	tjs_int stepx;   // source step x (* 65536)
	tjs_int stepy;   // source step y (* 65536)
};

// turntrans_table.cpp:6-4360. 64 phases x 64 scanlines. Phase 0 and phase 63
// are unused (the handler short-circuits to a plain copy at those phases,
// turn.cpp:242-265), but are present in the table for faithful reproduction.
static const tShimTurnTransParams ShimTurnTransParams[64][64] =
{
	{ /* n = 0 */
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
	},
	{ /* n = 1 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 2 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 3 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 4 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 5 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 6 */
		{0,1,0,0,0,0,0,0},
		{0,63,0,65536,4128768,0,66593,-1057},
		{0,63,0,131072,4128768,65536,66593,-1057},
		{0,63,0,196608,4128768,131072,66593,-1057},
		{0,63,0,262144,4128768,196608,66593,-1057},
		{0,63,0,327680,4128768,262144,66593,-1057},
		{0,63,0,393216,4128768,327680,66593,-1057},
		{0,63,0,458752,4128768,393216,66593,-1057},
		{0,63,0,524288,4128768,458752,66593,-1057},
		{0,63,0,589824,4128768,524288,66593,-1057},
		{0,63,0,655360,4128768,589824,66593,-1057},
		{0,63,0,720896,4128768,655360,66593,-1057},
		{0,63,0,786432,4128768,720896,66593,-1057},
		{0,63,0,851968,4128768,786432,66593,-1057},
		{0,63,0,917504,4128768,851968,66593,-1057},
		{0,63,0,983040,4128768,917504,66593,-1057},
		{0,63,0,1048576,4128768,983040,66593,-1057},
		{0,63,0,1114112,4128768,1048576,66593,-1057},
		{0,63,0,1179648,4128768,1114112,66593,-1057},
		{0,63,0,1245184,4128768,1179648,66593,-1057},
		{0,63,0,1310720,4128768,1245184,66593,-1057},
		{0,63,0,1376256,4128768,1310720,66593,-1057},
		{0,63,0,1441792,4128768,1376256,66593,-1057},
		{0,63,0,1507328,4128768,1441792,66593,-1057},
		{0,63,0,1572864,4128768,1507328,66593,-1057},
		{0,63,0,1638400,4128768,1572864,66593,-1057},
		{0,63,0,1703936,4128768,1638400,66593,-1057},
		{0,63,0,1769472,4128768,1703936,66593,-1057},
		{0,63,0,1835008,4128768,1769472,66593,-1057},
		{0,63,0,1900544,4128768,1835008,66593,-1057},
		{0,63,0,1966080,4128768,1900544,66593,-1057},
		{0,63,0,2031616,4128768,1966080,66593,-1057},
		{0,63,0,2097152,4128768,2031616,66593,-1057},
		{0,63,0,2162688,4128768,2097152,66593,-1057},
		{0,63,0,2228224,4128768,2162688,66593,-1057},
		{0,63,0,2293760,4128768,2228224,66593,-1057},
		{0,63,0,2359296,4128768,2293760,66593,-1057},
		{0,63,0,2424832,4128768,2359296,66593,-1057},
		{0,63,0,2490368,4128768,2424832,66593,-1057},
		{0,63,0,2555904,4128768,2490368,66593,-1057},
		{0,63,0,2621440,4128768,2555904,66593,-1057},
		{0,63,0,2686976,4128768,2621440,66593,-1057},
		{0,63,0,2752512,4128768,2686976,66593,-1057},
		{0,63,0,2818048,4128768,2752512,66593,-1057},
		{0,63,0,2883584,4128768,2818048,66593,-1057},
		{0,63,0,2949120,4128768,2883584,66593,-1057},
		{0,63,0,3014656,4128768,2949120,66593,-1057},
		{0,63,0,3080192,4128768,3014656,66593,-1057},
		{0,63,0,3145728,4128768,3080192,66593,-1057},
		{0,63,0,3211264,4128768,3145728,66593,-1057},
		{0,63,0,3276800,4128768,3211264,66593,-1057},
		{0,63,0,3342336,4128768,3276800,66593,-1057},
		{0,63,0,3407872,4128768,3342336,66593,-1057},
		{0,63,0,3473408,4128768,3407872,66593,-1057},
		{0,63,0,3538944,4128768,3473408,66593,-1057},
		{0,63,0,3604480,4128768,3538944,66593,-1057},
		{0,63,0,3670016,4128768,3604480,66593,-1057},
		{0,63,0,3735552,4128768,3670016,66593,-1057},
		{0,63,0,3801088,4128768,3735552,66593,-1057},
		{0,63,0,3866624,4128768,3801088,66593,-1057},
		{0,63,0,3932160,4128768,3866624,66593,-1057},
		{0,63,0,3997696,4128768,3932160,66593,-1057},
		{1,62,0,4128768,4128768,3997696,67684,-2148},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 7 */
		{0,1,0,0,0,0,0,0},
		{0,63,0,65536,4128768,0,66593,-1057},
		{0,63,0,131072,4128768,65536,66593,-1057},
		{0,63,0,196608,4128768,131072,66593,-1057},
		{0,63,0,262144,4128768,196608,66593,-1057},
		{0,63,0,327680,4128768,262144,66593,-1057},
		{0,63,0,393216,4128768,327680,66593,-1057},
		{0,63,0,458752,4128768,393216,66593,-1057},
		{0,63,0,524288,4128768,458752,66593,-1057},
		{0,63,0,589824,4128768,524288,66593,-1057},
		{0,63,0,655360,4128768,589824,66593,-1057},
		{0,63,0,720896,4128768,655360,66593,-1057},
		{0,63,0,786432,4128768,720896,66593,-1057},
		{0,63,0,851968,4128768,786432,66593,-1057},
		{0,63,0,917504,4128768,851968,66593,-1057},
		{0,63,0,983040,4128768,917504,66593,-1057},
		{0,63,0,1048576,4128768,983040,66593,-1057},
		{0,63,0,1114112,4128768,1048576,66593,-1057},
		{0,63,0,1179648,4128768,1114112,66593,-1057},
		{0,63,0,1245184,4128768,1179648,66593,-1057},
		{0,63,0,1310720,4128768,1245184,66593,-1057},
		{0,63,0,1376256,4128768,1310720,66593,-1057},
		{0,63,0,1441792,4128768,1376256,66593,-1057},
		{0,63,0,1507328,4128768,1441792,66593,-1057},
		{0,63,0,1572864,4128768,1507328,66593,-1057},
		{0,63,0,1638400,4128768,1572864,66593,-1057},
		{0,63,0,1703936,4128768,1638400,66593,-1057},
		{0,63,0,1769472,4128768,1703936,66593,-1057},
		{0,63,0,1835008,4128768,1769472,66593,-1057},
		{0,63,0,1900544,4128768,1835008,66593,-1057},
		{0,63,0,1966080,4128768,1900544,66593,-1057},
		{0,63,0,2031616,4128768,1966080,66593,-1057},
		{0,63,0,2097152,4128768,2031616,66593,-1057},
		{0,63,0,2162688,4128768,2097152,66593,-1057},
		{0,63,0,2228224,4128768,2162688,66593,-1057},
		{0,63,0,2293760,4128768,2228224,66593,-1057},
		{0,63,0,2359296,4128768,2293760,66593,-1057},
		{0,63,0,2424832,4128768,2359296,66593,-1057},
		{0,63,0,2490368,4128768,2424832,66593,-1057},
		{0,63,0,2555904,4128768,2490368,66593,-1057},
		{0,63,0,2621440,4128768,2555904,66593,-1057},
		{0,63,0,2686976,4128768,2621440,66593,-1057},
		{0,63,0,2752512,4128768,2686976,66593,-1057},
		{0,63,0,2818048,4128768,2752512,66593,-1057},
		{0,63,0,2883584,4128768,2818048,66593,-1057},
		{0,63,0,2949120,4128768,2883584,66593,-1057},
		{0,63,0,3014656,4128768,2949120,66593,-1057},
		{0,63,0,3080192,4128768,3014656,66593,-1057},
		{0,63,0,3145728,4128768,3080192,66593,-1057},
		{0,63,0,3211264,4128768,3145728,66593,-1057},
		{0,63,0,3276800,4128768,3211264,66593,-1057},
		{0,63,0,3342336,4128768,3276800,66593,-1057},
		{0,63,0,3407872,4128768,3342336,66593,-1057},
		{0,63,0,3473408,4128768,3407872,66593,-1057},
		{0,63,0,3538944,4128768,3473408,66593,-1057},
		{0,63,0,3604480,4128768,3538944,66593,-1057},
		{0,63,0,3670016,4128768,3604480,66593,-1057},
		{0,63,0,3735552,4128768,3670016,66593,-1057},
		{0,63,0,3801088,4128768,3735552,66593,-1057},
		{0,63,0,3866624,4128768,3801088,66593,-1057},
		{0,63,0,3932160,4128768,3866624,66593,-1057},
		{0,63,0,3997696,4128768,3932160,66593,-1057},
		{1,62,0,4128768,4128768,3997696,67684,-2148},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 8 */
		{0,1,0,0,0,0,0,0},
		{0,31,0,65536,2031616,0,67720,-2184},
		{0,62,0,131072,4128768,0,67684,-2148},
		{0,62,0,196608,4128768,65536,67684,-2148},
		{0,62,0,262144,4128768,131072,67684,-2148},
		{0,62,0,327680,4128768,196608,67684,-2148},
		{0,62,0,393216,4128768,262144,67684,-2148},
		{0,62,0,458752,4128768,327680,67684,-2148},
		{0,62,0,524288,4128768,393216,67684,-2148},
		{0,62,0,589824,4128768,458752,67684,-2148},
		{0,62,0,655360,4128768,524288,67684,-2148},
		{0,62,0,720896,4128768,589824,67684,-2148},
		{0,62,0,786432,4128768,655360,67684,-2148},
		{0,62,0,851968,4128768,720896,67684,-2148},
		{0,62,0,917504,4128768,786432,67684,-2148},
		{0,62,0,983040,4128768,851968,67684,-2148},
		{0,62,0,1048576,4128768,917504,67684,-2148},
		{0,62,0,1114112,4128768,983040,67684,-2148},
		{0,62,0,1179648,4128768,1048576,67684,-2148},
		{0,62,0,1245184,4128768,1114112,67684,-2148},
		{0,62,0,1310720,4128768,1179648,67684,-2148},
		{0,62,0,1376256,4128768,1245184,67684,-2148},
		{0,62,0,1441792,4128768,1310720,67684,-2148},
		{0,62,0,1507328,4128768,1376256,67684,-2148},
		{0,62,0,1572864,4128768,1441792,67684,-2148},
		{0,62,0,1638400,4128768,1507328,67684,-2148},
		{0,62,0,1703936,4128768,1572864,67684,-2148},
		{0,62,0,1769472,4128768,1638400,67684,-2148},
		{0,62,0,1835008,4128768,1703936,67684,-2148},
		{0,62,0,1900544,4128768,1769472,67684,-2148},
		{0,62,0,1966080,4128768,1835008,67684,-2148},
		{1,61,0,2097152,4128768,1900544,68812,-3276},
		{1,61,0,2162688,4128768,1966080,68812,-3276},
		{1,62,0,2228224,4128768,2097152,67684,-2148},
		{1,62,0,2293760,4128768,2162688,67684,-2148},
		{1,62,0,2359296,4128768,2228224,67684,-2148},
		{1,62,0,2424832,4128768,2293760,67684,-2148},
		{1,62,0,2490368,4128768,2359296,67684,-2148},
		{1,62,0,2555904,4128768,2424832,67684,-2148},
		{1,62,0,2621440,4128768,2490368,67684,-2148},
		{1,62,0,2686976,4128768,2555904,67684,-2148},
		{1,62,0,2752512,4128768,2621440,67684,-2148},
		{1,62,0,2818048,4128768,2686976,67684,-2148},
		{1,62,0,2883584,4128768,2752512,67684,-2148},
		{1,62,0,2949120,4128768,2818048,67684,-2148},
		{1,62,0,3014656,4128768,2883584,67684,-2148},
		{1,62,0,3080192,4128768,2949120,67684,-2148},
		{1,62,0,3145728,4128768,3014656,67684,-2148},
		{1,62,0,3211264,4128768,3080192,67684,-2148},
		{1,62,0,3276800,4128768,3145728,67684,-2148},
		{1,62,0,3342336,4128768,3211264,67684,-2148},
		{1,62,0,3407872,4128768,3276800,67684,-2148},
		{1,62,0,3473408,4128768,3342336,67684,-2148},
		{1,62,0,3538944,4128768,3407872,67684,-2148},
		{1,62,0,3604480,4128768,3473408,67684,-2148},
		{1,62,0,3670016,4128768,3538944,67684,-2148},
		{1,62,0,3735552,4128768,3604480,67684,-2148},
		{1,62,0,3801088,4128768,3670016,67684,-2148},
		{1,62,0,3866624,4128768,3735552,67684,-2148},
		{1,62,0,3932160,4128768,3801088,67684,-2148},
		{1,62,0,3997696,4128768,3866624,67684,-2148},
		{2,61,0,4128768,4128768,3932160,68812,-3276},
		{32,31,2031616,4128768,4128768,3997696,69905,-4369},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 9 */
		{0,1,0,0,0,0,0,0},
		{0,31,0,65536,2031616,0,67720,-2184},
		{0,62,0,131072,4128768,0,67684,-2148},
		{0,62,0,196608,4128768,65536,67684,-2148},
		{0,62,0,262144,4128768,131072,67684,-2148},
		{0,62,0,327680,4128768,196608,67684,-2148},
		{0,62,0,393216,4128768,262144,67684,-2148},
		{0,62,0,458752,4128768,327680,67684,-2148},
		{0,62,0,524288,4128768,393216,67684,-2148},
		{0,62,0,589824,4128768,458752,67684,-2148},
		{0,62,0,655360,4128768,524288,67684,-2148},
		{0,62,0,720896,4128768,589824,67684,-2148},
		{0,62,0,786432,4128768,655360,67684,-2148},
		{0,62,0,851968,4128768,720896,67684,-2148},
		{0,62,0,917504,4128768,786432,67684,-2148},
		{0,62,0,983040,4128768,851968,67684,-2148},
		{0,62,0,1048576,4128768,917504,67684,-2148},
		{0,62,0,1114112,4128768,983040,67684,-2148},
		{0,62,0,1179648,4128768,1048576,67684,-2148},
		{0,62,0,1245184,4128768,1114112,67684,-2148},
		{0,62,0,1310720,4128768,1179648,67684,-2148},
		{0,62,0,1376256,4128768,1245184,67684,-2148},
		{0,62,0,1441792,4128768,1310720,67684,-2148},
		{0,62,0,1507328,4128768,1376256,67684,-2148},
		{0,62,0,1572864,4128768,1441792,67684,-2148},
		{0,62,0,1638400,4128768,1507328,67684,-2148},
		{0,62,0,1703936,4128768,1572864,67684,-2148},
		{0,62,0,1769472,4128768,1638400,67684,-2148},
		{0,62,0,1835008,4128768,1703936,67684,-2148},
		{0,62,0,1900544,4128768,1769472,67684,-2148},
		{0,62,0,1966080,4128768,1835008,67684,-2148},
		{1,61,0,2097152,4128768,1900544,68812,-3276},
		{1,61,0,2162688,4128768,1966080,68812,-3276},
		{1,62,0,2228224,4128768,2097152,67684,-2148},
		{1,62,0,2293760,4128768,2162688,67684,-2148},
		{1,62,0,2359296,4128768,2228224,67684,-2148},
		{1,62,0,2424832,4128768,2293760,67684,-2148},
		{1,62,0,2490368,4128768,2359296,67684,-2148},
		{1,62,0,2555904,4128768,2424832,67684,-2148},
		{1,62,0,2621440,4128768,2490368,67684,-2148},
		{1,62,0,2686976,4128768,2555904,67684,-2148},
		{1,62,0,2752512,4128768,2621440,67684,-2148},
		{1,62,0,2818048,4128768,2686976,67684,-2148},
		{1,62,0,2883584,4128768,2752512,67684,-2148},
		{1,62,0,2949120,4128768,2818048,67684,-2148},
		{1,62,0,3014656,4128768,2883584,67684,-2148},
		{1,62,0,3080192,4128768,2949120,67684,-2148},
		{1,62,0,3145728,4128768,3014656,67684,-2148},
		{1,62,0,3211264,4128768,3080192,67684,-2148},
		{1,62,0,3276800,4128768,3145728,67684,-2148},
		{1,62,0,3342336,4128768,3211264,67684,-2148},
		{1,62,0,3407872,4128768,3276800,67684,-2148},
		{1,62,0,3473408,4128768,3342336,67684,-2148},
		{1,62,0,3538944,4128768,3407872,67684,-2148},
		{1,62,0,3604480,4128768,3473408,67684,-2148},
		{1,62,0,3670016,4128768,3538944,67684,-2148},
		{1,62,0,3735552,4128768,3604480,67684,-2148},
		{1,62,0,3801088,4128768,3670016,67684,-2148},
		{1,62,0,3866624,4128768,3735552,67684,-2148},
		{1,62,0,3932160,4128768,3801088,67684,-2148},
		{1,62,0,3997696,4128768,3866624,67684,-2148},
		{2,61,0,4128768,4128768,3932160,68812,-3276},
		{32,31,2031616,4128768,4128768,3997696,69905,-4369},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 10 */
		{0,1,0,0,0,0,0,0},
		{0,21,0,65536,1376256,0,68812,-3276},
		{0,41,0,131072,2752512,0,68812,-3276},
		{0,61,0,196608,4128768,0,68812,-3276},
		{0,61,0,262144,4128768,65536,68812,-3276},
		{0,61,0,327680,4128768,131072,68812,-3276},
		{0,61,0,393216,4128768,196608,68812,-3276},
		{0,61,0,458752,4128768,262144,68812,-3276},
		{0,61,0,524288,4128768,327680,68812,-3276},
		{0,61,0,589824,4128768,393216,68812,-3276},
		{0,61,0,655360,4128768,458752,68812,-3276},
		{0,61,0,720896,4128768,524288,68812,-3276},
		{0,61,0,786432,4128768,589824,68812,-3276},
		{0,61,0,851968,4128768,655360,68812,-3276},
		{0,61,0,917504,4128768,720896,68812,-3276},
		{0,61,0,983040,4128768,786432,68812,-3276},
		{0,61,0,1048576,4128768,851968,68812,-3276},
		{0,61,0,1114112,4128768,917504,68812,-3276},
		{0,61,0,1179648,4128768,983040,68812,-3276},
		{0,61,0,1245184,4128768,1048576,68812,-3276},
		{1,60,0,1376256,4128768,1114112,69979,-4443},
		{1,60,0,1441792,4128768,1179648,69979,-4443},
		{1,60,0,1507328,4128768,1245184,69979,-4443},
		{1,61,0,1572864,4128768,1376256,68812,-3276},
		{1,61,0,1638400,4128768,1441792,68812,-3276},
		{1,61,0,1703936,4128768,1507328,68812,-3276},
		{1,61,0,1769472,4128768,1572864,68812,-3276},
		{1,61,0,1835008,4128768,1638400,68812,-3276},
		{1,61,0,1900544,4128768,1703936,68812,-3276},
		{1,61,0,1966080,4128768,1769472,68812,-3276},
		{1,61,0,2031616,4128768,1835008,68812,-3276},
		{1,61,0,2097152,4128768,1900544,68812,-3276},
		{1,61,0,2162688,4128768,1966080,68812,-3276},
		{1,61,0,2228224,4128768,2031616,68812,-3276},
		{1,61,0,2293760,4128768,2097152,68812,-3276},
		{1,61,0,2359296,4128768,2162688,68812,-3276},
		{1,61,0,2424832,4128768,2228224,68812,-3276},
		{1,61,0,2490368,4128768,2293760,68812,-3276},
		{1,61,0,2555904,4128768,2359296,68812,-3276},
		{1,61,0,2621440,4128768,2424832,68812,-3276},
		{2,60,0,2752512,4128768,2490368,69979,-4443},
		{2,60,0,2818048,4128768,2555904,69979,-4443},
		{2,60,0,2883584,4128768,2621440,69979,-4443},
		{2,61,0,2949120,4128768,2752512,68812,-3276},
		{2,61,0,3014656,4128768,2818048,68812,-3276},
		{2,61,0,3080192,4128768,2883584,68812,-3276},
		{2,61,0,3145728,4128768,2949120,68812,-3276},
		{2,61,0,3211264,4128768,3014656,68812,-3276},
		{2,61,0,3276800,4128768,3080192,68812,-3276},
		{2,61,0,3342336,4128768,3145728,68812,-3276},
		{2,61,0,3407872,4128768,3211264,68812,-3276},
		{2,61,0,3473408,4128768,3276800,68812,-3276},
		{2,61,0,3538944,4128768,3342336,68812,-3276},
		{2,61,0,3604480,4128768,3407872,68812,-3276},
		{2,61,0,3670016,4128768,3473408,68812,-3276},
		{2,61,0,3735552,4128768,3538944,68812,-3276},
		{2,61,0,3801088,4128768,3604480,68812,-3276},
		{2,61,0,3866624,4128768,3670016,68812,-3276},
		{2,61,0,3932160,4128768,3735552,68812,-3276},
		{2,61,0,3997696,4128768,3801088,68812,-3276},
		{3,60,0,4128768,4128768,3866624,69979,-4443},
		{23,40,1376256,4128768,4128768,3932160,70577,-5041},
		{43,20,2752512,4128768,4128768,3997696,72434,-6898},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 11 */
		{0,1,0,0,0,0,0,0},
		{0,21,0,65536,1376256,0,68812,-3276},
		{0,41,0,131072,2752512,0,68812,-3276},
		{0,61,0,196608,4128768,0,68812,-3276},
		{0,61,0,262144,4128768,65536,68812,-3276},
		{0,61,0,327680,4128768,131072,68812,-3276},
		{0,61,0,393216,4128768,196608,68812,-3276},
		{0,61,0,458752,4128768,262144,68812,-3276},
		{0,61,0,524288,4128768,327680,68812,-3276},
		{0,61,0,589824,4128768,393216,68812,-3276},
		{0,61,0,655360,4128768,458752,68812,-3276},
		{0,61,0,720896,4128768,524288,68812,-3276},
		{0,61,0,786432,4128768,589824,68812,-3276},
		{0,61,0,851968,4128768,655360,68812,-3276},
		{0,61,0,917504,4128768,720896,68812,-3276},
		{0,61,0,983040,4128768,786432,68812,-3276},
		{0,61,0,1048576,4128768,851968,68812,-3276},
		{0,61,0,1114112,4128768,917504,68812,-3276},
		{0,61,0,1179648,4128768,983040,68812,-3276},
		{0,61,0,1245184,4128768,1048576,68812,-3276},
		{1,60,0,1376256,4128768,1114112,69979,-4443},
		{1,60,0,1441792,4128768,1179648,69979,-4443},
		{1,60,0,1507328,4128768,1245184,69979,-4443},
		{1,61,0,1572864,4128768,1376256,68812,-3276},
		{1,61,0,1638400,4128768,1441792,68812,-3276},
		{1,61,0,1703936,4128768,1507328,68812,-3276},
		{1,61,0,1769472,4128768,1572864,68812,-3276},
		{1,61,0,1835008,4128768,1638400,68812,-3276},
		{1,61,0,1900544,4128768,1703936,68812,-3276},
		{1,61,0,1966080,4128768,1769472,68812,-3276},
		{1,61,0,2031616,4128768,1835008,68812,-3276},
		{1,61,0,2097152,4128768,1900544,68812,-3276},
		{1,61,0,2162688,4128768,1966080,68812,-3276},
		{1,61,0,2228224,4128768,2031616,68812,-3276},
		{1,61,0,2293760,4128768,2097152,68812,-3276},
		{1,61,0,2359296,4128768,2162688,68812,-3276},
		{1,61,0,2424832,4128768,2228224,68812,-3276},
		{1,61,0,2490368,4128768,2293760,68812,-3276},
		{1,61,0,2555904,4128768,2359296,68812,-3276},
		{1,61,0,2621440,4128768,2424832,68812,-3276},
		{2,60,0,2752512,4128768,2490368,69979,-4443},
		{2,60,0,2818048,4128768,2555904,69979,-4443},
		{2,60,0,2883584,4128768,2621440,69979,-4443},
		{2,61,0,2949120,4128768,2752512,68812,-3276},
		{2,61,0,3014656,4128768,2818048,68812,-3276},
		{2,61,0,3080192,4128768,2883584,68812,-3276},
		{2,61,0,3145728,4128768,2949120,68812,-3276},
		{2,61,0,3211264,4128768,3014656,68812,-3276},
		{2,61,0,3276800,4128768,3080192,68812,-3276},
		{2,61,0,3342336,4128768,3145728,68812,-3276},
		{2,61,0,3407872,4128768,3211264,68812,-3276},
		{2,61,0,3473408,4128768,3276800,68812,-3276},
		{2,61,0,3538944,4128768,3342336,68812,-3276},
		{2,61,0,3604480,4128768,3407872,68812,-3276},
		{2,61,0,3670016,4128768,3473408,68812,-3276},
		{2,61,0,3735552,4128768,3538944,68812,-3276},
		{2,61,0,3801088,4128768,3604480,68812,-3276},
		{2,61,0,3866624,4128768,3670016,68812,-3276},
		{2,61,0,3932160,4128768,3735552,68812,-3276},
		{2,61,0,3997696,4128768,3801088,68812,-3276},
		{3,60,0,4128768,4128768,3866624,69979,-4443},
		{23,40,1376256,4128768,4128768,3932160,70577,-5041},
		{43,20,2752512,4128768,4128768,3997696,72434,-6898},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 12 */
		{0,1,0,0,0,0,0,0},
		{0,15,0,65536,983040,0,70217,-4681},
		{0,30,0,131072,2031616,0,70055,-4519},
		{0,45,0,196608,3080192,0,70004,-4468},
		{0,60,0,262144,4128768,0,69979,-4443},
		{0,60,0,327680,4128768,65536,69979,-4443},
		{0,60,0,393216,4128768,131072,69979,-4443},
		{0,60,0,458752,4128768,196608,69979,-4443},
		{0,60,0,524288,4128768,262144,69979,-4443},
		{0,60,0,589824,4128768,327680,69979,-4443},
		{0,60,0,655360,4128768,393216,69979,-4443},
		{0,60,0,720896,4128768,458752,69979,-4443},
		{0,60,0,786432,4128768,524288,69979,-4443},
		{0,60,0,851968,4128768,589824,69979,-4443},
		{0,60,0,917504,4128768,655360,69979,-4443},
		{1,59,0,1048576,4128768,720896,71185,-5649},
		{1,59,0,1114112,4128768,786432,71185,-5649},
		{1,59,0,1179648,4128768,851968,71185,-5649},
		{1,59,0,1245184,4128768,917504,71185,-5649},
		{1,60,0,1310720,4128768,1048576,69979,-4443},
		{1,60,0,1376256,4128768,1114112,69979,-4443},
		{1,60,0,1441792,4128768,1179648,69979,-4443},
		{1,60,0,1507328,4128768,1245184,69979,-4443},
		{1,60,0,1572864,4128768,1310720,69979,-4443},
		{1,60,0,1638400,4128768,1376256,69979,-4443},
		{1,60,0,1703936,4128768,1441792,69979,-4443},
		{1,60,0,1769472,4128768,1507328,69979,-4443},
		{1,60,0,1835008,4128768,1572864,69979,-4443},
		{1,60,0,1900544,4128768,1638400,69979,-4443},
		{1,60,0,1966080,4128768,1703936,69979,-4443},
		{2,59,0,2097152,4128768,1769472,71185,-5649},
		{2,59,0,2162688,4128768,1835008,71185,-5649},
		{2,59,0,2228224,4128768,1900544,71185,-5649},
		{2,59,0,2293760,4128768,1966080,71185,-5649},
		{2,60,0,2359296,4128768,2097152,69979,-4443},
		{2,60,0,2424832,4128768,2162688,69979,-4443},
		{2,60,0,2490368,4128768,2228224,69979,-4443},
		{2,60,0,2555904,4128768,2293760,69979,-4443},
		{2,60,0,2621440,4128768,2359296,69979,-4443},
		{2,60,0,2686976,4128768,2424832,69979,-4443},
		{2,60,0,2752512,4128768,2490368,69979,-4443},
		{2,60,0,2818048,4128768,2555904,69979,-4443},
		{2,60,0,2883584,4128768,2621440,69979,-4443},
		{2,60,0,2949120,4128768,2686976,69979,-4443},
		{2,60,0,3014656,4128768,2752512,69979,-4443},
		{3,59,0,3145728,4128768,2818048,71185,-5649},
		{3,59,0,3211264,4128768,2883584,71185,-5649},
		{3,59,0,3276800,4128768,2949120,71185,-5649},
		{3,59,0,3342336,4128768,3014656,71185,-5649},
		{3,60,0,3407872,4128768,3145728,69979,-4443},
		{3,60,0,3473408,4128768,3211264,69979,-4443},
		{3,60,0,3538944,4128768,3276800,69979,-4443},
		{3,60,0,3604480,4128768,3342336,69979,-4443},
		{3,60,0,3670016,4128768,3407872,69979,-4443},
		{3,60,0,3735552,4128768,3473408,69979,-4443},
		{3,60,0,3801088,4128768,3538944,69979,-4443},
		{3,60,0,3866624,4128768,3604480,69979,-4443},
		{3,60,0,3932160,4128768,3670016,69979,-4443},
		{3,60,0,3997696,4128768,3735552,69979,-4443},
		{4,59,0,4128768,4128768,3801088,71185,-5649},
		{18,45,983040,4128768,4128768,3866624,71493,-5957},
		{33,30,2031616,4128768,4128768,3932160,72315,-6779},
		{48,15,3080192,4128768,4128768,3997696,74898,-9362},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 13 */
		{0,1,0,0,0,0,0,0},
		{0,12,0,65536,786432,0,71493,-5957},
		{0,24,0,131072,1638400,0,71234,-5698},
		{0,35,0,196608,2424832,0,71318,-5782},
		{0,47,0,262144,3276800,0,71234,-5698},
		{0,59,0,327680,4128768,0,71185,-5649},
		{0,59,0,393216,4128768,65536,71185,-5649},
		{0,59,0,458752,4128768,131072,71185,-5649},
		{0,59,0,524288,4128768,196608,71185,-5649},
		{0,59,0,589824,4128768,262144,71185,-5649},
		{0,59,0,655360,4128768,327680,71185,-5649},
		{0,59,0,720896,4128768,393216,71185,-5649},
		{1,58,0,851968,4128768,458752,72434,-6898},
		{1,58,0,917504,4128768,524288,72434,-6898},
		{1,58,0,983040,4128768,589824,72434,-6898},
		{1,58,0,1048576,4128768,655360,72434,-6898},
		{1,58,0,1114112,4128768,720896,72434,-6898},
		{1,59,0,1179648,4128768,851968,71185,-5649},
		{1,59,0,1245184,4128768,917504,71185,-5649},
		{1,59,0,1310720,4128768,983040,71185,-5649},
		{1,59,0,1376256,4128768,1048576,71185,-5649},
		{1,59,0,1441792,4128768,1114112,71185,-5649},
		{1,59,0,1507328,4128768,1179648,71185,-5649},
		{1,59,0,1572864,4128768,1245184,71185,-5649},
		{2,58,0,1703936,4128768,1310720,72434,-6898},
		{2,58,0,1769472,4128768,1376256,72434,-6898},
		{2,58,0,1835008,4128768,1441792,72434,-6898},
		{2,58,0,1900544,4128768,1507328,72434,-6898},
		{2,58,0,1966080,4128768,1572864,72434,-6898},
		{2,59,0,2031616,4128768,1703936,71185,-5649},
		{2,59,0,2097152,4128768,1769472,71185,-5649},
		{2,59,0,2162688,4128768,1835008,71185,-5649},
		{2,59,0,2228224,4128768,1900544,71185,-5649},
		{2,59,0,2293760,4128768,1966080,71185,-5649},
		{2,59,0,2359296,4128768,2031616,71185,-5649},
		{3,58,0,2490368,4128768,2097152,72434,-6898},
		{3,58,0,2555904,4128768,2162688,72434,-6898},
		{3,58,0,2621440,4128768,2228224,72434,-6898},
		{3,58,0,2686976,4128768,2293760,72434,-6898},
		{3,58,0,2752512,4128768,2359296,72434,-6898},
		{3,59,0,2818048,4128768,2490368,71185,-5649},
		{3,59,0,2883584,4128768,2555904,71185,-5649},
		{3,59,0,2949120,4128768,2621440,71185,-5649},
		{3,59,0,3014656,4128768,2686976,71185,-5649},
		{3,59,0,3080192,4128768,2752512,71185,-5649},
		{3,59,0,3145728,4128768,2818048,71185,-5649},
		{3,59,0,3211264,4128768,2883584,71185,-5649},
		{4,58,0,3342336,4128768,2949120,72434,-6898},
		{4,58,0,3407872,4128768,3014656,72434,-6898},
		{4,58,0,3473408,4128768,3080192,72434,-6898},
		{4,58,0,3538944,4128768,3145728,72434,-6898},
		{4,58,0,3604480,4128768,3211264,72434,-6898},
		{4,59,0,3670016,4128768,3342336,71185,-5649},
		{4,59,0,3735552,4128768,3407872,71185,-5649},
		{4,59,0,3801088,4128768,3473408,71185,-5649},
		{4,59,0,3866624,4128768,3538944,71185,-5649},
		{4,59,0,3932160,4128768,3604480,71185,-5649},
		{4,59,0,3997696,4128768,3670016,71185,-5649},
		{5,58,0,4128768,4128768,3735552,72434,-6898},
		{16,47,786432,4128768,4128768,3801088,72659,-7123},
		{28,35,1638400,4128768,4128768,3866624,73246,-7710},
		{39,24,2424832,4128768,4128768,3932160,74084,-8548},
		{51,12,3276800,4128768,4128768,3997696,77451,-11915},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 14 */
		{0,1,0,0,0,0,0,0},
		{0,9,0,65536,589824,0,73728,-8192},
		{0,17,0,131072,1179648,0,73728,-8192},
		{0,25,0,196608,1769472,0,73728,-8192},
		{0,34,0,262144,2359296,0,71493,-7943},
		{0,42,0,327680,2949120,0,71929,-7992},
		{0,50,0,393216,3538944,0,72223,-8024},
		{0,59,0,458752,4128768,0,71185,-7909},
		{0,59,0,589824,4128768,65536,71185,-9039},
		{0,59,0,655360,4128768,131072,71185,-9039},
		{0,59,0,720896,4128768,196608,71185,-9039},
		{0,59,0,786432,4128768,262144,71185,-9039},
		{1,58,0,851968,4128768,327680,72434,-9198},
		{1,58,0,917504,4128768,393216,72434,-9198},
		{1,58,0,983040,4128768,458752,72434,-9198},
		{1,58,0,1048576,4128768,589824,72434,-8048},
		{1,58,0,1179648,4128768,655360,72434,-9198},
		{1,58,0,1245184,4128768,720896,72434,-9198},
		{1,58,0,1310720,4128768,786432,72434,-9198},
		{1,59,0,1376256,4128768,851968,71185,-9039},
		{1,59,0,1441792,4128768,917504,71185,-9039},
		{1,59,0,1507328,4128768,983040,71185,-9039},
		{1,59,0,1572864,4128768,1048576,71185,-9039},
		{2,58,0,1638400,4128768,1179648,72434,-8048},
		{2,58,0,1769472,4128768,1245184,72434,-9198},
		{2,58,0,1835008,4128768,1310720,72434,-9198},
		{2,58,0,1900544,4128768,1376256,72434,-9198},
		{2,58,0,1966080,4128768,1441792,72434,-9198},
		{2,58,0,2031616,4128768,1507328,72434,-9198},
		{2,58,0,2097152,4128768,1572864,72434,-9198},
		{2,59,0,2162688,4128768,1638400,71185,-9039},
		{2,59,0,2228224,4128768,1769472,71185,-7909},
		{2,59,0,2359296,4128768,1835008,71185,-9039},
		{2,59,0,2424832,4128768,1900544,71185,-9039},
		{3,58,0,2490368,4128768,1966080,72434,-9198},
		{3,58,0,2555904,4128768,2031616,72434,-9198},
		{3,58,0,2621440,4128768,2097152,72434,-9198},
		{3,58,0,2686976,4128768,2162688,72434,-9198},
		{3,58,0,2752512,4128768,2228224,72434,-9198},
		{3,58,0,2818048,4128768,2359296,72434,-8048},
		{3,58,0,2949120,4128768,2424832,72434,-9198},
		{3,59,0,3014656,4128768,2490368,71185,-9039},
		{3,59,0,3080192,4128768,2555904,71185,-9039},
		{3,59,0,3145728,4128768,2621440,71185,-9039},
		{3,59,0,3211264,4128768,2686976,71185,-9039},
		{4,58,0,3276800,4128768,2752512,72434,-9198},
		{4,58,0,3342336,4128768,2818048,72434,-9198},
		{4,58,0,3407872,4128768,2949120,72434,-8048},
		{4,58,0,3538944,4128768,3014656,72434,-9198},
		{4,58,0,3604480,4128768,3080192,72434,-9198},
		{4,58,0,3670016,4128768,3145728,72434,-9198},
		{4,58,0,3735552,4128768,3211264,72434,-9198},
		{4,59,0,3801088,4128768,3276800,71185,-9039},
		{4,59,0,3866624,4128768,3342336,71185,-9039},
		{4,59,0,3932160,4128768,3407872,71185,-9039},
		{4,59,0,3997696,4128768,3538944,71185,-7909},
		{5,58,0,4128768,4128768,3604480,72434,-9198},
		{13,50,589824,4128768,4128768,3670016,72223,-9362},
		{21,42,1179648,4128768,4128768,3735552,71929,-9590},
		{29,34,1769472,4128768,4128768,3801088,71493,-9929},
		{38,25,2359296,4128768,4128768,3866624,73728,-10922},
		{46,17,2949120,4128768,4128768,3932160,73728,-12288},
		{54,9,3538944,4128768,4128768,3997696,73728,-16384},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 15 */
		{0,1,0,0,0,0,0,0},
		{0,8,0,65536,458752,0,65536,-9362},
		{0,15,0,131072,983040,0,70217,-9362},
		{0,22,0,196608,1507328,0,71777,-9362},
		{0,29,0,262144,2031616,0,72557,-9362},
		{0,36,0,327680,2555904,0,73025,-9362},
		{0,43,0,393216,3080192,0,73337,-9362},
		{0,50,0,524288,3604480,0,73560,-10699},
		{0,58,0,589824,4128768,0,72434,-10347},
		{0,58,0,655360,4128768,65536,72434,-10347},
		{1,57,0,720896,4128768,131072,73728,-10532},
		{1,57,0,786432,4128768,196608,73728,-10532},
		{1,57,0,851968,4128768,262144,73728,-10532},
		{1,57,0,917504,4128768,327680,73728,-10532},
		{1,57,0,1048576,4128768,393216,73728,-11702},
		{1,57,0,1114112,4128768,524288,73728,-10532},
		{1,57,0,1179648,4128768,589824,73728,-10532},
		{1,57,0,1245184,4128768,655360,73728,-10532},
		{1,58,0,1310720,4128768,720896,72434,-10347},
		{2,57,0,1376256,4128768,786432,73728,-10532},
		{2,57,0,1441792,4128768,851968,73728,-10532},
		{2,57,0,1572864,4128768,917504,73728,-11702},
		{2,57,0,1638400,4128768,1048576,73728,-10532},
		{2,57,0,1703936,4128768,1114112,73728,-10532},
		{2,57,0,1769472,4128768,1179648,73728,-10532},
		{2,57,0,1835008,4128768,1245184,73728,-10532},
		{2,57,0,1900544,4128768,1310720,73728,-10532},
		{2,58,0,1966080,4128768,1376256,72434,-10347},
		{3,57,0,2097152,4128768,1441792,73728,-11702},
		{3,57,0,2162688,4128768,1572864,73728,-10532},
		{3,57,0,2228224,4128768,1638400,73728,-10532},
		{3,57,0,2293760,4128768,1703936,73728,-10532},
		{3,57,0,2359296,4128768,1769472,73728,-10532},
		{3,57,0,2424832,4128768,1835008,73728,-10532},
		{3,57,0,2490368,4128768,1900544,73728,-10532},
		{3,57,0,2621440,4128768,1966080,73728,-11702},
		{3,58,0,2686976,4128768,2097152,72434,-10347},
		{4,57,0,2752512,4128768,2162688,73728,-10532},
		{4,57,0,2818048,4128768,2228224,73728,-10532},
		{4,57,0,2883584,4128768,2293760,73728,-10532},
		{4,57,0,2949120,4128768,2359296,73728,-10532},
		{4,57,0,3014656,4128768,2424832,73728,-10532},
		{4,57,0,3145728,4128768,2490368,73728,-11702},
		{4,57,0,3211264,4128768,2621440,73728,-10532},
		{4,57,0,3276800,4128768,2686976,73728,-10532},
		{4,58,0,3342336,4128768,2752512,72434,-10347},
		{5,57,0,3407872,4128768,2818048,73728,-10532},
		{5,57,0,3473408,4128768,2883584,73728,-10532},
		{5,57,0,3538944,4128768,2949120,73728,-10532},
		{5,57,0,3670016,4128768,3014656,73728,-11702},
		{5,57,0,3735552,4128768,3145728,73728,-10532},
		{5,57,0,3801088,4128768,3211264,73728,-10532},
		{5,57,0,3866624,4128768,3276800,73728,-10532},
		{5,57,0,3932160,4128768,3342336,73728,-10532},
		{5,58,0,3997696,4128768,3407872,72434,-10347},
		{6,57,0,4128768,4128768,3473408,73728,-11702},
		{13,50,458752,4128768,4128768,3538944,74898,-12037},
		{20,43,983040,4128768,4128768,3670016,74898,-10922},
		{27,36,1507328,4128768,4128768,3735552,74898,-11234},
		{34,29,2031616,4128768,4128768,3801088,74898,-11702},
		{41,22,2555904,4128768,4128768,3866624,74898,-12483},
		{48,15,3080192,4128768,4128768,3932160,74898,-14043},
		{55,8,3604480,4128768,4128768,3997696,74898,-18724},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 16 */
		{0,1,0,0,0,0,0,0},
		{0,7,0,65536,458752,0,76458,-10922},
		{0,13,0,131072,917504,0,76458,-10922},
		{0,19,0,196608,1376256,0,76458,-10922},
		{0,25,0,262144,1835008,0,76458,-10922},
		{0,32,0,327680,2293760,0,73992,-10570},
		{0,38,0,458752,2752512,0,74392,-12398},
		{0,44,0,524288,3211264,0,74680,-12192},
		{1,49,0,589824,3670016,0,76458,-12288},
		{1,56,0,655360,4128768,0,75068,-11915},
		{1,56,0,720896,4128768,65536,75068,-11915},
		{1,56,0,786432,4128768,131072,75068,-11915},
		{1,56,0,917504,4128768,196608,75068,-13107},
		{1,56,0,983040,4128768,262144,75068,-13107},
		{1,56,0,1048576,4128768,327680,75068,-13107},
		{1,56,0,1114112,4128768,458752,75068,-11915},
		{2,55,0,1179648,4128768,524288,76458,-12136},
		{2,56,0,1245184,4128768,589824,75068,-11915},
		{2,56,0,1376256,4128768,655360,75068,-13107},
		{2,56,0,1441792,4128768,720896,75068,-13107},
		{2,56,0,1507328,4128768,786432,75068,-13107},
		{2,56,0,1572864,4128768,917504,75068,-11915},
		{2,56,0,1638400,4128768,983040,75068,-11915},
		{2,56,0,1703936,4128768,1048576,75068,-11915},
		{3,55,0,1835008,4128768,1114112,76458,-13349},
		{3,56,0,1900544,4128768,1179648,75068,-13107},
		{3,56,0,1966080,4128768,1245184,75068,-13107},
		{3,56,0,2031616,4128768,1376256,75068,-11915},
		{3,56,0,2097152,4128768,1441792,75068,-11915},
		{3,56,0,2162688,4128768,1507328,75068,-11915},
		{3,56,0,2293760,4128768,1572864,75068,-13107},
		{4,55,0,2359296,4128768,1638400,76458,-13349},
		{4,55,0,2424832,4128768,1703936,76458,-13349},
		{4,56,0,2490368,4128768,1835008,75068,-11915},
		{4,56,0,2555904,4128768,1900544,75068,-11915},
		{4,56,0,2621440,4128768,1966080,75068,-11915},
		{4,56,0,2752512,4128768,2031616,75068,-13107},
		{4,56,0,2818048,4128768,2097152,75068,-13107},
		{4,56,0,2883584,4128768,2162688,75068,-13107},
		{5,55,0,2949120,4128768,2293760,76458,-12136},
		{5,56,0,3014656,4128768,2359296,75068,-11915},
		{5,56,0,3080192,4128768,2424832,75068,-11915},
		{5,56,0,3211264,4128768,2490368,75068,-13107},
		{5,56,0,3276800,4128768,2555904,75068,-13107},
		{5,56,0,3342336,4128768,2621440,75068,-13107},
		{5,56,0,3407872,4128768,2752512,75068,-11915},
		{5,56,0,3473408,4128768,2818048,75068,-11915},
		{6,55,0,3538944,4128768,2883584,76458,-12136},
		{6,56,0,3670016,4128768,2949120,75068,-13107},
		{6,56,0,3735552,4128768,3014656,75068,-13107},
		{6,56,0,3801088,4128768,3080192,75068,-13107},
		{6,56,0,3866624,4128768,3211264,75068,-11915},
		{6,56,0,3932160,4128768,3276800,75068,-11915},
		{6,56,0,3997696,4128768,3342336,75068,-11915},
		{7,55,0,4128768,4128768,3407872,76458,-13349},
		{13,49,458752,4128768,4128768,3473408,76458,-13653},
		{19,44,917504,4128768,4128768,3538944,74680,-13716},
		{25,38,1376256,4128768,4128768,3670016,74392,-12398},
		{31,32,1835008,4128768,4128768,3735552,73992,-12684},
		{38,25,2293760,4128768,4128768,3801088,76458,-13653},
		{44,19,2752512,4128768,4128768,3866624,76458,-14563},
		{50,13,3211264,4128768,4128768,3932160,76458,-16384},
		{56,7,3670016,4128768,4128768,3997696,76458,-21845},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 17 */
		{0,1,0,0,0,0,0,0},
		{0,6,0,65536,393216,0,78643,-13107},
		{0,12,0,131072,786432,0,71493,-11915},
		{0,17,0,196608,1179648,0,73728,-12288},
		{0,23,0,262144,1638400,0,74472,-11915},
		{0,28,0,327680,2031616,0,75245,-12136},
		{0,34,0,458752,2424832,0,73479,-13901},
		{1,38,0,524288,2883584,0,77934,-14169},
		{1,44,0,589824,3276800,0,76204,-13716},
		{1,49,0,655360,3670016,0,76458,-13653},
		{1,55,0,720896,4128768,0,76458,-13349},
		{1,55,0,851968,4128768,65536,76458,-14563},
		{1,55,0,917504,4128768,131072,76458,-14563},
		{1,55,0,983040,4128768,196608,76458,-14563},
		{2,54,0,1048576,4128768,262144,77901,-14838},
		{2,54,0,1114112,4128768,327680,77901,-14838},
		{2,54,0,1245184,4128768,458752,77901,-14838},
		{2,55,0,1310720,4128768,524288,76458,-14563},
		{2,55,0,1376256,4128768,589824,76458,-14563},
		{2,55,0,1441792,4128768,655360,76458,-14563},
		{3,54,0,1507328,4128768,720896,77901,-14838},
		{3,54,0,1572864,4128768,851968,77901,-13601},
		{3,54,0,1703936,4128768,917504,77901,-14838},
		{3,54,0,1769472,4128768,983040,77901,-14838},
		{3,55,0,1835008,4128768,1048576,76458,-14563},
		{3,55,0,1900544,4128768,1114112,76458,-14563},
		{3,55,0,1966080,4128768,1245184,76458,-13349},
		{4,54,0,2097152,4128768,1310720,77901,-14838},
		{4,54,0,2162688,4128768,1376256,77901,-14838},
		{4,54,0,2228224,4128768,1441792,77901,-14838},
		{4,55,0,2293760,4128768,1507328,76458,-14563},
		{4,55,0,2359296,4128768,1572864,76458,-14563},
		{4,55,0,2490368,4128768,1703936,76458,-14563},
		{4,55,0,2555904,4128768,1769472,76458,-14563},
		{5,54,0,2621440,4128768,1835008,77901,-14838},
		{5,54,0,2686976,4128768,1900544,77901,-14838},
		{5,54,0,2752512,4128768,1966080,77901,-14838},
		{5,55,0,2818048,4128768,2097152,76458,-13349},
		{5,55,0,2949120,4128768,2162688,76458,-14563},
		{5,55,0,3014656,4128768,2228224,76458,-14563},
		{6,54,0,3080192,4128768,2293760,77901,-14838},
		{6,54,0,3145728,4128768,2359296,77901,-14838},
		{6,54,0,3211264,4128768,2490368,77901,-13601},
		{6,54,0,3342336,4128768,2555904,77901,-14838},
		{6,55,0,3407872,4128768,2621440,76458,-14563},
		{6,55,0,3473408,4128768,2686976,76458,-14563},
		{6,55,0,3538944,4128768,2752512,76458,-14563},
		{7,54,0,3604480,4128768,2818048,77901,-14838},
		{7,54,0,3735552,4128768,2949120,77901,-14838},
		{7,54,0,3801088,4128768,3014656,77901,-14838},
		{7,55,0,3866624,4128768,3080192,76458,-14563},
		{7,55,0,3932160,4128768,3145728,76458,-14563},
		{7,55,0,3997696,4128768,3211264,76458,-14563},
		{8,54,0,4128768,4128768,3342336,77901,-14838},
		{13,49,393216,4128768,4128768,3407872,77824,-15018},
		{19,43,786432,4128768,4128768,3473408,79579,-15603},
		{24,38,1179648,4128768,4128768,3538944,79705,-15941},
		{30,33,1638400,4128768,4128768,3604480,77824,-16384},
		{35,28,2031616,4128768,4128768,3735552,77672,-14563},
		{41,22,2424832,4128768,4128768,3801088,81139,-15603},
		{46,17,2883584,4128768,4128768,3866624,77824,-16384},
		{52,11,3276800,4128768,4128768,3932160,85196,-19660},
		{57,6,3670016,4128768,4128768,3997696,91750,-26214},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 18 */
		{0,1,0,0,0,0,0,0},
		{0,5,0,65536,327680,0,81920,-16384},
		{0,10,0,131072,720896,0,80099,-14563},
		{0,15,0,196608,1114112,0,79579,-14043},
		{0,20,0,262144,1441792,0,75883,-13797},
		{0,25,0,393216,1835008,0,76458,-16384},
		{1,29,0,458752,2228224,0,79579,-16384},
		{1,34,0,524288,2621440,0,79437,-15887},
		{1,39,0,589824,2949120,0,77608,-15521},
		{1,44,0,655360,3342336,0,77728,-15240},
		{1,49,0,786432,3735552,0,77824,-16384},
		{1,54,0,851968,4128768,0,77901,-16074},
		{2,53,0,917504,4128768,65536,79399,-16384},
		{2,53,0,983040,4128768,131072,79399,-16384},
		{2,53,0,1048576,4128768,196608,79399,-16384},
		{2,53,0,1179648,4128768,262144,79399,-17644},
		{2,53,0,1245184,4128768,393216,79399,-16384},
		{2,54,0,1310720,4128768,458752,77901,-16074},
		{3,53,0,1376256,4128768,524288,79399,-16384},
		{3,53,0,1507328,4128768,589824,79399,-17644},
		{3,53,0,1572864,4128768,655360,79399,-17644},
		{3,53,0,1638400,4128768,786432,79399,-16384},
		{3,53,0,1703936,4128768,851968,79399,-16384},
		{3,54,0,1769472,4128768,917504,77901,-16074},
		{4,53,0,1900544,4128768,983040,79399,-17644},
		{4,53,0,1966080,4128768,1048576,79399,-17644},
		{4,53,0,2031616,4128768,1179648,79399,-16384},
		{4,53,0,2097152,4128768,1245184,79399,-16384},
		{4,53,0,2162688,4128768,1310720,79399,-16384},
		{5,53,0,2293760,4128768,1376256,79399,-17644},
		{5,53,0,2359296,4128768,1507328,79399,-16384},
		{5,53,0,2424832,4128768,1572864,79399,-16384},
		{5,53,0,2490368,4128768,1638400,79399,-16384},
		{5,53,0,2555904,4128768,1703936,79399,-16384},
		{5,53,0,2686976,4128768,1769472,79399,-17644},
		{6,53,0,2752512,4128768,1900544,79399,-16384},
		{6,53,0,2818048,4128768,1966080,79399,-16384},
		{6,53,0,2883584,4128768,2031616,79399,-16384},
		{6,53,0,3014656,4128768,2097152,79399,-17644},
		{6,53,0,3080192,4128768,2162688,79399,-17644},
		{6,54,0,3145728,4128768,2293760,77901,-16074},
		{7,53,0,3211264,4128768,2359296,79399,-16384},
		{7,53,0,3276800,4128768,2424832,79399,-16384},
		{7,53,0,3407872,4128768,2490368,79399,-17644},
		{7,53,0,3473408,4128768,2555904,79399,-17644},
		{7,53,0,3538944,4128768,2686976,79399,-16384},
		{7,54,0,3604480,4128768,2752512,77901,-16074},
		{8,53,0,3670016,4128768,2818048,79399,-16384},
		{8,53,0,3801088,4128768,2883584,79399,-17644},
		{8,53,0,3866624,4128768,3014656,79399,-16384},
		{8,53,0,3932160,4128768,3080192,79399,-16384},
		{8,53,0,3997696,4128768,3145728,79399,-16384},
		{9,53,0,4128768,4128768,3211264,79399,-17644},
		{13,49,327680,4128768,4128768,3276800,79189,-17749},
		{18,44,720896,4128768,4128768,3407872,79252,-16765},
		{23,39,1114112,4128768,4128768,3473408,79333,-17246},
		{28,34,1441792,4128768,4128768,3538944,81423,-17873},
		{33,29,1835008,4128768,4128768,3604480,81920,-18724},
		{38,25,2228224,4128768,4128768,3670016,79189,-19114},
		{43,20,2621440,4128768,4128768,3801088,79333,-17246},
		{48,15,2949120,4128768,4128768,3866624,84260,-18724},
		{53,10,3342336,4128768,4128768,3932160,87381,-21845},
		{58,5,3735552,4128768,4128768,3997696,98304,-32768},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 19 */
		{0,1,0,0,0,0,0,0},
		{0,5,0,65536,262144,0,65536,-16384},
		{0,9,0,131072,589824,0,73728,-16384},
		{0,13,0,196608,917504,0,76458,-16384},
		{0,17,0,327680,1245184,0,77824,-20480},
		{0,21,0,393216,1572864,0,78643,-19660},
		{1,24,0,458752,1900544,0,82632,-19945},
		{1,29,0,524288,2162688,0,77238,-18724},
		{1,33,0,655360,2490368,0,77824,-20480},
		{1,37,0,720896,2818048,0,78279,-20024},
		{1,41,0,786432,3145728,0,78643,-19660},
		{1,45,0,851968,3473408,0,78941,-19362},
		{2,48,0,983040,3801088,0,80874,-20915},
		{2,53,0,1048576,4128768,0,79399,-20164},
		{2,53,0,1114112,4128768,65536,79399,-20164},
		{2,53,0,1179648,4128768,131072,79399,-20164},
		{2,53,0,1310720,4128768,196608,79399,-21425},
		{3,52,0,1376256,4128768,327680,80956,-20560},
		{3,52,0,1441792,4128768,393216,80956,-20560},
		{3,53,0,1507328,4128768,458752,79399,-20164},
		{3,53,0,1638400,4128768,524288,79399,-21425},
		{3,53,0,1703936,4128768,655360,79399,-20164},
		{3,53,0,1769472,4128768,720896,79399,-20164},
		{4,52,0,1835008,4128768,786432,80956,-20560},
		{4,52,0,1966080,4128768,851968,80956,-21845},
		{4,53,0,2031616,4128768,983040,79399,-20164},
		{4,53,0,2097152,4128768,1048576,79399,-20164},
		{4,53,0,2228224,4128768,1114112,79399,-21425},
		{5,52,0,2293760,4128768,1179648,80956,-21845},
		{5,52,0,2359296,4128768,1310720,80956,-20560},
		{5,53,0,2424832,4128768,1376256,79399,-20164},
		{5,53,0,2555904,4128768,1441792,79399,-21425},
		{5,53,0,2621440,4128768,1507328,79399,-21425},
		{5,53,0,2686976,4128768,1638400,79399,-20164},
		{6,52,0,2752512,4128768,1703936,80956,-20560},
		{6,52,0,2883584,4128768,1769472,80956,-21845},
		{6,53,0,2949120,4128768,1835008,79399,-21425},
		{6,53,0,3014656,4128768,1966080,79399,-20164},
		{6,53,0,3080192,4128768,2031616,79399,-20164},
		{7,52,0,3211264,4128768,2097152,80956,-21845},
		{7,52,0,3276800,4128768,2228224,80956,-20560},
		{7,53,0,3342336,4128768,2293760,79399,-20164},
		{7,53,0,3407872,4128768,2359296,79399,-20164},
		{7,53,0,3538944,4128768,2424832,79399,-21425},
		{7,53,0,3604480,4128768,2555904,79399,-20164},
		{8,52,0,3670016,4128768,2621440,80956,-20560},
		{8,52,0,3735552,4128768,2686976,80956,-20560},
		{8,53,0,3866624,4128768,2752512,79399,-21425},
		{8,53,0,3932160,4128768,2883584,79399,-20164},
		{8,53,0,3997696,4128768,2949120,79399,-20164},
		{9,52,0,4128768,4128768,3014656,80956,-21845},
		{13,48,262144,4128768,4128768,3080192,82268,-22310},
		{17,45,589824,4128768,4128768,3211264,80430,-20852},
		{21,41,917504,4128768,4128768,3276800,80281,-21299},
		{25,37,1245184,4128768,4128768,3342336,80099,-21845},
		{29,33,1572864,4128768,4128768,3407872,79872,-22528},
		{33,29,1900544,4128768,4128768,3538944,79579,-21065},
		{38,24,2162688,4128768,4128768,3604480,85481,-22795},
		{42,21,2490368,4128768,4128768,3670016,81920,-22937},
		{46,17,2818048,4128768,4128768,3735552,81920,-24576},
		{50,13,3145728,4128768,4128768,3866624,81920,-21845},
		{54,9,3473408,4128768,4128768,3932160,81920,-24576},
		{58,5,3801088,4128768,4128768,3997696,81920,-32768},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 20 */
		{0,1,0,0,0,0,0,0},
		{0,4,0,65536,262144,0,87381,-21845},
		{0,8,0,131072,589824,0,84260,-18724},
		{0,12,0,196608,851968,0,77451,-17873},
		{0,16,0,327680,1179648,0,78643,-21845},
		{1,18,0,393216,1441792,0,84811,-23130},
		{1,22,0,458752,1769472,0,84260,-21845},
		{1,26,0,589824,2031616,0,81264,-23592},
		{1,30,0,655360,2359296,0,81355,-22598},
		{1,34,0,720896,2621440,0,79437,-21845},
		{2,36,0,786432,2949120,0,84260,-22469},
		{2,40,0,917504,3211264,0,82340,-23525},
		{2,44,0,983040,3538944,0,82301,-22861},
		{2,48,0,1048576,3801088,0,80874,-22310},
		{2,52,0,1179648,4128768,0,80956,-23130},
		{3,51,0,1245184,4128768,65536,82575,-23592},
		{3,51,0,1310720,4128768,131072,82575,-23592},
		{3,51,0,1376256,4128768,196608,82575,-23592},
		{3,51,0,1507328,4128768,327680,82575,-23592},
		{3,52,0,1572864,4128768,393216,80956,-23130},
		{4,51,0,1638400,4128768,458752,82575,-23592},
		{4,51,0,1769472,4128768,589824,82575,-23592},
		{4,51,0,1835008,4128768,655360,82575,-23592},
		{4,51,0,1900544,4128768,720896,82575,-23592},
		{4,52,0,1966080,4128768,786432,80956,-23130},
		{5,51,0,2097152,4128768,917504,82575,-23592},
		{5,51,0,2162688,4128768,983040,82575,-23592},
		{5,51,0,2228224,4128768,1048576,82575,-23592},
		{5,51,0,2359296,4128768,1179648,82575,-23592},
		{5,52,0,2424832,4128768,1245184,80956,-23130},
		{6,51,0,2490368,4128768,1310720,82575,-23592},
		{6,51,0,2555904,4128768,1376256,82575,-23592},
		{6,51,0,2686976,4128768,1507328,82575,-23592},
		{6,51,0,2752512,4128768,1572864,82575,-23592},
		{6,52,0,2818048,4128768,1638400,80956,-23130},
		{7,51,0,2949120,4128768,1769472,82575,-23592},
		{7,51,0,3014656,4128768,1835008,82575,-23592},
		{7,51,0,3080192,4128768,1900544,82575,-23592},
		{7,51,0,3145728,4128768,1966080,82575,-23592},
		{7,52,0,3276800,4128768,2097152,80956,-23130},
		{8,51,0,3342336,4128768,2162688,82575,-23592},
		{8,51,0,3407872,4128768,2228224,82575,-23592},
		{8,51,0,3538944,4128768,2359296,82575,-23592},
		{8,51,0,3604480,4128768,2424832,82575,-23592},
		{8,52,0,3670016,4128768,2490368,80956,-23130},
		{9,51,0,3735552,4128768,2555904,82575,-23592},
		{9,51,0,3866624,4128768,2686976,82575,-23592},
		{9,51,0,3932160,4128768,2752512,82575,-23592},
		{9,51,0,3997696,4128768,2818048,82575,-23592},
		{10,51,0,4128768,4128768,2949120,82575,-23592},
		{13,48,262144,4128768,4128768,3014656,82268,-23704},
		{17,44,589824,4128768,4128768,3080192,82301,-24385},
		{21,40,851968,4128768,4128768,3145728,84020,-25206},
		{25,36,1179648,4128768,4128768,3276800,84260,-24341},
		{28,34,1441792,4128768,4128768,3342336,81423,-23831},
		{32,30,1769472,4128768,4128768,3407872,81355,-24858},
		{36,26,2031616,4128768,4128768,3538944,83886,-23592},
		{40,22,2359296,4128768,4128768,3604480,84260,-24966},
		{44,18,2621440,4128768,4128768,3670016,88666,-26985},
		{47,16,2949120,4128768,4128768,3735552,78643,-26214},
		{51,12,3211264,4128768,4128768,3866624,83409,-23831},
		{55,8,3538944,4128768,4128768,3932160,84260,-28086},
		{59,4,3801088,4128768,4128768,3997696,109226,-43690},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 21 */
		{0,1,0,0,0,0,0,0},
		{0,4,0,65536,196608,0,65536,-21845},
		{0,7,0,131072,458752,0,76458,-21845},
		{0,10,0,262144,720896,0,80099,-29127},
		{1,12,0,327680,983040,0,89367,-29789},
		{1,15,0,393216,1245184,0,88941,-28086},
		{1,19,0,524288,1507328,0,83740,-29127},
		{1,22,0,589824,1769472,0,84260,-28086},
		{2,24,0,655360,2031616,0,88331,-28493},
		{2,27,0,786432,2293760,0,88221,-30247},
		{2,30,0,851968,2555904,0,88134,-29378},
		{2,34,0,917504,2818048,0,85395,-27803},
		{3,36,0,1048576,3080192,0,88005,-29959},
		{3,39,0,1114112,3342336,0,87956,-29318},
		{3,42,0,1179648,3604480,0,87914,-28771},
		{3,45,0,1310720,3866624,0,87877,-29789},
		{4,48,0,1376256,4128768,0,87846,-29282},
		{4,48,0,1441792,4128768,65536,87846,-29282},
		{4,48,0,1572864,4128768,131072,87846,-30676},
		{4,48,0,1638400,4128768,262144,87846,-29282},
		{5,48,0,1703936,4128768,327680,87846,-29282},
		{5,48,0,1835008,4128768,393216,87846,-30676},
		{5,48,0,1900544,4128768,524288,87846,-29282},
		{5,48,0,1966080,4128768,589824,87846,-29282},
		{6,48,0,2097152,4128768,655360,87846,-30676},
		{6,48,0,2162688,4128768,786432,87846,-29282},
		{6,48,0,2228224,4128768,851968,87846,-29282},
		{6,48,0,2359296,4128768,917504,87846,-30676},
		{7,48,0,2424832,4128768,1048576,87846,-29282},
		{7,48,0,2490368,4128768,1114112,87846,-29282},
		{7,48,0,2621440,4128768,1179648,87846,-30676},
		{7,48,0,2686976,4128768,1310720,87846,-29282},
		{8,48,0,2752512,4128768,1376256,87846,-29282},
		{8,48,0,2883584,4128768,1441792,87846,-30676},
		{8,48,0,2949120,4128768,1572864,87846,-29282},
		{8,48,0,3014656,4128768,1638400,87846,-29282},
		{9,48,0,3145728,4128768,1703936,87846,-30676},
		{9,48,0,3211264,4128768,1835008,87846,-29282},
		{9,48,0,3276800,4128768,1900544,87846,-29282},
		{9,48,0,3407872,4128768,1966080,87846,-30676},
		{10,48,0,3473408,4128768,2097152,87846,-29282},
		{10,48,0,3538944,4128768,2162688,87846,-29282},
		{10,48,0,3670016,4128768,2228224,87846,-30676},
		{10,48,0,3735552,4128768,2359296,87846,-29282},
		{11,48,0,3801088,4128768,2424832,87846,-29282},
		{11,48,0,3932160,4128768,2490368,87846,-30676},
		{11,48,0,3997696,4128768,2621440,87846,-29282},
		{12,47,0,4128768,4128768,2686976,89755,-31343},
		{15,45,196608,4128768,4128768,2752512,89367,-31278},
		{18,42,458752,4128768,4128768,2883584,89512,-30370},
		{21,39,720896,4128768,4128768,2949120,89680,-31043},
		{24,36,983040,4128768,4128768,3014656,89877,-31831},
		{27,34,1245184,4128768,4128768,3145728,87381,-29789},
		{31,30,1507328,4128768,4128768,3211264,90394,-31638},
		{34,27,1769472,4128768,4128768,3276800,90742,-32768},
		{37,24,2031616,4128768,4128768,3407872,91180,-31343},
		{40,22,2293760,4128768,4128768,3473408,87381,-31207},
		{43,19,2555904,4128768,4128768,3538944,87381,-32768},
		{47,15,2818048,4128768,4128768,3670016,93622,-32768},
		{50,12,3080192,4128768,4128768,3735552,95325,-35746},
		{53,10,3342336,4128768,4128768,3801088,87381,-36408},
		{56,7,3604480,4128768,4128768,3932160,87381,-32768},
		{59,4,3866624,4128768,4128768,3997696,87381,-43690},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 22 */
		{0,1,0,0,0,0,0,0},
		{0,3,0,65536,196608,0,98304,-32768},
		{0,6,0,131072,458752,0,91750,-26214},
		{0,9,0,262144,720896,0,90112,-32768},
		{1,11,0,327680,917504,0,91750,-32768},
		{1,14,0,393216,1179648,0,90742,-30247},
		{1,17,0,524288,1441792,0,90112,-32768},
		{1,20,0,589824,1638400,0,86231,-31043},
		{2,22,0,655360,1900544,0,90502,-31207},
		{2,25,0,786432,2162688,0,90112,-32768},
		{2,28,0,851968,2424832,0,89808,-31554},
		{3,30,0,983040,2621440,0,90394,-33897},
		{3,33,0,1048576,2883584,0,90112,-32768},
		{3,36,0,1114112,3145728,0,89877,-31831},
		{3,39,0,1245184,3342336,0,87956,-32768},
		{4,41,0,1310720,3604480,0,90112,-32768},
		{4,44,0,1376256,3866624,0,89921,-32005},
		{4,47,0,1507328,4128768,0,89755,-32768},
		{5,46,0,1572864,4128768,65536,91750,-33496},
		{5,46,0,1703936,4128768,131072,91750,-34952},
		{5,46,0,1769472,4128768,262144,91750,-33496},
		{5,47,0,1835008,4128768,327680,89755,-32768},
		{6,46,0,1966080,4128768,393216,91750,-34952},
		{6,46,0,2031616,4128768,524288,91750,-33496},
		{6,46,0,2097152,4128768,589824,91750,-33496},
		{7,46,0,2228224,4128768,655360,91750,-34952},
		{7,46,0,2293760,4128768,786432,91750,-33496},
		{7,46,0,2359296,4128768,851968,91750,-33496},
		{7,47,0,2490368,4128768,983040,89755,-32768},
		{8,46,0,2555904,4128768,1048576,91750,-33496},
		{8,46,0,2686976,4128768,1114112,91750,-34952},
		{8,46,0,2752512,4128768,1245184,91750,-33496},
		{9,46,0,2818048,4128768,1310720,91750,-33496},
		{9,46,0,2949120,4128768,1376256,91750,-34952},
		{9,46,0,3014656,4128768,1507328,91750,-33496},
		{9,47,0,3080192,4128768,1572864,89755,-32768},
		{10,46,0,3211264,4128768,1703936,91750,-33496},
		{10,46,0,3276800,4128768,1769472,91750,-33496},
		{10,46,0,3407872,4128768,1835008,91750,-34952},
		{11,46,0,3473408,4128768,1966080,91750,-33496},
		{11,46,0,3538944,4128768,2031616,91750,-33496},
		{11,46,0,3670016,4128768,2097152,91750,-34952},
		{11,47,0,3735552,4128768,2228224,89755,-32768},
		{12,46,0,3801088,4128768,2293760,91750,-33496},
		{12,46,0,3932160,4128768,2359296,91750,-34952},
		{12,46,0,3997696,4128768,2490368,91750,-33496},
		{13,46,0,4128768,4128768,2555904,91750,-34952},
		{15,44,196608,4128768,4128768,2686976,91445,-33530},
		{18,41,458752,4128768,4128768,2752512,91750,-34406},
		{21,39,720896,4128768,4128768,2818048,89680,-34492},
		{24,36,917504,4128768,4128768,2949120,91750,-33704},
		{27,33,1179648,4128768,4128768,3014656,92160,-34816},
		{30,30,1441792,4128768,4128768,3080192,92654,-36157},
		{33,28,1638400,4128768,4128768,3211264,92235,-33981},
		{36,25,1900544,4128768,4128768,3276800,92842,-35498},
		{39,22,2162688,4128768,4128768,3407872,93622,-34328},
		{42,20,2424832,4128768,4128768,3473408,89680,-34492},
		{45,17,2621440,4128768,4128768,3538944,94208,-36864},
		{48,14,2883584,4128768,4128768,3670016,95783,-35288},
		{51,11,3145728,4128768,4128768,3735552,98304,-39321},
		{54,9,3342336,4128768,4128768,3801088,98304,-40960},
		{57,6,3604480,4128768,4128768,3932160,104857,-39321},
		{60,3,3866624,4128768,4128768,3997696,131072,-65536},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 23 */
		{0,1,0,0,0,0,0,0},
		{0,3,0,65536,196608,0,98304,-32768},
		{0,6,0,131072,393216,0,78643,-26214},
		{1,7,0,262144,589824,0,98304,-43690},
		{1,10,0,327680,851968,0,94663,-36408},
		{1,12,0,458752,1048576,0,95325,-41704},
		{2,14,0,524288,1245184,0,95783,-40329},
		{2,16,0,655360,1507328,0,100488,-43690},
		{2,19,0,720896,1703936,0,94663,-40049},
		{3,20,0,786432,1900544,0,100028,-41391},
		{3,23,0,917504,2162688,0,98304,-41704},
		{3,25,0,983040,2359296,0,98304,-40960},
		{4,27,0,1114112,2555904,0,98304,-42850},
		{4,29,0,1179648,2818048,0,100644,-42130},
		{4,32,0,1310720,3014656,0,97246,-42281},
		{5,33,0,1376256,3211264,0,100352,-43008},
		{5,36,0,1441792,3473408,0,99240,-41194},
		{5,38,0,1572864,3670016,0,99189,-42509},
		{6,40,0,1638400,3866624,0,99144,-42010},
		{6,43,0,1769472,4128768,0,98304,-42130},
		{6,43,0,1835008,4128768,65536,98304,-42130},
		{7,42,0,1966080,4128768,131072,100701,-44756},
		{7,43,0,2031616,4128768,262144,98304,-42130},
		{7,43,0,2097152,4128768,327680,98304,-42130},
		{8,42,0,2228224,4128768,458752,100701,-43157},
		{8,43,0,2293760,4128768,524288,98304,-42130},
		{8,43,0,2424832,4128768,655360,98304,-42130},
		{9,42,0,2490368,4128768,720896,100701,-43157},
		{9,43,0,2621440,4128768,786432,98304,-43690},
		{9,43,0,2686976,4128768,917504,98304,-42130},
		{10,42,0,2752512,4128768,983040,100701,-43157},
		{10,43,0,2883584,4128768,1114112,98304,-42130},
		{10,43,0,2949120,4128768,1179648,98304,-42130},
		{11,42,0,3080192,4128768,1310720,100701,-43157},
		{11,43,0,3145728,4128768,1376256,98304,-42130},
		{11,43,0,3276800,4128768,1441792,98304,-43690},
		{12,42,0,3342336,4128768,1572864,100701,-43157},
		{12,43,0,3407872,4128768,1638400,98304,-42130},
		{12,43,0,3538944,4128768,1769472,98304,-42130},
		{13,42,0,3604480,4128768,1835008,100701,-43157},
		{13,43,0,3735552,4128768,1966080,98304,-42130},
		{13,43,0,3801088,4128768,2031616,98304,-42130},
		{14,42,0,3932160,4128768,2097152,100701,-44756},
		{14,43,0,3997696,4128768,2228224,98304,-42130},
		{15,42,0,4128768,4128768,2293760,100701,-44756},
		{17,40,196608,4128768,4128768,2424832,100824,-43690},
		{20,38,393216,4128768,4128768,2490368,100960,-44281},
		{22,36,589824,4128768,4128768,2621440,101112,-43066},
		{25,33,851968,4128768,4128768,2686976,102400,-45056},
		{27,32,1048576,4128768,4128768,2752512,99361,-44395},
		{30,29,1245184,4128768,4128768,2883584,102985,-44470},
		{32,27,1507328,4128768,4128768,2949120,100824,-45371},
		{35,25,1703936,4128768,4128768,3080192,101034,-43690},
		{37,23,1900544,4128768,4128768,3145728,101282,-44683},
		{40,20,2162688,4128768,4128768,3276800,103477,-44840},
		{42,19,2359296,4128768,4128768,3342336,98304,-43690},
		{45,16,2555904,4128768,4128768,3407872,104857,-48059},
		{47,14,2818048,4128768,4128768,3538944,100824,-45371},
		{50,12,3014656,4128768,4128768,3604480,101282,-47662},
		{52,10,3211264,4128768,4128768,3735552,101944,-43690},
		{55,7,3473408,4128768,4128768,3801088,109226,-54613},
		{57,6,3670016,4128768,4128768,3932160,91750,-39321},
		{60,3,3866624,4128768,4128768,3997696,131072,-65536},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 24 */
		{0,1,0,0,0,0,0,0},
		{0,3,0,65536,196608,0,98304,-32768},
		{0,5,0,196608,393216,0,98304,-49152},
		{1,6,0,262144,589824,0,117964,-52428},
		{1,9,0,393216,786432,0,98304,-49152},
		{1,11,0,458752,983040,0,98304,-45875},
		{2,12,0,589824,1179648,0,107240,-53620},
		{2,15,0,655360,1376256,0,98304,-46811},
		{2,17,0,786432,1572864,0,98304,-49152},
		{3,18,0,851968,1769472,0,104086,-50115},
		{3,20,0,983040,1966080,0,103477,-51738},
		{3,23,0,1048576,2162688,0,98304,-47662},
		{4,24,0,1179648,2359296,0,102578,-51289},
		{4,26,0,1245184,2555904,0,102236,-49807},
		{5,28,0,1376256,2752512,0,101944,-50972},
		{5,30,0,1441792,2949120,0,101693,-49716},
		{5,32,0,1572864,3145728,0,101475,-50737},
		{6,33,0,1638400,3342336,0,104448,-51200},
		{6,36,0,1769472,3538944,0,101112,-50556},
		{6,38,0,1835008,3735552,0,100960,-49594},
		{7,39,0,1966080,3932160,0,103477,-51738},
		{7,42,0,2031616,4128768,0,100701,-49551},
		{7,42,0,2162688,4128768,65536,100701,-51150},
		{8,41,0,2228224,4128768,196608,103219,-50790},
		{8,42,0,2359296,4128768,262144,100701,-51150},
		{8,42,0,2424832,4128768,393216,100701,-49551},
		{9,41,0,2555904,4128768,458752,103219,-52428},
		{9,42,0,2621440,4128768,589824,100701,-49551},
		{10,41,0,2752512,4128768,655360,103219,-52428},
		{10,41,0,2818048,4128768,786432,103219,-50790},
		{10,42,0,2949120,4128768,851968,100701,-51150},
		{11,41,0,3014656,4128768,983040,103219,-50790},
		{11,41,0,3145728,4128768,1048576,103219,-52428},
		{11,42,0,3211264,4128768,1179648,100701,-49551},
		{12,41,0,3342336,4128768,1245184,103219,-52428},
		{12,42,0,3407872,4128768,1376256,100701,-49551},
		{12,42,0,3538944,4128768,1441792,100701,-51150},
		{13,41,0,3604480,4128768,1572864,103219,-50790},
		{13,42,0,3735552,4128768,1638400,100701,-51150},
		{13,42,0,3801088,4128768,1769472,100701,-49551},
		{14,41,0,3932160,4128768,1835008,103219,-52428},
		{14,42,0,3997696,4128768,1966080,100701,-49551},
		{15,41,0,4128768,4128768,2031616,103219,-52428},
		{17,39,196608,4128768,4128768,2162688,103477,-51738},
		{19,38,393216,4128768,4128768,2228224,100960,-51366},
		{21,36,589824,4128768,4128768,2359296,101112,-50556},
		{24,33,786432,4128768,4128768,2424832,104448,-53248},
		{26,32,983040,4128768,4128768,2555904,101475,-50737},
		{28,30,1179648,4128768,4128768,2621440,101693,-51976},
		{31,28,1376256,4128768,4128768,2752512,101944,-50972},
		{33,26,1572864,4128768,4128768,2818048,102236,-52428},
		{35,24,1769472,4128768,4128768,2949120,102578,-51289},
		{37,23,1966080,4128768,4128768,3014656,98304,-50641},
		{40,20,2162688,4128768,4128768,3145728,103477,-51738},
		{42,18,2359296,4128768,4128768,3211264,104086,-53970},
		{44,17,2555904,4128768,4128768,3342336,98304,-49152},
		{47,14,2752512,4128768,4128768,3407872,105865,-55453},
		{49,12,2949120,4128768,4128768,3538944,107240,-53620},
		{51,11,3145728,4128768,4128768,3604480,98304,-52428},
		{53,9,3342336,4128768,4128768,3735552,98304,-49152},
		{56,6,3538944,4128768,4128768,3801088,117964,-65536},
		{58,5,3735552,4128768,4128768,3932160,98304,-49152},
		{60,3,3932160,4128768,4128768,3997696,98304,-65536},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 25 */
		{0,1,0,0,0,0,0,0},
		{0,3,0,65536,131072,0,65536,-32768},
		{0,5,0,196608,327680,0,81920,-49152},
		{1,6,0,262144,524288,0,104857,-52428},
		{1,8,0,393216,655360,0,93622,-56173},
		{2,9,0,458752,851968,0,106496,-57344},
		{2,11,0,589824,1048576,0,104857,-58982},
		{2,13,0,720896,1245184,0,103765,-60074},
		{3,14,0,786432,1376256,0,105865,-60494},
		{3,16,0,917504,1572864,0,104857,-61166},
		{4,17,0,983040,1769472,0,110592,-61440},
		{4,19,0,1114112,1966080,0,109226,-61895},
		{5,20,0,1179648,2097152,0,110376,-62086},
		{5,22,0,1310720,2293760,0,109226,-62415},
		{5,24,0,1441792,2490368,0,108276,-62686},
		{6,25,0,1507328,2686976,0,111957,-62805},
		{6,27,0,1638400,2818048,0,108386,-63015},
		{7,28,0,1703936,3014656,0,111653,-63108},
		{7,30,0,1835008,3211264,0,110733,-63276},
		{8,31,0,1900544,3407872,0,113595,-63351},
		{8,33,0,2031616,3538944,0,110592,-63488},
		{8,35,0,2162688,3735552,0,109869,-63608},
		{9,36,0,2228224,3932160,0,112347,-63663},
		{9,38,0,2359296,4128768,0,111588,-63764},
		{10,37,0,2424832,4128768,65536,114688,-65536},
		{10,37,0,2555904,4128768,196608,114688,-65536},
		{11,37,0,2621440,4128768,262144,114688,-65536},
		{11,37,0,2752512,4128768,393216,114688,-65536},
		{11,38,0,2883584,4128768,458752,111588,-65536},
		{12,37,0,2949120,4128768,589824,114688,-65536},
		{12,37,0,3080192,4128768,720896,114688,-65536},
		{13,37,0,3145728,4128768,786432,114688,-65536},
		{13,37,0,3276800,4128768,917504,114688,-65536},
		{14,37,0,3342336,4128768,983040,114688,-65536},
		{14,37,0,3473408,4128768,1114112,114688,-65536},
		{14,38,0,3604480,4128768,1179648,111588,-65536},
		{15,37,0,3670016,4128768,1310720,114688,-65536},
		{15,37,0,3801088,4128768,1441792,114688,-65536},
		{16,37,0,3866624,4128768,1507328,114688,-65536},
		{16,37,0,3997696,4128768,1638400,114688,-65536},
		{17,37,0,4128768,4128768,1703936,114688,-67356},
		{19,35,131072,4128768,4128768,1835008,117579,-67463},
		{21,34,327680,4128768,4128768,1900544,115184,-67521},
		{23,32,524288,4128768,4128768,2031616,116273,-67650},
		{25,30,655360,4128768,4128768,2162688,119772,-67795},
		{27,29,851968,4128768,4128768,2228224,117028,-67876},
		{29,27,1048576,4128768,4128768,2359296,118468,-68056},
		{31,26,1245184,4128768,4128768,2424832,115343,-68157},
		{33,24,1376256,4128768,4128768,2555904,119674,-68385},
		{35,23,1572864,4128768,4128768,2621440,116177,-68514},
		{37,21,1769472,4128768,4128768,2752512,117964,-68812},
		{39,19,1966080,4128768,4128768,2883584,120149,-69176},
		{41,18,2097152,4128768,4128768,2949120,119506,-69391},
		{43,16,2293760,4128768,4128768,3080192,122333,-69905},
		{45,15,2490368,4128768,4128768,3145728,117028,-70217},
		{47,13,2686976,4128768,4128768,3276800,120149,-70997},
		{49,12,2818048,4128768,4128768,3342336,119156,-71493},
		{51,10,3014656,4128768,4128768,3473408,123790,-72817},
		{53,8,3211264,4128768,4128768,3604480,131072,-74898},
		{55,7,3407872,4128768,4128768,3670016,120149,-76458},
		{57,5,3538944,4128768,4128768,3801088,147456,-81920},
		{59,4,3735552,4128768,4128768,3866624,131072,-87381},
		{61,2,3932160,4128768,4128768,3997696,196608,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 26 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,65536,131072,0,131072,-65536},
		{0,4,0,196608,327680,0,109226,-65536},
		{1,5,0,262144,458752,0,114688,-65536},
		{1,7,0,393216,655360,0,109226,-65536},
		{2,8,0,524288,851968,0,121709,-74898},
		{2,10,0,589824,983040,0,109226,-65536},
		{3,11,0,720896,1179648,0,117964,-72089},
		{3,13,0,786432,1376256,0,114688,-65536},
		{4,13,0,917504,1507328,0,125610,-76458},
		{4,15,0,1048576,1703936,0,121709,-74898},
		{5,16,0,1114112,1835008,0,122333,-74274},
		{5,18,0,1245184,2031616,0,119506,-73246},
		{6,19,0,1376256,2228224,0,123790,-76458},
		{6,21,0,1441792,2359296,0,117964,-72089},
		{6,23,0,1572864,2555904,0,116177,-71493},
		{7,24,0,1638400,2752512,0,119674,-71234},
		{7,25,0,1769472,2883584,0,120149,-73728},
		{8,26,0,1900544,3080192,0,123207,-76021},
		{8,28,0,1966080,3211264,0,118935,-72817},
		{9,29,0,2097152,3407872,0,121709,-74898},
		{9,31,0,2162688,3604480,0,120149,-72089},
		{10,32,0,2293760,3735552,0,120501,-73992},
		{10,34,0,2424832,3932160,0,119156,-73479},
		{11,35,0,2490368,4128768,0,121434,-73246},
		{11,35,0,2621440,4128768,65536,121434,-75173},
		{12,34,0,2752512,4128768,196608,125114,-77451},
		{12,35,0,2818048,4128768,262144,121434,-75173},
		{12,35,0,2949120,4128768,393216,121434,-75173},
		{13,35,0,3014656,4128768,524288,121434,-73246},
		{13,35,0,3145728,4128768,589824,121434,-75173},
		{14,35,0,3276800,4128768,720896,121434,-75173},
		{14,35,0,3342336,4128768,786432,121434,-75173},
		{15,35,0,3473408,4128768,917504,121434,-75173},
		{15,35,0,3538944,4128768,1048576,121434,-73246},
		{16,35,0,3670016,4128768,1114112,121434,-75173},
		{16,35,0,3801088,4128768,1245184,121434,-75173},
		{17,35,0,3866624,4128768,1376256,121434,-73246},
		{17,35,0,3997696,4128768,1441792,121434,-75173},
		{18,34,0,4128768,4128768,1572864,125114,-77451},
		{19,34,131072,4128768,4128768,1638400,121142,-75465},
		{21,32,327680,4128768,4128768,1769472,122615,-76106},
		{23,31,458752,4128768,4128768,1900544,122333,-74274},
		{25,29,655360,4128768,4128768,1966080,124050,-77238},
		{27,28,851968,4128768,4128768,2097152,121362,-75245},
		{29,26,983040,4128768,4128768,2162688,125829,-78643},
		{31,25,1179648,4128768,4128768,2293760,122880,-76458},
		{33,23,1376256,4128768,4128768,2424832,125114,-77451},
		{34,23,1507328,4128768,4128768,2490368,119156,-74472},
		{36,21,1703936,4128768,4128768,2621440,121241,-75366},
		{38,20,1835008,4128768,4128768,2752512,120724,-72434},
		{40,18,2031616,4128768,4128768,2818048,123361,-77101},
		{42,16,2228224,4128768,4128768,2949120,126702,-78643},
		{44,15,2359296,4128768,4128768,3014656,126390,-79579},
		{46,13,2555904,4128768,4128768,3145728,131072,-81920},
		{48,12,2752512,4128768,4128768,3276800,125114,-77451},
		{49,11,2883584,4128768,4128768,3342336,124518,-78643},
		{51,10,3080192,4128768,4128768,3473408,116508,-72817},
		{53,8,3211264,4128768,4128768,3538944,131072,-84260},
		{55,7,3407872,4128768,4128768,3670016,120149,-76458},
		{57,5,3604480,4128768,4128768,3801088,131072,-81920},
		{59,4,3735552,4128768,4128768,3866624,131072,-87381},
		{61,2,3932160,4128768,4128768,3997696,196608,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 27 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,65536,131072,0,131072,-65536},
		{1,3,0,196608,262144,0,131072,-98304},
		{1,4,0,327680,458752,0,152917,-109226},
		{2,5,0,393216,589824,0,147456,-98304},
		{2,7,0,524288,786432,0,131072,-87381},
		{3,7,0,655360,917504,0,152917,-109226},
		{3,9,0,720896,1048576,0,131072,-90112},
		{4,10,0,851968,1245184,0,138353,-94663},
		{4,11,0,983040,1376256,0,137625,-98304},
		{5,12,0,1114112,1572864,0,142987,-101282},
		{5,14,0,1179648,1703936,0,131072,-90742},
		{6,14,0,1310720,1900544,0,146195,-100824},
		{7,15,0,1441792,2031616,0,145115,-102985},
		{7,17,0,1507328,2162688,0,135168,-94208},
		{8,17,0,1638400,2359296,0,147456,-102400},
		{8,19,0,1769472,2490368,0,138353,-98304},
		{9,20,0,1835008,2686976,0,141419,-96579},
		{9,21,0,1966080,2818048,0,140902,-98304},
		{10,22,0,2097152,3014656,0,143555,-99864},
		{10,24,0,2228224,3145728,0,136770,-96879},
		{11,24,0,2293760,3276800,0,142469,-99728},
		{11,26,0,2424832,3473408,0,138936,-96993},
		{12,27,0,2555904,3604480,0,138633,-98304},
		{12,28,0,2621440,3801088,0,140781,-97090},
		{13,29,0,2752512,3932160,0,140434,-98304},
		{14,30,0,2883584,4128768,0,142371,-99433},
		{14,30,0,2949120,4128768,65536,142371,-99433},
		{15,30,0,3080192,4128768,196608,142371,-99433},
		{15,30,0,3211264,4128768,327680,142371,-99433},
		{16,30,0,3342336,4128768,393216,142371,-101693},
		{16,30,0,3407872,4128768,524288,142371,-99433},
		{17,30,0,3538944,4128768,655360,142371,-99433},
		{17,30,0,3670016,4128768,720896,142371,-101693},
		{18,30,0,3735552,4128768,851968,142371,-99433},
		{18,30,0,3866624,4128768,983040,142371,-99433},
		{19,30,0,3997696,4128768,1114112,142371,-99433},
		{20,29,0,4128768,4128768,1179648,147456,-105325},
		{21,29,131072,4128768,4128768,1310720,142774,-100644},
		{23,28,262144,4128768,4128768,1441792,143208,-99517},
		{24,27,458752,4128768,4128768,1507328,141154,-100824},
		{26,26,589824,4128768,4128768,1638400,141557,-99614},
		{28,24,786432,4128768,4128768,1769472,145318,-102578},
		{29,24,917504,4128768,4128768,1835008,139620,-99728},
		{31,22,1048576,4128768,4128768,1966080,146675,-102985},
		{33,21,1245184,4128768,4128768,2097152,144179,-101580},
		{34,20,1376256,4128768,4128768,2228224,144869,-100028},
		{36,19,1572864,4128768,4128768,2293760,141994,-101944},
		{38,17,1703936,4128768,4128768,2424832,151552,-106496},
		{39,17,1900544,4128768,4128768,2555904,139264,-98304},
		{41,15,2031616,4128768,4128768,2621440,149796,-107666},
		{43,14,2162688,4128768,4128768,2752512,151236,-105865},
		{44,14,2359296,4128768,4128768,2883584,136113,-95783},
		{46,12,2490368,4128768,4128768,2949120,148945,-107240},
		{48,11,2686976,4128768,4128768,3080192,144179,-104857},
		{49,10,2818048,4128768,4128768,3211264,145635,-101944},
		{51,9,3014656,4128768,4128768,3342336,139264,-98304},
		{53,7,3145728,4128768,4128768,3407872,163840,-120149},
		{54,7,3276800,4128768,4128768,3538944,141994,-98304},
		{56,5,3473408,4128768,4128768,3670016,163840,-114688},
		{58,4,3604480,4128768,4128768,3735552,174762,-131072},
		{59,3,3801088,4128768,4128768,3866624,163840,-131072},
		{61,2,3932160,4128768,4128768,3997696,196608,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 28 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,65536,131072,0,131072,-65536},
		{1,2,0,196608,262144,0,262144,-196608},
		{1,4,0,327680,393216,0,131072,-109226},
		{2,4,0,458752,589824,0,196608,-152917},
		{3,5,0,589824,720896,0,180224,-147456},
		{3,6,0,655360,851968,0,170393,-131072},
		{4,7,0,786432,983040,0,163840,-131072},
		{5,7,0,917504,1179648,0,196608,-152917},
		{5,9,0,1048576,1310720,0,163840,-131072},
		{6,9,0,1179648,1441792,0,180224,-147456},
		{6,11,0,1245184,1572864,0,157286,-124518},
		{7,11,0,1376256,1769472,0,176947,-137625},
		{8,12,0,1507328,1900544,0,172776,-137029},
		{8,13,0,1638400,2031616,0,169301,-136533},
		{9,13,0,1769472,2162688,0,180224,-147456},
		{10,14,0,1835008,2359296,0,181484,-141154},
		{10,15,0,1966080,2490368,0,177883,-140434},
		{11,16,0,2097152,2621440,0,174762,-139810},
		{11,17,0,2228224,2752512,0,172032,-139264},
		{12,18,0,2359296,2949120,0,173477,-138782},
		{13,18,0,2424832,3080192,0,181187,-142637},
		{13,20,0,2555904,3211264,0,169013,-134521},
		{14,20,0,2686976,3342336,0,175912,-141419},
		{15,21,0,2818048,3538944,0,176947,-140902},
		{15,22,0,2949120,3670016,0,174762,-140434},
		{16,23,0,3014656,3801088,0,172776,-137029},
		{16,24,0,3145728,3932160,0,170963,-136770},
		{17,25,0,3276800,4128768,0,172032,-136533},
		{18,24,0,3407872,4128768,65536,179511,-145318},
		{18,25,0,3538944,4128768,196608,172032,-139264},
		{19,24,0,3604480,4128768,327680,179511,-142469},
		{20,24,0,3735552,4128768,458752,179511,-142469},
		{20,25,0,3866624,4128768,589824,172032,-136533},
		{21,24,0,3997696,4128768,655360,179511,-145318},
		{22,24,0,4128768,4128768,786432,179511,-145318},
		{23,24,131072,4128768,4128768,917504,173812,-139620},
		{24,23,262144,4128768,4128768,1048576,175755,-140008},
		{26,22,393216,4128768,4128768,1179648,177883,-140434},
		{27,21,589824,4128768,4128768,1245184,176947,-144179},
		{29,20,720896,4128768,4128768,1376256,179361,-144869},
		{30,20,851968,4128768,4128768,1507328,172463,-137970},
		{32,18,983040,4128768,4128768,1638400,185042,-146492},
		{33,18,1179648,4128768,4128768,1769472,173477,-138782},
		{35,17,1310720,4128768,4128768,1835008,176128,-143360},
		{36,16,1441792,4128768,4128768,1966080,179131,-144179},
		{38,15,1572864,4128768,4128768,2097152,182564,-145115},
		{39,14,1769472,4128768,4128768,2228224,181484,-146195},
		{41,13,1900544,4128768,4128768,2359296,185685,-147456},
		{42,13,2031616,4128768,4128768,2424832,174762,-141994},
		{43,12,2162688,4128768,4128768,2555904,178734,-142987},
		{45,11,2359296,4128768,4128768,2686976,176947,-144179},
		{46,11,2490368,4128768,4128768,2818048,163840,-131072},
		{48,9,2621440,4128768,4128768,2949120,188416,-147456},
		{49,9,2752512,4128768,4128768,3014656,172032,-139264},
		{51,7,2949120,4128768,4128768,3145728,196608,-163840},
		{52,7,3080192,4128768,4128768,3276800,174762,-141994},
		{54,6,3211264,4128768,4128768,3407872,183500,-144179},
		{55,5,3342336,4128768,4128768,3538944,196608,-147456},
		{57,4,3538944,4128768,4128768,3604480,196608,-174762},
		{58,4,3670016,4128768,4128768,3735552,152917,-131072},
		{60,2,3801088,4128768,4128768,3866624,327680,-262144},
		{61,2,3932160,4128768,4128768,3997696,196608,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 29 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,65536,131072,0,131072,-65536},
		{1,2,0,196608,262144,0,262144,-196608},
		{2,2,0,327680,393216,0,393216,-327680},
		{2,4,0,458752,524288,0,174762,-152917},
		{3,4,0,589824,655360,0,218453,-196608},
		{4,4,0,720896,786432,0,262144,-240298},
		{5,5,0,851968,917504,0,229376,-212992},
		{5,6,0,983040,1048576,0,209715,-196608},
		{6,6,0,1114112,1179648,0,235929,-222822},
		{7,7,0,1245184,1376256,0,229376,-207530},
		{8,7,0,1376256,1507328,0,251221,-229376},
		{8,8,0,1441792,1638400,0,234057,-205970},
		{9,8,0,1572864,1769472,0,252781,-224694},
		{10,9,0,1703936,1900544,0,237568,-212992},
		{10,10,0,1835008,2031616,0,225735,-203889},
		{11,10,0,1966080,2162688,0,240298,-218453},
		{12,11,0,2097152,2293760,0,229376,-209715},
		{13,11,0,2228224,2424832,0,242483,-222822},
		{13,12,0,2359296,2555904,0,232354,-214481},
		{14,13,0,2490368,2752512,0,229376,-207530},
		{15,13,0,2621440,2883584,0,240298,-218453},
		{16,13,0,2752512,3014656,0,251221,-229376},
		{16,14,0,2818048,3145728,0,241979,-216772},
		{17,15,0,2949120,3276800,0,234057,-210651},
		{18,15,0,3080192,3407872,0,243419,-220013},
		{18,16,0,3211264,3538944,0,235929,-214084},
		{19,17,0,3342336,3670016,0,229376,-208896},
		{20,17,0,3473408,3801088,0,237568,-217088},
		{21,17,0,3604480,3932160,0,245760,-225280},
		{21,19,0,3735552,4128768,0,229376,-207530},
		{22,18,0,3866624,4128768,65536,242868,-223593},
		{23,18,0,3997696,4128768,196608,242868,-223593},
		{24,18,0,4128768,4128768,327680,242868,-223593},
		{25,17,131072,4128768,4128768,458752,249856,-229376},
		{26,17,262144,4128768,4128768,589824,241664,-221184},
		{27,17,393216,4128768,4128768,720896,233472,-212992},
		{29,16,524288,4128768,4128768,851968,240298,-218453},
		{30,15,655360,4128768,4128768,983040,248100,-224694},
		{31,15,786432,4128768,4128768,1114112,238738,-215332},
		{33,14,917504,4128768,4128768,1245184,247020,-221814},
		{34,14,1048576,4128768,4128768,1376256,236937,-211731},
		{35,13,1179648,4128768,4128768,1441792,245760,-223914},
		{37,12,1376256,4128768,4128768,1572864,250228,-232354},
		{38,12,1507328,4128768,4128768,1703936,238312,-220439},
		{39,11,1638400,4128768,4128768,1835008,249036,-229376},
		{40,11,1769472,4128768,4128768,1966080,235929,-216268},
		{42,10,1900544,4128768,4128768,2097152,247580,-225735},
		{43,10,2031616,4128768,4128768,2228224,233016,-211171},
		{44,9,2162688,4128768,4128768,2359296,245760,-221184},
		{46,8,2293760,4128768,4128768,2490368,262144,-234057},
		{47,8,2424832,4128768,4128768,2621440,243419,-215332},
		{48,8,2555904,4128768,4128768,2752512,224694,-196608},
		{50,6,2752512,4128768,4128768,2818048,275251,-262144},
		{51,6,2883584,4128768,4128768,2949120,249036,-235929},
		{52,6,3014656,4128768,4128768,3080192,222822,-209715},
		{53,5,3145728,4128768,4128768,3211264,245760,-229376},
		{55,4,3276800,4128768,4128768,3342336,283989,-262144},
		{56,4,3407872,4128768,4128768,3473408,240298,-218453},
		{57,4,3538944,4128768,4128768,3604480,196608,-174762},
		{59,2,3670016,4128768,4128768,3735552,458752,-393216},
		{60,2,3801088,4128768,4128768,3866624,327680,-262144},
		{61,2,3932160,4128768,4128768,3997696,196608,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 30 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,131072,65536,0,65536,-131072},
		{1,2,0,262144,196608,0,196608,-262144},
		{2,2,0,393216,327680,0,327680,-393216},
		{3,2,0,524288,458752,0,458752,-524288},
		{4,2,0,655360,589824,0,589824,-655360},
		{5,2,0,786432,720896,0,720896,-786432},
		{5,4,0,917504,851968,0,283989,-305834},
		{6,4,0,1048576,983040,0,327680,-349525},
		{7,4,0,1179648,1114112,0,371370,-393216},
		{8,4,0,1310720,1245184,0,415061,-436906},
		{9,4,0,1441792,1376256,0,458752,-480597},
		{10,4,0,1572864,1507328,0,502442,-524288},
		{10,6,0,1703936,1638400,0,327680,-340787},
		{11,6,0,1835008,1769472,0,353894,-367001},
		{12,6,0,1966080,1900544,0,380108,-393216},
		{13,6,0,2097152,2031616,0,406323,-419430},
		{14,6,0,2228224,2162688,0,432537,-445644},
		{15,6,0,2359296,2293760,0,458752,-471859},
		{15,7,0,2490368,2424832,0,404138,-415061},
		{16,8,0,2621440,2555904,0,365129,-374491},
		{17,8,0,2752512,2686976,0,383853,-393216},
		{18,8,0,2883584,2818048,0,402578,-411940},
		{19,8,0,3014656,2949120,0,421302,-430665},
		{20,8,0,3145728,3080192,0,440027,-449389},
		{20,9,0,3276800,3211264,0,401408,-409600},
		{21,10,0,3407872,3342336,0,371370,-378652},
		{22,10,0,3538944,3473408,0,385934,-393216},
		{23,10,0,3670016,3604480,0,400497,-407779},
		{24,10,0,3801088,3735552,0,415061,-422343},
		{25,10,0,3932160,3866624,0,429624,-436906},
		{26,10,0,4128768,3997696,0,444188,-458752},
		{27,11,65536,4128768,4128768,0,406323,-412876},
		{28,10,196608,4128768,4128768,131072,436906,-444188},
		{29,10,327680,4128768,4128768,262144,422343,-429624},
		{30,10,458752,4128768,4128768,393216,407779,-415061},
		{31,10,589824,4128768,4128768,524288,393216,-400497},
		{32,10,720896,4128768,4128768,655360,378652,-385934},
		{34,9,851968,4128768,4128768,786432,409600,-417792},
		{35,8,983040,4128768,4128768,917504,449389,-458752},
		{36,8,1114112,4128768,4128768,1048576,430665,-440027},
		{37,8,1245184,4128768,4128768,1179648,411940,-421302},
		{38,8,1376256,4128768,4128768,1310720,393216,-402578},
		{39,8,1507328,4128768,4128768,1441792,374491,-383853},
		{41,7,1638400,4128768,4128768,1572864,415061,-425984},
		{42,6,1769472,4128768,4128768,1703936,471859,-484966},
		{43,6,1900544,4128768,4128768,1835008,445644,-458752},
		{44,6,2031616,4128768,4128768,1966080,419430,-432537},
		{45,6,2162688,4128768,4128768,2097152,393216,-406323},
		{46,6,2293760,4128768,4128768,2228224,367001,-380108},
		{47,6,2424832,4128768,4128768,2359296,340787,-353894},
		{49,4,2555904,4128768,4128768,2490368,524288,-546133},
		{50,4,2686976,4128768,4128768,2621440,480597,-502442},
		{51,4,2818048,4128768,4128768,2752512,436906,-458752},
		{52,4,2949120,4128768,4128768,2883584,393216,-415061},
		{53,4,3080192,4128768,4128768,3014656,349525,-371370},
		{54,4,3211264,4128768,4128768,3145728,305834,-327680},
		{56,2,3342336,4128768,4128768,3276800,786432,-851968},
		{57,2,3473408,4128768,4128768,3407872,655360,-720896},
		{58,2,3604480,4128768,4128768,3538944,524288,-589824},
		{59,2,3735552,4128768,4128768,3670016,393216,-458752},
		{60,2,3866624,4128768,4128768,3801088,262144,-327680},
		{61,2,3997696,4128768,4128768,3932160,131072,-196608},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 31 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,131072,65536,0,65536,-131072},
		{1,2,0,262144,196608,0,196608,-262144},
		{2,2,0,393216,327680,0,327680,-393216},
		{3,2,0,524288,458752,0,458752,-524288},
		{4,2,0,655360,589824,0,589824,-655360},
		{5,2,0,851968,720896,0,720896,-851968},
		{6,2,0,983040,786432,0,786432,-983040},
		{7,2,0,1114112,917504,0,917504,-1114112},
		{8,2,0,1245184,1048576,0,1048576,-1245184},
		{9,2,0,1376256,1179648,0,1179648,-1376256},
		{10,2,0,1507328,1310720,0,1310720,-1507328},
		{11,2,0,1703936,1441792,0,1441792,-1703936},
		{12,2,0,1835008,1572864,0,1572864,-1835008},
		{13,2,0,1966080,1638400,0,1638400,-1966080},
		{14,2,0,2097152,1769472,0,1769472,-2097152},
		{15,2,0,2228224,1900544,0,1900544,-2228224},
		{16,2,0,2359296,2031616,0,2031616,-2359296},
		{17,2,0,2555904,2162688,0,2162688,-2555904},
		{18,2,0,2686976,2293760,0,2293760,-2686976},
		{19,2,0,2818048,2424832,0,2424832,-2818048},
		{20,2,0,2949120,2490368,0,2490368,-2949120},
		{21,2,0,3080192,2621440,0,2621440,-3080192},
		{22,2,0,3211264,2752512,0,2752512,-3211264},
		{23,2,0,3407872,2883584,0,2883584,-3407872},
		{24,2,0,3538944,3014656,0,3014656,-3538944},
		{25,2,0,3670016,3145728,0,3145728,-3670016},
		{26,2,0,3801088,3276800,0,3276800,-3801088},
		{27,2,0,3932160,3342336,0,3342336,-3932160},
		{28,2,0,4128768,3473408,0,3473408,-4128768},
		{29,2,65536,4128768,3604480,0,3538944,-4128768},
		{30,2,196608,4128768,3735552,0,3538944,-4128768},
		{31,2,327680,4128768,3866624,0,3538944,-4128768},
		{32,2,458752,4128768,3997696,0,3538944,-4128768},
		{33,3,589824,4128768,4128768,0,1769472,-2064384},
		{34,2,720896,4128768,4128768,131072,3407872,-3997696},
		{35,2,786432,4128768,4128768,262144,3342336,-3866624},
		{36,2,917504,4128768,4128768,393216,3211264,-3735552},
		{37,2,1048576,4128768,4128768,524288,3080192,-3604480},
		{38,2,1179648,4128768,4128768,655360,2949120,-3473408},
		{39,2,1310720,4128768,4128768,851968,2818048,-3276800},
		{40,2,1441792,4128768,4128768,983040,2686976,-3145728},
		{41,2,1572864,4128768,4128768,1114112,2555904,-3014656},
		{42,2,1638400,4128768,4128768,1245184,2490368,-2883584},
		{43,2,1769472,4128768,4128768,1376256,2359296,-2752512},
		{44,2,1900544,4128768,4128768,1507328,2228224,-2621440},
		{45,2,2031616,4128768,4128768,1703936,2097152,-2424832},
		{46,2,2162688,4128768,4128768,1835008,1966080,-2293760},
		{47,2,2293760,4128768,4128768,1966080,1835008,-2162688},
		{48,2,2424832,4128768,4128768,2097152,1703936,-2031616},
		{49,2,2490368,4128768,4128768,2228224,1638400,-1900544},
		{50,2,2621440,4128768,4128768,2359296,1507328,-1769472},
		{51,2,2752512,4128768,4128768,2555904,1376256,-1572864},
		{52,2,2883584,4128768,4128768,2686976,1245184,-1441792},
		{53,2,3014656,4128768,4128768,2818048,1114112,-1310720},
		{54,2,3145728,4128768,4128768,2949120,983040,-1179648},
		{55,2,3276800,4128768,4128768,3080192,851968,-1048576},
		{56,2,3342336,4128768,4128768,3211264,786432,-917504},
		{57,2,3473408,4128768,4128768,3407872,655360,-720896},
		{58,2,3604480,4128768,4128768,3538944,524288,-589824},
		{59,2,3735552,4128768,4128768,3670016,393216,-458752},
		{60,2,3866624,4128768,4128768,3801088,262144,-327680},
		{61,2,3997696,4128768,4128768,3932160,131072,-196608},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 32 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,65536,131072,0,131072,-65536},
		{1,2,0,196608,262144,0,262144,-196608},
		{2,2,0,327680,393216,0,393216,-327680},
		{3,2,0,458752,589824,0,589824,-458752},
		{4,2,0,589824,720896,0,720896,-589824},
		{5,2,0,655360,851968,0,851968,-655360},
		{6,2,0,786432,983040,0,983040,-786432},
		{7,2,0,917504,1179648,0,1179648,-917504},
		{8,2,0,1048576,1310720,0,1310720,-1048576},
		{9,2,0,1179648,1441792,0,1441792,-1179648},
		{10,2,0,1245184,1572864,0,1572864,-1245184},
		{11,2,0,1376256,1769472,0,1769472,-1376256},
		{12,2,0,1507328,1900544,0,1900544,-1507328},
		{13,2,0,1638400,2031616,0,2031616,-1638400},
		{14,2,0,1769472,2162688,0,2162688,-1769472},
		{15,2,0,1835008,2359296,0,2359296,-1835008},
		{16,2,0,1966080,2490368,0,2490368,-1966080},
		{17,2,0,2097152,2621440,0,2621440,-2097152},
		{18,2,0,2228224,2752512,0,2752512,-2228224},
		{19,2,0,2359296,2949120,0,2949120,-2359296},
		{20,2,0,2424832,3080192,0,3080192,-2424832},
		{21,2,0,2555904,3211264,0,3211264,-2555904},
		{22,2,0,2686976,3342336,0,3342336,-2686976},
		{23,2,0,2818048,3538944,0,3538944,-2818048},
		{24,2,0,2949120,3670016,0,3670016,-2949120},
		{25,2,0,3014656,3801088,0,3801088,-3014656},
		{26,2,0,3145728,3932160,0,3932160,-3145728},
		{27,3,0,3276800,4128768,0,2064384,-1638400},
		{28,2,0,3407872,4128768,65536,4128768,-3342336},
		{29,2,0,3538944,4128768,196608,4128768,-3342336},
		{30,2,0,3604480,4128768,327680,4128768,-3276800},
		{31,2,0,3735552,4128768,458752,4128768,-3276800},
		{32,2,0,3866624,4128768,589824,4128768,-3276800},
		{33,2,0,3997696,4128768,655360,4128768,-3342336},
		{34,2,0,4128768,4128768,786432,4128768,-3342336},
		{35,2,131072,4128768,4128768,917504,3997696,-3211264},
		{36,2,262144,4128768,4128768,1048576,3866624,-3080192},
		{37,2,393216,4128768,4128768,1179648,3735552,-2949120},
		{38,2,589824,4128768,4128768,1245184,3538944,-2883584},
		{39,2,720896,4128768,4128768,1376256,3407872,-2752512},
		{40,2,851968,4128768,4128768,1507328,3276800,-2621440},
		{41,2,983040,4128768,4128768,1638400,3145728,-2490368},
		{42,2,1179648,4128768,4128768,1769472,2949120,-2359296},
		{43,2,1310720,4128768,4128768,1835008,2818048,-2293760},
		{44,2,1441792,4128768,4128768,1966080,2686976,-2162688},
		{45,2,1572864,4128768,4128768,2097152,2555904,-2031616},
		{46,2,1769472,4128768,4128768,2228224,2359296,-1900544},
		{47,2,1900544,4128768,4128768,2359296,2228224,-1769472},
		{48,2,2031616,4128768,4128768,2424832,2097152,-1703936},
		{49,2,2162688,4128768,4128768,2555904,1966080,-1572864},
		{50,2,2359296,4128768,4128768,2686976,1769472,-1441792},
		{51,2,2490368,4128768,4128768,2818048,1638400,-1310720},
		{52,2,2621440,4128768,4128768,2949120,1507328,-1179648},
		{53,2,2752512,4128768,4128768,3014656,1376256,-1114112},
		{54,2,2949120,4128768,4128768,3145728,1179648,-983040},
		{55,2,3080192,4128768,4128768,3276800,1048576,-851968},
		{56,2,3211264,4128768,4128768,3407872,917504,-720896},
		{57,2,3342336,4128768,4128768,3538944,786432,-589824},
		{58,2,3538944,4128768,4128768,3604480,589824,-524288},
		{59,2,3670016,4128768,4128768,3735552,458752,-393216},
		{60,2,3801088,4128768,4128768,3866624,327680,-262144},
		{61,2,3932160,4128768,4128768,3997696,196608,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 33 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,65536,131072,0,131072,-65536},
		{1,2,0,196608,262144,0,262144,-196608},
		{2,2,0,327680,458752,0,458752,-327680},
		{3,2,0,393216,589824,0,589824,-393216},
		{4,2,0,524288,786432,0,786432,-524288},
		{5,3,0,655360,917504,0,458752,-327680},
		{6,3,0,720896,1048576,0,524288,-360448},
		{6,4,0,851968,1245184,0,415061,-283989},
		{7,4,0,983040,1376256,0,458752,-327680},
		{8,4,0,1114112,1572864,0,524288,-371370},
		{9,5,0,1179648,1703936,0,425984,-294912},
		{10,5,0,1310720,1900544,0,475136,-327680},
		{11,5,0,1441792,2031616,0,507904,-360448},
		{12,5,0,1507328,2162688,0,540672,-376832},
		{12,6,0,1638400,2359296,0,471859,-327680},
		{13,7,0,1769472,2490368,0,415061,-294912},
		{14,7,0,1835008,2686976,0,447829,-305834},
		{15,7,0,1966080,2818048,0,469674,-327680},
		{16,7,0,2097152,3014656,0,502442,-349525},
		{17,7,0,2228224,3145728,0,524288,-371370},
		{18,8,0,2293760,3276800,0,468114,-327680},
		{19,8,0,2424832,3473408,0,496201,-346404},
		{19,9,0,2555904,3604480,0,450560,-319488},
		{20,9,0,2621440,3801088,0,475136,-327680},
		{21,9,0,2752512,3932160,0,491520,-344064},
		{22,10,0,2883584,4128768,0,458752,-320398},
		{23,9,0,2949120,4128768,65536,516096,-360448},
		{24,9,0,3080192,4128768,196608,516096,-360448},
		{25,9,0,3211264,4128768,327680,516096,-360448},
		{25,10,0,3342336,4128768,393216,458752,-327680},
		{26,10,0,3407872,4128768,524288,458752,-320398},
		{27,10,0,3538944,4128768,655360,458752,-320398},
		{28,10,0,3670016,4128768,720896,458752,-327680},
		{29,9,0,3735552,4128768,851968,516096,-360448},
		{30,9,0,3866624,4128768,983040,516096,-360448},
		{31,9,0,3997696,4128768,1114112,516096,-360448},
		{32,9,0,4128768,4128768,1179648,516096,-368640},
		{33,9,131072,4128768,4128768,1310720,499712,-352256},
		{34,9,262144,4128768,4128768,1441792,483328,-335872},
		{35,9,458752,4128768,4128768,1507328,458752,-327680},
		{36,8,589824,4128768,4128768,1638400,505563,-355766},
		{37,8,786432,4128768,4128768,1769472,477476,-337042},
		{39,7,917504,4128768,4128768,1835008,535210,-382293},
		{40,7,1048576,4128768,4128768,1966080,513365,-360448},
		{41,7,1245184,4128768,4128768,2097152,480597,-338602},
		{42,7,1376256,4128768,4128768,2228224,458752,-316757},
		{43,7,1572864,4128768,4128768,2293760,425984,-305834},
		{45,6,1703936,4128768,4128768,2424832,484966,-340787},
		{46,5,1900544,4128768,4128768,2555904,557056,-393216},
		{47,5,2031616,4128768,4128768,2621440,524288,-376832},
		{48,5,2162688,4128768,4128768,2752512,491520,-344064},
		{49,5,2359296,4128768,4128768,2883584,442368,-311296},
		{51,4,2490368,4128768,4128768,2949120,546133,-393216},
		{52,4,2686976,4128768,4128768,3080192,480597,-349525},
		{53,4,2818048,4128768,4128768,3211264,436906,-305834},
		{54,3,3014656,4128768,4128768,3342336,557056,-393216},
		{55,3,3145728,4128768,4128768,3407872,491520,-360448},
		{57,2,3276800,4128768,4128768,3538944,851968,-589824},
		{58,2,3473408,4128768,4128768,3670016,655360,-458752},
		{59,2,3604480,4128768,4128768,3735552,524288,-393216},
		{60,2,3801088,4128768,4128768,3866624,327680,-262144},
		{61,2,3932160,4128768,4128768,3997696,196608,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 34 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,65536,131072,0,131072,-65536},
		{1,2,0,196608,327680,0,327680,-196608},
		{2,3,0,262144,458752,0,229376,-131072},
		{3,3,0,393216,655360,0,327680,-196608},
		{3,4,0,524288,851968,0,283989,-174762},
		{4,5,0,589824,983040,0,245760,-147456},
		{5,5,0,720896,1179648,0,294912,-180224},
		{6,6,0,786432,1376256,0,275251,-157286},
		{6,7,0,917504,1507328,0,251221,-152917},
		{7,7,0,1048576,1703936,0,283989,-174762},
		{8,8,0,1114112,1835008,0,262144,-159158},
		{9,8,0,1245184,2031616,0,290230,-177883},
		{10,8,0,1376256,2228224,0,318317,-196608},
		{10,10,0,1441792,2359296,0,262144,-160199},
		{11,10,0,1572864,2555904,0,283989,-174762},
		{12,11,0,1638400,2752512,0,275251,-163840},
		{13,11,0,1769472,2883584,0,288358,-176947},
		{13,12,0,1900544,3080192,0,280017,-172776},
		{14,13,0,1966080,3211264,0,267605,-163840},
		{15,13,0,2097152,3407872,0,283989,-174762},
		{16,13,0,2162688,3604480,0,300373,-180224},
		{16,15,0,2293760,3735552,0,266825,-163840},
		{17,15,0,2424832,3932160,0,280868,-173202},
		{18,16,0,2490368,4128768,0,275251,-166024},
		{19,15,0,2621440,4128768,65536,294912,-182564},
		{20,15,0,2752512,4128768,196608,294912,-182564},
		{20,16,0,2818048,4128768,262144,275251,-170393},
		{21,16,0,2949120,4128768,393216,275251,-170393},
		{22,15,0,3014656,4128768,524288,294912,-177883},
		{23,15,0,3145728,4128768,589824,294912,-182564},
		{23,16,0,3276800,4128768,720896,275251,-170393},
		{24,16,0,3342336,4128768,786432,275251,-170393},
		{25,15,0,3473408,4128768,917504,294912,-182564},
		{26,15,0,3538944,4128768,1048576,294912,-177883},
		{26,16,0,3670016,4128768,1114112,275251,-170393},
		{27,16,0,3801088,4128768,1245184,275251,-170393},
		{28,16,0,3866624,4128768,1376256,275251,-166024},
		{29,15,0,3997696,4128768,1441792,294912,-182564},
		{30,15,0,4128768,4128768,1572864,294912,-182564},
		{31,15,131072,4128768,4128768,1638400,285549,-177883},
		{32,15,327680,4128768,4128768,1769472,271506,-168521},
		{34,13,458752,4128768,4128768,1900544,305834,-185685},
		{35,13,655360,4128768,4128768,1966080,289450,-180224},
		{36,13,851968,4128768,4128768,2097152,273066,-169301},
		{38,12,983040,4128768,4128768,2162688,285975,-178734},
		{39,11,1179648,4128768,4128768,2293760,294912,-183500},
		{41,10,1376256,4128768,4128768,2424832,305834,-189326},
		{42,10,1507328,4128768,4128768,2490368,291271,-182044},
		{43,10,1703936,4128768,4128768,2621440,269425,-167480},
		{45,9,1835008,4128768,4128768,2752512,286720,-172032},
		{46,8,2031616,4128768,4128768,2818048,299593,-187245},
		{47,8,2228224,4128768,4128768,2949120,271506,-168521},
		{49,7,2359296,4128768,4128768,3014656,294912,-185685},
		{50,7,2555904,4128768,4128768,3145728,262144,-163840},
		{52,5,2752512,4128768,4128768,3276800,344064,-212992},
		{53,5,2883584,4128768,4128768,3342336,311296,-196608},
		{54,5,3080192,4128768,4128768,3473408,262144,-163840},
		{56,4,3211264,4128768,4128768,3538944,305834,-196608},
		{57,3,3407872,4128768,4128768,3670016,360448,-229376},
		{58,3,3604480,4128768,4128768,3801088,262144,-163840},
		{60,2,3735552,4128768,4128768,3866624,393216,-262144},
		{61,2,3932160,4128768,4128768,3997696,196608,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 35 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,65536,131072,0,131072,-65536},
		{1,3,0,196608,327680,0,163840,-98304},
		{2,3,0,262144,524288,0,262144,-131072},
		{2,5,0,393216,720896,0,180224,-98304},
		{3,5,0,458752,917504,0,229376,-114688},
		{4,6,0,589824,1114112,0,222822,-117964},
		{4,8,0,655360,1310720,0,187245,-93622},
		{5,8,0,786432,1441792,0,205970,-112347},
		{6,9,0,851968,1638400,0,204800,-106496},
		{6,10,0,983040,1835008,0,203889,-109226},
		{7,11,0,1048576,2031616,0,203161,-104857},
		{8,12,0,1179648,2228224,0,202565,-107240},
		{8,13,0,1245184,2424832,0,202069,-103765},
		{9,14,0,1376256,2621440,0,201649,-105865},
		{10,14,0,1507328,2752512,0,211731,-115948},
		{10,16,0,1572864,2949120,0,196608,-104857},
		{11,17,0,1703936,3145728,0,196608,-106496},
		{12,17,0,1769472,3342336,0,208896,-110592},
		{12,19,0,1900544,3538944,0,196608,-105585},
		{13,19,0,1966080,3735552,0,207530,-109226},
		{14,20,0,2097152,3932160,0,206955,-110376},
		{15,21,0,2162688,4128768,0,206438,-108134},
		{15,21,0,2293760,4128768,65536,206438,-111411},
		{16,21,0,2359296,4128768,196608,206438,-108134},
		{17,21,0,2490368,4128768,262144,206438,-111411},
		{17,21,0,2555904,4128768,393216,206438,-108134},
		{18,21,0,2686976,4128768,458752,206438,-111411},
		{19,21,0,2818048,4128768,589824,206438,-111411},
		{19,21,0,2883584,4128768,655360,206438,-111411},
		{20,21,0,3014656,4128768,786432,206438,-111411},
		{21,21,0,3080192,4128768,851968,206438,-111411},
		{21,21,0,3211264,4128768,983040,206438,-111411},
		{22,21,0,3276800,4128768,1048576,206438,-111411},
		{23,21,0,3407872,4128768,1179648,206438,-111411},
		{23,21,0,3473408,4128768,1245184,206438,-111411},
		{24,21,0,3604480,4128768,1376256,206438,-111411},
		{25,21,0,3670016,4128768,1507328,206438,-108134},
		{25,21,0,3801088,4128768,1572864,206438,-111411},
		{26,21,0,3866624,4128768,1703936,206438,-108134},
		{27,21,0,3997696,4128768,1769472,206438,-111411},
		{28,20,0,4128768,4128768,1900544,217303,-117274},
		{29,20,131072,4128768,4128768,1966080,210405,-113825},
		{31,19,327680,4128768,4128768,2097152,211171,-112867},
		{32,19,524288,4128768,4128768,2162688,200248,-109226},
		{34,17,720896,4128768,4128768,2293760,212992,-114688},
		{35,17,917504,4128768,4128768,2359296,200704,-110592},
		{37,16,1114112,4128768,4128768,2490368,200977,-109226},
		{39,14,1310720,4128768,4128768,2555904,216772,-120989},
		{40,14,1441792,4128768,4128768,2686976,206690,-110907},
		{42,13,1638400,4128768,4128768,2818048,207530,-109226},
		{43,12,1835008,4128768,4128768,2883584,208523,-113198},
		{45,11,2031616,4128768,4128768,3014656,209715,-111411},
		{47,10,2228224,4128768,4128768,3080192,211171,-116508},
		{48,9,2424832,4128768,4128768,3211264,212992,-114688},
		{50,8,2621440,4128768,4128768,3276800,215332,-121709},
		{51,8,2752512,4128768,4128768,3407872,196608,-102985},
		{53,6,2949120,4128768,4128768,3473408,235929,-131072},
		{55,5,3145728,4128768,4128768,3604480,245760,-131072},
		{56,5,3342336,4128768,4128768,3670016,196608,-114688},
		{58,3,3538944,4128768,4128768,3801088,294912,-163840},
		{59,3,3735552,4128768,4128768,3866624,196608,-131072},
		{61,2,3932160,4128768,4128768,3997696,196608,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 36 */
		{0,1,0,0,0,0,0,0},
		{0,2,0,65536,196608,0,196608,-65536},
		{1,3,0,131072,393216,0,196608,-65536},
		{1,5,0,262144,589824,0,147456,-65536},
		{2,6,0,327680,786432,0,157286,-65536},
		{3,7,0,458752,983040,0,163840,-76458},
		{3,9,0,524288,1179648,0,147456,-65536},
		{4,9,0,655360,1441792,0,180224,-81920},
		{4,11,0,720896,1638400,0,163840,-72089},
		{5,12,0,851968,1835008,0,166818,-77451},
		{6,13,0,917504,2031616,0,169301,-76458},
		{6,15,0,1048576,2228224,0,159158,-74898},
		{7,16,0,1114112,2424832,0,161655,-74274},
		{7,18,0,1245184,2621440,0,154202,-73246},
		{8,18,0,1310720,2883584,0,169622,-77101},
		{9,19,0,1376256,3080192,0,171121,-76458},
		{9,21,0,1507328,3276800,0,163840,-75366},
		{10,22,0,1572864,3473408,0,165400,-74898},
		{10,24,0,1703936,3670016,0,159565,-74084},
		{11,25,0,1769472,3866624,0,161109,-73728},
		{12,26,0,1900544,4128768,0,165150,-76021},
		{12,26,0,1966080,4128768,65536,165150,-76021},
		{13,26,0,2097152,4128768,131072,165150,-78643},
		{13,26,0,2162688,4128768,262144,165150,-76021},
		{14,26,0,2293760,4128768,327680,165150,-78643},
		{15,26,0,2359296,4128768,458752,165150,-76021},
		{15,26,0,2490368,4128768,524288,165150,-78643},
		{16,26,0,2555904,4128768,655360,165150,-76021},
		{16,26,0,2686976,4128768,720896,165150,-78643},
		{17,26,0,2752512,4128768,851968,165150,-76021},
		{18,26,0,2818048,4128768,917504,165150,-76021},
		{18,26,0,2949120,4128768,1048576,165150,-76021},
		{19,26,0,3014656,4128768,1114112,165150,-76021},
		{19,26,0,3145728,4128768,1245184,165150,-76021},
		{20,26,0,3211264,4128768,1310720,165150,-76021},
		{21,26,0,3342336,4128768,1376256,165150,-78643},
		{21,26,0,3407872,4128768,1507328,165150,-76021},
		{22,26,0,3538944,4128768,1572864,165150,-78643},
		{22,26,0,3604480,4128768,1703936,165150,-76021},
		{23,26,0,3735552,4128768,1769472,165150,-78643},
		{24,26,0,3801088,4128768,1900544,165150,-76021},
		{24,26,0,3932160,4128768,1966080,165150,-78643},
		{25,26,0,3997696,4128768,2097152,165150,-76021},
		{26,25,0,4128768,4128768,2162688,172032,-81920},
		{27,25,196608,4128768,4128768,2293760,163840,-76458},
		{29,24,393216,4128768,4128768,2359296,162415,-76933},
		{31,22,589824,4128768,4128768,2490368,168521,-78019},
		{33,21,786432,4128768,4128768,2555904,167116,-78643},
		{35,19,983040,4128768,4128768,2686976,174762,-80099},
		{37,18,1179648,4128768,4128768,2752512,173477,-80956},
		{38,18,1441792,4128768,4128768,2818048,158057,-77101},
		{40,16,1638400,4128768,4128768,2949120,166024,-78643},
		{42,15,1835008,4128768,4128768,3014656,163840,-79579},
		{44,13,2031616,4128768,4128768,3145728,174762,-81920},
		{46,12,2228224,4128768,4128768,3211264,172776,-83409},
		{48,11,2424832,4128768,4128768,3342336,170393,-78643},
		{50,9,2621440,4128768,4128768,3407872,188416,-90112},
		{51,9,2883584,4128768,4128768,3538944,155648,-73728},
		{53,7,3080192,4128768,4128768,3604480,174762,-87381},
		{55,6,3276800,4128768,4128768,3735552,170393,-78643},
		{57,5,3473408,4128768,4128768,3801088,163840,-81920},
		{59,3,3670016,4128768,4128768,3932160,229376,-98304},
		{61,2,3866624,4128768,4128768,3997696,262144,-131072},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 37 */
		{0,1,0,0,0,0,0,0},
		{0,3,0,65536,196608,0,98304,-32768},
		{1,4,0,131072,458752,0,152917,-43690},
		{1,6,0,262144,655360,0,131072,-52428},
		{2,7,0,327680,917504,0,152917,-54613},
		{2,9,0,458752,1114112,0,139264,-57344},
		{3,11,0,524288,1376256,0,137625,-52428},
		{3,13,0,589824,1572864,0,131072,-49152},
		{4,14,0,720896,1835008,0,141154,-55453},
		{4,16,0,786432,2031616,0,135441,-52428},
		{5,17,0,917504,2293760,0,143360,-57344},
		{5,19,0,983040,2490368,0,138353,-54613},
		{6,21,0,1048576,2752512,0,137625,-52428},
		{6,23,0,1179648,2949120,0,134050,-53620},
		{7,24,0,1245184,3211264,0,139620,-54138},
		{8,25,0,1376256,3407872,0,141994,-57344},
		{8,27,0,1441792,3670016,0,141154,-55453},
		{9,28,0,1507328,3866624,0,143208,-55826},
		{9,31,0,1638400,4128768,0,137625,-54613},
		{10,30,0,1703936,4128768,65536,142371,-56496},
		{10,31,0,1835008,4128768,131072,137625,-56797},
		{11,30,0,1900544,4128768,262144,142371,-56496},
		{11,31,0,1966080,4128768,327680,137625,-54613},
		{12,30,0,2097152,4128768,458752,142371,-56496},
		{12,31,0,2162688,4128768,524288,137625,-54613},
		{13,30,0,2293760,4128768,589824,142371,-58756},
		{13,31,0,2359296,4128768,720896,137625,-54613},
		{14,30,0,2424832,4128768,786432,142371,-56496},
		{14,31,0,2555904,4128768,917504,137625,-54613},
		{15,30,0,2621440,4128768,983040,142371,-56496},
		{16,30,0,2752512,4128768,1048576,142371,-58756},
		{16,30,0,2818048,4128768,1179648,142371,-56496},
		{17,30,0,2883584,4128768,1245184,142371,-56496},
		{17,31,0,3014656,4128768,1376256,137625,-54613},
		{18,30,0,3080192,4128768,1441792,142371,-56496},
		{18,31,0,3211264,4128768,1507328,137625,-56797},
		{19,30,0,3276800,4128768,1638400,142371,-56496},
		{19,31,0,3342336,4128768,1703936,137625,-54613},
		{20,30,0,3473408,4128768,1835008,142371,-56496},
		{20,31,0,3538944,4128768,1900544,137625,-54613},
		{21,30,0,3670016,4128768,1966080,142371,-58756},
		{21,31,0,3735552,4128768,2097152,137625,-54613},
		{22,30,0,3801088,4128768,2162688,142371,-56496},
		{22,31,0,3932160,4128768,2293760,137625,-54613},
		{23,30,0,3997696,4128768,2359296,142371,-56496},
		{24,30,0,4128768,4128768,2424832,142371,-58756},
		{26,28,196608,4128768,4128768,2555904,145635,-58254},
		{28,27,458752,4128768,4128768,2621440,141154,-57974},
		{30,26,655360,4128768,4128768,2752512,138936,-55050},
		{32,24,917504,4128768,4128768,2818048,139620,-56987},
		{34,23,1114112,4128768,4128768,2883584,137029,-56599},
		{37,20,1376256,4128768,4128768,3014656,144869,-58637},
		{39,19,1572864,4128768,4128768,3080192,141994,-58254},
		{41,17,1835008,4128768,4128768,3211264,143360,-57344},
		{43,16,2031616,4128768,4128768,3276800,139810,-56797},
		{45,14,2293760,4128768,4128768,3342336,141154,-60494},
		{47,13,2490368,4128768,4128768,3473408,136533,-54613},
		{50,10,2752512,4128768,4128768,3538944,152917,-65536},
		{52,9,2949120,4128768,4128768,3670016,147456,-57344},
		{54,7,3211264,4128768,4128768,3735552,152917,-65536},
		{56,6,3407872,4128768,4128768,3801088,144179,-65536},
		{58,4,3670016,4128768,4128768,3932160,152917,-65536},
		{60,3,3866624,4128768,4128768,3997696,131072,-65536},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 38 */
		{0,1,0,0,0,0,0,0},
		{0,3,0,65536,196608,0,98304,-32768},
		{1,4,0,131072,458752,0,152917,-43690},
		{1,7,0,262144,720896,0,120149,-43690},
		{2,8,0,327680,917504,0,131072,-46811},
		{2,10,0,393216,1179648,0,131072,-43690},
		{3,12,0,524288,1441792,0,131072,-47662},
		{3,14,0,589824,1638400,0,126030,-45371},
		{4,15,0,655360,1900544,0,135753,-46811},
		{4,18,0,786432,2162688,0,127216,-46260},
		{5,19,0,851968,2424832,0,134712,-47331},
		{5,21,0,983040,2621440,0,131072,-49152},
		{6,23,0,1048576,2883584,0,131072,-47662},
		{6,25,0,1114112,3145728,0,131072,-46421},
		{7,26,0,1245184,3342336,0,133693,-49807},
		{7,29,0,1310720,3604480,0,128731,-46811},
		{8,30,0,1376256,3866624,0,133331,-47457},
		{8,33,0,1507328,4128768,0,129024,-47104},
		{9,32,0,1572864,4128768,65536,133186,-48623},
		{9,33,0,1703936,4128768,131072,129024,-49152},
		{10,32,0,1769472,4128768,262144,133186,-48623},
		{10,33,0,1835008,4128768,327680,129024,-47104},
		{11,32,0,1966080,4128768,393216,133186,-50737},
		{11,33,0,2031616,4128768,524288,129024,-47104},
		{12,32,0,2097152,4128768,589824,133186,-48623},
		{12,33,0,2228224,4128768,655360,129024,-49152},
		{13,32,0,2293760,4128768,786432,133186,-48623},
		{13,33,0,2359296,4128768,851968,129024,-47104},
		{14,32,0,2490368,4128768,983040,133186,-48623},
		{14,33,0,2555904,4128768,1048576,129024,-47104},
		{15,32,0,2686976,4128768,1114112,133186,-50737},
		{15,33,0,2752512,4128768,1245184,129024,-47104},
		{16,32,0,2818048,4128768,1310720,133186,-48623},
		{16,33,0,2949120,4128768,1376256,129024,-49152},
		{17,32,0,3014656,4128768,1507328,133186,-48623},
		{17,33,0,3080192,4128768,1572864,129024,-47104},
		{18,32,0,3211264,4128768,1703936,133186,-48623},
		{18,33,0,3276800,4128768,1769472,129024,-47104},
		{19,32,0,3407872,4128768,1835008,133186,-50737},
		{19,33,0,3473408,4128768,1966080,129024,-47104},
		{20,32,0,3538944,4128768,2031616,133186,-48623},
		{20,33,0,3670016,4128768,2097152,129024,-49152},
		{21,32,0,3735552,4128768,2228224,133186,-48623},
		{21,33,0,3801088,4128768,2293760,129024,-47104},
		{22,32,0,3932160,4128768,2359296,133186,-50737},
		{22,33,0,3997696,4128768,2490368,129024,-47104},
		{23,32,0,4128768,4128768,2555904,133186,-50737},
		{25,31,196608,4128768,4128768,2686976,131072,-48059},
		{27,29,458752,4128768,4128768,2752512,131072,-49152},
		{30,27,720896,4128768,4128768,2818048,131072,-50412},
		{32,25,917504,4128768,4128768,2949120,133802,-49152},
		{34,24,1179648,4128768,4128768,3014656,128222,-48439},
		{37,21,1441792,4128768,4128768,3080192,134348,-52428},
		{39,20,1638400,4128768,4128768,3211264,131072,-48289},
		{41,18,1900544,4128768,4128768,3276800,131072,-50115},
		{44,16,2162688,4128768,4128768,3407872,131072,-48059},
		{46,14,2424832,4128768,4128768,3473408,131072,-50412},
		{48,13,2621440,4128768,4128768,3538944,125610,-49152},
		{51,10,2883584,4128768,4128768,3670016,138353,-50972},
		{53,9,3145728,4128768,4128768,3735552,122880,-49152},
		{55,7,3342336,4128768,4128768,3801088,131072,-54613},
		{58,5,3604480,4128768,4128768,3932160,131072,-49152},
		{60,3,3866624,4128768,4128768,3997696,131072,-65536},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 39 */
		{0,1,0,0,0,0,0,0},
		{0,3,0,65536,262144,0,131072,-32768},
		{0,6,0,131072,524288,0,104857,-26214},
		{1,8,0,196608,786432,0,112347,-28086},
		{1,11,0,327680,1048576,0,104857,-32768},
		{2,13,0,393216,1376256,0,114688,-32768},
		{2,15,0,458752,1638400,0,117028,-32768},
		{3,17,0,589824,1900544,0,118784,-36864},
		{3,20,0,655360,2162688,0,113825,-34492},
		{3,23,0,720896,2424832,0,110219,-32768},
		{4,25,0,851968,2752512,0,114688,-35498},
		{4,27,0,917504,3014656,0,115948,-35288},
		{5,29,0,983040,3276800,0,117028,-35108},
		{5,32,0,1114112,3538944,0,114159,-35939},
		{6,34,0,1179648,3801088,0,115184,-35746},
		{6,37,0,1245184,4128768,0,114688,-34588},
		{7,36,0,1376256,4128768,65536,117964,-37449},
		{7,36,0,1441792,4128768,131072,117964,-37449},
		{7,37,0,1507328,4128768,196608,114688,-36408},
		{8,36,0,1572864,4128768,327680,117964,-35576},
		{8,37,0,1703936,4128768,393216,114688,-36408},
		{9,36,0,1769472,4128768,458752,117964,-37449},
		{9,37,0,1835008,4128768,589824,114688,-34588},
		{10,36,0,1966080,4128768,655360,117964,-37449},
		{10,36,0,2031616,4128768,720896,117964,-37449},
		{10,37,0,2097152,4128768,851968,114688,-34588},
		{11,36,0,2228224,4128768,917504,117964,-37449},
		{11,37,0,2293760,4128768,983040,114688,-36408},
		{12,36,0,2359296,4128768,1114112,117964,-35576},
		{12,37,0,2490368,4128768,1179648,114688,-36408},
		{13,36,0,2555904,4128768,1245184,117964,-37449},
		{13,37,0,2621440,4128768,1376256,114688,-34588},
		{14,36,0,2752512,4128768,1441792,117964,-37449},
		{14,36,0,2818048,4128768,1507328,117964,-37449},
		{14,37,0,2883584,4128768,1572864,114688,-36408},
		{15,36,0,2949120,4128768,1703936,117964,-35576},
		{15,37,0,3080192,4128768,1769472,114688,-36408},
		{16,36,0,3145728,4128768,1835008,117964,-37449},
		{16,37,0,3211264,4128768,1966080,114688,-34588},
		{17,36,0,3342336,4128768,2031616,117964,-37449},
		{17,36,0,3407872,4128768,2097152,117964,-37449},
		{17,37,0,3473408,4128768,2228224,114688,-34588},
		{18,36,0,3604480,4128768,2293760,117964,-37449},
		{18,37,0,3670016,4128768,2359296,114688,-36408},
		{19,36,0,3735552,4128768,2490368,117964,-35576},
		{19,37,0,3866624,4128768,2555904,114688,-36408},
		{20,36,0,3932160,4128768,2621440,117964,-37449},
		{20,37,0,3997696,4128768,2752512,114688,-34588},
		{21,36,0,4128768,4128768,2818048,117964,-37449},
		{23,34,262144,4128768,4128768,2883584,117170,-37732},
		{26,32,524288,4128768,4128768,2949120,116273,-38053},
		{29,29,786432,4128768,4128768,3080192,119369,-37449},
		{32,27,1048576,4128768,4128768,3145728,118468,-37809},
		{35,24,1376256,4128768,4128768,3211264,119674,-39891},
		{37,23,1638400,4128768,4128768,3342336,113198,-35746},
		{40,20,1900544,4128768,4128768,3407872,117274,-37941},
		{43,17,2162688,4128768,4128768,3473408,122880,-40960},
		{46,15,2424832,4128768,4128768,3604480,121709,-37449},
		{49,12,2752512,4128768,4128768,3670016,125114,-41704},
		{51,11,3014656,4128768,4128768,3735552,111411,-39321},
		{54,8,3276800,4128768,4128768,3866624,121709,-37449},
		{57,6,3538944,4128768,4128768,3932160,117964,-39321},
		{60,3,3801088,4128768,4128768,3997696,163840,-65536},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 40 */
		{0,1,0,0,0,0,0,0},
		{0,3,0,65536,262144,0,131072,-32768},
		{0,6,0,131072,524288,0,104857,-26214},
		{1,8,0,196608,786432,0,112347,-28086},
		{1,11,0,327680,1048576,0,104857,-32768},
		{1,14,0,393216,1376256,0,105865,-30247},
		{2,16,0,458752,1638400,0,109226,-30583},
		{2,19,0,589824,1900544,0,105585,-32768},
		{3,21,0,655360,2162688,0,108134,-32768},
		{3,24,0,720896,2424832,0,105427,-31343},
		{3,27,0,851968,2752512,0,105865,-32768},
		{4,29,0,917504,3014656,0,107666,-32768},
		{4,32,0,983040,3276800,0,105703,-31710},
		{5,34,0,1114112,3538944,0,107240,-33760},
		{5,37,0,1179648,3801088,0,105585,-32768},
		{5,40,0,1245184,4128768,0,105865,-31927},
		{6,39,0,1376256,4128768,65536,108651,-34492},
		{6,39,0,1441792,4128768,131072,108651,-34492},
		{7,39,0,1507328,4128768,196608,108651,-34492},
		{7,39,0,1572864,4128768,327680,108651,-32768},
		{7,39,0,1703936,4128768,393216,108651,-34492},
		{8,39,0,1769472,4128768,458752,108651,-34492},
		{8,39,0,1835008,4128768,589824,108651,-32768},
		{9,39,0,1966080,4128768,655360,108651,-34492},
		{9,39,0,2031616,4128768,720896,108651,-34492},
		{9,39,0,2097152,4128768,851968,108651,-32768},
		{10,39,0,2228224,4128768,917504,108651,-34492},
		{10,39,0,2293760,4128768,983040,108651,-34492},
		{11,39,0,2359296,4128768,1114112,108651,-32768},
		{11,39,0,2490368,4128768,1179648,108651,-34492},
		{11,39,0,2555904,4128768,1245184,108651,-34492},
		{12,39,0,2621440,4128768,1376256,108651,-32768},
		{12,39,0,2752512,4128768,1441792,108651,-34492},
		{13,39,0,2818048,4128768,1507328,108651,-34492},
		{13,39,0,2883584,4128768,1572864,108651,-34492},
		{13,39,0,2949120,4128768,1703936,108651,-32768},
		{14,39,0,3080192,4128768,1769472,108651,-34492},
		{14,39,0,3145728,4128768,1835008,108651,-34492},
		{15,39,0,3211264,4128768,1966080,108651,-32768},
		{15,39,0,3342336,4128768,2031616,108651,-34492},
		{15,39,0,3407872,4128768,2097152,108651,-34492},
		{16,39,0,3473408,4128768,2228224,108651,-32768},
		{16,39,0,3604480,4128768,2293760,108651,-34492},
		{17,39,0,3670016,4128768,2359296,108651,-34492},
		{17,39,0,3735552,4128768,2490368,108651,-32768},
		{17,39,0,3866624,4128768,2555904,108651,-34492},
		{18,39,0,3932160,4128768,2621440,108651,-34492},
		{18,39,0,3997696,4128768,2752512,108651,-32768},
		{19,39,0,4128768,4128768,2818048,108651,-34492},
		{21,37,262144,4128768,4128768,2883584,107406,-34588},
		{24,34,524288,4128768,4128768,2949120,109226,-35746},
		{27,32,786432,4128768,4128768,3080192,107817,-33825},
		{30,29,1048576,4128768,4128768,3145728,110006,-35108},
		{33,27,1376256,4128768,4128768,3211264,105865,-35288},
		{36,24,1638400,4128768,4128768,3342336,108276,-34192},
		{39,21,1900544,4128768,4128768,3407872,111411,-36044},
		{42,19,2162688,4128768,4128768,3473408,109226,-36408},
		{45,16,2424832,4128768,4128768,3604480,113595,-34952},
		{48,14,2752512,4128768,4128768,3670016,105865,-35288},
		{51,11,3014656,4128768,4128768,3735552,111411,-39321},
		{54,8,3276800,4128768,4128768,3866624,121709,-37449},
		{57,6,3538944,4128768,4128768,3932160,117964,-39321},
		{60,3,3801088,4128768,4128768,3997696,163840,-65536},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 41 */
		{0,1,0,0,0,0,0,0},
		{0,4,0,65536,262144,0,87381,-21845},
		{0,8,0,131072,589824,0,84260,-18724},
		{1,10,0,196608,917504,0,101944,-21845},
		{1,14,0,327680,1245184,0,95783,-25206},
		{1,17,0,393216,1572864,0,98304,-24576},
		{2,20,0,458752,1900544,0,100028,-24144},
		{2,23,0,524288,2162688,0,98304,-23831},
		{2,27,0,655360,2490368,0,95783,-25206},
		{3,29,0,720896,2818048,0,100644,-25746},
		{3,33,0,786432,3145728,0,98304,-24576},
		{3,36,0,851968,3473408,0,99240,-24341},
		{4,39,0,983040,3801088,0,100028,-25869},
		{4,43,0,1048576,4128768,0,98304,-24966},
		{4,43,0,1114112,4128768,65536,98304,-24966},
		{5,42,0,1179648,4128768,131072,100701,-25575},
		{5,43,0,1310720,4128768,196608,98304,-26526},
		{5,43,0,1376256,4128768,327680,98304,-24966},
		{6,42,0,1441792,4128768,393216,100701,-25575},
		{6,43,0,1507328,4128768,458752,98304,-24966},
		{6,43,0,1638400,4128768,524288,98304,-26526},
		{7,42,0,1703936,4128768,655360,100701,-25575},
		{7,43,0,1769472,4128768,720896,98304,-24966},
		{7,43,0,1835008,4128768,786432,98304,-24966},
		{8,42,0,1966080,4128768,851968,100701,-27173},
		{8,43,0,2031616,4128768,983040,98304,-24966},
		{8,43,0,2097152,4128768,1048576,98304,-24966},
		{9,42,0,2228224,4128768,1114112,100701,-27173},
		{9,43,0,2293760,4128768,1179648,98304,-26526},
		{9,43,0,2359296,4128768,1310720,98304,-24966},
		{10,42,0,2424832,4128768,1376256,100701,-25575},
		{10,43,0,2555904,4128768,1441792,98304,-26526},
		{10,43,0,2621440,4128768,1507328,98304,-26526},
		{11,42,0,2686976,4128768,1638400,100701,-25575},
		{11,43,0,2752512,4128768,1703936,98304,-24966},
		{11,43,0,2883584,4128768,1769472,98304,-26526},
		{12,42,0,2949120,4128768,1835008,100701,-27173},
		{12,43,0,3014656,4128768,1966080,98304,-24966},
		{12,43,0,3080192,4128768,2031616,98304,-24966},
		{13,42,0,3211264,4128768,2097152,100701,-27173},
		{13,43,0,3276800,4128768,2228224,98304,-24966},
		{13,43,0,3342336,4128768,2293760,98304,-24966},
		{14,42,0,3407872,4128768,2359296,100701,-25575},
		{14,43,0,3538944,4128768,2424832,98304,-26526},
		{14,43,0,3604480,4128768,2555904,98304,-24966},
		{15,42,0,3670016,4128768,2621440,100701,-25575},
		{15,43,0,3735552,4128768,2686976,98304,-24966},
		{15,43,0,3866624,4128768,2752512,98304,-26526},
		{16,42,0,3932160,4128768,2883584,100701,-25575},
		{16,43,0,3997696,4128768,2949120,98304,-24966},
		{17,42,0,4128768,4128768,3014656,100701,-27173},
		{20,39,262144,4128768,4128768,3080192,101753,-27594},
		{24,36,589824,4128768,4128768,3211264,101112,-26214},
		{27,33,917504,4128768,4128768,3276800,100352,-26624},
		{31,29,1245184,4128768,4128768,3342336,102985,-28086},
		{34,27,1572864,4128768,4128768,3407872,98304,-27726},
		{38,23,1900544,4128768,4128768,3538944,101282,-26810},
		{41,20,2162688,4128768,4128768,3604480,103477,-27594},
		{45,17,2490368,4128768,4128768,3670016,102400,-28672},
		{48,14,2818048,4128768,4128768,3735552,100824,-30247},
		{52,10,3145728,4128768,4128768,3866624,109226,-29127},
		{55,8,3473408,4128768,4128768,3932160,93622,-28086},
		{59,4,3801088,4128768,4128768,3997696,109226,-43690},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 42 */
		{0,1,0,0,0,0,0,0},
		{0,4,0,65536,327680,0,109226,-21845},
		{0,8,0,131072,655360,0,93622,-18724},
		{0,12,0,196608,983040,0,89367,-17873},
		{1,15,0,262144,1376256,0,98304,-18724},
		{1,19,0,393216,1703936,0,94663,-21845},
		{1,23,0,458752,2031616,0,92346,-20852},
		{2,26,0,524288,2359296,0,94371,-20971},
		{2,30,0,589824,2752512,0,94914,-20338},
		{2,34,0,720896,3080192,0,93339,-21845},
		{3,37,0,786432,3407872,0,94663,-21845},
		{3,41,0,851968,3735552,0,93388,-21299},
		{3,45,0,917504,4128768,0,93835,-20852},
		{4,44,0,1048576,4128768,65536,96017,-22861},
		{4,44,0,1114112,4128768,131072,96017,-22861},
		{4,44,0,1179648,4128768,196608,96017,-22861},
		{5,44,0,1245184,4128768,262144,96017,-22861},
		{5,44,0,1376256,4128768,393216,96017,-22861},
		{5,44,0,1441792,4128768,458752,96017,-22861},
		{5,45,0,1507328,4128768,524288,93835,-22341},
		{6,44,0,1572864,4128768,589824,96017,-22861},
		{6,44,0,1638400,4128768,720896,96017,-21337},
		{6,45,0,1769472,4128768,786432,93835,-22341},
		{7,44,0,1835008,4128768,851968,96017,-22861},
		{7,44,0,1900544,4128768,917504,96017,-22861},
		{7,45,0,1966080,4128768,1048576,93835,-20852},
		{8,44,0,2097152,4128768,1114112,96017,-22861},
		{8,44,0,2162688,4128768,1179648,96017,-22861},
		{8,45,0,2228224,4128768,1245184,93835,-22341},
		{9,44,0,2293760,4128768,1376256,96017,-21337},
		{9,44,0,2424832,4128768,1441792,96017,-22861},
		{9,44,0,2490368,4128768,1507328,96017,-22861},
		{10,44,0,2555904,4128768,1572864,96017,-22861},
		{10,44,0,2621440,4128768,1638400,96017,-22861},
		{10,44,0,2752512,4128768,1769472,96017,-22861},
		{10,45,0,2818048,4128768,1835008,93835,-22341},
		{11,44,0,2883584,4128768,1900544,96017,-22861},
		{11,44,0,2949120,4128768,1966080,96017,-22861},
		{11,45,0,3014656,4128768,2097152,93835,-20852},
		{12,44,0,3145728,4128768,2162688,96017,-22861},
		{12,44,0,3211264,4128768,2228224,96017,-22861},
		{12,45,0,3276800,4128768,2293760,93835,-22341},
		{13,44,0,3342336,4128768,2424832,96017,-21337},
		{13,44,0,3473408,4128768,2490368,96017,-22861},
		{13,45,0,3538944,4128768,2555904,93835,-22341},
		{14,44,0,3604480,4128768,2621440,96017,-22861},
		{14,44,0,3670016,4128768,2752512,96017,-21337},
		{14,44,0,3801088,4128768,2818048,96017,-22861},
		{15,44,0,3866624,4128768,2883584,96017,-22861},
		{15,44,0,3932160,4128768,2949120,96017,-22861},
		{15,44,0,3997696,4128768,3014656,96017,-22861},
		{16,44,0,4128768,4128768,3145728,96017,-22861},
		{19,41,327680,4128768,4128768,3211264,95027,-22937},
		{23,37,655360,4128768,4128768,3276800,96483,-23665},
		{27,34,983040,4128768,4128768,3342336,95325,-23831},
		{31,30,1376256,4128768,4128768,3473408,94914,-22598},
		{35,26,1703936,4128768,4128768,3538944,96993,-23592},
		{39,23,2031616,4128768,4128768,3604480,95325,-23831},
		{43,19,2359296,4128768,4128768,3670016,98304,-25486},
		{47,15,2752512,4128768,4128768,3801088,98304,-23405},
		{51,12,3080192,4128768,4128768,3866624,95325,-23831},
		{55,8,3407872,4128768,4128768,3932160,102985,-28086},
		{59,4,3735552,4128768,4128768,3997696,131072,-43690},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 43 */
		{0,1,0,0,0,0,0,0},
		{0,5,0,65536,393216,0,98304,-16384},
		{0,10,0,131072,786432,0,87381,-14563},
		{0,15,0,196608,1179648,0,84260,-14043},
		{1,19,0,262144,1638400,0,91022,-14563},
		{1,24,0,327680,2031616,0,88331,-14246},
		{1,29,0,458752,2424832,0,86601,-16384},
		{1,34,0,524288,2883584,0,87381,-15887},
		{2,38,0,589824,3276800,0,88562,-15941},
		{2,43,0,655360,3670016,0,87381,-15603},
		{2,48,0,720896,4128768,0,87846,-15338},
		{2,48,0,851968,4128768,65536,87846,-16732},
		{3,47,0,917504,4128768,131072,89755,-17096},
		{3,47,0,983040,4128768,196608,89755,-17096},
		{3,48,0,1048576,4128768,262144,87846,-16732},
		{3,48,0,1114112,4128768,327680,87846,-16732},
		{4,47,0,1245184,4128768,458752,89755,-17096},
		{4,47,0,1310720,4128768,524288,89755,-17096},
		{4,48,0,1376256,4128768,589824,87846,-16732},
		{5,47,0,1441792,4128768,655360,89755,-17096},
		{5,47,0,1507328,4128768,720896,89755,-17096},
		{5,47,0,1572864,4128768,851968,89755,-15671},
		{5,48,0,1703936,4128768,917504,87846,-16732},
		{6,47,0,1769472,4128768,983040,89755,-17096},
		{6,47,0,1835008,4128768,1048576,89755,-17096},
		{6,47,0,1900544,4128768,1114112,89755,-17096},
		{6,48,0,1966080,4128768,1245184,87846,-15338},
		{7,47,0,2097152,4128768,1310720,89755,-17096},
		{7,47,0,2162688,4128768,1376256,89755,-17096},
		{7,48,0,2228224,4128768,1441792,87846,-16732},
		{7,48,0,2293760,4128768,1507328,87846,-16732},
		{8,47,0,2359296,4128768,1572864,89755,-17096},
		{8,47,0,2490368,4128768,1703936,89755,-17096},
		{8,48,0,2555904,4128768,1769472,87846,-16732},
		{8,48,0,2621440,4128768,1835008,87846,-16732},
		{9,47,0,2686976,4128768,1900544,89755,-17096},
		{9,47,0,2752512,4128768,1966080,89755,-17096},
		{9,48,0,2818048,4128768,2097152,87846,-15338},
		{10,47,0,2949120,4128768,2162688,89755,-17096},
		{10,47,0,3014656,4128768,2228224,89755,-17096},
		{10,47,0,3080192,4128768,2293760,89755,-17096},
		{10,48,0,3145728,4128768,2359296,87846,-16732},
		{11,47,0,3211264,4128768,2490368,89755,-15671},
		{11,47,0,3342336,4128768,2555904,89755,-17096},
		{11,47,0,3407872,4128768,2621440,89755,-17096},
		{11,48,0,3473408,4128768,2686976,87846,-16732},
		{12,47,0,3538944,4128768,2752512,89755,-17096},
		{12,47,0,3604480,4128768,2818048,89755,-17096},
		{12,48,0,3735552,4128768,2949120,87846,-16732},
		{12,48,0,3801088,4128768,3014656,87846,-16732},
		{13,47,0,3866624,4128768,3080192,89755,-17096},
		{13,47,0,3932160,4128768,3145728,89755,-17096},
		{13,48,0,3997696,4128768,3211264,87846,-16732},
		{14,47,0,4128768,4128768,3342336,89755,-17096},
		{18,43,393216,4128768,4128768,3407872,88941,-17164},
		{23,38,786432,4128768,4128768,3473408,90333,-17712},
		{28,34,1179648,4128768,4128768,3538944,89367,-17873},
		{33,29,1638400,4128768,4128768,3604480,88941,-18724},
		{38,24,2031616,4128768,4128768,3735552,91180,-17096},
		{43,19,2424832,4128768,4128768,3801088,94663,-18204},
		{48,15,2883584,4128768,4128768,3866624,88941,-18724},
		{53,10,3276800,4128768,4128768,3932160,94663,-21845},
		{58,5,3670016,4128768,4128768,3997696,114688,-32768},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 44 */
		{0,1,0,0,0,0,0,0},
		{0,6,0,65536,458752,0,91750,-13107},
		{0,12,0,131072,917504,0,83409,-11915},
		{0,17,0,196608,1376256,0,86016,-12288},
		{0,23,0,262144,1835008,0,83409,-11915},
		{1,27,0,327680,2293760,0,88221,-12603},
		{1,33,0,458752,2752512,0,86016,-14336},
		{1,38,0,524288,3211264,0,86790,-14169},
		{1,44,0,589824,3670016,0,85349,-13716},
		{2,49,0,655360,4128768,0,86016,-13653},
		{2,49,0,720896,4128768,65536,86016,-13653},
		{2,49,0,786432,4128768,131072,86016,-13653},
		{2,49,0,917504,4128768,196608,86016,-15018},
		{3,48,0,983040,4128768,262144,87846,-15338},
		{3,49,0,1048576,4128768,327680,86016,-15018},
		{3,49,0,1114112,4128768,458752,86016,-13653},
		{3,49,0,1179648,4128768,524288,86016,-13653},
		{4,48,0,1245184,4128768,589824,87846,-13943},
		{4,49,0,1376256,4128768,655360,86016,-15018},
		{4,49,0,1441792,4128768,720896,86016,-15018},
		{4,49,0,1507328,4128768,786432,86016,-15018},
		{5,48,0,1572864,4128768,917504,87846,-13943},
		{5,49,0,1638400,4128768,983040,86016,-13653},
		{5,49,0,1703936,4128768,1048576,86016,-13653},
		{5,49,0,1835008,4128768,1114112,86016,-15018},
		{6,48,0,1900544,4128768,1179648,87846,-15338},
		{6,49,0,1966080,4128768,1245184,86016,-15018},
		{6,49,0,2031616,4128768,1376256,86016,-13653},
		{6,49,0,2097152,4128768,1441792,86016,-13653},
		{6,49,0,2162688,4128768,1507328,86016,-13653},
		{7,49,0,2293760,4128768,1572864,86016,-15018},
		{7,49,0,2359296,4128768,1638400,86016,-15018},
		{7,49,0,2424832,4128768,1703936,86016,-15018},
		{7,49,0,2490368,4128768,1835008,86016,-13653},
		{8,49,0,2555904,4128768,1900544,86016,-13653},
		{8,49,0,2621440,4128768,1966080,86016,-13653},
		{8,49,0,2752512,4128768,2031616,86016,-15018},
		{8,49,0,2818048,4128768,2097152,86016,-15018},
		{9,48,0,2883584,4128768,2162688,87846,-15338},
		{9,49,0,2949120,4128768,2293760,86016,-13653},
		{9,49,0,3014656,4128768,2359296,86016,-13653},
		{9,49,0,3080192,4128768,2424832,86016,-13653},
		{10,48,0,3211264,4128768,2490368,87846,-15338},
		{10,49,0,3276800,4128768,2555904,86016,-15018},
		{10,49,0,3342336,4128768,2621440,86016,-15018},
		{10,49,0,3407872,4128768,2752512,86016,-13653},
		{11,48,0,3473408,4128768,2818048,87846,-13943},
		{11,49,0,3538944,4128768,2883584,86016,-13653},
		{11,49,0,3670016,4128768,2949120,86016,-15018},
		{11,49,0,3735552,4128768,3014656,86016,-15018},
		{12,48,0,3801088,4128768,3080192,87846,-15338},
		{12,49,0,3866624,4128768,3211264,86016,-13653},
		{12,49,0,3932160,4128768,3276800,86016,-13653},
		{12,49,0,3997696,4128768,3342336,86016,-13653},
		{13,48,0,4128768,4128768,3407872,87846,-15338},
		{18,44,458752,4128768,4128768,3473408,85349,-15240},
		{24,38,917504,4128768,4128768,3538944,86790,-15941},
		{29,33,1376256,4128768,4128768,3670016,86016,-14336},
		{35,27,1835008,4128768,4128768,3735552,88221,-15123},
		{40,23,2293760,4128768,4128768,3801088,83409,-14894},
		{46,17,2752512,4128768,4128768,3866624,86016,-16384},
		{51,12,3211264,4128768,4128768,3932160,83409,-17873},
		{57,6,3670016,4128768,4128768,3997696,91750,-26214},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 45 */
		{0,1,0,0,0,0,0,0},
		{0,6,0,65536,458752,0,91750,-13107},
		{0,12,0,131072,917504,0,83409,-11915},
		{0,18,0,196608,1376256,0,80956,-11565},
		{0,24,0,262144,1835008,0,79782,-11397},
		{1,28,0,327680,2293760,0,84954,-12136},
		{1,34,0,458752,2752512,0,83409,-13901},
		{1,40,0,524288,3211264,0,82340,-13443},
		{1,46,0,589824,3670016,0,81555,-13107},
		{1,52,0,655360,4128768,0,80956,-12850},
		{2,51,0,720896,4128768,65536,82575,-13107},
		{2,51,0,786432,4128768,131072,82575,-13107},
		{2,51,0,917504,4128768,196608,82575,-14417},
		{2,51,0,983040,4128768,262144,82575,-14417},
		{2,52,0,1048576,4128768,327680,80956,-14135},
		{3,51,0,1114112,4128768,458752,82575,-13107},
		{3,51,0,1179648,4128768,524288,82575,-13107},
		{3,51,0,1245184,4128768,589824,82575,-13107},
		{3,51,0,1376256,4128768,655360,82575,-14417},
		{3,52,0,1441792,4128768,720896,80956,-14135},
		{4,51,0,1507328,4128768,786432,82575,-14417},
		{4,51,0,1572864,4128768,917504,82575,-13107},
		{4,51,0,1638400,4128768,983040,82575,-13107},
		{4,51,0,1703936,4128768,1048576,82575,-13107},
		{4,52,0,1835008,4128768,1114112,80956,-14135},
		{5,51,0,1900544,4128768,1179648,82575,-14417},
		{5,51,0,1966080,4128768,1245184,82575,-14417},
		{5,51,0,2031616,4128768,1376256,82575,-13107},
		{5,51,0,2097152,4128768,1441792,82575,-13107},
		{5,52,0,2162688,4128768,1507328,80956,-12850},
		{6,51,0,2293760,4128768,1572864,82575,-14417},
		{6,51,0,2359296,4128768,1638400,82575,-14417},
		{6,51,0,2424832,4128768,1703936,82575,-14417},
		{6,51,0,2490368,4128768,1835008,82575,-13107},
		{6,52,0,2555904,4128768,1900544,80956,-12850},
		{7,51,0,2621440,4128768,1966080,82575,-13107},
		{7,51,0,2752512,4128768,2031616,82575,-14417},
		{7,51,0,2818048,4128768,2097152,82575,-14417},
		{7,51,0,2883584,4128768,2162688,82575,-14417},
		{7,52,0,2949120,4128768,2293760,80956,-12850},
		{8,51,0,3014656,4128768,2359296,82575,-13107},
		{8,51,0,3080192,4128768,2424832,82575,-13107},
		{8,51,0,3211264,4128768,2490368,82575,-14417},
		{8,51,0,3276800,4128768,2555904,82575,-14417},
		{8,52,0,3342336,4128768,2621440,80956,-14135},
		{9,51,0,3407872,4128768,2752512,82575,-13107},
		{9,51,0,3473408,4128768,2818048,82575,-13107},
		{9,51,0,3538944,4128768,2883584,82575,-13107},
		{9,51,0,3670016,4128768,2949120,82575,-14417},
		{9,52,0,3735552,4128768,3014656,80956,-14135},
		{10,51,0,3801088,4128768,3080192,82575,-14417},
		{10,51,0,3866624,4128768,3211264,82575,-13107},
		{10,51,0,3932160,4128768,3276800,82575,-13107},
		{10,51,0,3997696,4128768,3342336,82575,-13107},
		{11,51,0,4128768,4128768,3407872,82575,-14417},
		{16,46,458752,4128768,4128768,3473408,81555,-14563},
		{22,40,917504,4128768,4128768,3538944,82340,-15123},
		{28,34,1376256,4128768,4128768,3670016,83409,-13901},
		{34,28,1835008,4128768,4128768,3735552,84954,-14563},
		{39,24,2293760,4128768,4128768,3801088,79782,-14246},
		{45,18,2752512,4128768,4128768,3866624,80956,-15420},
		{51,12,3211264,4128768,4128768,3932160,83409,-17873},
		{57,6,3670016,4128768,4128768,3997696,91750,-26214},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 46 */
		{0,1,0,0,0,0,0,0},
		{0,7,0,65536,458752,0,76458,-10922},
		{0,14,0,131072,983040,0,75618,-10082},
		{0,20,0,196608,1507328,0,79333,-10347},
		{0,27,0,262144,2031616,0,78139,-10082},
		{0,34,0,327680,2555904,0,77451,-9929},
		{1,39,0,393216,3080192,0,81057,-10347},
		{1,46,0,524288,3604480,0,80099,-11650},
		{1,53,0,589824,4128768,0,79399,-11342},
		{1,53,0,655360,4128768,65536,79399,-11342},
		{1,53,0,720896,4128768,131072,79399,-11342},
		{2,52,0,786432,4128768,196608,80956,-11565},
		{2,52,0,851968,4128768,262144,80956,-11565},
		{2,52,0,917504,4128768,327680,80956,-11565},
		{2,53,0,1048576,4128768,393216,79399,-12603},
		{2,53,0,1114112,4128768,524288,79399,-11342},
		{2,53,0,1179648,4128768,589824,79399,-11342},
		{3,52,0,1245184,4128768,655360,80956,-11565},
		{3,52,0,1310720,4128768,720896,80956,-11565},
		{3,53,0,1376256,4128768,786432,79399,-11342},
		{3,53,0,1441792,4128768,851968,79399,-11342},
		{3,53,0,1572864,4128768,917504,79399,-12603},
		{4,52,0,1638400,4128768,1048576,80956,-11565},
		{4,52,0,1703936,4128768,1114112,80956,-11565},
		{4,52,0,1769472,4128768,1179648,80956,-11565},
		{4,53,0,1835008,4128768,1245184,79399,-11342},
		{4,53,0,1900544,4128768,1310720,79399,-11342},
		{4,53,0,1966080,4128768,1376256,79399,-11342},
		{5,52,0,2097152,4128768,1441792,80956,-12850},
		{5,52,0,2162688,4128768,1572864,80956,-11565},
		{5,53,0,2228224,4128768,1638400,79399,-11342},
		{5,53,0,2293760,4128768,1703936,79399,-11342},
		{5,53,0,2359296,4128768,1769472,79399,-11342},
		{6,52,0,2424832,4128768,1835008,80956,-11565},
		{6,52,0,2490368,4128768,1900544,80956,-11565},
		{6,52,0,2621440,4128768,1966080,80956,-12850},
		{6,53,0,2686976,4128768,2097152,79399,-11342},
		{6,53,0,2752512,4128768,2162688,79399,-11342},
		{6,53,0,2818048,4128768,2228224,79399,-11342},
		{7,52,0,2883584,4128768,2293760,80956,-11565},
		{7,52,0,2949120,4128768,2359296,80956,-11565},
		{7,53,0,3014656,4128768,2424832,79399,-11342},
		{7,53,0,3145728,4128768,2490368,79399,-12603},
		{7,53,0,3211264,4128768,2621440,79399,-11342},
		{8,52,0,3276800,4128768,2686976,80956,-11565},
		{8,52,0,3342336,4128768,2752512,80956,-11565},
		{8,52,0,3407872,4128768,2818048,80956,-11565},
		{8,53,0,3473408,4128768,2883584,79399,-11342},
		{8,53,0,3538944,4128768,2949120,79399,-11342},
		{8,53,0,3670016,4128768,3014656,79399,-12603},
		{9,52,0,3735552,4128768,3145728,80956,-11565},
		{9,52,0,3801088,4128768,3211264,80956,-11565},
		{9,53,0,3866624,4128768,3276800,79399,-11342},
		{9,53,0,3932160,4128768,3342336,79399,-11342},
		{9,53,0,3997696,4128768,3407872,79399,-11342},
		{10,52,0,4128768,4128768,3473408,80956,-12850},
		{16,46,458752,4128768,4128768,3538944,81555,-13107},
		{23,39,983040,4128768,4128768,3670016,82782,-12072},
		{29,34,1507328,4128768,4128768,3735552,79437,-11915},
		{36,27,2031616,4128768,4128768,3801088,80659,-12603},
		{43,20,2555904,4128768,4128768,3866624,82782,-13797},
		{49,14,3080192,4128768,4128768,3932160,80659,-15123},
		{56,7,3604480,4128768,4128768,3997696,87381,-21845},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 47 */
		{0,1,0,0,0,0,0,0},
		{0,8,0,65536,589824,0,84260,-9362},
		{0,16,0,131072,1179648,0,78643,-8738},
		{0,24,0,196608,1769472,0,76933,-8548},
		{0,31,0,262144,2359296,0,78643,-8738},
		{0,39,0,327680,2949120,0,77608,-8623},
		{0,47,0,393216,3538944,0,76933,-8548},
		{1,54,0,458752,4128768,0,77901,-8655},
		{1,54,0,589824,4128768,65536,77901,-9892},
		{1,54,0,655360,4128768,131072,77901,-9892},
		{1,54,0,720896,4128768,196608,77901,-9892},
		{1,54,0,786432,4128768,262144,77901,-9892},
		{1,54,0,851968,4128768,327680,77901,-9892},
		{2,53,0,917504,4128768,393216,79399,-10082},
		{2,54,0,983040,4128768,458752,77901,-9892},
		{2,54,0,1048576,4128768,589824,77901,-8655},
		{2,54,0,1179648,4128768,655360,77901,-9892},
		{2,54,0,1245184,4128768,720896,77901,-9892},
		{2,54,0,1310720,4128768,786432,77901,-9892},
		{3,53,0,1376256,4128768,851968,79399,-10082},
		{3,54,0,1441792,4128768,917504,77901,-9892},
		{3,54,0,1507328,4128768,983040,77901,-9892},
		{3,54,0,1572864,4128768,1048576,77901,-9892},
		{3,54,0,1638400,4128768,1179648,77901,-8655},
		{3,54,0,1769472,4128768,1245184,77901,-9892},
		{4,53,0,1835008,4128768,1310720,79399,-10082},
		{4,54,0,1900544,4128768,1376256,77901,-9892},
		{4,54,0,1966080,4128768,1441792,77901,-9892},
		{4,54,0,2031616,4128768,1507328,77901,-9892},
		{4,54,0,2097152,4128768,1572864,77901,-9892},
		{4,54,0,2162688,4128768,1638400,77901,-9892},
		{4,54,0,2228224,4128768,1769472,77901,-8655},
		{5,54,0,2359296,4128768,1835008,77901,-9892},
		{5,54,0,2424832,4128768,1900544,77901,-9892},
		{5,54,0,2490368,4128768,1966080,77901,-9892},
		{5,54,0,2555904,4128768,2031616,77901,-9892},
		{5,54,0,2621440,4128768,2097152,77901,-9892},
		{5,54,0,2686976,4128768,2162688,77901,-9892},
		{6,53,0,2752512,4128768,2228224,79399,-10082},
		{6,54,0,2818048,4128768,2359296,77901,-8655},
		{6,54,0,2949120,4128768,2424832,77901,-9892},
		{6,54,0,3014656,4128768,2490368,77901,-9892},
		{6,54,0,3080192,4128768,2555904,77901,-9892},
		{6,54,0,3145728,4128768,2621440,77901,-9892},
		{7,53,0,3211264,4128768,2686976,79399,-10082},
		{7,54,0,3276800,4128768,2752512,77901,-9892},
		{7,54,0,3342336,4128768,2818048,77901,-9892},
		{7,54,0,3407872,4128768,2949120,77901,-8655},
		{7,54,0,3538944,4128768,3014656,77901,-9892},
		{7,54,0,3604480,4128768,3080192,77901,-9892},
		{8,53,0,3670016,4128768,3145728,79399,-10082},
		{8,54,0,3735552,4128768,3211264,77901,-9892},
		{8,54,0,3801088,4128768,3276800,77901,-9892},
		{8,54,0,3866624,4128768,3342336,77901,-9892},
		{8,54,0,3932160,4128768,3407872,77901,-9892},
		{8,54,0,3997696,4128768,3538944,77901,-8655},
		{9,53,0,4128768,4128768,3604480,79399,-10082},
		{16,47,589824,4128768,4128768,3670016,76933,-9972},
		{24,39,1179648,4128768,4128768,3735552,77608,-10347},
		{32,31,1769472,4128768,4128768,3801088,78643,-10922},
		{39,24,2359296,4128768,4128768,3866624,76933,-11397},
		{47,16,2949120,4128768,4128768,3932160,78643,-13107},
		{55,8,3538944,4128768,4128768,3997696,84260,-18724},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 48 */
		{0,1,0,0,0,0,0,0},
		{0,10,0,65536,655360,0,72817,-7281},
		{0,19,0,131072,1376256,0,76458,-7281},
		{0,28,0,196608,2031616,0,75245,-7281},
		{0,37,0,262144,2752512,0,76458,-7281},
		{0,46,0,327680,3407872,0,75730,-7281},
		{0,56,0,393216,4128768,0,75068,-7149},
		{0,56,0,458752,4128768,65536,75068,-7149},
		{1,55,0,524288,4128768,131072,76458,-7281},
		{1,55,0,589824,4128768,196608,76458,-7281},
		{1,55,0,720896,4128768,262144,76458,-8495},
		{1,55,0,786432,4128768,327680,76458,-8495},
		{1,55,0,851968,4128768,393216,76458,-8495},
		{1,55,0,917504,4128768,458752,76458,-8495},
		{1,56,0,983040,4128768,524288,75068,-8340},
		{2,55,0,1048576,4128768,589824,76458,-8495},
		{2,55,0,1114112,4128768,720896,76458,-7281},
		{2,55,0,1179648,4128768,786432,76458,-7281},
		{2,55,0,1245184,4128768,851968,76458,-7281},
		{2,55,0,1376256,4128768,917504,76458,-8495},
		{2,55,0,1441792,4128768,983040,76458,-8495},
		{2,56,0,1507328,4128768,1048576,75068,-8340},
		{3,55,0,1572864,4128768,1114112,76458,-8495},
		{3,55,0,1638400,4128768,1179648,76458,-8495},
		{3,55,0,1703936,4128768,1245184,76458,-8495},
		{3,55,0,1769472,4128768,1376256,76458,-7281},
		{3,55,0,1835008,4128768,1441792,76458,-7281},
		{3,55,0,1900544,4128768,1507328,76458,-7281},
		{3,56,0,1966080,4128768,1572864,75068,-7149},
		{4,55,0,2097152,4128768,1638400,76458,-8495},
		{4,55,0,2162688,4128768,1703936,76458,-8495},
		{4,55,0,2228224,4128768,1769472,76458,-8495},
		{4,55,0,2293760,4128768,1835008,76458,-8495},
		{4,55,0,2359296,4128768,1900544,76458,-8495},
		{4,55,0,2424832,4128768,1966080,76458,-8495},
		{4,56,0,2490368,4128768,2097152,75068,-7149},
		{5,55,0,2555904,4128768,2162688,76458,-7281},
		{5,55,0,2621440,4128768,2228224,76458,-7281},
		{5,55,0,2752512,4128768,2293760,76458,-8495},
		{5,55,0,2818048,4128768,2359296,76458,-8495},
		{5,55,0,2883584,4128768,2424832,76458,-8495},
		{5,55,0,2949120,4128768,2490368,76458,-8495},
		{5,56,0,3014656,4128768,2555904,75068,-8340},
		{6,55,0,3080192,4128768,2621440,76458,-8495},
		{6,55,0,3145728,4128768,2752512,76458,-7281},
		{6,55,0,3211264,4128768,2818048,76458,-7281},
		{6,55,0,3276800,4128768,2883584,76458,-7281},
		{6,55,0,3342336,4128768,2949120,76458,-7281},
		{6,55,0,3473408,4128768,3014656,76458,-8495},
		{6,56,0,3538944,4128768,3080192,75068,-8340},
		{7,55,0,3604480,4128768,3145728,76458,-8495},
		{7,55,0,3670016,4128768,3211264,76458,-8495},
		{7,55,0,3735552,4128768,3276800,76458,-8495},
		{7,55,0,3801088,4128768,3342336,76458,-8495},
		{7,55,0,3866624,4128768,3473408,76458,-7281},
		{7,55,0,3932160,4128768,3538944,76458,-7281},
		{7,56,0,3997696,4128768,3604480,75068,-7149},
		{8,55,0,4128768,4128768,3670016,76458,-8495},
		{17,46,655360,4128768,4128768,3735552,77186,-8738},
		{26,37,1376256,4128768,4128768,3801088,76458,-9102},
		{35,28,2031616,4128768,4128768,3866624,77672,-9709},
		{44,19,2752512,4128768,4128768,3932160,76458,-10922},
		{53,10,3407872,4128768,4128768,3997696,80099,-14563},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 49 */
		{0,1,0,0,0,0,0,0},
		{0,12,0,65536,786432,0,71493,-5957},
		{0,23,0,131072,1638400,0,74472,-5957},
		{0,34,0,196608,2424832,0,73479,-5957},
		{0,45,0,262144,3276800,0,74472,-5957},
		{0,57,0,327680,4128768,0,73728,-5851},
		{0,57,0,393216,4128768,65536,73728,-5851},
		{0,57,0,458752,4128768,131072,73728,-5851},
		{0,57,0,524288,4128768,196608,73728,-5851},
		{1,56,0,589824,4128768,262144,75068,-5957},
		{1,56,0,655360,4128768,327680,75068,-5957},
		{1,56,0,720896,4128768,393216,75068,-5957},
		{1,56,0,851968,4128768,458752,75068,-7149},
		{1,56,0,917504,4128768,524288,75068,-7149},
		{1,57,0,983040,4128768,589824,73728,-7021},
		{1,57,0,1048576,4128768,655360,73728,-7021},
		{1,57,0,1114112,4128768,720896,73728,-7021},
		{2,56,0,1179648,4128768,851968,75068,-5957},
		{2,56,0,1245184,4128768,917504,75068,-5957},
		{2,56,0,1310720,4128768,983040,75068,-5957},
		{2,56,0,1376256,4128768,1048576,75068,-5957},
		{2,56,0,1441792,4128768,1114112,75068,-5957},
		{2,57,0,1507328,4128768,1179648,73728,-5851},
		{2,57,0,1572864,4128768,1245184,73728,-5851},
		{2,57,0,1703936,4128768,1310720,73728,-7021},
		{3,56,0,1769472,4128768,1376256,75068,-7149},
		{3,56,0,1835008,4128768,1441792,75068,-7149},
		{3,56,0,1900544,4128768,1507328,75068,-7149},
		{3,56,0,1966080,4128768,1572864,75068,-7149},
		{3,56,0,2031616,4128768,1703936,75068,-5957},
		{3,57,0,2097152,4128768,1769472,73728,-5851},
		{3,57,0,2162688,4128768,1835008,73728,-5851},
		{3,57,0,2228224,4128768,1900544,73728,-5851},
		{3,57,0,2293760,4128768,1966080,73728,-5851},
		{4,56,0,2359296,4128768,2031616,75068,-5957},
		{4,56,0,2490368,4128768,2097152,75068,-7149},
		{4,56,0,2555904,4128768,2162688,75068,-7149},
		{4,56,0,2621440,4128768,2228224,75068,-7149},
		{4,56,0,2686976,4128768,2293760,75068,-7149},
		{4,57,0,2752512,4128768,2359296,73728,-7021},
		{4,57,0,2818048,4128768,2490368,73728,-5851},
		{4,57,0,2883584,4128768,2555904,73728,-5851},
		{5,56,0,2949120,4128768,2621440,75068,-5957},
		{5,56,0,3014656,4128768,2686976,75068,-5957},
		{5,56,0,3080192,4128768,2752512,75068,-5957},
		{5,56,0,3145728,4128768,2818048,75068,-5957},
		{5,56,0,3211264,4128768,2883584,75068,-5957},
		{5,57,0,3342336,4128768,2949120,73728,-7021},
		{5,57,0,3407872,4128768,3014656,73728,-7021},
		{5,57,0,3473408,4128768,3080192,73728,-7021},
		{6,56,0,3538944,4128768,3145728,75068,-7149},
		{6,56,0,3604480,4128768,3211264,75068,-7149},
		{6,56,0,3670016,4128768,3342336,75068,-5957},
		{6,56,0,3735552,4128768,3407872,75068,-5957},
		{6,56,0,3801088,4128768,3473408,75068,-5957},
		{6,57,0,3866624,4128768,3538944,73728,-5851},
		{6,57,0,3932160,4128768,3604480,73728,-5851},
		{6,57,0,3997696,4128768,3670016,73728,-5851},
		{7,56,0,4128768,4128768,3735552,75068,-7149},
		{18,45,786432,4128768,4128768,3801088,75962,-7447},
		{29,34,1638400,4128768,4128768,3866624,75465,-7943},
		{40,23,2424832,4128768,4128768,3932160,77451,-8936},
		{51,12,3276800,4128768,4128768,3997696,77451,-11915},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 50 */
		{0,1,0,0,0,0,0,0},
		{0,12,0,65536,786432,0,71493,-5957},
		{0,24,0,131072,1638400,0,71234,-5698},
		{0,35,0,196608,2424832,0,71318,-5782},
		{0,47,0,262144,3276800,0,71234,-5698},
		{0,59,0,327680,4128768,0,71185,-5649},
		{0,59,0,393216,4128768,65536,71185,-5649},
		{0,59,0,458752,4128768,131072,71185,-5649},
		{0,59,0,524288,4128768,196608,71185,-5649},
		{0,59,0,589824,4128768,262144,71185,-5649},
		{0,59,0,655360,4128768,327680,71185,-5649},
		{0,59,0,720896,4128768,393216,71185,-5649},
		{1,58,0,851968,4128768,458752,72434,-6898},
		{1,58,0,917504,4128768,524288,72434,-6898},
		{1,58,0,983040,4128768,589824,72434,-6898},
		{1,58,0,1048576,4128768,655360,72434,-6898},
		{1,58,0,1114112,4128768,720896,72434,-6898},
		{1,59,0,1179648,4128768,851968,71185,-5649},
		{1,59,0,1245184,4128768,917504,71185,-5649},
		{1,59,0,1310720,4128768,983040,71185,-5649},
		{1,59,0,1376256,4128768,1048576,71185,-5649},
		{1,59,0,1441792,4128768,1114112,71185,-5649},
		{1,59,0,1507328,4128768,1179648,71185,-5649},
		{1,59,0,1572864,4128768,1245184,71185,-5649},
		{2,58,0,1703936,4128768,1310720,72434,-6898},
		{2,58,0,1769472,4128768,1376256,72434,-6898},
		{2,58,0,1835008,4128768,1441792,72434,-6898},
		{2,58,0,1900544,4128768,1507328,72434,-6898},
		{2,58,0,1966080,4128768,1572864,72434,-6898},
		{2,59,0,2031616,4128768,1703936,71185,-5649},
		{2,59,0,2097152,4128768,1769472,71185,-5649},
		{2,59,0,2162688,4128768,1835008,71185,-5649},
		{2,59,0,2228224,4128768,1900544,71185,-5649},
		{2,59,0,2293760,4128768,1966080,71185,-5649},
		{2,59,0,2359296,4128768,2031616,71185,-5649},
		{3,58,0,2490368,4128768,2097152,72434,-6898},
		{3,58,0,2555904,4128768,2162688,72434,-6898},
		{3,58,0,2621440,4128768,2228224,72434,-6898},
		{3,58,0,2686976,4128768,2293760,72434,-6898},
		{3,58,0,2752512,4128768,2359296,72434,-6898},
		{3,59,0,2818048,4128768,2490368,71185,-5649},
		{3,59,0,2883584,4128768,2555904,71185,-5649},
		{3,59,0,2949120,4128768,2621440,71185,-5649},
		{3,59,0,3014656,4128768,2686976,71185,-5649},
		{3,59,0,3080192,4128768,2752512,71185,-5649},
		{3,59,0,3145728,4128768,2818048,71185,-5649},
		{3,59,0,3211264,4128768,2883584,71185,-5649},
		{4,58,0,3342336,4128768,2949120,72434,-6898},
		{4,58,0,3407872,4128768,3014656,72434,-6898},
		{4,58,0,3473408,4128768,3080192,72434,-6898},
		{4,58,0,3538944,4128768,3145728,72434,-6898},
		{4,58,0,3604480,4128768,3211264,72434,-6898},
		{4,59,0,3670016,4128768,3342336,71185,-5649},
		{4,59,0,3735552,4128768,3407872,71185,-5649},
		{4,59,0,3801088,4128768,3473408,71185,-5649},
		{4,59,0,3866624,4128768,3538944,71185,-5649},
		{4,59,0,3932160,4128768,3604480,71185,-5649},
		{4,59,0,3997696,4128768,3670016,71185,-5649},
		{5,58,0,4128768,4128768,3735552,72434,-6898},
		{16,47,786432,4128768,4128768,3801088,72659,-7123},
		{28,35,1638400,4128768,4128768,3866624,73246,-7710},
		{39,24,2424832,4128768,4128768,3932160,74084,-8548},
		{51,12,3276800,4128768,4128768,3997696,77451,-11915},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 51 */
		{0,1,0,0,0,0,0,0},
		{0,15,0,65536,983040,0,70217,-4681},
		{0,30,0,131072,2031616,0,70055,-4519},
		{0,45,0,196608,3080192,0,70004,-4468},
		{0,60,0,262144,4128768,0,69979,-4443},
		{0,60,0,327680,4128768,65536,69979,-4443},
		{0,60,0,393216,4128768,131072,69979,-4443},
		{0,60,0,458752,4128768,196608,69979,-4443},
		{0,60,0,524288,4128768,262144,69979,-4443},
		{0,60,0,589824,4128768,327680,69979,-4443},
		{0,60,0,655360,4128768,393216,69979,-4443},
		{0,60,0,720896,4128768,458752,69979,-4443},
		{0,60,0,786432,4128768,524288,69979,-4443},
		{0,60,0,851968,4128768,589824,69979,-4443},
		{0,60,0,917504,4128768,655360,69979,-4443},
		{1,59,0,1048576,4128768,720896,71185,-5649},
		{1,59,0,1114112,4128768,786432,71185,-5649},
		{1,59,0,1179648,4128768,851968,71185,-5649},
		{1,59,0,1245184,4128768,917504,71185,-5649},
		{1,60,0,1310720,4128768,1048576,69979,-4443},
		{1,60,0,1376256,4128768,1114112,69979,-4443},
		{1,60,0,1441792,4128768,1179648,69979,-4443},
		{1,60,0,1507328,4128768,1245184,69979,-4443},
		{1,60,0,1572864,4128768,1310720,69979,-4443},
		{1,60,0,1638400,4128768,1376256,69979,-4443},
		{1,60,0,1703936,4128768,1441792,69979,-4443},
		{1,60,0,1769472,4128768,1507328,69979,-4443},
		{1,60,0,1835008,4128768,1572864,69979,-4443},
		{1,60,0,1900544,4128768,1638400,69979,-4443},
		{1,60,0,1966080,4128768,1703936,69979,-4443},
		{2,59,0,2097152,4128768,1769472,71185,-5649},
		{2,59,0,2162688,4128768,1835008,71185,-5649},
		{2,59,0,2228224,4128768,1900544,71185,-5649},
		{2,59,0,2293760,4128768,1966080,71185,-5649},
		{2,60,0,2359296,4128768,2097152,69979,-4443},
		{2,60,0,2424832,4128768,2162688,69979,-4443},
		{2,60,0,2490368,4128768,2228224,69979,-4443},
		{2,60,0,2555904,4128768,2293760,69979,-4443},
		{2,60,0,2621440,4128768,2359296,69979,-4443},
		{2,60,0,2686976,4128768,2424832,69979,-4443},
		{2,60,0,2752512,4128768,2490368,69979,-4443},
		{2,60,0,2818048,4128768,2555904,69979,-4443},
		{2,60,0,2883584,4128768,2621440,69979,-4443},
		{2,60,0,2949120,4128768,2686976,69979,-4443},
		{2,60,0,3014656,4128768,2752512,69979,-4443},
		{3,59,0,3145728,4128768,2818048,71185,-5649},
		{3,59,0,3211264,4128768,2883584,71185,-5649},
		{3,59,0,3276800,4128768,2949120,71185,-5649},
		{3,59,0,3342336,4128768,3014656,71185,-5649},
		{3,60,0,3407872,4128768,3145728,69979,-4443},
		{3,60,0,3473408,4128768,3211264,69979,-4443},
		{3,60,0,3538944,4128768,3276800,69979,-4443},
		{3,60,0,3604480,4128768,3342336,69979,-4443},
		{3,60,0,3670016,4128768,3407872,69979,-4443},
		{3,60,0,3735552,4128768,3473408,69979,-4443},
		{3,60,0,3801088,4128768,3538944,69979,-4443},
		{3,60,0,3866624,4128768,3604480,69979,-4443},
		{3,60,0,3932160,4128768,3670016,69979,-4443},
		{3,60,0,3997696,4128768,3735552,69979,-4443},
		{4,59,0,4128768,4128768,3801088,71185,-5649},
		{18,45,983040,4128768,4128768,3866624,71493,-5957},
		{33,30,2031616,4128768,4128768,3932160,72315,-6779},
		{48,15,3080192,4128768,4128768,3997696,74898,-9362},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 52 */
		{0,1,0,0,0,0,0,0},
		{0,21,0,65536,1376256,0,68812,-3276},
		{0,41,0,131072,2752512,0,68812,-3276},
		{0,61,0,196608,4128768,0,68812,-3276},
		{0,61,0,262144,4128768,65536,68812,-3276},
		{0,61,0,327680,4128768,131072,68812,-3276},
		{0,61,0,393216,4128768,196608,68812,-3276},
		{0,61,0,458752,4128768,262144,68812,-3276},
		{0,61,0,524288,4128768,327680,68812,-3276},
		{0,61,0,589824,4128768,393216,68812,-3276},
		{0,61,0,655360,4128768,458752,68812,-3276},
		{0,61,0,720896,4128768,524288,68812,-3276},
		{0,61,0,786432,4128768,589824,68812,-3276},
		{0,61,0,851968,4128768,655360,68812,-3276},
		{0,61,0,917504,4128768,720896,68812,-3276},
		{0,61,0,983040,4128768,786432,68812,-3276},
		{0,61,0,1048576,4128768,851968,68812,-3276},
		{0,61,0,1114112,4128768,917504,68812,-3276},
		{0,61,0,1179648,4128768,983040,68812,-3276},
		{0,61,0,1245184,4128768,1048576,68812,-3276},
		{1,60,0,1376256,4128768,1114112,69979,-4443},
		{1,60,0,1441792,4128768,1179648,69979,-4443},
		{1,60,0,1507328,4128768,1245184,69979,-4443},
		{1,61,0,1572864,4128768,1376256,68812,-3276},
		{1,61,0,1638400,4128768,1441792,68812,-3276},
		{1,61,0,1703936,4128768,1507328,68812,-3276},
		{1,61,0,1769472,4128768,1572864,68812,-3276},
		{1,61,0,1835008,4128768,1638400,68812,-3276},
		{1,61,0,1900544,4128768,1703936,68812,-3276},
		{1,61,0,1966080,4128768,1769472,68812,-3276},
		{1,61,0,2031616,4128768,1835008,68812,-3276},
		{1,61,0,2097152,4128768,1900544,68812,-3276},
		{1,61,0,2162688,4128768,1966080,68812,-3276},
		{1,61,0,2228224,4128768,2031616,68812,-3276},
		{1,61,0,2293760,4128768,2097152,68812,-3276},
		{1,61,0,2359296,4128768,2162688,68812,-3276},
		{1,61,0,2424832,4128768,2228224,68812,-3276},
		{1,61,0,2490368,4128768,2293760,68812,-3276},
		{1,61,0,2555904,4128768,2359296,68812,-3276},
		{1,61,0,2621440,4128768,2424832,68812,-3276},
		{2,60,0,2752512,4128768,2490368,69979,-4443},
		{2,60,0,2818048,4128768,2555904,69979,-4443},
		{2,60,0,2883584,4128768,2621440,69979,-4443},
		{2,61,0,2949120,4128768,2752512,68812,-3276},
		{2,61,0,3014656,4128768,2818048,68812,-3276},
		{2,61,0,3080192,4128768,2883584,68812,-3276},
		{2,61,0,3145728,4128768,2949120,68812,-3276},
		{2,61,0,3211264,4128768,3014656,68812,-3276},
		{2,61,0,3276800,4128768,3080192,68812,-3276},
		{2,61,0,3342336,4128768,3145728,68812,-3276},
		{2,61,0,3407872,4128768,3211264,68812,-3276},
		{2,61,0,3473408,4128768,3276800,68812,-3276},
		{2,61,0,3538944,4128768,3342336,68812,-3276},
		{2,61,0,3604480,4128768,3407872,68812,-3276},
		{2,61,0,3670016,4128768,3473408,68812,-3276},
		{2,61,0,3735552,4128768,3538944,68812,-3276},
		{2,61,0,3801088,4128768,3604480,68812,-3276},
		{2,61,0,3866624,4128768,3670016,68812,-3276},
		{2,61,0,3932160,4128768,3735552,68812,-3276},
		{2,61,0,3997696,4128768,3801088,68812,-3276},
		{3,60,0,4128768,4128768,3866624,69979,-4443},
		{23,40,1376256,4128768,4128768,3932160,70577,-5041},
		{43,20,2752512,4128768,4128768,3997696,72434,-6898},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 53 */
		{0,1,0,0,0,0,0,0},
		{0,21,0,65536,1376256,0,68812,-3276},
		{0,41,0,131072,2752512,0,68812,-3276},
		{0,61,0,196608,4128768,0,68812,-3276},
		{0,61,0,262144,4128768,65536,68812,-3276},
		{0,61,0,327680,4128768,131072,68812,-3276},
		{0,61,0,393216,4128768,196608,68812,-3276},
		{0,61,0,458752,4128768,262144,68812,-3276},
		{0,61,0,524288,4128768,327680,68812,-3276},
		{0,61,0,589824,4128768,393216,68812,-3276},
		{0,61,0,655360,4128768,458752,68812,-3276},
		{0,61,0,720896,4128768,524288,68812,-3276},
		{0,61,0,786432,4128768,589824,68812,-3276},
		{0,61,0,851968,4128768,655360,68812,-3276},
		{0,61,0,917504,4128768,720896,68812,-3276},
		{0,61,0,983040,4128768,786432,68812,-3276},
		{0,61,0,1048576,4128768,851968,68812,-3276},
		{0,61,0,1114112,4128768,917504,68812,-3276},
		{0,61,0,1179648,4128768,983040,68812,-3276},
		{0,61,0,1245184,4128768,1048576,68812,-3276},
		{1,60,0,1376256,4128768,1114112,69979,-4443},
		{1,60,0,1441792,4128768,1179648,69979,-4443},
		{1,60,0,1507328,4128768,1245184,69979,-4443},
		{1,61,0,1572864,4128768,1376256,68812,-3276},
		{1,61,0,1638400,4128768,1441792,68812,-3276},
		{1,61,0,1703936,4128768,1507328,68812,-3276},
		{1,61,0,1769472,4128768,1572864,68812,-3276},
		{1,61,0,1835008,4128768,1638400,68812,-3276},
		{1,61,0,1900544,4128768,1703936,68812,-3276},
		{1,61,0,1966080,4128768,1769472,68812,-3276},
		{1,61,0,2031616,4128768,1835008,68812,-3276},
		{1,61,0,2097152,4128768,1900544,68812,-3276},
		{1,61,0,2162688,4128768,1966080,68812,-3276},
		{1,61,0,2228224,4128768,2031616,68812,-3276},
		{1,61,0,2293760,4128768,2097152,68812,-3276},
		{1,61,0,2359296,4128768,2162688,68812,-3276},
		{1,61,0,2424832,4128768,2228224,68812,-3276},
		{1,61,0,2490368,4128768,2293760,68812,-3276},
		{1,61,0,2555904,4128768,2359296,68812,-3276},
		{1,61,0,2621440,4128768,2424832,68812,-3276},
		{2,60,0,2752512,4128768,2490368,69979,-4443},
		{2,60,0,2818048,4128768,2555904,69979,-4443},
		{2,60,0,2883584,4128768,2621440,69979,-4443},
		{2,61,0,2949120,4128768,2752512,68812,-3276},
		{2,61,0,3014656,4128768,2818048,68812,-3276},
		{2,61,0,3080192,4128768,2883584,68812,-3276},
		{2,61,0,3145728,4128768,2949120,68812,-3276},
		{2,61,0,3211264,4128768,3014656,68812,-3276},
		{2,61,0,3276800,4128768,3080192,68812,-3276},
		{2,61,0,3342336,4128768,3145728,68812,-3276},
		{2,61,0,3407872,4128768,3211264,68812,-3276},
		{2,61,0,3473408,4128768,3276800,68812,-3276},
		{2,61,0,3538944,4128768,3342336,68812,-3276},
		{2,61,0,3604480,4128768,3407872,68812,-3276},
		{2,61,0,3670016,4128768,3473408,68812,-3276},
		{2,61,0,3735552,4128768,3538944,68812,-3276},
		{2,61,0,3801088,4128768,3604480,68812,-3276},
		{2,61,0,3866624,4128768,3670016,68812,-3276},
		{2,61,0,3932160,4128768,3735552,68812,-3276},
		{2,61,0,3997696,4128768,3801088,68812,-3276},
		{3,60,0,4128768,4128768,3866624,69979,-4443},
		{23,40,1376256,4128768,4128768,3932160,70577,-5041},
		{43,20,2752512,4128768,4128768,3997696,72434,-6898},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 54 */
		{0,1,0,0,0,0,0,0},
		{0,31,0,65536,2031616,0,67720,-2184},
		{0,62,0,131072,4128768,0,67684,-2148},
		{0,62,0,196608,4128768,65536,67684,-2148},
		{0,62,0,262144,4128768,131072,67684,-2148},
		{0,62,0,327680,4128768,196608,67684,-2148},
		{0,62,0,393216,4128768,262144,67684,-2148},
		{0,62,0,458752,4128768,327680,67684,-2148},
		{0,62,0,524288,4128768,393216,67684,-2148},
		{0,62,0,589824,4128768,458752,67684,-2148},
		{0,62,0,655360,4128768,524288,67684,-2148},
		{0,62,0,720896,4128768,589824,67684,-2148},
		{0,62,0,786432,4128768,655360,67684,-2148},
		{0,62,0,851968,4128768,720896,67684,-2148},
		{0,62,0,917504,4128768,786432,67684,-2148},
		{0,62,0,983040,4128768,851968,67684,-2148},
		{0,62,0,1048576,4128768,917504,67684,-2148},
		{0,62,0,1114112,4128768,983040,67684,-2148},
		{0,62,0,1179648,4128768,1048576,67684,-2148},
		{0,62,0,1245184,4128768,1114112,67684,-2148},
		{0,62,0,1310720,4128768,1179648,67684,-2148},
		{0,62,0,1376256,4128768,1245184,67684,-2148},
		{0,62,0,1441792,4128768,1310720,67684,-2148},
		{0,62,0,1507328,4128768,1376256,67684,-2148},
		{0,62,0,1572864,4128768,1441792,67684,-2148},
		{0,62,0,1638400,4128768,1507328,67684,-2148},
		{0,62,0,1703936,4128768,1572864,67684,-2148},
		{0,62,0,1769472,4128768,1638400,67684,-2148},
		{0,62,0,1835008,4128768,1703936,67684,-2148},
		{0,62,0,1900544,4128768,1769472,67684,-2148},
		{0,62,0,1966080,4128768,1835008,67684,-2148},
		{1,61,0,2097152,4128768,1900544,68812,-3276},
		{1,61,0,2162688,4128768,1966080,68812,-3276},
		{1,62,0,2228224,4128768,2097152,67684,-2148},
		{1,62,0,2293760,4128768,2162688,67684,-2148},
		{1,62,0,2359296,4128768,2228224,67684,-2148},
		{1,62,0,2424832,4128768,2293760,67684,-2148},
		{1,62,0,2490368,4128768,2359296,67684,-2148},
		{1,62,0,2555904,4128768,2424832,67684,-2148},
		{1,62,0,2621440,4128768,2490368,67684,-2148},
		{1,62,0,2686976,4128768,2555904,67684,-2148},
		{1,62,0,2752512,4128768,2621440,67684,-2148},
		{1,62,0,2818048,4128768,2686976,67684,-2148},
		{1,62,0,2883584,4128768,2752512,67684,-2148},
		{1,62,0,2949120,4128768,2818048,67684,-2148},
		{1,62,0,3014656,4128768,2883584,67684,-2148},
		{1,62,0,3080192,4128768,2949120,67684,-2148},
		{1,62,0,3145728,4128768,3014656,67684,-2148},
		{1,62,0,3211264,4128768,3080192,67684,-2148},
		{1,62,0,3276800,4128768,3145728,67684,-2148},
		{1,62,0,3342336,4128768,3211264,67684,-2148},
		{1,62,0,3407872,4128768,3276800,67684,-2148},
		{1,62,0,3473408,4128768,3342336,67684,-2148},
		{1,62,0,3538944,4128768,3407872,67684,-2148},
		{1,62,0,3604480,4128768,3473408,67684,-2148},
		{1,62,0,3670016,4128768,3538944,67684,-2148},
		{1,62,0,3735552,4128768,3604480,67684,-2148},
		{1,62,0,3801088,4128768,3670016,67684,-2148},
		{1,62,0,3866624,4128768,3735552,67684,-2148},
		{1,62,0,3932160,4128768,3801088,67684,-2148},
		{1,62,0,3997696,4128768,3866624,67684,-2148},
		{2,61,0,4128768,4128768,3932160,68812,-3276},
		{32,31,2031616,4128768,4128768,3997696,69905,-4369},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 55 */
		{0,1,0,0,0,0,0,0},
		{0,31,0,65536,2031616,0,67720,-2184},
		{0,62,0,131072,4128768,0,67684,-2148},
		{0,62,0,196608,4128768,65536,67684,-2148},
		{0,62,0,262144,4128768,131072,67684,-2148},
		{0,62,0,327680,4128768,196608,67684,-2148},
		{0,62,0,393216,4128768,262144,67684,-2148},
		{0,62,0,458752,4128768,327680,67684,-2148},
		{0,62,0,524288,4128768,393216,67684,-2148},
		{0,62,0,589824,4128768,458752,67684,-2148},
		{0,62,0,655360,4128768,524288,67684,-2148},
		{0,62,0,720896,4128768,589824,67684,-2148},
		{0,62,0,786432,4128768,655360,67684,-2148},
		{0,62,0,851968,4128768,720896,67684,-2148},
		{0,62,0,917504,4128768,786432,67684,-2148},
		{0,62,0,983040,4128768,851968,67684,-2148},
		{0,62,0,1048576,4128768,917504,67684,-2148},
		{0,62,0,1114112,4128768,983040,67684,-2148},
		{0,62,0,1179648,4128768,1048576,67684,-2148},
		{0,62,0,1245184,4128768,1114112,67684,-2148},
		{0,62,0,1310720,4128768,1179648,67684,-2148},
		{0,62,0,1376256,4128768,1245184,67684,-2148},
		{0,62,0,1441792,4128768,1310720,67684,-2148},
		{0,62,0,1507328,4128768,1376256,67684,-2148},
		{0,62,0,1572864,4128768,1441792,67684,-2148},
		{0,62,0,1638400,4128768,1507328,67684,-2148},
		{0,62,0,1703936,4128768,1572864,67684,-2148},
		{0,62,0,1769472,4128768,1638400,67684,-2148},
		{0,62,0,1835008,4128768,1703936,67684,-2148},
		{0,62,0,1900544,4128768,1769472,67684,-2148},
		{0,62,0,1966080,4128768,1835008,67684,-2148},
		{1,61,0,2097152,4128768,1900544,68812,-3276},
		{1,61,0,2162688,4128768,1966080,68812,-3276},
		{1,62,0,2228224,4128768,2097152,67684,-2148},
		{1,62,0,2293760,4128768,2162688,67684,-2148},
		{1,62,0,2359296,4128768,2228224,67684,-2148},
		{1,62,0,2424832,4128768,2293760,67684,-2148},
		{1,62,0,2490368,4128768,2359296,67684,-2148},
		{1,62,0,2555904,4128768,2424832,67684,-2148},
		{1,62,0,2621440,4128768,2490368,67684,-2148},
		{1,62,0,2686976,4128768,2555904,67684,-2148},
		{1,62,0,2752512,4128768,2621440,67684,-2148},
		{1,62,0,2818048,4128768,2686976,67684,-2148},
		{1,62,0,2883584,4128768,2752512,67684,-2148},
		{1,62,0,2949120,4128768,2818048,67684,-2148},
		{1,62,0,3014656,4128768,2883584,67684,-2148},
		{1,62,0,3080192,4128768,2949120,67684,-2148},
		{1,62,0,3145728,4128768,3014656,67684,-2148},
		{1,62,0,3211264,4128768,3080192,67684,-2148},
		{1,62,0,3276800,4128768,3145728,67684,-2148},
		{1,62,0,3342336,4128768,3211264,67684,-2148},
		{1,62,0,3407872,4128768,3276800,67684,-2148},
		{1,62,0,3473408,4128768,3342336,67684,-2148},
		{1,62,0,3538944,4128768,3407872,67684,-2148},
		{1,62,0,3604480,4128768,3473408,67684,-2148},
		{1,62,0,3670016,4128768,3538944,67684,-2148},
		{1,62,0,3735552,4128768,3604480,67684,-2148},
		{1,62,0,3801088,4128768,3670016,67684,-2148},
		{1,62,0,3866624,4128768,3735552,67684,-2148},
		{1,62,0,3932160,4128768,3801088,67684,-2148},
		{1,62,0,3997696,4128768,3866624,67684,-2148},
		{2,61,0,4128768,4128768,3932160,68812,-3276},
		{32,31,2031616,4128768,4128768,3997696,69905,-4369},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 56 */
		{0,1,0,0,0,0,0,0},
		{0,63,0,65536,4128768,0,66593,-1057},
		{0,63,0,131072,4128768,65536,66593,-1057},
		{0,63,0,196608,4128768,131072,66593,-1057},
		{0,63,0,262144,4128768,196608,66593,-1057},
		{0,63,0,327680,4128768,262144,66593,-1057},
		{0,63,0,393216,4128768,327680,66593,-1057},
		{0,63,0,458752,4128768,393216,66593,-1057},
		{0,63,0,524288,4128768,458752,66593,-1057},
		{0,63,0,589824,4128768,524288,66593,-1057},
		{0,63,0,655360,4128768,589824,66593,-1057},
		{0,63,0,720896,4128768,655360,66593,-1057},
		{0,63,0,786432,4128768,720896,66593,-1057},
		{0,63,0,851968,4128768,786432,66593,-1057},
		{0,63,0,917504,4128768,851968,66593,-1057},
		{0,63,0,983040,4128768,917504,66593,-1057},
		{0,63,0,1048576,4128768,983040,66593,-1057},
		{0,63,0,1114112,4128768,1048576,66593,-1057},
		{0,63,0,1179648,4128768,1114112,66593,-1057},
		{0,63,0,1245184,4128768,1179648,66593,-1057},
		{0,63,0,1310720,4128768,1245184,66593,-1057},
		{0,63,0,1376256,4128768,1310720,66593,-1057},
		{0,63,0,1441792,4128768,1376256,66593,-1057},
		{0,63,0,1507328,4128768,1441792,66593,-1057},
		{0,63,0,1572864,4128768,1507328,66593,-1057},
		{0,63,0,1638400,4128768,1572864,66593,-1057},
		{0,63,0,1703936,4128768,1638400,66593,-1057},
		{0,63,0,1769472,4128768,1703936,66593,-1057},
		{0,63,0,1835008,4128768,1769472,66593,-1057},
		{0,63,0,1900544,4128768,1835008,66593,-1057},
		{0,63,0,1966080,4128768,1900544,66593,-1057},
		{0,63,0,2031616,4128768,1966080,66593,-1057},
		{0,63,0,2097152,4128768,2031616,66593,-1057},
		{0,63,0,2162688,4128768,2097152,66593,-1057},
		{0,63,0,2228224,4128768,2162688,66593,-1057},
		{0,63,0,2293760,4128768,2228224,66593,-1057},
		{0,63,0,2359296,4128768,2293760,66593,-1057},
		{0,63,0,2424832,4128768,2359296,66593,-1057},
		{0,63,0,2490368,4128768,2424832,66593,-1057},
		{0,63,0,2555904,4128768,2490368,66593,-1057},
		{0,63,0,2621440,4128768,2555904,66593,-1057},
		{0,63,0,2686976,4128768,2621440,66593,-1057},
		{0,63,0,2752512,4128768,2686976,66593,-1057},
		{0,63,0,2818048,4128768,2752512,66593,-1057},
		{0,63,0,2883584,4128768,2818048,66593,-1057},
		{0,63,0,2949120,4128768,2883584,66593,-1057},
		{0,63,0,3014656,4128768,2949120,66593,-1057},
		{0,63,0,3080192,4128768,3014656,66593,-1057},
		{0,63,0,3145728,4128768,3080192,66593,-1057},
		{0,63,0,3211264,4128768,3145728,66593,-1057},
		{0,63,0,3276800,4128768,3211264,66593,-1057},
		{0,63,0,3342336,4128768,3276800,66593,-1057},
		{0,63,0,3407872,4128768,3342336,66593,-1057},
		{0,63,0,3473408,4128768,3407872,66593,-1057},
		{0,63,0,3538944,4128768,3473408,66593,-1057},
		{0,63,0,3604480,4128768,3538944,66593,-1057},
		{0,63,0,3670016,4128768,3604480,66593,-1057},
		{0,63,0,3735552,4128768,3670016,66593,-1057},
		{0,63,0,3801088,4128768,3735552,66593,-1057},
		{0,63,0,3866624,4128768,3801088,66593,-1057},
		{0,63,0,3932160,4128768,3866624,66593,-1057},
		{0,63,0,3997696,4128768,3932160,66593,-1057},
		{1,62,0,4128768,4128768,3997696,67684,-2148},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 57 */
		{0,1,0,0,0,0,0,0},
		{0,63,0,65536,4128768,0,66593,-1057},
		{0,63,0,131072,4128768,65536,66593,-1057},
		{0,63,0,196608,4128768,131072,66593,-1057},
		{0,63,0,262144,4128768,196608,66593,-1057},
		{0,63,0,327680,4128768,262144,66593,-1057},
		{0,63,0,393216,4128768,327680,66593,-1057},
		{0,63,0,458752,4128768,393216,66593,-1057},
		{0,63,0,524288,4128768,458752,66593,-1057},
		{0,63,0,589824,4128768,524288,66593,-1057},
		{0,63,0,655360,4128768,589824,66593,-1057},
		{0,63,0,720896,4128768,655360,66593,-1057},
		{0,63,0,786432,4128768,720896,66593,-1057},
		{0,63,0,851968,4128768,786432,66593,-1057},
		{0,63,0,917504,4128768,851968,66593,-1057},
		{0,63,0,983040,4128768,917504,66593,-1057},
		{0,63,0,1048576,4128768,983040,66593,-1057},
		{0,63,0,1114112,4128768,1048576,66593,-1057},
		{0,63,0,1179648,4128768,1114112,66593,-1057},
		{0,63,0,1245184,4128768,1179648,66593,-1057},
		{0,63,0,1310720,4128768,1245184,66593,-1057},
		{0,63,0,1376256,4128768,1310720,66593,-1057},
		{0,63,0,1441792,4128768,1376256,66593,-1057},
		{0,63,0,1507328,4128768,1441792,66593,-1057},
		{0,63,0,1572864,4128768,1507328,66593,-1057},
		{0,63,0,1638400,4128768,1572864,66593,-1057},
		{0,63,0,1703936,4128768,1638400,66593,-1057},
		{0,63,0,1769472,4128768,1703936,66593,-1057},
		{0,63,0,1835008,4128768,1769472,66593,-1057},
		{0,63,0,1900544,4128768,1835008,66593,-1057},
		{0,63,0,1966080,4128768,1900544,66593,-1057},
		{0,63,0,2031616,4128768,1966080,66593,-1057},
		{0,63,0,2097152,4128768,2031616,66593,-1057},
		{0,63,0,2162688,4128768,2097152,66593,-1057},
		{0,63,0,2228224,4128768,2162688,66593,-1057},
		{0,63,0,2293760,4128768,2228224,66593,-1057},
		{0,63,0,2359296,4128768,2293760,66593,-1057},
		{0,63,0,2424832,4128768,2359296,66593,-1057},
		{0,63,0,2490368,4128768,2424832,66593,-1057},
		{0,63,0,2555904,4128768,2490368,66593,-1057},
		{0,63,0,2621440,4128768,2555904,66593,-1057},
		{0,63,0,2686976,4128768,2621440,66593,-1057},
		{0,63,0,2752512,4128768,2686976,66593,-1057},
		{0,63,0,2818048,4128768,2752512,66593,-1057},
		{0,63,0,2883584,4128768,2818048,66593,-1057},
		{0,63,0,2949120,4128768,2883584,66593,-1057},
		{0,63,0,3014656,4128768,2949120,66593,-1057},
		{0,63,0,3080192,4128768,3014656,66593,-1057},
		{0,63,0,3145728,4128768,3080192,66593,-1057},
		{0,63,0,3211264,4128768,3145728,66593,-1057},
		{0,63,0,3276800,4128768,3211264,66593,-1057},
		{0,63,0,3342336,4128768,3276800,66593,-1057},
		{0,63,0,3407872,4128768,3342336,66593,-1057},
		{0,63,0,3473408,4128768,3407872,66593,-1057},
		{0,63,0,3538944,4128768,3473408,66593,-1057},
		{0,63,0,3604480,4128768,3538944,66593,-1057},
		{0,63,0,3670016,4128768,3604480,66593,-1057},
		{0,63,0,3735552,4128768,3670016,66593,-1057},
		{0,63,0,3801088,4128768,3735552,66593,-1057},
		{0,63,0,3866624,4128768,3801088,66593,-1057},
		{0,63,0,3932160,4128768,3866624,66593,-1057},
		{0,63,0,3997696,4128768,3932160,66593,-1057},
		{1,62,0,4128768,4128768,3997696,67684,-2148},
		{63,1,4128768,4128768,4128768,4128768,0,0},
	},
	{ /* n = 58 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 59 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 60 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 61 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 62 */
		{0,64,0,0,4128768,0,65536,0},
		{0,64,0,65536,4128768,65536,65536,0},
		{0,64,0,131072,4128768,131072,65536,0},
		{0,64,0,196608,4128768,196608,65536,0},
		{0,64,0,262144,4128768,262144,65536,0},
		{0,64,0,327680,4128768,327680,65536,0},
		{0,64,0,393216,4128768,393216,65536,0},
		{0,64,0,458752,4128768,458752,65536,0},
		{0,64,0,524288,4128768,524288,65536,0},
		{0,64,0,589824,4128768,589824,65536,0},
		{0,64,0,655360,4128768,655360,65536,0},
		{0,64,0,720896,4128768,720896,65536,0},
		{0,64,0,786432,4128768,786432,65536,0},
		{0,64,0,851968,4128768,851968,65536,0},
		{0,64,0,917504,4128768,917504,65536,0},
		{0,64,0,983040,4128768,983040,65536,0},
		{0,64,0,1048576,4128768,1048576,65536,0},
		{0,64,0,1114112,4128768,1114112,65536,0},
		{0,64,0,1179648,4128768,1179648,65536,0},
		{0,64,0,1245184,4128768,1245184,65536,0},
		{0,64,0,1310720,4128768,1310720,65536,0},
		{0,64,0,1376256,4128768,1376256,65536,0},
		{0,64,0,1441792,4128768,1441792,65536,0},
		{0,64,0,1507328,4128768,1507328,65536,0},
		{0,64,0,1572864,4128768,1572864,65536,0},
		{0,64,0,1638400,4128768,1638400,65536,0},
		{0,64,0,1703936,4128768,1703936,65536,0},
		{0,64,0,1769472,4128768,1769472,65536,0},
		{0,64,0,1835008,4128768,1835008,65536,0},
		{0,64,0,1900544,4128768,1900544,65536,0},
		{0,64,0,1966080,4128768,1966080,65536,0},
		{0,64,0,2031616,4128768,2031616,65536,0},
		{0,64,0,2097152,4128768,2097152,65536,0},
		{0,64,0,2162688,4128768,2162688,65536,0},
		{0,64,0,2228224,4128768,2228224,65536,0},
		{0,64,0,2293760,4128768,2293760,65536,0},
		{0,64,0,2359296,4128768,2359296,65536,0},
		{0,64,0,2424832,4128768,2424832,65536,0},
		{0,64,0,2490368,4128768,2490368,65536,0},
		{0,64,0,2555904,4128768,2555904,65536,0},
		{0,64,0,2621440,4128768,2621440,65536,0},
		{0,64,0,2686976,4128768,2686976,65536,0},
		{0,64,0,2752512,4128768,2752512,65536,0},
		{0,64,0,2818048,4128768,2818048,65536,0},
		{0,64,0,2883584,4128768,2883584,65536,0},
		{0,64,0,2949120,4128768,2949120,65536,0},
		{0,64,0,3014656,4128768,3014656,65536,0},
		{0,64,0,3080192,4128768,3080192,65536,0},
		{0,64,0,3145728,4128768,3145728,65536,0},
		{0,64,0,3211264,4128768,3211264,65536,0},
		{0,64,0,3276800,4128768,3276800,65536,0},
		{0,64,0,3342336,4128768,3342336,65536,0},
		{0,64,0,3407872,4128768,3407872,65536,0},
		{0,64,0,3473408,4128768,3473408,65536,0},
		{0,64,0,3538944,4128768,3538944,65536,0},
		{0,64,0,3604480,4128768,3604480,65536,0},
		{0,64,0,3670016,4128768,3670016,65536,0},
		{0,64,0,3735552,4128768,3735552,65536,0},
		{0,64,0,3801088,4128768,3801088,65536,0},
		{0,64,0,3866624,4128768,3866624,65536,0},
		{0,64,0,3932160,4128768,3932160,65536,0},
		{0,64,0,3997696,4128768,3997696,65536,0},
		{0,64,0,4063232,4128768,4063232,65536,0},
		{0,64,0,4128768,4128768,4128768,65536,0},
	},
	{ /* n = 63 */
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
		{0,0,0,0,0,0,0,0},
	},
};


// turn.cpp:22-33. Per-phase gloss (specular highlight) table. A bump of
// brightness that travels across the tile as it turns, blended into the
// source pixel with ShimBlend at ratio `gl`.
static const tjs_int ShimTurnGloss[64] =
{
	   0,   0,   0,   0,  16,  48,  80, 128,
	 192, 128,  80,  48,  16,   0,   0,   0,
	   0,   0,   0,   0,   0,   0,   0,   0,
	   0,   0,   0,   0,   0,   0,   0,   0,
	   0,   0,   0,   0,   0,   0,   0,   0,
	   0,   0,   0,   0,   0,   0,   0,   0,
	   0,   0,   0,   0,   0,   0,   0,   0,
	   0,   0,   0,   0,   0,   0,   0,   0,
};

// turn.cpp:34. 2 = more tiles flip at once than 1 would.
#define SHIM_TURN_WIDTH_FACTOR 2

// turn.cpp:37-117.
class tShimTurnTransHandler : public iTVPDivisibleTransHandler
{
	tjs_int RefCount;

	tjs_uint64 StartTick;
	tjs_uint64 Time;
	tjs_uint64 CurTime;
	tjs_int Width;
	tjs_int Height;
	tjs_int BGColor;
	tjs_int Phase;
	bool First;

public:
	tShimTurnTransHandler(tjs_uint64 time, tjs_int width, tjs_int height,
		tjs_uint32 bgcolor)
	{
		RefCount = 1;
		Width = width;
		Height = height;
		Time = time;
		BGColor = bgcolor;
		First = true;
	}

	virtual ~tShimTurnTransHandler() {}

	virtual tjs_error SHIM_INTF_METHOD AddRef() { RefCount++; return SHIM_TJS_S_OK; }
	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1) delete this;
		else RefCount--;
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD SetOption(iTVPSimpleOptionProvider *)
	{
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD StartProcess(tjs_uint64 tick);
	virtual tjs_error SHIM_INTF_METHOD EndProcess();
	virtual tjs_error SHIM_INTF_METHOD Process(tTVPDivisibleData *data);

	virtual tjs_error SHIM_INTF_METHOD MakeFinalImage(
		iTVPScanLineProvider **dest, iTVPScanLineProvider *,
		iTVPScanLineProvider *src2)
	{
		*dest = src2;
		return SHIM_TJS_S_OK;
	}
};

// turn.cpp:119-148. Phase sweeps from bottom-left to top-right over the tile
// grid; each tile's phase is offset by its (x-y) diagonal position.
tjs_error SHIM_INTF_METHOD tShimTurnTransHandler::StartProcess(tjs_uint64 tick)
{
	if (First)
	{
		First = false;
		StartTick = tick;
	}

	CurTime = (tick - StartTick);
	if (CurTime > Time) CurTime = Time;
	tjs_int xcount = (Width - 1) / 64 + 1;
	tjs_int ycount = (Height - 1) / 64 + 1;
	Phase = CurTime * (64 + (xcount + ycount) * SHIM_TURN_WIDTH_FACTOR) /
		(tjs_int64)Time - ycount * SHIM_TURN_WIDTH_FACTOR;

	return SHIM_TJS_S_TRUE;
}

// turn.cpp:150-157.
tjs_error SHIM_INTF_METHOD tShimTurnTransHandler::EndProcess()
{
	if (CurTime == Time) return SHIM_TJS_S_FALSE;
	return SHIM_TJS_S_TRUE;
}

// turn.cpp:159-490. For each 64x64 tile in the requested rectangle: phase 0
// copies src1, phase 63 copies src2, and the phases between deform the source
// through TurnTransParams (a perspective-like squash along the tile's flip
// axis), with gloss blended on top. Edge/bottom tiles clip carefully to avoid
// reading past the source buffer.
tjs_error SHIM_INTF_METHOD tShimTurnTransHandler::Process(
	tTVPDivisibleData *data)
{
	tjs_uint8 *dest;
	tjs_int destpitch;
	const tjs_uint8 *src1;
	tjs_int src1pitch;
	const tjs_uint8 *src2;
	tjs_int src2pitch;
	if (SHIM_TJS_FAILED(data->Dest->GetScanLineForWrite(0, (void **)&dest)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src1->GetScanLine(0, (const void **)&src1)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src2->GetScanLine(0, (const void **)&src2)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Dest->GetPitchBytes(&destpitch)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src1->GetPitchBytes(&src1pitch)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src2->GetPitchBytes(&src2pitch)))
		return SHIM_TJS_E_FAIL;

	tjs_int destxofs = data->DestLeft - data->Left;
	tjs_int destyofs = data->DestTop - data->Top;

	// turn.cpp:207-210. Tile range covering the requested rectangle.
	tjs_int startx = data->Left / 64;
	tjs_int starty = data->Top / 64;
	tjs_int endx = (data->Left + data->Width - 1) / 64;
	tjs_int endy = (data->Top + data->Height - 1) / 64;

	for (tjs_int y = starty; y <= endy; y++)
	{
		for (tjs_int x = startx; x <= endx; x++)
		{
			// turn.cpp:223-226. Phase per tile, by diagonal offset.
			tjs_int phase = Phase - (x - y) * SHIM_TURN_WIDTH_FACTOR;
			if (phase < 0) phase = 0;
			if (phase > 63) phase = 63;
			tjs_int gl = ShimTurnGloss[phase];

			// turn.cpp:227-366. Edge/bottom tiles: clip to the rectangle and
			// (for bottom-row tiles) guard against source overread.
			if (y * 64 + 64 >= Height || x == startx || x == endx)
			{
				tjs_int l = x * 64;
				tjs_int t = y * 64;
				tjs_int r = l + 64;
				tjs_int b = t + 64;
				if (ShimClip(l, r, data->Left, data->Left + data->Width) &&
					ShimClip(t, b, data->Top, data->Top + data->Height))
				{
					if (phase == 0)
					{
						// turn.cpp:242-253. Pure src1.
						tjs_uint8 *dp = dest + (t + destyofs) * destpitch +
							(l + destxofs) * sizeof(tjs_uint32);
						const tjs_uint8 *sp = src1 + t * src1pitch +
							l * sizeof(tjs_uint32);
						tjs_int count = b - t;
						tjs_int len = (r - l) * sizeof(tjs_uint32);
						while (count--)
						{
							memcpy(dp, sp, len);
							dp += destpitch;
							sp += src1pitch;
						}
					}
					else if (phase == 63)
					{
						// turn.cpp:254-265. Pure src2.
						tjs_uint8 *dp = dest + (t + destyofs) * destpitch +
							(l + destxofs) * sizeof(tjs_uint32);
						const tjs_uint8 *sp = src2 + t * src2pitch +
							l * sizeof(tjs_uint32);
						tjs_int count = b - t;
						tjs_int len = (r - l) * sizeof(tjs_uint32);
						while (count--)
						{
							memcpy(dp, sp, len);
							dp += destpitch;
							sp += src2pitch;
						}
					}
					else
					{
						// turn.cpp:267-364. Deformed transfer with clipping.
						const tShimTurnTransParams *params =
							ShimTurnTransParams[phase];
						const tjs_uint8 *src;
						tjs_int srcpitch;
						if (phase < 32)
						{
							src = src1;
							srcpitch = src1pitch;
						}
						else
						{
							src = src2;
							srcpitch = src2pitch;
						}

						tjs_int line = t - y * 64;
						tjs_int start = l - x * 64;
						tjs_int end = r - x * 64;

						params += line;
						src += x * 64 * sizeof(tjs_uint32);

						tjs_int count = b - t;
						tjs_uint8 *dp = (tjs_uint8 *)
							((tjs_uint32 *)(dest + (t + destyofs) * destpitch) +
								x * 64 + destxofs);
						while (count--)
						{
							tjs_int fl, fr;

							// turn.cpp:300-307. Left background.
							fl = 0;
							fr = params->start;
							if (ShimClip(fl, fr, start, end))
								g_fill_argb((tjs_uint32 *)dp + fl, fr - fl,
									BGColor);

							// turn.cpp:309-316. Right background.
							fl = params->start + params->len;
							fr = 64;
							if (ShimClip(fl, fr, start, end))
								g_fill_argb((tjs_uint32 *)dp + fl, fr - fl,
									BGColor);

							// turn.cpp:318-360. Deformed source span.
							fl = params->start;
							fr = params->start + params->len;
							if (ShimClip(fl, fr, start, end))
							{
								tjs_int sx = params->sx;
								tjs_int sy = params->sy;
								sx += params->stepx * (fl - params->start);
								sy += params->stepy * (fl - params->start);
								if (gl)
								{
									for (; fl < fr; fl++)
									{
										tjs_int yy = y * 64 + (sy >> 16);
										if (yy >= Height)
											((tjs_uint32 *)dp)[fl] = BGColor;
										else
											((tjs_uint32 *)dp)[fl] = ShimBlend(
												*(const tjs_uint32 *)
													(src + (sx >> 16) * sizeof(tjs_uint32) +
													yy * srcpitch),
												0xffffff, gl);
										sx += params->stepx;
										sy += params->stepy;
									}
								}
								else
								{
									for (; fl < fr; fl++)
									{
										tjs_int yy = y * 64 + (sy >> 16);
										if (yy >= Height)
											((tjs_uint32 *)dp)[fl] = BGColor;
										else
											((tjs_uint32 *)dp)[fl] =
												*(const tjs_uint32 *)
													(src + (sx >> 16) * sizeof(tjs_uint32) +
													yy * srcpitch);
									sx += params->stepx;
									sy += params->stepy;
								}
								}
							}
							dp += destpitch;
							params++;
						}
					}
				}
			}
			else
			{
				// turn.cpp:367-485. Interior tiles: no left/right clip needed.
				tjs_int l = x * 64;
				tjs_int t = y * 64;
				tjs_int r = l + 64;
				tjs_int b = t + 64;
				if (ShimClip(t, b, data->Top, data->Top + data->Height))
				{
					if (phase == 0)
					{
						tjs_uint8 *dp = dest + (t + destyofs) * destpitch +
							(l + destxofs) * sizeof(tjs_uint32);
						const tjs_uint8 *sp = src1 + t * src1pitch +
							l * sizeof(tjs_uint32);
						tjs_int count = b - t;
						tjs_int len = (r - l) * sizeof(tjs_uint32);
						while (count--)
						{
							memcpy(dp, sp, len);
							dp += destpitch;
							sp += src1pitch;
						}
					}
					else if (phase == 63)
					{
						tjs_uint8 *dp = dest + (t + destyofs) * destpitch +
							(l + destxofs) * sizeof(tjs_uint32);
						const tjs_uint8 *sp = src2 + t * src2pitch +
							l * sizeof(tjs_uint32);
						tjs_int count = b - t;
						tjs_int len = (r - l) * sizeof(tjs_uint32);
						while (count--)
						{
							memcpy(dp, sp, len);
							dp += destpitch;
							sp += src2pitch;
						}
					}
					else
					{
						const tShimTurnTransParams *params =
							ShimTurnTransParams[phase];
						const tjs_uint8 *src;
						tjs_int srcpitch;
						if (phase < 32)
						{
							src = src1;
							srcpitch = src1pitch;
						}
						else
						{
							src = src2;
							srcpitch = src2pitch;
						}

						tjs_int line = t - y * 64;
						params += line;
						src += l * sizeof(tjs_uint32) + y * 64 * srcpitch;

						tjs_int count = b - t;
						tjs_uint8 *dp = (tjs_uint8 *)
							((tjs_uint32 *)(dest + (t + destyofs) * destpitch) +
								l + destxofs);
						while (count--)
						{
							tjs_int fl, fr;

							// turn.cpp:437-438. Left background (full width).
							g_fill_argb((tjs_uint32 *)dp + 0, params->start,
								BGColor);

							// turn.cpp:441-443. Right background.
							fl = params->start + params->len;
							g_fill_argb((tjs_uint32 *)dp + fl, 64 - fl,
								BGColor);

							// turn.cpp:445-479. Deformed source span.
							fl = params->start;
							fr = params->start + params->len;
							tjs_int sx = params->sx;
							tjs_int sy = params->sy;
							if (gl)
							{
								for (; fl < fr; fl++)
								{
									((tjs_uint32 *)dp)[fl] = ShimBlend(
										*(const tjs_uint32 *)
											(src + (sx >> 16) * sizeof(tjs_uint32) +
												(sy >> 16) * srcpitch),
										0xffffff, gl);
									sx += params->stepx;
									sy += params->stepy;
								}
							}
							else
							{
								// turn.cpp:465-467. Fast path: engine's affine copy.
								g_lintrans_copy((tjs_uint32 *)dp + fl, fr - fl,
									(const tjs_uint32 *)src, sx, sy,
									params->stepx, params->stepy, srcpitch);
							}
							dp += destpitch;
							params++;
						}
					}
				}
			}
		}
	}

	return SHIM_TJS_S_OK;
}

// turn.cpp:496-570.
class tShimTurnTransHandlerProvider : public iTVPTransHandlerProvider
{
	tjs_int RefCount;
public:
	tShimTurnTransHandlerProvider() { RefCount = 1; }
	virtual ~tShimTurnTransHandlerProvider() {}

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}
	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1) delete this;
		else RefCount--;
		return SHIM_TJS_S_OK;
	}

	// turn.cpp:526. The binary's name for this handler is "turn" (verified in
	// .rdata at 0x10033638; the leading ")" the decompiler saw is a Ghidra
	// display artifact, not part of the string).
	virtual tjs_error SHIM_INTF_METHOD GetName(const tjs_char **name)
	{
		if (name)
		{
			*name = SHIM_TJS_W("turn");
			return SHIM_TJS_S_OK;
		}
		return SHIM_TJS_E_FAIL;
	}

	virtual tjs_error SHIM_INTF_METHOD StartTransition(
		iTVPSimpleOptionProvider *options,
		iTVPSimpleImageProvider *,
		tTVPLayerType,
		tjs_uint src1w, tjs_uint src1h,
		tjs_uint src2w, tjs_uint src2h,
		tTVPTransType *type,
		tTVPTransUpdateType *updatetype,
		iTVPBaseTransHandler **handler)
	{
		if (type) *type = ttExchange;
		if (updatetype) *updatetype = tutDivisible;
		if (!handler) return SHIM_TJS_E_FAIL;
		if (!options) return SHIM_TJS_E_FAIL;
		if (src1w != src2w || src1h != src2h)
			return SHIM_TJS_E_FAIL;

		// turn.cpp:554-555. bgcolor defaults to 0 (black).
		tjs_int64 time;
		tjs_int64 bgcolor = 0;

		if (!ShimOption(options, SHIM_TJS_W("time"), &time))
			return SHIM_TJS_E_FAIL;
		if (time < 2) time = 2;

		ShimOption(options, SHIM_TJS_W("bgcolor"), &bgcolor);

		*handler = new tShimTurnTransHandler((tjs_uint64)time,
			(tjs_int)src1w, (tjs_int)src1h, (tjs_uint32)bgcolor);
		return SHIM_TJS_S_OK;
	}
};

//--------------------------------------------------------------------------
// `rotatezoom`, `rotatevanish`, `rotateswap` — rotate-and-zoom transitions
struct tShimPoint { tjs_int x; tjs_int y; };

//--------------------------------------------------------------------------
// Ported from extrans/rotatebase.cpp (lines 1-636) and extrans/rotatetrans.cpp
// (lines 1-511). All three share a base class (tTVPBaseRotateTransHandler) that
// manages a per-scanline region list: each line is divided into background /
// src1 / src2 spans, and Process() blits each span with the appropriate
// primitive (memcpy, TVPStretchCopy, or TVPLinTransCopy depending on whether the
// span is unscaled, x-scaled, or fully affine).
//
// rotatezoom (rotatetrans.cpp:18-120): src2 rotates and zooms around a centre
// while src1 stays fixed. rotatevanish (rotatetrans.cpp:222-316): same handler
// class as rotatezoom but with FixSrc1=false and factor=1/targetfactor=0, so
// it is src1 that zooms out to nothing while src2 is fixed. rotateswap
// (rotatetrans.cpp:318-392, 394-475): a distinct handler where src1 recedes
// and src2 approaches, both rotating, meeting at the half-way point.

// rotatebase.h:11-14. A point in 16.16 fixed-point.

// rotatebase.h:16-22. One span's source mapping: the destination starts at
// `start`, and each pixel steps by (stepx, stepy) in the source (16.16).
struct tShimRotateDrawLine
{
	tjs_int start;   // destination start x
	tjs_int sx;      // source start x (* 65536)
	tjs_int sy;      // source start y (* 65536)
	tjs_int stepx;   // source step x (* 65536)
	tjs_int stepy;   // source step y (* 65536)
};

// rotatebase.h:24-28. One span within a scanline: [left, right) drawn from
// src1, src2, or background.
struct tShimRotateDrawRegionData
{
	tjs_int left;
	tjs_int right;
	tjs_int type;   // 0 = bgcolor, 1 = src1, 2 = src2
};

// rotatebase.h:30-35. A scanline's draw list: up to 5 regions (background,
// src1-left, src2-middle, src1-right, background) plus the source lines for
// src1 and src2.
struct tShimRotateDrawData
{
	int count;
	tShimRotateDrawLine src1;
	tShimRotateDrawLine src2;
	tShimRotateDrawRegionData region[5];
};

// rotatebase.cpp:16-117. The base handler: manages the per-scanline region
// list and the Process() blit loop. Subclasses implement CalcPosition() to
// fill the region list for the current frame via AddSource()/AddLine().
class tShimBaseRotateTransHandler : public iTVPDivisibleTransHandler
{
	tjs_int RefCount;

protected:
	tjs_uint64 StartTick;
	tjs_uint64 Time;
	tjs_uint64 CurTime;
	tjs_int Width;
	tjs_int Height;
	tjs_int BGColor;
	tjs_int Phase;
	bool First;
	tShimRotateDrawData *DrawData;

public:
	tShimBaseRotateTransHandler(tjs_uint64 time, tjs_int width,
		tjs_int height, tjs_uint32 bgcolor)
	{
		RefCount = 1;
		Width = width;
		Height = height;
		Time = time;
		BGColor = bgcolor;
		DrawData = new tShimRotateDrawData[height];
		First = true;
	}

	virtual ~tShimBaseRotateTransHandler() { delete[] DrawData; }

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}
	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1) delete this;
		else RefCount--;
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD SetOption(iTVPSimpleOptionProvider *)
	{
		return SHIM_TJS_S_OK;
	}

	virtual tjs_error SHIM_INTF_METHOD StartProcess(tjs_uint64 tick);
	virtual tjs_error SHIM_INTF_METHOD EndProcess();
	virtual tjs_error SHIM_INTF_METHOD Process(tTVPDivisibleData *data);

	virtual tjs_error SHIM_INTF_METHOD MakeFinalImage(
		iTVPScanLineProvider **dest, iTVPScanLineProvider *,
		iTVPScanLineProvider *src2)
	{
		*dest = src2;
		return SHIM_TJS_S_OK;
	}

protected:
	void AddLine(tjs_int line, tjs_int left, tjs_int right, tjs_int type);
	void AddSource(const tShimPoint *points, tjs_int type);
	virtual void CalcPosition() = 0;
};

// rotatebase.cpp:35-68.
tjs_error SHIM_INTF_METHOD tShimBaseRotateTransHandler::StartProcess(
	tjs_uint64 tick)
{
	if (First)
	{
		First = false;
		StartTick = tick;
	}

	CurTime = (tick - StartTick);
	if (CurTime > Time) CurTime = Time;

	// Clear each scanline to a single full-width background region.
	for (tjs_int i = 0; i < Height; i++)
	{
		DrawData[i].count = 1;
		DrawData[i].region[0].left = 0;
		DrawData[i].region[0].right = Width;
		DrawData[i].region[0].type = 0;
	}

	CalcPosition();

	return SHIM_TJS_S_TRUE;
}

// rotatebase.cpp:70-77.
tjs_error SHIM_INTF_METHOD tShimBaseRotateTransHandler::EndProcess()
{
	if (CurTime == Time) return SHIM_TJS_S_FALSE;
	return SHIM_TJS_S_TRUE;
}

// rotatebase.cpp:79-186. For each scanline in the requested rectangle, walk
// the region list and blit each span: background -> TVPFillARGB, src1/src2 ->
// memcpy (unscaled), TVPStretchCopy (x-scaled), or TVPLinTransCopy (affine).
tjs_error SHIM_INTF_METHOD tShimBaseRotateTransHandler::Process(
	tTVPDivisibleData *data)
{
	tjs_int destxofs = data->DestLeft - data->Left;

	tjs_uint8 *dest;
	tjs_int destpitch;
	const tjs_uint8 *src1;
	tjs_int src1pitch;
	const tjs_uint8 *src2;
	tjs_int src2pitch;
	if (SHIM_TJS_FAILED(data->Dest->GetScanLineForWrite(data->DestTop,
			(void **)&dest)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src1->GetScanLine(0, (const void **)&src1)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src2->GetScanLine(0, (const void **)&src2)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Dest->GetPitchBytes(&destpitch)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src1->GetPitchBytes(&src1pitch)))
		return SHIM_TJS_E_FAIL;
	if (SHIM_TJS_FAILED(data->Src2->GetPitchBytes(&src2pitch)))
		return SHIM_TJS_E_FAIL;

	tjs_int h = data->Height;
	tjs_int y = data->Top;
	while (h--)
	{
		tShimRotateDrawData *line = DrawData + y;

		for (tjs_int i = 0; i < line->count; i++)
		{
			if (line->region[i].left == line->region[i].right) continue;
			tjs_int l = line->region[i].left;
			tjs_int r = line->region[i].right;
			if (ShimClip(l, r, data->Left, data->Left + data->Width))
			{
				tjs_int type = line->region[i].type;
				if (type == 0)
				{
					// Background.
					g_fill_argb((tjs_uint32 *)dest + destxofs + l,
						r - l, BGColor);
				}
				else
				{
					// src1 or src2.
					const tShimRotateDrawLine &drawline =
						(type == 1) ? line->src1 : line->src2;

					if (drawline.stepx == 65536 && drawline.stepy == 0)
					{
						// rotatebase.cpp:144-153. Unscaled 1:1 copy.
						memcpy((tjs_uint32 *)dest + destxofs + l,
							(const tjs_uint32 *)
								((type == 1 ? src1 : src2) +
								((drawline.sy +
									(l - drawline.start) * drawline.stepy)
									>> 16) *
								(type == 1 ? src1pitch : src2pitch)) +
							((drawline.sx +
								(l - drawline.start) * drawline.stepx)
								>> 16),
							(r - l) * sizeof(tjs_uint32));
					}
					else if (drawline.stepy == 0)
					{
						// rotatebase.cpp:154-164. X-scaled (stretch) copy.
						g_stretch_copy((tjs_uint32 *)dest + destxofs + l,
							r - l,
							(const tjs_uint32 *)
								((type == 1 ? src1 : src2) +
								((drawline.sy +
									(l - drawline.start) * drawline.stepy)
									>> 16) *
								(type == 1 ? src1pitch : src2pitch)),
							drawline.sx +
								(l - drawline.start) * drawline.stepx,
							drawline.stepx);
					}
					else
					{
						// rotatebase.cpp:165-176. Full affine copy.
						g_lintrans_copy(
							(tjs_uint32 *)dest + destxofs + l, r - l,
							(const tjs_uint32 *)
								(type == 1 ? src1 : src2),
							drawline.sx +
								(l - drawline.start) * drawline.stepx,
							drawline.sy +
								(l - drawline.start) * drawline.stepy,
							drawline.stepx, drawline.stepy,
							type == 1 ? src1pitch : src2pitch);
					}
				}
			}
		}

		dest += destpitch;
		y++;
	}

	return SHIM_TJS_S_OK;
}

// rotatebase.cpp:188-277. Overwrite [left,right) on `line` with `type`,
// splitting/clipping existing regions as needed. The region array holds at
// most 5 entries (background + two source spans, each possibly split).
void tShimBaseRotateTransHandler::AddLine(tjs_int line, tjs_int left,
	tjs_int right, tjs_int type)
{
	if (ShimClip(left, right, 0, Width))
	{
		tShimRotateDrawData *data = DrawData + line;
		tjs_int i;
		for (i = 0; i < data->count; i++)
		{
			if (data->region[i].left == data->region[i].right) continue;

			if (data->region[i].left >= left &&
				data->region[i].right <= right)
			{
				// Fully contained: delete (zero-length).
				data->region[i].right = data->region[i].left;
				continue;
			}

			if (data->region[i].left < left &&
				data->region[i].right > right)
			{
				// Straddles: split into two, insert right part.
				data->region[data->count].left = right;
				data->region[data->count].right = data->region[i].right;
				data->region[data->count].type = data->region[i].type;
				data->count++;
				data->region[i].right = left;
				break;
			}

			if (data->region[i].left < left &&
				data->region[i].right > left)
			{
				data->region[i].right = left;
				continue;
			}

			if (data->region[i].left < right &&
				data->region[i].right > right)
			{
				data->region[i].left = right;
				continue;
			}
		}

		// Find a free slot (zero-length) or append.
		for (i = 0; i < data->count; i++)
		{
			if (data->region[i].left == data->region[i].right) break;
		}

		data->region[i].left = left;
		data->region[i].right = right;
		data->region[i].type = type;

		if (i == data->count) data->count++;
	}
}

// rotatebase.cpp:279-634. Given three corner points (upper-left, upper-right,
// bottom-left) defining a parallelogram in destination space, set up the
// per-scanline source mapping for `type` (1=src1, 2=src2). This is a
// generalised affine texture mapper, ported verbatim including the descent/
// ascent vertex tracking and the fixed-point source stepping. The source
// coordinate mapping assumes the source image is mapped to the parallelogram
// with corners at (0,0)->points[0], (W,0)->points[1], (0,H)->points[2].
void tShimBaseRotateTransHandler::AddSource(const tShimPoint *points,
	tjs_int type)
{
	tjs_int points_x[4];
	tjs_int points_y[4];

	tjs_int scanlinestart, scanlineend;
	tjs_int leftlimit, rightlimit;
	tjs_int toppoint, bottompoint;

	// rotatebase.cpp:295-301. upper-left.
	points_x[0] = points[0].x;
	points_y[0] = points[0].y;
	leftlimit = points_x[0];
	rightlimit = points_x[0];
	scanlinestart = points_y[0]; toppoint = 0;
	scanlineend = points_y[0]; bottompoint = 0;

	// rotatebase.cpp:303-309. upper-right.
	points_x[1] = points[1].x;
	points_y[1] = points[1].y;
	if (leftlimit > points_x[1]) leftlimit = points_x[1];
	if (rightlimit < points_x[1]) rightlimit = points_x[1];
	if (scanlinestart > points_y[1])
		{ scanlinestart = points_y[1]; toppoint = 1; }
	if (scanlineend < points_y[1])
		{ scanlineend = points_y[1]; bottompoint = 1; }

	// rotatebase.cpp:311-317. bottom-right (computed from the three given).
	points_x[2] = points[1].x - points[0].x + points[2].x;
	points_y[2] = points[1].y - points[0].y + points[2].y;
	if (leftlimit > points_x[2]) leftlimit = points_x[2];
	if (rightlimit < points_x[2]) rightlimit = points_x[2];
	if (scanlinestart > points_y[2])
		{ scanlinestart = points_y[2]; toppoint = 2; }
	if (scanlineend < points_y[2])
		{ scanlineend = points_y[2]; bottompoint = 2; }

	// rotatebase.cpp:319-325. bottom-left.
	points_x[3] = points[2].x;
	points_y[3] = points[2].y;
	if (leftlimit > points_x[3]) leftlimit = points_x[3];
	if (rightlimit < points_x[3]) rightlimit = points_x[3];
	if (scanlinestart > points_y[3])
		{ scanlinestart = points_y[3]; toppoint = 3; }
	if (scanlineend < points_y[3])
		{ scanlineend = points_y[3]; bottompoint = 3; }

	// rotatebase.cpp:327-331. Cull if entirely outside.
	if (leftlimit >= Width) return;
	if (rightlimit < 0) return;
	if (scanlinestart >= Height) return;
	if (scanlineend < 0) return;

	tjs_int pd, pa, pdnext, panext;
	tjs_int pdstepx, pastepx;
	tjs_int pdx, pax;
	tjs_int sdstep, sastep;
	tjs_int sd, sa;
	tjs_int y = 0 < scanlinestart ? scanlinestart : 0;
	tjs_int ylim = (Height - 1) < scanlineend ? (Height - 1) : scanlineend;

	// rotatebase.cpp:344-363. Skip to the first visible scanline (descent).
	pd = pdnext = toppoint;
	pdnext--;
	pdnext &= 3;
	while (pdnext != bottompoint && points_y[pdnext] < y)
	{
		pdnext--; pdnext &= 3; pd--; pd &= 3;
	}
	while (pdnext != bottompoint && points_y[pdnext] == y)
	{
		pd--; pdnext--; pd &= 3; pdnext &= 3;
	}

	// rotatebase.cpp:365-384. ...and ascent.
	pa = panext = toppoint;
	panext++; panext &= 3;
	while (panext != bottompoint && points_y[panext] < y)
	{
		panext++; panext &= 3; pa++; pa &= 3;
	}
	while (panext != bottompoint && points_y[panext] == y)
	{
		pa++; panext++; pa &= 3; panext &= 3;
	}

	// rotatebase.cpp:388-400. Horizontal step per line.
	if (points_y[pdnext] - points_y[pd] + 1)
		pdstepx = 65536 * (points_x[pdnext] - points_x[pd]) /
			(points_y[pdnext] - points_y[pd] + 1);
	else
		pdstepx = 65536;
	if (points_y[panext] - points_y[pa] + 1)
		pastepx = 65536 * (points_x[panext] - points_x[pa]) /
			(points_y[panext] - points_y[pa] + 1);
	else
		pastepx = 65536;

	// rotatebase.cpp:405-443. Source step per line. The switch maps the
	// descent/ascent vertex index to the source axis it walks along.
	if (points_y[pdnext] - points_y[pd] + 1)
	{
		switch (pd)
		{
		case 0: sdstep = 65536 * Height / (points_y[pdnext] - points_y[pd] + 1); break;
		case 1: sdstep = 65536 * (-Width) / (points_y[pdnext] - points_y[pd] + 1); break;
		case 2: sdstep = 65536 * (-Height) / (points_y[pdnext] - points_y[pd] + 1); break;
		case 3: sdstep = 65536 * Width / (points_y[pdnext] - points_y[pd] + 1); break;
		}
	}
	else sdstep = 65536;

	if (points_y[panext] - points_y[pa] + 1)
	{
		switch (pa)
		{
		case 0: sastep = 65536 * Width / (points_y[panext] - points_y[pa] + 1); break;
		case 1: sastep = 65536 * Height / (points_y[panext] - points_y[pa] + 1); break;
		case 2: sastep = 65536 * (-Width) / (points_y[panext] - points_y[pa] + 1); break;
		case 3: sastep = 65536 * (-Height) / (points_y[panext] - points_y[pa] + 1); break;
		}
	}
	else sastep = 65536;

	// rotatebase.cpp:447-475. Initial positions.
	pdx = points_x[pd] * 65536;
	if (points_y[pd] < y) pdx += pdstepx * (y - points_y[pd]);
	pax = points_x[pa] * 65536;
	if (points_y[pa] < y) pax += pastepx * (y - points_y[pa]);

	switch (pd)
	{
	case 0: sd = 0; break;
	case 1: sd = Width * 65536 - 1; break;
	case 2: sd = Height * 65536 - 1; break;
	case 3: sd = 0; break;
	}
	if (points_y[pd] < y) sd += sdstep * (y - points_y[pd]);

	switch (pa)
	{
	case 0: sa = 0; break;
	case 1: sa = 0; break;
	case 2: sa = Width * 65536 - 1; break;
	case 3: sa = Height * 65536 - 1; break;
	}
	if (points_y[pa] < y) sa += sastep * (y - points_y[pa]);

	// rotatebase.cpp:477-633. Per-scanline loop: compute the left/right
	// edges and source mapping, add the span, then advance the edge trackers.
	for (; y <= ylim; y++)
	{
		tjs_int ddx = pdx >> 16;
		tjs_int adx = pax >> 16;

		tjs_int dx, dy, ax, ay;
		switch (pd)
		{
		case 0: dx = 0; dy = sd; break;
		case 1: dx = sd; dy = 0; break;
		case 2: dx = Width * 65536 - 1; dy = sd; break;
		case 3: dx = sd; dy = Height * 65536 - 1; break;
		}
		switch (pa)
		{
		case 0: ax = sa; ay = 0; break;
		case 1: ax = Width * 65536 - 1; ay = sa; break;
		case 2: ax = sa; ay = Height * 65536 - 1; break;
		case 3: ax = 0; ay = sa; break;
		}

		// Ensure left < right by swapping.
		if (adx < ddx)
		{
			tjs_int t;
			t = dx; dx = ax; ax = t;
			t = dy; dy = ay; ay = t;
			t = ddx; ddx = adx; adx = t;
		}

		tjs_int sxstep, systep;
		if (adx != ddx)
		{
			sxstep = (ax - dx + 1) / (adx - ddx + 1);
			systep = (ay - dy + 1) / (adx - ddx + 1);
		}
		else
			sxstep = systep = 65536;

		AddLine(y, ddx, adx + 1, type);

		tShimRotateDrawLine &drawline =
			(type == 1) ? DrawData[y].src1 : DrawData[y].src2;
		drawline.start = ddx;
		drawline.sx = dx;
		drawline.sy = dy;
		drawline.stepx = sxstep;
		drawline.stepy = systep;

		// Advance descent edge.
		if (points_y[pdnext] == y)
		{
			do {
				pd--; pdnext--; pd &= 3; pdnext &= 3;
			} while (pdnext != bottompoint && points_y[pdnext] == y);

			if (points_y[pdnext] - points_y[pd] + 1)
				pdstepx = 65536 * (points_x[pdnext] - points_x[pd]) /
					(points_y[pdnext] - points_y[pd] + 1);
			else
				pdstepx = 65536;

			if (points_y[pdnext] - points_y[pd] + 1)
			{
				switch (pd)
				{
				case 0: sdstep = 65536 * Height / (points_y[pdnext] - points_y[pd] + 1); break;
				case 1: sdstep = 65536 * (-Width) / (points_y[pdnext] - points_y[pd] + 1); break;
				case 2: sdstep = 65536 * (-Height) / (points_y[pdnext] - points_y[pd] + 1); break;
				case 3: sdstep = 65536 * Width / (points_y[pdnext] - points_y[pd] + 1); break;
				}
			}
			else sdstep = 65536;

			switch (pd)
			{
			case 0: sd = 0; break;
			case 1: sd = Width * 65536 - 1; break;
			case 2: sd = Height * 65536 - 1; break;
			case 3: sd = 0; break;
			}
			pdx = points_x[pd] * 65536;
		}

		// Advance ascent edge.
		if (points_y[panext] == y)
		{
			do {
				pa++; panext++; pa &= 3; panext &= 3;
			} while (panext != bottompoint && points_y[panext] == y);

			if (points_y[panext] - points_y[pa] + 1)
				pastepx = 65536 * (points_x[panext] - points_x[pa]) /
					(points_y[panext] - points_y[pa] + 1);
			else
				pastepx = 65536;

			if (points_y[panext] - points_y[pa] + 1)
			{
				switch (pa)
				{
				case 0: sastep = 65536 * Width / (points_y[panext] - points_y[pa] + 1); break;
				case 1: sastep = 65536 * Height / (points_y[panext] - points_y[pa] + 1); break;
				case 2: sastep = 65536 * (-Width) / (points_y[panext] - points_y[pa] + 1); break;
				case 3: sastep = 65536 * (-Height) / (points_y[panext] - points_y[pa] + 1); break;
				}
			}
			else sastep = 65536;

			switch (pa)
			{
			case 0: sa = 0; break;
			case 1: sa = 0; break;
			case 2: sa = Width * 65536 - 1; break;
			case 3: sa = Height * 65536 - 1; break;
			}
			pax = points_x[pa] * 65536;
		}

		pdx += pdstepx;
		pax += pastepx;
		sd += sdstep;
		sa += sastep;
	}
}

// rotatetrans.cpp:18-120. src2 rotates and zooms around a centre; src1 stays
// fixed (or vice versa if FixSrc1 is false). The zoom and rotation are eased
// by accel/twistaccel power curves.
class tShimRotateZoomTransHandler : public tShimBaseRotateTransHandler
{
	double Factor;
	double TargetFactor;
	double Accel;
	double Twist;
	double TwistAccel;
	tjs_int CenterX;
	tjs_int CenterY;
	bool FixSrc1;

public:
	tShimRotateZoomTransHandler(tjs_uint64 time, tjs_int width,
		tjs_int height, double factor, double targetfactor, double accel,
		double twist, double twistaccel, tjs_int centerx, tjs_int centery,
		bool fixsrc1)
		: tShimBaseRotateTransHandler(time, width, height, 0)
	{
		Factor = factor;
		TargetFactor = targetfactor;
		Accel = accel;
		Twist = twist;
		TwistAccel = twistaccel;
		CenterX = centerx;
		CenterY = centery;
		FixSrc1 = fixsrc1;
	}

	// rotatetrans.cpp:57-118.
	virtual void CalcPosition()
	{
		tShimPoint points[3];

		// src1: the full image, fixed in place.
		points[0].x = 0; points[0].y = 0;
		points[1].x = Width - 1; points[1].y = 0;
		points[2].x = 0; points[2].y = Height - 1;
		AddSource(points, FixSrc1 ? 1 : 2);

		// src2: rotated and zoomed.
		float zm = (float)(tjs_int)CurTime / (float)(tjs_int)Time;
		float tm = zm;
		if (Accel < 0)
		{
			// rotatetrans.cpp:74-80. Ease-out (fast then slow).
			zm = 1.0f - zm;
			zm = powf(zm, (float)(-Accel));
			zm = 1.0f - zm;
		}
		else if (Accel > 0)
		{
			// rotatetrans.cpp:81-85. Ease-in (slow then fast).
			zm = powf(zm, (float)Accel);
		}

		tjs_int scx = Width / 2;
		tjs_int scy = Height / 2;
		tjs_int cx = (tjs_int)((scx - CenterX) * zm + CenterX);
		tjs_int cy = (tjs_int)((scy - CenterY) * zm + CenterY);

		if (TwistAccel < 0)
		{
			tm = 1.0f - tm;
			tm = powf(tm, (float)(-TwistAccel));
			tm = 1.0f - tm;
		}
		else if (TwistAccel > 0)
		{
			tm = powf(tm, (float)TwistAccel);
		}

		float rad = CurTime == Time ? 0 :
			(float)(2 * 3.14159265368979) * (float)Twist * tm;
		zm = (float)((TargetFactor - Factor) * zm + Factor);
		float s = sinf(rad) * zm;
		float c = cosf(rad) * zm;

		// rotatetrans.cpp:110-115. The four corners of the rotated/scaled
		// source image, mapped into destination space.
		points[0].x = (tjs_int)(-cx * c + -cy * s + scx);
		points[0].y = (tjs_int)(-cx * -s + -cy * c + scy);
		points[1].x = (tjs_int)((Width - 1 - cx) * c + -cy * s + scx);
		points[1].y = (tjs_int)((Width - 1 - cx) * -s + -cy * c + scy);
		points[2].x = (tjs_int)(-cx * c + (Height - 1 - cy) * s + scx);
		points[2].y = (tjs_int)(-cx * -s + (Height - 1 - cy) * c + scy);

		AddSource(points, FixSrc1 ? 2 : 1);
	}
};

// rotatetrans.cpp:318-392. src1 recedes while src2 approaches, both rotating.
// The two halves of the transition use different zoom curves and meet at the
// midpoint. The while/switch/case fallthrough is upstream's structure and is
// reproduced verbatim.
class tShimRotateSwapTransHandler : public tShimBaseRotateTransHandler
{
	float Twist;

public:
	tShimRotateSwapTransHandler(tjs_uint64 time, tjs_int width,
		tjs_int height, tjs_uint32 bgcolor, double twist)
		: tShimBaseRotateTransHandler(time, width, height, bgcolor)
	{
		Twist = (float)(twist * 3.14159265368979 * 2);
	}

	// rotatetrans.cpp:333-390.
	virtual void CalcPosition()
	{
		tShimPoint points[3];
		float rad;
		tjs_int cx, cy;
		tjs_int scx = Width / 2;
		tjs_int scy = Height / 2;
		float zm = (float)(tjs_int)CurTime / (float)(tjs_int)Time;
		float tm, s, c;

		tjs_int cnt = 2;

		switch (CurTime >= Time / 2 ? 0 : 1)
		{
			while (true)
			{
			case 0:
				// src1 receding (second half of transition).
				tm = zm * zm;
				cx = (tjs_int)((-scx) * tm + scx +
					sin(tm * (float)3.14159265368979) * scx * 1.5f);
				cy = (tjs_int)((-scy) * tm + scy);
				rad = tm * Twist;
				tm = 1.0f - tm;
				s = sinf(rad) * tm;
				c = cosf(rad) * tm;

				points[0].x = (tjs_int)(-scx * c + -scy * s + cx);
				points[0].y = (tjs_int)((-scx * -s + -scy * c) * tm + cy);
				points[1].x = (tjs_int)((Width - 1 - scx) * c + -scy * s + cx);
				points[1].y = (tjs_int)(((Width - 1 - scx) * -s + -scy * c) * tm + cy);
				points[2].x = (tjs_int)(-scx * c + (Height - 1 - scy) * s + cx);
				points[2].y = (tjs_int)((-scx * -s + (Height - 1 - scy) * c) * tm + cy);
				AddSource(points, 1);
				cnt--;
				if (cnt == 0) break;

			case 1:
				// src2 approaching (first half of transition).
				tm = 1.0f - (1.0f - zm) * (1.0f - zm);
				cx = (tjs_int)((scx - (Width - 1)) * tm + (Width - 1) -
					sin(tm * (float)3.14159265368979) * scx * 1.5f);
				cy = (tjs_int)((scy - (Height - 1)) * tm + (Height - 1));
				rad = (-1.0f + tm) * Twist;
				s = sinf(rad) * tm;
				c = cosf(rad) * tm;

				points[0].x = (tjs_int)(-scx * c + -scy * s + cx);
				points[0].y = (tjs_int)((-scx * -s + -scy * c) * tm + cy);
				points[1].x = (tjs_int)((Width - 1 - scx) * c + -scy * s + cx);
				points[1].y = (tjs_int)(((Width - 1 - scx) * -s + -scy * c) * tm + cy);
				points[2].x = (tjs_int)(-scx * c + (Height - 1 - scy) * s + cx);
				points[2].y = (tjs_int)((-scx * -s + (Height - 1 - scy) * c) * tm + cy);
				AddSource(points, 2);
				cnt--;
				if (cnt == 0) break;
			}
		}
	}
};

// rotatetrans.cpp:122-220.
class tShimRotateZoomTransHandlerProvider : public iTVPTransHandlerProvider
{
	tjs_int RefCount;
public:
	tShimRotateZoomTransHandlerProvider() { RefCount = 1; }
	virtual ~tShimRotateZoomTransHandlerProvider() {}

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}
	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1) delete this;
		else RefCount--;
		return SHIM_TJS_S_OK;
	}

	// rotatetrans.cpp:152.
	virtual tjs_error SHIM_INTF_METHOD GetName(const tjs_char **name)
	{
		if (name)
		{
			*name = SHIM_TJS_W("rotatezoom");
			return SHIM_TJS_S_OK;
		}
		return SHIM_TJS_E_FAIL;
	}

	virtual tjs_error SHIM_INTF_METHOD StartTransition(
		iTVPSimpleOptionProvider *options,
		iTVPSimpleImageProvider *,
		tTVPLayerType,
		tjs_uint src1w, tjs_uint src1h,
		tjs_uint src2w, tjs_uint src2h,
		tTVPTransType *type,
		tTVPTransUpdateType *updatetype,
		iTVPBaseTransHandler **handler)
	{
		if (type) *type = ttExchange;
		if (updatetype) *updatetype = tutDivisible;
		if (!handler) return SHIM_TJS_E_FAIL;
		if (!options) return SHIM_TJS_E_FAIL;
		if (src1w != src2w || src1h != src2h)
			return SHIM_TJS_E_FAIL;

		// rotatetrans.cpp:181-186. Defaults.
		tjs_int64 time;
		// factor/accel/twist/twistaccel are real-valued upstream, read
		// through GetValue (tTJSVariant). GetAsNumber returns int64, so
		// fractional values are truncated — same caveat as wave's maxomega.
		// Fate never passes these (no rotatezoom call site in the corpus),
		// so the defaults are what the game gets.
		double factor = 1;
		double accel = 0;
		double twist = 2;
		double twistaccel = -2;
		tjs_int centerx = src1w / 2;
		tjs_int centery = src1h / 2;

		if (!ShimOption(options, SHIM_TJS_W("time"), &time))
			return SHIM_TJS_E_FAIL;
		if (time < 2) time = 2;

		tjs_int64 tmp;
		if (ShimOption(options, SHIM_TJS_W("factor"), &tmp))
			factor = (double)tmp;
		if (ShimOption(options, SHIM_TJS_W("accel"), &tmp))
			accel = (double)tmp;
		if (ShimOption(options, SHIM_TJS_W("twist"), &tmp))
			twist = (double)tmp;
		if (ShimOption(options, SHIM_TJS_W("twistaccel"), &tmp))
			twistaccel = (double)tmp;
		if (ShimOption(options, SHIM_TJS_W("centerx"), &tmp))
			centerx = (tjs_int)tmp;
		if (ShimOption(options, SHIM_TJS_W("centery"), &tmp))
			centery = (tjs_int)tmp;

		// rotatetrans.cpp:213-215. targetfactor=1, FixSrc1=true.
		*handler = new tShimRotateZoomTransHandler((tjs_uint64)time,
			(tjs_int)src1w, (tjs_int)src1h, factor, 1, accel, twist,
			twistaccel, centerx, centery, true);
		return SHIM_TJS_S_OK;
	}
};

// rotatetrans.cpp:222-316.
class tShimRotateVanishTransHandlerProvider : public iTVPTransHandlerProvider
{
	tjs_int RefCount;
public:
	tShimRotateVanishTransHandlerProvider() { RefCount = 1; }
	virtual ~tShimRotateVanishTransHandlerProvider() {}

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}
	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1) delete this;
		else RefCount--;
		return SHIM_TJS_S_OK;
	}

	// rotatetrans.cpp:252.
	virtual tjs_error SHIM_INTF_METHOD GetName(const tjs_char **name)
	{
		if (name)
		{
			*name = SHIM_TJS_W("rotatevanish");
			return SHIM_TJS_S_OK;
		}
		return SHIM_TJS_E_FAIL;
	}

	virtual tjs_error SHIM_INTF_METHOD StartTransition(
		iTVPSimpleOptionProvider *options,
		iTVPSimpleImageProvider *,
		tTVPLayerType,
		tjs_uint src1w, tjs_uint src1h,
		tjs_uint src2w, tjs_uint src2h,
		tTVPTransType *type,
		tTVPTransUpdateType *updatetype,
		iTVPBaseTransHandler **handler)
	{
		if (type) *type = ttExchange;
		if (updatetype) *updatetype = tutDivisible;
		if (!handler) return SHIM_TJS_E_FAIL;
		if (!options) return SHIM_TJS_E_FAIL;
		if (src1w != src2w || src1h != src2h)
			return SHIM_TJS_E_FAIL;

		// rotatetrans.cpp:281-285. Defaults: no factor (vanish always goes
		// from 1 to 0). Same real-valued option caveat as rotatezoom.
		tjs_int64 time;
		double accel = 2;
		double twist = 2;
		double twistaccel = 2;
		tjs_int centerx = src1w / 2;
		tjs_int centery = src1h / 2;

		if (!ShimOption(options, SHIM_TJS_W("time"), &time))
			return SHIM_TJS_E_FAIL;
		if (time < 2) time = 2;

		tjs_int64 tmp;
		if (ShimOption(options, SHIM_TJS_W("accel"), &tmp))
			accel = (double)tmp;
		if (ShimOption(options, SHIM_TJS_W("twist"), &tmp))
			twist = (double)tmp;
		if (ShimOption(options, SHIM_TJS_W("twistaccel"), &tmp))
			twistaccel = (double)tmp;
		if (ShimOption(options, SHIM_TJS_W("centerx"), &tmp))
			centerx = (tjs_int)tmp;
		if (ShimOption(options, SHIM_TJS_W("centery"), &tmp))
			centery = (tjs_int)tmp;

		// rotatetrans.cpp:309-311. Reuses tShimRotateZoomTransHandler with
		// factor=1, targetfactor=0, FixSrc1=false: src1 zooms out to nothing
		// while src2 stays fixed.
		*handler = new tShimRotateZoomTransHandler((tjs_uint64)time,
			(tjs_int)src1w, (tjs_int)src1h, 1, 0, accel, twist,
			twistaccel, centerx, centery, false);
		return SHIM_TJS_S_OK;
	}
};

// rotatetrans.cpp:394-475.
class tShimRotateSwapTransHandlerProvider : public iTVPTransHandlerProvider
{
	tjs_int RefCount;
public:
	tShimRotateSwapTransHandlerProvider() { RefCount = 1; }
	virtual ~tShimRotateSwapTransHandlerProvider() {}

	virtual tjs_error SHIM_INTF_METHOD AddRef()
	{
		RefCount++;
		return SHIM_TJS_S_OK;
	}
	virtual tjs_error SHIM_INTF_METHOD Release()
	{
		if (RefCount == 1) delete this;
		else RefCount--;
		return SHIM_TJS_S_OK;
	}

	// rotatetrans.cpp:424.
	virtual tjs_error SHIM_INTF_METHOD GetName(const tjs_char **name)
	{
		if (name)
		{
			*name = SHIM_TJS_W("rotateswap");
			return SHIM_TJS_S_OK;
		}
		return SHIM_TJS_E_FAIL;
	}

	virtual tjs_error SHIM_INTF_METHOD StartTransition(
		iTVPSimpleOptionProvider *options,
		iTVPSimpleImageProvider *,
		tTVPLayerType,
		tjs_uint src1w, tjs_uint src1h,
		tjs_uint src2w, tjs_uint src2h,
		tTVPTransType *type,
		tTVPTransUpdateType *updatetype,
		iTVPBaseTransHandler **handler)
	{
		if (type) *type = ttExchange;
		if (updatetype) *updatetype = tutDivisible;
		if (!handler) return SHIM_TJS_E_FAIL;
		if (!options) return SHIM_TJS_E_FAIL;
		if (src1w != src2w || src1h != src2h)
			return SHIM_TJS_E_FAIL;

		// rotatetrans.cpp:453-454. Defaults.
		tjs_int64 time;
		tjs_int64 bgcolor = 0;
		double twist = 1;

		if (!ShimOption(options, SHIM_TJS_W("time"), &time))
			return SHIM_TJS_E_FAIL;
		if (time < 2) time = 2;

		ShimOption(options, SHIM_TJS_W("bgcolor"), &bgcolor);

		tjs_int64 tmp;
		if (ShimOption(options, SHIM_TJS_W("twist"), &tmp))
			twist = (double)tmp;

		// rotatetrans.cpp:469-470.
		*handler = new tShimRotateSwapTransHandler((tjs_uint64)time,
			(tjs_int)src1w, (tjs_int)src1h, (tjs_uint32)bgcolor, twist);
		return SHIM_TJS_S_OK;
	}
};

//---------------------------------------------------------------------------
// Registration state
//---------------------------------------------------------------------------
// File-static, owned by this translation unit (BuiltinShims.h state contract,
// rule 1). Null means "not registered right now", which is also what makes the
// rollback in V2Link and the removal in V2Unlink idempotent.
static tShimMosaicTransHandlerProvider *g_mosaic_provider = 0;
static tShimWaveTransHandlerProvider *g_wave_provider = 0;
static tShimRippleTransHandlerProvider *g_ripple_provider = 0;
static tShimTurnTransHandlerProvider *g_turn_provider = 0;
static tShimRotateZoomTransHandlerProvider *g_rotatezoom_provider = 0;
static tShimRotateVanishTransHandlerProvider *g_rotatevanish_provider = 0;
static tShimRotateSwapTransHandlerProvider *g_rotateswap_provider = 0;

// Undo exactly what V2Link did. Safe to call when nothing is installed, so
// both the failure path and V2Unlink can use it. Removes in reverse order of
// registration, matching upstream's Unregister* (Main.cpp:45-49).
static void ShimRemoveProviders()
{
	if (g_rotateswap_provider)
	{
		if (g_remove_provider)
			g_remove_provider(g_rotateswap_provider);
		g_rotateswap_provider->Release();
		g_rotateswap_provider = 0;
	}
	if (g_rotatevanish_provider)
	{
		if (g_remove_provider)
			g_remove_provider(g_rotatevanish_provider);
		g_rotatevanish_provider->Release();
		g_rotatevanish_provider = 0;
	}
	if (g_rotatezoom_provider)
	{
		if (g_remove_provider)
			g_remove_provider(g_rotatezoom_provider);
		g_rotatezoom_provider->Release();
		g_rotatezoom_provider = 0;
	}
	if (g_turn_provider)
	{
		if (g_remove_provider)
			g_remove_provider(g_turn_provider);
		g_turn_provider->Release();
		g_turn_provider = 0;
	}
	if (g_ripple_provider)
	{
		if (g_remove_provider)
			g_remove_provider(g_ripple_provider);
		g_ripple_provider->Release();
		// Clear the ripple table cache on teardown, as upstream does in
		// UnregisterRippleTransHandlerProvider (ripple.cpp:1557).
		ShimClearRippleTableCache();
		g_ripple_provider = 0;
	}
	if (g_wave_provider)
	{
		if (g_remove_provider)
			g_remove_provider(g_wave_provider);
		g_wave_provider->Release();
		g_wave_provider = 0;
	}
	if (g_mosaic_provider)
	{
		if (g_remove_provider)
			g_remove_provider(g_mosaic_provider);
		// Releases the plugin's own reference; the registry held the other one.
		// (mosaic.cpp:464-470.)
		g_mosaic_provider->Release();
		g_mosaic_provider = 0;
	}
}

//---------------------------------------------------------------------------
// The plugin ABI
//---------------------------------------------------------------------------
// extern "C" is required in addition to SHIM_EXPORT, because this is a .cpp: the
// built-in build reaches the entry points through names the registry declares
// with C linkage (BuiltinShims.cpp: SHIM_LINK(extrans)), and C++ name mangling
// would make those names not match. shim_abi.h renames V2Link per shim in that
// build but does not itself apply C linkage. Nesting the two is legal.
extern "C" {

// Register all seven transitions the upstream plugin registers, in the same
// order upstream's V2Link does (Main.cpp:32-36). The decompiled V2Link at
// 0x10001000 makes five calls; one of them (the rotate registrar) spawns
// three providers internally (rotatetrans.cpp:487-493), so seven providers
// total: mosaic, wave, turn, ripple, rotatezoom, rotatevanish, rotateswap.
SHIM_EXPORT SHIM_HIDDEN int V2Link(iTVPFunctionExporter *exporter)
{
	// Resolve everything this shim needs from the engine by name. This is what
	// the original did too (tp_stub resolved each engine function through the
	// same exporter), and it is what keeps the file buildable on both paths.
	g_fill_argb = (tFillARGB)ShimResolve(exporter,
		"void ::TVPFillARGB(tjs_uint32 *,tjs_int,tjs_uint32)");
	g_blend_sd = (tConstAlphaBlendSD)ShimResolve(exporter,
		"void ::TVPConstAlphaBlend_SD(tjs_uint32 *,const tjs_uint32 *,const tjs_uint32 *,tjs_int,tjs_int)");
	g_blend_sd_d = (tConstAlphaBlendSD)ShimResolve(exporter,
		"void ::TVPConstAlphaBlend_SD_d(tjs_uint32 *,const tjs_uint32 *,const tjs_uint32 *,tjs_int,tjs_int)");
	g_blend_sd_a = (tConstAlphaBlendSD)ShimResolve(exporter,
		"void ::TVPConstAlphaBlend_SD_a(tjs_uint32 *,const tjs_uint32 *,const tjs_uint32 *,tjs_int,tjs_int)");
	// The turn and rotate handlers use these affine/ stretch copy primitives
	// (turn.cpp:465-467, rotatebase.cpp:157-176). Resolved by name the same
	// way as the blend primitives above.
	g_stretch_copy = (tStretchCopy)ShimResolve(exporter,
		"void ::TVPStretchCopy(tjs_uint32 *,tjs_int,const tjs_uint32 *,tjs_int,tjs_int)");
	g_lintrans_copy = (tLinTransCopy)ShimResolve(exporter,
		"void ::TVPLinTransCopy(tjs_uint32 *,tjs_int,const tjs_uint32 *,tjs_int,tjs_int,tjs_int,tjs_int,tjs_int)");
	g_add_provider = (tAddTransHandlerProvider)ShimResolve(exporter,
		"void ::TVPAddTransHandlerProvider(iTVPTransHandlerProvider *)");
	g_remove_provider = (tRemoveTransHandlerProvider)ShimResolve(exporter,
		"void ::TVPRemoveTransHandlerProvider(iTVPTransHandlerProvider *)");

	// Without the pixel primitives the handlers would write nothing (or crash),
	// and without the registration calls nothing would happen at all. Fail the
	// link instead: the engine logs that the shim did not link, which is a
	// diagnosable outcome, rather than a transition that silently does nothing.
	if (!g_fill_argb || !g_blend_sd || !g_blend_sd_d || !g_blend_sd_a ||
		!g_stretch_copy || !g_lintrans_copy ||
		!g_add_provider || !g_remove_provider)
	{
		ShimRemoveProviders();
		return -1;
	}

	// Register all seven, and undo what landed if a later one fails.
	// TVPAddTransHandlerProvider throws on a duplicate or a bad provider
	// (TransIntf.cpp:307-324), and the contract requires that whatever link
	// installs, unlink can remove — so a half-finished link must leave nothing
	// behind.
	try
	{
		g_mosaic_provider = new tShimMosaicTransHandlerProvider();
		g_add_provider(g_mosaic_provider);

		g_wave_provider = new tShimWaveTransHandlerProvider();
		g_add_provider(g_wave_provider);

		g_turn_provider = new tShimTurnTransHandlerProvider();
		g_add_provider(g_turn_provider);

		g_ripple_provider = new tShimRippleTransHandlerProvider();
		g_add_provider(g_ripple_provider);

		// The rotate family: three providers from one registrar
		// (rotatetrans.cpp:487-493).
		g_rotatezoom_provider = new tShimRotateZoomTransHandlerProvider();
		g_add_provider(g_rotatezoom_provider);

		g_rotatevanish_provider = new tShimRotateVanishTransHandlerProvider();
		g_add_provider(g_rotatevanish_provider);

		g_rotateswap_provider = new tShimRotateSwapTransHandlerProvider();
		g_add_provider(g_rotateswap_provider);
	}
	catch (...)
	{
		ShimRemoveProviders();
		return -1;
	}

	return SHIM_TJS_S_OK;
}

// Symmetric with V2Link: remove what it registered, in reverse.
// (decompiled V2Unlink at 0x10001030; upstream extrans/Main.cpp:41-56.)
SHIM_EXPORT SHIM_HIDDEN int V2Unlink(void)
{
	ShimRemoveProviders();

	// Drop the resolved pointers as well: the exporter they came from belongs to
	// an engine that may be gone by the time a later link happens.
	g_fill_argb = 0;
	g_blend_sd = 0;
	g_blend_sd_d = 0;
	g_blend_sd_a = 0;
	g_stretch_copy = 0;
	g_lintrans_copy = 0;
	g_add_provider = 0;
	g_remove_provider = 0;

	return SHIM_TJS_S_OK;
}

} // extern "C"
