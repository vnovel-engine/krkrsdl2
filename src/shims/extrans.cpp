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
// Scope: why this file exists at all, and why only two names
// ---------------------------------------------------------
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
//
// ...and neither name resolves. KAGLayer.tjs:189-233 forwards `method` to
// Layer.beginTransition, which reaches tTJSNI_BaseLayer::StartTransition, whose
// first action is TVPFindTransHandlerProvider(name) (LayerIntf.cpp:6781). The
// registry holds only the engine's own crossfade/universal/scroll
// (TransIntf.cpp:1196-1213), so the lookup throws TVPCannotFindTransHander
// ("Cannot find transition handler %1", src/core/msg/sdl2/MsgLoad.cpp:162).
// Nothing in TJS catches it around the trans path, so it reaches Conductor's
// timerCallback catch (system/Conductor.tjs:163) and aborts the scene. So the
// no-op disposition is wrong for these two names specifically: without them the
// @prick scenes and the @wave scenes die at the transition.
//
// The other five names the upstream plugin registers — `ripple`, `turn`,
// `rotatezoom`, `rotatevanish`, `rotateswap` (ripple.cpp:1448, turn.cpp:526,
// rotatetrans.cpp:152/252/424, registered at ripple.cpp:1547, turn.cpp:579,
// rotatetrans.cpp:488-494) — appear nowhere in the extracted corpus
// (`grep -rniE 'ripple|rotatezoom|rotatevanish|rotateswap' ~/dev/fate-scripts`
// is empty, and there is no `method=turn`). They are deliberately NOT ported:
// ripple alone drags in ~1500 lines plus large generated tables
// (turntrans_table.cpp is 4360 lines), and none of it can ever run here.
// This file therefore provides exactly the two live names.
//
// (Worth recording, because it corroborates that the install copy is an older
// upstream revision: the *binary's* fifth name is not `turn`. The name pointer
// at 0x10033636 resolves to the UTF-16 bytes `)turn`, while the current source
// says TJS_W("turn") (turn.cpp:526). The decompilation's name table normalises
// it to `turn`. Neither spelling is requested by any script, so it changes
// nothing here — but the port must not assume the binary's names match the
// source tree's character for character.)
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

static tFillARGB g_fill_argb = 0;
static tConstAlphaBlendSD g_blend_sd = 0;
static tConstAlphaBlendSD g_blend_sd_d = 0;
static tConstAlphaBlendSD g_blend_sd_a = 0;
static tAddTransHandlerProvider g_add_provider = 0;
static tRemoveTransHandlerProvider g_remove_provider = 0;

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
// Small helpers shared by the two handlers
//---------------------------------------------------------------------------

// Clip a segment [l,r) to [cl,cr). False when nothing remains.
// Ported from extrans/common.h:16-24, which both upstream handlers include.
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

//---------------------------------------------------------------------------
// Registration state
//---------------------------------------------------------------------------
// File-static, owned by this translation unit (BuiltinShims.h state contract,
// rule 1). Null means "not registered right now", which is also what makes the
// rollback in V2Link and the removal in V2Unlink idempotent.
static tShimMosaicTransHandlerProvider *g_mosaic_provider = 0;
static tShimWaveTransHandlerProvider *g_wave_provider = 0;

// Undo exactly what the two Install* below did. Safe to call when nothing is
// installed, so both the failure path and V2Unlink can use it.
static void ShimRemoveProviders()
{
	if (g_mosaic_provider)
	{
		if (g_remove_provider)
			g_remove_provider(g_mosaic_provider);
		// Releases the plugin's own reference; the registry held the other one.
		// (mosaic.cpp:464-470.)
		g_mosaic_provider->Release();
		g_mosaic_provider = 0;
	}
	if (g_wave_provider)
	{
		if (g_remove_provider)
			g_remove_provider(g_wave_provider);
		g_wave_provider->Release();
		g_wave_provider = 0;
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

// Register the two transitions the game asks for, the way the original's V2Link
// registered its five (decompiled V2Link at 0x10001000; upstream
// extrans/Main.cpp:29-38).
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
	g_add_provider = (tAddTransHandlerProvider)ShimResolve(exporter,
		"void ::TVPAddTransHandlerProvider(iTVPTransHandlerProvider *)");
	g_remove_provider = (tRemoveTransHandlerProvider)ShimResolve(exporter,
		"void ::TVPRemoveTransHandlerProvider(iTVPTransHandlerProvider *)");

	// Without the pixel primitives the handlers would write nothing (or crash),
	// and without the registration calls nothing would happen at all. Fail the
	// link instead: the engine logs that the shim did not link, which is a
	// diagnosable outcome, rather than a transition that silently does nothing.
	if (!g_fill_argb || !g_blend_sd || !g_blend_sd_d || !g_blend_sd_a ||
		!g_add_provider || !g_remove_provider)
	{
		ShimRemoveProviders();
		return -1;
	}

	// Register both, and undo the first if the second fails. TVPAddTransHandlerProvider
	// throws on a duplicate or a bad provider (TransIntf.cpp:307-324), and the
	// contract requires that whatever link installs, unlink can remove — so a
	// half-finished link must leave nothing behind.
	try
	{
		g_mosaic_provider = new tShimMosaicTransHandlerProvider();
		g_add_provider(g_mosaic_provider);

		g_wave_provider = new tShimWaveTransHandlerProvider();
		g_add_provider(g_wave_provider);
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
	g_add_provider = 0;
	g_remove_provider = 0;

	return SHIM_TJS_S_OK;
}

} // extern "C"
