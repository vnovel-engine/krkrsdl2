//---------------------------------------------------------------------------
// shims/firespark.cpp — Fate/stay night's `firespark.dll`, replaced.
//
// What the plugin was
// -------------------
// `firespark.dll` is NOT stock KiriKiri: it is absent from the upstream tree
// (no entry in bin/win32/plugin or src/plugins/win32) and ships with Fate. It
// registers four global TJS functions and draws a time- and RNG-driven particle
// "fire spark" directly into a Layer's own pixel buffer, the same
// mainImageBuffer* mechanism drawer.dll uses.
//
// Recovered from the binary
// -------------------------
// The port below is reconstructed from the shipped DLL, not from source. Where
// a line's behaviour is taken straight from the disassembly or from Ghidra's
// decompilation of a named function, the comment names the address, e.g.
// "(0x100017d1, FUN_100014c0)" — FUN_100014c0/10001f50/10002de0/10002e20 are
// drawFireSpark / initFireSpark / finishFireSpark / changeFireSpark, which
// Ghidra had left undefined because they are only reached through the global
// dispatch's member table; they were recovered by force-disassembling the
// region 0x100014c0..0x10003300.
//
// Note the original's own argument order, which the export notes describe more
// loosely than the code does:
//
//     initFireSpark(layer, options)     <- two arguments (0x10001fce, 0x10001ff1)
//     changeFireSpark(options)          <- one, re-reads options.layer (0x10002e80)
//     drawFireSpark([clock])            <- optional object with a .tick member
//     finishFireSpark()                 <- no arguments
//
// One source, two consumers: see shim_abi.h. Built into the engine the four
// names are reached through the override table's `@builtin:firespark`; built as
// a PE it is the plugin the game links.
//
// State contract (BuiltinShims.h)
// -------------------------------
// Everything the effect owns is in this translation unit and is allocated by
// link()/initFireSpark and released by unlink()/finishFireSpark. No engine
// global is assigned. What is registered with the engine is unregistered by
// V2Unlink, through the engine's own helpers.
//
// Pixel-surface contract
// ----------------------
// The surface is the Layer's own buffer, reached through the Layer's TJS
// properties exactly as the original did (`mainImageBufferForWrite`,
// `mainImageBufferPitch`, `imageWidth`, `imageHeight`). Nothing here crosses a
// host boundary, so no host callback is involved. The assumptions, stated once:
//
//   * the buffer holds 32bpp ARGB words, i.e. 0x00RRGGBB as a little-endian
//     word (bytes B,G,R,A in memory). The original packs its palette exactly
//     that way (0x10002c2d: bits 16-23 from table[+0], 8-15 from table[+1],
//     0-7 from table[+2]);
//   * rows are addressed by a BYTE pitch — `mainImageBufferPitch` maps to
//     Layer::GetPitchBytes() (visual/LayerIntf.cpp:2688), so the accessor's
//     "Pixel" in the name is misleading. Rows are padded; never assume
//     width * 4, and never index by a word stride.
//
// Every write is clipped to the layer rectangle, so a hostile option value can
// only produce an empty or offset effect, never an out-of-bounds store.
//---------------------------------------------------------------------------

// The shim's registry name. shim_abi.h turns this into the per-shim entry point
// symbols when built into the engine; see that header for why.
#define SHIM_NAME firespark
#include "shim_abi.h"

#include "tp_stub.h"

// TVPGetRandomBits128 (utils/Random.h) and TVPGetTickCount (utils/TickCount.h)
// are the two engine functions the original resolved through the exporter; the
// effect is time- and RNG-driven, so the same sources keep it comparable.
// TickCount.h comes in through tp_stub.h; Random.h is not part of that set.
#include "Random.h"

#include <cmath>
#include <cstring>
#include <new>

//---------------------------------------------------------------------------
// Exported-name check
//
// The shim depends on engine functions that a standalone PE plugin resolves
// through V2Link's exporter. TP_stub's integrated-plugin build gives them as
// direct symbols, and the same two are in the engine's plugin export table, so
// a plugin built from this source finds them the ordinary way:
//
//   TVPGetTickCount      crkrz/utils/TickCount.h,   engine export (FuncStubs.cpp:1552)
//   TVPGetRandomBits128  crkrz/utils/Random.h,      engine export (FuncStubs.cpp:1534)
//
// Both are __cdecl and take no engine type the caller must construct, so the
// dependency is the same on both paths.
//---------------------------------------------------------------------------

namespace {

//---------------------------------------------------------------------------
// TJS error codes used below. Values taken from tjsErrorDefs.h; named here only
// so the returns read as what the original returned (0xfffffc17 / 0xfffffc14).
//---------------------------------------------------------------------------
const tjs_error kESucceed = TJS_S_OK;         // 0
const tjs_error kEBadParamCount = -1004;      // TJS_E_BADPARAMCOUNT  (0xfffffc14)
const tjs_error kEMemberNotFound = -1001;     // TJS_E_MEMBERNOTFOUND (0xfffffc17)

//---------------------------------------------------------------------------
// The effect's own state.
//
// The original kept this in its .data image: the option values at 0x1000f040
// onwards, the live state at 0x10012a38..0x10012ec4. Same split here, but as a
// single struct owned by this file so nothing is shared and nothing is an
// engine global. Two shims can coexist precisely because this type is local.
//---------------------------------------------------------------------------

// One particle. The original's record is 20 dwords (0x50 bytes: the stride at
// 0x10001a8a, and the fields are addressed off `esi = record + 8`). The names
// are the roles recovered from the spawn and update code.
struct Particle {
	tjs_int x, y;          // position, px.                (0x100017c3: [esi-8], [esi-4])
	tjs_int dx, dy;        // velocity, px per tick.       (0x1000180b: [esi], [esi+4])
	tjs_int fx, fy;        // sub-pixel accumulators.      (0x1000184c: [esi+8], [esi+0xc])
	tjs_int lifetime;      // total life, in ticks.        (0x10001868: [esi+0x10])
	tjs_int _pad;          //                              (unused in the original)
	tjs_int bornLo, bornHi;// birth tick, 64-bit.          (0x10001a63: [esi+0x18..0x24])
	tjs_int accel;         // per-tick acceleration.       (0x10001881: [esi+0x28])
	tjs_int w, h;          // size, px.                    (0x100018a7/0x10001a58: [esi+0x2c/0x30])
	tjs_int rectX, rectY;  // last drawn rect, for erase.  (0x10001c7d: [esi+0x34..0x40])
	tjs_int rectW, rectH;
	tjs_int active;        // 1 while live.                (0x10001a6f: [esi+0x44])
};

// The option set, with the values the DLL's .data image holds as defaults
// (0x1000f040..0x1000f0a4). A missing key leaves the default in place, which is
// what the original's "only assign when the member exists" tests did.
struct Options {
	tjs_int maxgen;                 // 0x1000f044, default 500, capped at 500
	tjs_int basevelocity;           // 0x1000f048, default 5504  (angle-based, 1.0 == 65535)
	tjs_int velocityrange;          // 0x1000f04c, default 2359
	tjs_int baselifetime;           // 0x1000f050, default 1500
	tjs_int lifetimerange;          // 0x1000f054, default 2500
	tjs_int basetop;                // 0x1000f058, default -50
	tjs_int heightrange;            // 0x1000f05c, default -50
	tjs_int basesizew;              // 0x1000f060, default 2
	tjs_int sizewrange;             // 0x1000f064, default 4
	tjs_int basesizeh;              // 0x1000f068, default 2
	tjs_int sizehrange;             // 0x1000f06c, default 4
	tjs_int windvelocity;           // 0x10012eac, default 0
	tjs_real windvariancecapability;// 0x1000f070, default 0.0003
	tjs_real maxwindvelocity;       // 0x1000f078, default 0.2
	tjs_int samesize;               // 0x1000f080, default 1
	tjs_real directionbase;         // 0x10012a70, default 0.0
	tjs_real directionrange;        // 0x10012e80, default 0.0
	tjs_int baseaccel;              // 0x10012ea4, default -50
	tjs_int accelrange;             // 0x10012ea8, default -50
	tjs_int transtime;              // 0x10012a6c, default 5000
	tjs_int pause;                  // 0x10012ea1, default 0
	tjs_int effect;                 // 0x1000f040, default 1

	// `colortable` is a comma-separated list of decimal channel triples, three
	// characters per colour, read as (R,G,B) (0x1000298b-0x10002b9f: it walks
	// the string, splits on ',', takes each token's integer value, and stores a
	// byte per channel). The DLL's default is the 7 colours
	// ff ff 00 / ff c0 00 / ff 80 00 / ff 40 00 / ff 20 00 / c0 00 00 / 00 00 00
	// at 0x1000f084 — yellow through orange and red to black.
	tjs_uint8 colors[3 * 64];
	tjs_int ncolors;

	explicit Options() { Reset(); }

	void Reset()
	{
		maxgen = 500;
		basevelocity = 5504;
		velocityrange = 2359;
		baselifetime = 1500;
		lifetimerange = 2500;
		basetop = -50;
		heightrange = -50;
		basesizew = 2;
		sizewrange = 4;
		basesizeh = 2;
		sizehrange = 4;
		windvelocity = 0;
		windvariancecapability = 0.0003;
		maxwindvelocity = 0.2;
		samesize = 1;
		directionbase = 0.0;
		directionrange = 0.0;
		baseaccel = -50;
		accelrange = -50;
		transtime = 5000;
		pause = 0;
		effect = 1;

		static const tjs_uint8 kDefaultColors[] = {
			0xff, 0xff, 0x00, 0xff, 0xc0, 0x00, 0xff, 0x80, 0x00, 0xff, 0x40, 0x00,
			0xff, 0x20, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		std::memcpy(colors, kDefaultColors, sizeof(kDefaultColors));
		ncolors = sizeof(kDefaultColors) / 3;
	}
};

// The live effect. One instance per process, as the original had one global set.
struct Effect {
	bool live;                     // 0x10012ea0
	bool paused;                   // 0x10012ea1
	bool effectEnabled;            // 0x1000f040
	bool transitioning;            // 0x10012e8c

	iTJSDispatch2 *layer;          // 0x10012a50, addref'd while held
	tjs_int width;                 // 0x10012e88
	tjs_int height;                // 0x10012a5c
	tjs_int pitch;                 // 0x10012e9c, BYTES per row
	tjs_uint32 *pixels;            // 0x10012a48, mainImageBufferForWrite
	tjs_uint32 clearColor;         // 0x10012eb4, the layer's own first pixel

	Options opt;
	Particle *particles;           // 0x10012e78, 0x50 * maxgen bytes
	tjs_int capacity;              // slots allocated in `particles`
	tjs_int liveCount;             // 0x10012a4c
	tjs_uint32 palette[256];       // 0x10012a78, built once per init

	tjs_int spawnRate;             // 0x10012e98, particles per tick, 16.16
	tjs_uint32 spawnAccum;         // 0x10012a58, spawn accumulator

	tjs_int transMaxGen;           // 0x10012a44, target of the current transition
	tjs_int transStartTick;        // 0x10012a60, the tick the transition began
	tjs_int transBaseMaxGen;       // 0x10012e90, the bound the transition started from

	// The live spawn bound. Distinct from opt.maxgen, which is the tuning value:
	// during a transition this walks towards transMaxGen, and it is what both the
	// spawn loop and the capacity check use, so a tuning value that outruns the
	// allocation cannot be reached.
	tjs_int limit;

	tjs_uint64 lastTick;           // 0x10012a38/0x10012a3c, last advanced tick

	// noise (0x10012eac/0x10012eb0): wind speed and its per-tick delta
	tjs_int windSpeed;
	tjs_int windDelta;

	Effect()
		: live(false), paused(false), effectEnabled(true), transitioning(false),
		  layer(NULL), width(0), height(0), pitch(0), pixels(NULL),
		  clearColor(0), particles(NULL), capacity(0), liveCount(0), spawnRate(0), spawnAccum(0),
		  transMaxGen(0), transStartTick(0), transBaseMaxGen(0), limit(0), lastTick(0),
		  windSpeed(0), windDelta(0)
	{
		std::memset(palette, 0, sizeof(palette));
	}

	// Release everything the effect owns. Called by finishFireSpark and, for
	// anything still live, by V2Unlink — a plugin may be unloaded mid-effect.
	void Release()
	{
		delete[] particles;
		particles = NULL;
		capacity = 0;
		if (layer) { layer->Release(); layer = NULL; }
		pixels = NULL;
		pitch = width = height = 0;
		liveCount = 0;
		limit = 0;
		live = false;
	}

	// The hard bound for every loop that walks `particles`. `opt.maxgen` is the
	// tuning target and can exceed the allocation after changeFireSpark raises it,
	// so the loops must be bounded by what exists, not by what was asked for.
	tjs_int slots() const { return capacity; }

	// Grow (or drop) the particle array to `n` slots, discarding whatever was
	// live. Used by initFireSpark and when changeFireSpark re-targets maxgen.
	bool Resize(tjs_int n)
	{
		Particle *fresh = NULL;
		if (n > 0)
		{
			fresh = new (std::nothrow) Particle[n];
			if (!fresh) return false;
			std::memset(fresh, 0, sizeof(Particle) * (size_t)n);
		}
		delete[] particles;
		particles = fresh;
		capacity = n;
		liveCount = 0;
		return true;
	}
};

Effect g_effect;

// Reads the option dictionary into `o`, leaving any key the dictionary does not
// carry at its current value. Defined below the entry points so they read in the
// original's order; declared here because they call it.
void ReadOptions(Options &o, iTJSDispatch2 *dict);

//---------------------------------------------------------------------------
// The property names, built once.
//
// A ttstr caches a hint alongside the text, and GetHint() is non-const, so a
// shared name has to be a mutable object rather than a `const ttstr`. Building
// them per call would allocate on every property read — four times a frame in
// RefreshLayer alone — so they live here and are constructed on first use.
//
// One instance for the process is enough: TJS property access happens on the
// script thread, and a function-local static is initialised once even if that
// ever stops being true.
//---------------------------------------------------------------------------
struct Keys {
	ttstr initFireSpark, finishFireSpark, changeFireSpark, drawFireSpark;

	ttstr imageWidth, imageHeight, mainImageBufferForWrite, mainImageBufferPitch;
	ttstr updateName, tick, layer;

	ttstr colortable, colortableTypo;

	ttstr maxgen, basevelocity, velocityrange, baselifetime, lifetimerange;
	ttstr basetop, heightrange, basesizew, sizewrange, basesizeh, sizehrange;
	ttstr windvelocity, windvariancecapability, maxwindvelocity, samesize;
	ttstr directionbase, directionrange, baseaccel, accelrange;
	ttstr pause, effect, transtime;

	Keys()
		: initFireSpark(TJS_W("initFireSpark")),
		  finishFireSpark(TJS_W("finishFireSpark")),
		  changeFireSpark(TJS_W("changeFireSpark")),
		  drawFireSpark(TJS_W("drawFireSpark")),
		  imageWidth(TJS_W("imageWidth")),
		  imageHeight(TJS_W("imageHeight")),
		  mainImageBufferForWrite(TJS_W("mainImageBufferForWrite")),
		  mainImageBufferPitch(TJS_W("mainImageBufferPitch")),
		  updateName(TJS_W("update")),
		  tick(TJS_W("tick")),
		  layer(TJS_W("layer")),
		  colortable(TJS_W("colortable")),
		  // The literal in the binary is misspelled (0x1000f48e). Both spellings
		  // are accepted: the correct one is what scripts write, the binary's own
		  // is what a caller copied from the plugin may use.
		  colortableTypo(TJS_W(")colortable")),
		  maxgen(TJS_W("maxgen")),
		  basevelocity(TJS_W("basevelocity")),
		  velocityrange(TJS_W("velocityrange")),
		  baselifetime(TJS_W("baselifetime")),
		  lifetimerange(TJS_W("lifetimerange")),
		  basetop(TJS_W("basetop")),
		  heightrange(TJS_W("heightrange")),
		  basesizew(TJS_W("basesizew")),
		  sizewrange(TJS_W("sizewrange")),
		  basesizeh(TJS_W("basesizeh")),
		  sizehrange(TJS_W("sizehrange")),
		  windvelocity(TJS_W("windvelocity")),
		  windvariancecapability(TJS_W("windvariancecapability")),
		  maxwindvelocity(TJS_W("maxwindvelocity")),
		  samesize(TJS_W("samesize")),
		  directionbase(TJS_W("directionbase")),
		  directionrange(TJS_W("directionrange")),
		  baseaccel(TJS_W("baseaccel")),
		  accelrange(TJS_W("accelrange")),
		  pause(TJS_W("pause")),
		  effect(TJS_W("effect")),
		  transtime(TJS_W("transtime"))
	{
	}
};

inline Keys &K()
{
	static Keys keys;
	return keys;
}


//---------------------------------------------------------------------------
// The engine RNG, scaled to [0,1).
//
// The original's FUN_10001060 is exactly this: fill 16 bytes from
// TVPGetRandomBits128, take the low 32 bits, and multiply by 2^-32
// (0x1000d0f8 = 2.3283064370807974e-10) as an unsigned value
// (0x10001091: the low dword is zeroed, so fild reads it unsigned).
//---------------------------------------------------------------------------
inline double Rand01()
{
	tjs_uint8 buf[16];
	TVPGetRandomBits128(buf);
	tjs_uint32 v;
	std::memcpy(&v, buf, sizeof(v));
	return (double)v * (1.0 / 4294967296.0);
}

inline tjs_int RandInt(tjs_int range)   // (tjs_int)(Rand01() * range), as __ftol
{
	return (tjs_int)std::floor(Rand01() * (double)range);
}

//---------------------------------------------------------------------------
// Extract one option, leaving the destination untouched when the key is absent.
//
// This is the shape the original repeats for every key: Get(TJS_MEMBERMUSTEXIST)
// on the options object, and only on success assign. A missing key therefore
// keeps the previous value, which is the documented fallback.
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
// `colortable`: a comma-separated string of decimal values, three per colour.
//
// Recovered from 0x1000298b..0x10002b9f: walk the string by character, split on
// ',' and ' ', take each token's integer value, and store one byte per channel.
// The original also had a fallback for a token that is not a number (it derived
// a shade from the clear colour); a non-numeric token is simply skipped here,
// which keeps the colour list usable rather than aborting the effect.
//
// Returns the number of colours read; 0 means "leave the table alone".
//---------------------------------------------------------------------------
tjs_int ParseColorTable(const ttstr &text, tjs_uint8 *out, tjs_int outBytes)
{
	const tjs_char *p = text.c_str();
	if (!p || !*p) return 0;

	// `out` is a flat list of channel bytes: one token appends one byte, and a
	// colour is every three of them. That matches the original, which stored
	// bytes as it parsed and later divided the count by 3 (0x10002b9f:
	// `idiv` by 3 into DAT_1000f0a4).
	tjs_int bytes = 0;
	while (*p && bytes < outBytes)
	{
		while (*p == TJS_W(',') || *p == TJS_W(' ') || *p == TJS_W('\t')) p++;
		if (!*p) break;

		// One token: an optional sign then digits.
		bool negative = false;
		if (*p == TJS_W('-')) { negative = true; p++; }
		if (*p < TJS_W('0') || *p > TJS_W('9'))
		{
			// Not a value. Skip to the next separator rather than stopping, so
			// one stray token costs that token only, and the loop always makes
			// progress — a malformed table cannot hang the shim.
			while (*p && *p != TJS_W(',') && *p != TJS_W(' ') && *p != TJS_W('\t')) p++;
			continue;
		}

		tjs_int v = 0;
		while (*p >= TJS_W('0') && *p <= TJS_W('9'))
		{
			v = v * 10 + (tjs_int)(*p - TJS_W('0'));
			if (v > 255) v = 255;   // clamp on the way in so v cannot overflow
			p++;
		}
		if (negative) v = 0;

		out[bytes++] = (tjs_uint8)v;

		// Consume the remainder of the token so the next iteration starts on a
		// separator.
		while (*p && *p != TJS_W(',') && *p != TJS_W(' ') && *p != TJS_W('\t')) p++;
	}

	return bytes / 3;
}

//---------------------------------------------------------------------------
// Read the channel layout the original uses.
//
// A colour is three consecutive bytes, read as (R,G,B) — the default table
// decodes as yellow (ff,ff,00) through to black, which is the ramp a fire spark
// wants. The palette word is built as 0x00RRGGBB so that a little-endian 32bpp
// surface stores it as bytes B,G,R,0 (0x10002c2d).
//---------------------------------------------------------------------------
inline tjs_uint32 PackRGB(tjs_int r, tjs_int g, tjs_int b)
{
	if (r < 0) r = 0; else if (r > 255) r = 255;
	if (g < 0) g = 0; else if (g > 255) g = 255;
	if (b < 0) b = 0; else if (b > 255) b = 255;
	return ((tjs_uint32)r << 16) | ((tjs_uint32)g << 8) | (tjs_uint32)b;
}

//---------------------------------------------------------------------------
// Build the 256-entry palette by interpolating across the colour table.
//
// Recovered from 0x10002be0..0x10002cbe: a running fraction starting at 0 and
// stepping by 1/256 (0x1000d2e0 = 0.00390625) once per entry, scaled by
// (ncolors - 1); each entry is the channel lerp between colour i and colour
// i + 1 at that fraction (the `fisub` of the truncated index at 0x10002c2f is
// how the original obtained the fractional part).
//
// One clamp the original did not have: at the far end the index reaches
// ncolors - 1 and reads one colour past the table. In the shipped DLL that slot
// is zero, so the result is identical, but a caller-supplied table would make it
// a wild read. Clamping the upper index removes that with no visible change.
//---------------------------------------------------------------------------
void BuildPalette(const Options &o, tjs_uint32 *palette)
{
	const tjs_int ncolors = o.ncolors > 0 ? o.ncolors : 1;
	const double span = (double)(ncolors - 1);

	for (tjs_int j = 0; j < 256; j++)
	{
		const double t = (double)j / 256.0 * span;
		tjs_int i = (tjs_int)t;
		double frac = t - (double)i;
		if (i < 0) { i = 0; frac = 0.0; }
		if (i > ncolors - 1) { i = ncolors - 1; frac = 0.0; }

		const tjs_int i0 = i;
		const tjs_int i1 = (i + 1 < ncolors) ? i + 1 : i;

		tjs_uint8 *c0 = (tjs_uint8 *)o.colors + i0 * 3;
		tjs_uint8 *c1 = (tjs_uint8 *)o.colors + i1 * 3;

		tjs_int r = (tjs_int)((double)c0[0] + ((double)c1[0] - (double)c0[0]) * frac);
		tjs_int g = (tjs_int)((double)c0[1] + ((double)c1[1] - (double)c0[1]) * frac);
		tjs_int b = (tjs_int)((double)c0[2] + ((double)c1[2] - (double)c0[2]) * frac);

		palette[j] = PackRGB(r, g, b);
	}
}

//---------------------------------------------------------------------------
// Read the Layer's surface through its own TJS properties. Same accessors and
// the same failure message the original used (0x1000f83c / 0x1000f7d8 /
// 0x1000f744 / 0x1000f6b8, thrown via TVPThrowExceptionMessage).
//
// Pitch is BYTES: Layer::mainImageBufferPitch is GetPitchBytes()
// (visual/LayerIntf.cpp:2688).
//---------------------------------------------------------------------------
bool BindLayer(Effect &e, iTJSDispatch2 *layer)
{
	if (!layer) return false;

	Keys &k = K();
	tTJSVariant val;

	if (TJS_FAILED(layer->PropGet(0, k.imageWidth.c_str(), k.imageWidth.GetHint(), &val, layer)))
	{
		TVPThrowExceptionMessage(TJS_W("invoking of Layer.imageWidth failed."));
		return false;
	}
	e.width = (tjs_int)(tTVInteger)val;

	if (TJS_FAILED(layer->PropGet(0, k.imageHeight.c_str(), k.imageHeight.GetHint(), &val, layer)))
	{
		TVPThrowExceptionMessage(TJS_W("invoking of Layer.imageHeight failed."));
		return false;
	}
	e.height = (tjs_int)(tTVInteger)val;

	if (TJS_FAILED(layer->PropGet(0, k.mainImageBufferForWrite.c_str(), k.mainImageBufferForWrite.GetHint(), &val, layer)))
	{
		TVPThrowExceptionMessage(TJS_W("invoking of Layer.mainImageBufferForWrite failed."));
		return false;
	}
	e.pixels = (tjs_uint32 *)(tjs_intptr_t)(tjs_int64)val;

	if (TJS_FAILED(layer->PropGet(0, k.mainImageBufferPitch.c_str(), k.mainImageBufferPitch.GetHint(), &val, layer)))
	{
		TVPThrowExceptionMessage(TJS_W("invoking of Layer.mainImageBufferPitch failed."));
		return false;
	}
	e.pitch = (tjs_int)(tTVInteger)val;

	// The reported pitch KEEPS ITS SIGN, because its sign is the row direction
	// and not a detail to normalise away.
	//
	// This previously rebased the pointer to the bitmap's low end and flipped
	// the sign, so that "index downward" was true by construction. That is a
	// vertical MIRROR of the right addresses, and it is measurable: the engine
	// puts image row y at
	//     (Height - y - 1) * PitchBytes + Bits    (tTVPBitmap::GetScanLine,
	//                                              LayerBitmapImpl.cpp:528)
	// so from GetScanLine(0) - which is what this buffer pointer is - row y is
	// at base + pitch*y with the pitch as reported (-3200 for an 800x600
	// Layer). Rebase-then-flip yields Bits + |pitch|*y, i.e. row (Height-1-y).
	// The same hazard was found and fixed the same way in shims/drawer.cpp,
	// where a probe Layer proved it: keeping the sign renders at the row asked
	// for, and getting it wrong is silent.
	//
	// Nothing else changes: FillRect already steps the row pointer by the byte
	// pitch, so it needs only a signed stride.

	// The clear colour is the layer's own first pixel (0x10001f.. in init:
	// DAT_10012eb4 = *DAT_10012a48). Erasing with what is already there is how
	// the original avoided inventing an alpha: the palette is written with a
	// zero alpha byte, so the erased area must be restored to a known state.
	/* Row 0 of the image is exactly this pointer once the stride keeps its sign,
	 * so the sample no longer depends on the sign being positive. */
	e.clearColor = (e.pixels && e.pitch != 0 && e.width > 0 && e.height > 0) ? e.pixels[0] : 0;

	if (e.layer != layer)
	{
		if (e.layer) e.layer->Release();
		e.layer = layer;
		layer->AddRef();
	}
	return true;
}

//---------------------------------------------------------------------------
// A clipped solid fill of one rectangle.
//
// The original writes each particle as a run of identical words, one row at a
// time (0x10001c5a: `rep stosd` per row, stepping the row pointer by the BYTE
// pitch). It clips with a set of range tests around 0x10001b8f..0x10001bfa;
// the same clip is expressed once here so a hostile option value cannot store
// outside the surface.
//---------------------------------------------------------------------------
void FillRect(Effect &e, tjs_int x, tjs_int y, tjs_int w, tjs_int h, tjs_uint32 color)
{
	if (!e.pixels || e.pitch == 0 || e.width <= 0 || e.height <= 0) return;
	if (w <= 0 || h <= 0) return;

	tjs_int x0 = x, y0 = y;
	tjs_int x1 = x + w, y1 = y + h;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > e.width) x1 = e.width;
	if (y1 > e.height) y1 = e.height;
	if (x0 >= x1 || y0 >= y1) return;

	// Bytes per row is the pitch; the row pointer is advanced by it, not by
	// width, so padded rows stay correct.
	for (tjs_int row = y0; row < y1; row++)
	{
		tjs_uint32 *dst = (tjs_uint32 *)((tjs_uint8 *)e.pixels + (tjs_intptr_t)e.pitch * row) + x0;
		const tjs_int n = x1 - x0;
		for (tjs_int i = 0; i < n; i++) dst[i] = color;
	}
}

//---------------------------------------------------------------------------
// Tell the Layer which part changed, via its own `update` member.
//
// FuncCall(0, "update", 0, NULL, 4, args, layer) — Layer.update(left, top,
// width, height), as called at 0x100019f1 and 0x10001df0.
//---------------------------------------------------------------------------
void LayerUpdate(Effect &e, tjs_int x, tjs_int y, tjs_int w, tjs_int h)
{
	if (!e.layer || w <= 0 || h <= 0) return;

	tTJSVariant a[4];
	a[0] = (tTVInteger)x;
	a[1] = (tTVInteger)y;
	a[2] = (tTVInteger)w;
	a[3] = (tTVInteger)h;
	tTJSVariant *args[4] = { &a[0], &a[1], &a[2], &a[3] };

	Keys &k = K();
	e.layer->FuncCall(0, k.updateName.c_str(), k.updateName.GetHint(), NULL, 4, args, e.layer);
}

//---------------------------------------------------------------------------
// Bring the cached surface up to date with the layer's current geometry.
//
// The original did this when `layer` was supplied to changeFireSpark
// (0x100030b8 onwards) and once in initFireSpark; a Layer can be resized
// between frames, so the cached width/height/pitch/buffer are re-read here
// before every frame that writes, which is strictly safer than the original's
// cache-and-hope and costs four property reads.
//---------------------------------------------------------------------------
bool RefreshLayer(Effect &e)
{
	if (!e.layer) return false;
	Keys &k = K();
	tTJSVariant val;

	if (TJS_FAILED(e.layer->PropGet(0, k.imageWidth.c_str(), k.imageWidth.GetHint(), &val, e.layer))) return false;
	e.width = (tjs_int)(tTVInteger)val;
	if (TJS_FAILED(e.layer->PropGet(0, k.imageHeight.c_str(), k.imageHeight.GetHint(), &val, e.layer))) return false;
	e.height = (tjs_int)(tTVInteger)val;
	if (TJS_FAILED(e.layer->PropGet(0, k.mainImageBufferForWrite.c_str(), k.mainImageBufferForWrite.GetHint(), &val, e.layer))) return false;
	tjs_uint32 *newPixels = (tjs_uint32 *)(tjs_intptr_t)(tjs_int64)val;
	if (TJS_FAILED(e.layer->PropGet(0, k.mainImageBufferPitch.c_str(), k.mainImageBufferPitch.GetHint(), &val, e.layer))) return false;
	const tjs_int newPitch = (tjs_int)(tTVInteger)val;

	/* Same convention as BindLayer: the pitch keeps its sign, because the sign
	 * IS the row direction. This used to rebase the pointer and negate the
	 * stride, which mirrored the surface and - because that made the pointer
	 * differ on every call - re-sampled clearColor from the wrong address each
	 * frame. One rule, stated once, applied in both places. */
	if (newPixels != e.pixels || newPitch != e.pitch)
		e.clearColor = (newPixels && newPitch != 0 && e.width > 0 && e.height > 0) ? newPixels[0] : 0;

	e.pixels = newPixels;
	e.pitch = newPitch;
	return true;
}

//---------------------------------------------------------------------------
// Erase every live particle over the rect it recorded, and tell the layer.
//
// Shared by the frame's first step and by changeFireSpark when it re-sizes the
// array — the particles are about to be discarded there, so their pixels must
// go back to the clear colour before the record of where they were is lost.
//---------------------------------------------------------------------------
void EraseDrawn(Effect &e)
{
	if (!e.particles) return;

	tjs_int x0 = e.width, y0 = e.height, x1 = 0, y1 = 0;
	bool any = false;
	for (tjs_int i = 0; i < e.slots(); i++)
	{
		Particle &p = e.particles[i];
		if (!p.active || p.rectW <= 0 || p.rectH <= 0) continue;
		FillRect(e, p.rectX, p.rectY, p.rectW, p.rectH, e.clearColor);
		if (!any) { x0 = p.rectX; y0 = p.rectY; x1 = p.rectX + p.rectW; y1 = p.rectY + p.rectH; any = true; }
		else
		{
			if (p.rectX < x0) x0 = p.rectX;
			if (p.rectY < y0) y0 = p.rectY;
			if (p.rectX + p.rectW > x1) x1 = p.rectX + p.rectW;
			if (p.rectY + p.rectH > y1) y1 = p.rectY + p.rectH;
		}
	}
	if (any)
		LayerUpdate(e, x0, y0, x1 - x0, y1 - y0);
}

//---------------------------------------------------------------------------
// Spawn one particle, using each option exactly as the original's generator did
// (0x100017d1..0x10001a8f):
//
//   angle    = rand * directionrange + directionbase         (0x100017d6)
//   speed    = rand * velocityrange  + basevelocity          (0x100017eb)
//   dx, dy   = cos(angle) * speed, sin(angle) * speed        (0x1000182d/0x1000183e)
//   x        = rand * layerWidth                             (0x10001800)
//   y        = rand * heightrange + basetop                  (0x10001813)
//   lifetime = rand * lifetimerange + baselifetime           (0x10001857)
//   accel    = rand * accelrange + baseaccel                 (0x10001870)
//   w, h     = samesize ? (rand * sizewrange + basesizew) twice
//                        : rand * sizewrange + basesizew, rand * sizehrange + basesizeh
//
// The integer option values are the fixed-point quantities the original kept
// (the fmul/fiadd pairs treat them as such); they are used unchanged so a
// tuning the game shipped keeps its proportions.
//---------------------------------------------------------------------------
bool SpawnParticle(Effect &e, tjs_uint64 now)
{
	if (!e.particles || e.liveCount >= e.slots() || e.slots() <= 0) return false;

	Particle *p = NULL;
	for (tjs_int i = 0; i < e.slots(); i++)
	{
		if (!e.particles[i].active) { p = &e.particles[i]; break; }
	}
	if (!p) return false;

	const double angle = Rand01() * e.opt.directionrange + e.opt.directionbase;
	const double speed = Rand01() * (double)e.opt.velocityrange + (double)e.opt.basevelocity;

	std::memset(p, 0, sizeof(*p));
	p->x = RandInt(e.width);
	p->y = RandInt(e.opt.heightrange) + e.opt.basetop;
	p->dx = (tjs_int)(std::cos(angle) * speed);
	p->dy = (tjs_int)(std::sin(angle) * speed);
	p->fx = 0;
	p->fy = 0;

	// Every value below is accumulated per frame (`+= accel * elapsed`,
	// `+= dx * elapsed`, `age << 15`), so an option value near the limits of
	// tjs_int would overflow the accumulator rather than simply look wrong. The
	// clamp keeps hostile options inside a range the arithmetic survives; the
	// precision the game shipped with is many orders of magnitude smaller.
	const tjs_int kMaxSpan = 1 << 20;

	p->dx = (tjs_int)(std::cos(angle) * speed);
	p->dy = (tjs_int)(std::sin(angle) * speed);
	if (p->dx > kMaxSpan) p->dx = kMaxSpan; else if (p->dx < -kMaxSpan) p->dx = -kMaxSpan;
	if (p->dy > kMaxSpan) p->dy = kMaxSpan; else if (p->dy < -kMaxSpan) p->dy = -kMaxSpan;
	p->fx = 0;
	p->fy = 0;

	p->lifetime = RandInt(e.opt.lifetimerange) + e.opt.baselifetime;
	if (p->lifetime <= 0) p->lifetime = 1;
	if (p->lifetime > kMaxSpan) p->lifetime = kMaxSpan;

	// Clamped here rather than after the multiply in the frame loop: `accel *
	// elapsed` is computed before any check on the product could run, so the
	// bound has to be on the operand.
	p->accel = RandInt(e.opt.accelrange) + e.opt.baseaccel;
	if (p->accel > kMaxSpan) p->accel = kMaxSpan; else if (p->accel < -kMaxSpan) p->accel = -kMaxSpan;

	tjs_int w = RandInt(e.opt.sizewrange) + e.opt.basesizew;
	tjs_int h = e.opt.samesize ? w : RandInt(e.opt.sizehrange) + e.opt.basesizeh;
	if (w > kMaxSpan) w = kMaxSpan;
	if (h > kMaxSpan) h = kMaxSpan;
	p->w = w;
	p->h = h;
	if (p->w < 1) p->w = 1;
	if (p->h < 1) p->h = 1;

	p->bornLo = (tjs_int)(now & 0xffffffffu);
	p->bornHi = (tjs_int)(now >> 32);
	p->rectX = p->rectY = p->rectW = p->rectH = 0;
	p->active = 1;

	e.liveCount++;
	return true;
}

//---------------------------------------------------------------------------
// One frame.
//
// The original's draw (FUN_100014c0) is one long function; the sequencing is:
//
//   1. erase last frame's particles, each over the rect it recorded
//      (0x100016a1..0x10001739, then Layer.update on the old bounding box);
//   2. advance the transition, which scales maxgen towards transMaxGen while it
//      is running (0x100018b2..0x100019ed);
//   3. accumulate spawn credit and emit particles while a free slot exists
//      (0x100017a6..0x10001a95);
//   4. integrate and redraw every live particle (0x10001ab4..0x10001cc7),
//      recording the new rect and updating the layer with the new bounding box.
//
// The integration is the original's fixed-point step: velocity is added to a
// 16.16 accumulator whose carry moves the integer position
// (0x10001b20..0x10001b82: `imul by elapsed`, `and 0xffff` for the fraction,
// `sar 16` for the carry), acceleration is added to the velocity, and the
// palette index sweeps the particle's life
// (0x10001c0b..0x10001c24: `(age << 15) / lifetime >> 7 & 0xff`).
//---------------------------------------------------------------------------
void Advance(Effect &e, tjs_uint64 now)
{
	if (!e.live) return;
	if (!e.particles) return;

	if (now <= e.lastTick) { e.lastTick = now; return; }
	tjs_uint64 elapsed64 = now - e.lastTick;
	e.lastTick = now;

	// A long stall (a window drag, a breakpoint) would otherwise integrate a
	// particle far past its life and spawn a burst in one step. The original
	// used the raw delta; capping it keeps the effect bounded and is the only
	// departure here, and it is one the caller cannot observe as a wrong value.
	if (elapsed64 > 100) elapsed64 = 100;
	const tjs_int elapsed = (tjs_int)elapsed64;

	if (e.paused || !e.effectEnabled) return;

	// 1. erase the previous frame.
	EraseDrawn(e);

	// 2. transition: while it runs, the live bound is pulled towards
	//    transMaxGen (0x100018b2: on entry, transMaxGen = the target,
	//    transBaseMaxGen = the bound the ramp started from, and the deadline is
	//    transStartTick + transtime).
	if (e.transitioning)
	{
		if ((tjs_int)(now - (tjs_uint64)(tjs_uint32)e.transStartTick) < e.opt.transtime)
		{
			// One step per frame, proportional to the time that frame took: a
			// step of 0 when the span is shorter than a tick is fine, because
			// the next frame adds more.
			const tjs_int step = e.opt.transtime > 0
				? (tjs_int)((tjs_int64)e.transBaseMaxGen * elapsed / e.opt.transtime)
				: e.transMaxGen;
			tjs_int target = e.limit + step;
			if (target > e.transMaxGen) target = e.transMaxGen;
			e.limit = target;
		}
		else
		{
			e.transitioning = false;
			e.limit = e.transMaxGen;
		}

		// The bound is what spawns are counted against, so it must stay inside
		// the allocation. It can only exceed it if the target was raised after
		// the array was sized, which changeFireSpark re-sizes to prevent.
		if (e.limit > e.slots()) e.limit = e.slots();
		if (e.limit <= 0)
		{
			// The original tears the effect down when the bound reaches zero
			// mid-transition (0x100018c0: it frees the particles and finishes).
			e.Release();
			return;
		}
	}

	// 3. spawn. The rate is particles-per-tick in 16.16 — the original's
	//    spawnRate = (maxgen << 16) / (baselifetime + lifetimerange)
	//    (0x10002be2: `shl eax,0x10; idiv ecx`) — and the accumulator drops one
	//    whole particle per unit the high half reaches (0x10001633: it takes the
	//    delta, multiplies by the rate, and emits `>> 16` particles).
	if (e.limit > 0)
	{
		const tjs_int life = e.opt.baselifetime + e.opt.lifetimerange;
		e.spawnRate = life > 0 ? (tjs_int)(((tjs_int64)e.limit << 16) / life) : 0;
		// The credit add is done in 64 bits: spawnRate is up to limit << 16 and
		// elapsed up to 100, so the product can pass tjs_int even though neither
		// factor does. The accumulator itself is 32 bits and only its high half
		// is consumed, which is the form the original kept.
		const tjs_int64 credit = (tjs_int64)e.spawnRate * elapsed;
		e.spawnAccum += (tjs_uint32)(credit & 0xffffffffu);
		while ((e.spawnAccum >> 16) > 0)
		{
			if (e.liveCount >= e.limit) { e.spawnAccum = 0; break; }
			if (!SpawnParticle(e, now)) { e.spawnAccum = 0; break; }
			e.spawnAccum -= 0x10000;
		}
	}

	// Wind. `windvelocity` is the steady horizontal push (the original's
	// DAT_10012eac, default 0) and it is added as a per-tick velocity on the
	// same fixed-point scale as the spawn velocity; the occasional re-roll adds
	// a bounded noise on top (0x10001e23..0x10001e63, gated on
	// `rand < elapsed * windvariancecapability` and bounded by maxwindvelocity).
	//
	// The noise term is what the original recomputed; the steady term is the
	// value the `windvelocity` key sets. When windvelocity is left at its default
	// of 0 — which is what Fate's own options do — this reduces to the noise-only
	// drift the original produced.
	// Bounded for the same reason `accel` is: `windDelta * elapsed` runs before
	// any check on the product.
	const tjs_int kMaxWind = 1 << 20;
	e.windDelta = e.opt.windvelocity;
	if (e.windDelta > kMaxWind) e.windDelta = kMaxWind;
	else if (e.windDelta < -kMaxWind) e.windDelta = -kMaxWind;
	if (e.opt.maxwindvelocity > 0.0 &&
	    Rand01() < (double)elapsed * e.opt.windvariancecapability)
	{
		const double w = (Rand01() * 2.0 - 1.0) * e.opt.maxwindvelocity;
		e.windSpeed = (tjs_int)(w * 65535.0);
		e.windDelta = e.opt.windvelocity + (e.windSpeed >> 11);
		if (e.windDelta > kMaxWind) e.windDelta = kMaxWind;
		else if (e.windDelta < -kMaxWind) e.windDelta = -kMaxWind;
	}

	// 4. integrate and redraw.
	tjs_int newX0 = e.width, newY0 = e.height, newX1 = 0, newY1 = 0;
	bool anyNew = false;

	for (tjs_int i = 0; i < e.slots(); i++)
	{
		Particle &p = e.particles[i];
		if (!p.active) continue;

		const tjs_uint64 born = ((tjs_uint64)(tjs_uint32)p.bornHi << 32) | (tjs_uint32)p.bornLo;
		const tjs_uint64 age = now - born;
		if (age >= (tjs_uint64)(tjs_uint32)p.lifetime)
		{
			p.active = 0;
			if (e.liveCount > 0) e.liveCount--;
			continue;
		}

		// The integration is bounded so a hostile option value cannot make the
		// accumulators wrap. A particle far outside the layer draws nothing
		// either way, so the bound is invisible; it exists so a long-lived fast
		// particle cannot walk its position into overflow.
		const tjs_int kMaxSpeed = 1 << 24;
		const tjs_int kMaxPos = 1 << 20;

		// Acceleration first, then velocity.
		p.dx += p.accel * elapsed;   // 0x10001b82..0x10001b8c
		p.dy += p.accel * elapsed;

		// Wind is added to the horizontal velocity.
		if (e.windDelta != 0)
			p.dx += e.windDelta * elapsed;

		if (p.dx > kMaxSpeed) p.dx = kMaxSpeed; else if (p.dx < -kMaxSpeed) p.dx = -kMaxSpeed;
		if (p.dy > kMaxSpeed) p.dy = kMaxSpeed; else if (p.dy < -kMaxSpeed) p.dy = -kMaxSpeed;

		// Position: the fraction accumulates in 16.16 and its carry advances the
		// integer pixel (0x10001b20: `imul edx, elapsed`, `and 0xffff`, `sar 16`).
		// The accumulator is reduced modulo 65536 by the mask below, so 64-bit
		// intermediates keep the product from overflowing before the shift.
		p.fx = (tjs_int)((tjs_int64)p.fx + (tjs_int64)p.dx * elapsed);
		p.fy = (tjs_int)((tjs_int64)p.fy + (tjs_int64)p.dy * elapsed);
		p.x += p.fx >> 16;
		p.y += p.fy >> 16;
		p.fx &= 0xffff;
		p.fy &= 0xffff;
		if (p.x > kMaxPos) p.x = kMaxPos; else if (p.x < -kMaxPos) p.x = -kMaxPos;
		if (p.y > kMaxPos) p.y = kMaxPos; else if (p.y < -kMaxPos) p.y = -kMaxPos;

		// Palette index sweeps the particle's life exactly once
		// (0x10001c0b..0x10001c24).
		const tjs_int li = (tjs_int)(((tjs_uint64)(tjs_uint32)age << 15) / (tjs_uint64)(tjs_uint32)p.lifetime);
		const tjs_uint32 color = e.palette[(li >> 7) & 0xff];

		// Clip and draw. Clip cached so the recorded rect matches what was
		// written and the next frame's erase is exact.
		tjs_int drawX = p.x, drawY = p.y, drawW = p.w, drawH = p.h;
		if (drawX < 0) { drawW += drawX; drawX = 0; }
		if (drawY < 0) { drawH += drawY; drawY = 0; }
		if (drawX + drawW > e.width) drawW = e.width - drawX;
		if (drawY + drawH > e.height) drawH = e.height - drawY;

		p.rectX = drawX; p.rectY = drawY; p.rectW = drawW; p.rectH = drawH;

		if (drawW > 0 && drawH > 0)
		{
			FillRect(e, drawX, drawY, drawW, drawH, color);

			if (!anyNew) { newX0 = drawX; newY0 = drawY; newX1 = drawX + drawW; newY1 = drawY + drawH; anyNew = true; }
			else
			{
				if (drawX < newX0) newX0 = drawX;
				if (drawY < newY0) newY0 = drawY;
				if (drawX + drawW > newX1) newX1 = drawX + drawW;
				if (drawY + drawH > newY1) newY1 = drawY + drawH;
			}
		}
	}

	if (anyNew)
		LayerUpdate(e, newX0, newY0, newX1 - newX0, newY1 - newY0);
}

//---------------------------------------------------------------------------
// Read a frame's optional clock argument.
//
// drawFireSpark([obj]) — the original reads member `tick` from the first
// argument when one is passed and falls back to TVPGetTickCount when it is
// absent or the member is missing (0x1000150..: the Get on "tick", and the two
// paths at 0x10001573 / 0x10001592 that choose between the member and
// TVPGetTickCount).
//---------------------------------------------------------------------------
tjs_uint64 FrameTick(tjs_int numparams, tTJSVariant **param)
{
	if (numparams >= 1 && param && param[0] && param[0]->Type() == tvtObject)
	{
		iTJSDispatch2 *obj = param[0]->AsObjectNoAddRef();
		if (obj)
		{
			Keys &k = K();
			tTJSVariant val;
			if (!TJS_FAILED(obj->PropGet(0, k.tick.c_str(), k.tick.GetHint(), &val, obj)))
				return (tjs_uint64)(tTVInteger)val;
		}
	}
	return TVPGetTickCount();
}

//---------------------------------------------------------------------------
// The four TJS entry points. The callback shape is the engine's own
// (tTJSNativeClassMethodCallback in tjsNative.h), which is also the shape the
// original's FuncCall used.
//---------------------------------------------------------------------------

// initFireSpark(layer, options)
tjs_error TJS_INTF_METHOD InitFireSpark(tTJSVariant * /*result*/, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 * /*objthis*/)
{
	if (numparams < 1)
		return kEBadParamCount;

	Effect &e = g_effect;

	// A second init on a live effect frees the old one first (0x10001fa3).
	if (e.live)
		e.Release();

	iTJSDispatch2 *layer = (param[0] && param[0]->Type() == tvtObject) ? param[0]->AsObjectNoAddRef() : NULL;
	iTJSDispatch2 *options = (numparams >= 2 && param[1] && param[1]->Type() == tvtObject)
		? param[1]->AsObjectNoAddRef() : NULL;

	e.opt.Reset();
	if (options)
		ReadOptions(e.opt, options);

	if (!BindLayer(e, layer))
		return kESucceed;   // BindLayer already raised the layer-specific error

	if (e.width <= 0 || e.height <= 0 || !e.pixels || e.pitch == 0)
		return kESucceed;

	BuildPalette(e.opt, e.palette);

	// The particle array is maxgen records, allocated once (0x10002ccd:
	// `operator_new(maxgen * 0x50)`) and zeroed field by field (0x10002ce8).
	if (!e.Resize(e.opt.maxgen))
		return kESucceed;
	e.spawnAccum = 0;
	e.transitioning = false;
	e.transMaxGen = e.opt.maxgen;
	e.transBaseMaxGen = e.opt.maxgen;
	e.limit = e.slots();
	e.lastTick = TVPGetTickCount();
	e.live = true;
	e.paused = (e.opt.pause != 0);
	e.effectEnabled = (e.opt.effect != 0);
	return kESucceed;
}

// changeFireSpark(options)
//
// One argument, and `layer` is an option key here rather than a positional
// argument (0x10002e80: the options object is the only parameter, and 0x100030b8
// reads `layer` from it). Missing keys leave the live values alone, which is how
// the original let a caller retune one parameter at a time.
tjs_error TJS_INTF_METHOD ChangeFireSpark(tTJSVariant * /*result*/, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 * /*objthis*/)
{
	if (numparams < 1)
		return kEBadParamCount;

	Effect &e = g_effect;
	if (!e.live)
		return kESucceed;   // 0x10002e73: no live effect, nothing to change

	iTJSDispatch2 *options = (param[0] && param[0]->Type() == tvtObject) ? param[0]->AsObjectNoAddRef() : NULL;
	if (!options)
		return kESucceed;

	ReadOptions(e.opt, options);

	// The array must cover the new maxgen before anything can walk it: the loops
	// are bounded by capacity, so a raised maxgen would otherwise be silently
	// ignored. Re-sizing drops the particles in flight, so whatever they have
	// already drawn is erased first — otherwise their pixels stay on the layer
	// with no record left to erase them.
	if (e.opt.maxgen > e.slots())
	{
		EraseDrawn(e);
		e.Resize(e.opt.maxgen);
	}

	// transtime starts a transition from the bound in force towards the new
	// maxgen. The original keeps the old bound as the ramp's base and the new
	// value as its target (0x10002fc4: `DAT_10012e90 = DAT_1000f044`, the live
	// value, while DAT_10012a44 holds the newly read maxgen), then re-arms the
	// deadline with the current tick.
	e.transBaseMaxGen = e.limit;
	e.transMaxGen = e.opt.maxgen;
	e.transStartTick = (tjs_int)(tjs_uint32)TVPGetTickCount();
	e.transitioning = true;

	// `layer` re-points the surface (0x100030b8): take the new one and re-read
	// its geometry, since a different layer has its own size and pitch.
	e.effectEnabled = (e.opt.effect != 0);
	e.paused = (e.opt.pause != 0);

	{
		Keys &k = K();
		tTJSVariant val;
		if (!TJS_FAILED(options->PropGet(0, k.layer.c_str(), k.layer.GetHint(), &val, options)) &&
		    val.Type() == tvtObject)
		{
			iTJSDispatch2 *newLayer = val.AsObjectNoAddRef();
			if (newLayer && newLayer != e.layer)
				BindLayer(e, newLayer);
		}
	}
	return kESucceed;
}

// drawFireSpark([clock])
tjs_error TJS_INTF_METHOD DrawFireSpark(tTJSVariant * /*result*/, tjs_int numparams,
	tTJSVariant **param, iTJSDispatch2 * /*objthis*/)
{
	Effect &e = g_effect;
	if (!e.live || !e.particles)
		return kESucceed;

	if (!RefreshLayer(e))
		return kESucceed;

	Advance(e, FrameTick(numparams, param));
	return kESucceed;
}

// finishFireSpark()
//
// 0x10002de0: no arguments; release the particles and clear the live flag.
// Calling it when nothing is live is a no-op, so a script that finishes twice
// is harmless.
tjs_error TJS_INTF_METHOD FinishFireSpark(tTJSVariant * /*result*/, tjs_int /*numparams*/,
	tTJSVariant ** /*param*/, iTJSDispatch2 * /*objthis*/)
{
	g_effect.Release();
	return kESucceed;
}

} // namespace

//---------------------------------------------------------------------------
// Option parsing.
//
// Kept out of the anonymous namespace's head only so the entry points read in
// the order of the original; it is still file-local.
//---------------------------------------------------------------------------
namespace {

void ReadOptions(Options &o, iTJSDispatch2 *dict)
{
	Keys &k = K();

	// maxgen is bounded: the original rejects a value >= 500 and uses 500
	// (0x1000223c: `cmp eax,0x1f4; jge` → 500). A negative value is clamped to
	// zero here so the array size cannot wrap.
	if (GetIntOpt(dict, k.maxgen, &o.maxgen))
	{
		if (o.maxgen >= 500) o.maxgen = 500;
		if (o.maxgen < 0) o.maxgen = 0;
	}

	GetIntOpt(dict, k.basevelocity, &o.basevelocity);
	GetIntOpt(dict, k.velocityrange, &o.velocityrange);
	GetIntOpt(dict, k.baselifetime, &o.baselifetime);
	GetIntOpt(dict, k.lifetimerange, &o.lifetimerange);
	GetIntOpt(dict, k.basetop, &o.basetop);
	GetIntOpt(dict, k.heightrange, &o.heightrange);
	GetIntOpt(dict, k.basesizew, &o.basesizew);
	GetIntOpt(dict, k.sizewrange, &o.sizewrange);
	GetIntOpt(dict, k.basesizeh, &o.basesizeh);
	GetIntOpt(dict, k.sizehrange, &o.sizehrange);
	GetIntOpt(dict, k.windvelocity, &o.windvelocity);
	GetRealOpt(dict, k.windvariancecapability, &o.windvariancecapability);
	GetRealOpt(dict, k.maxwindvelocity, &o.maxwindvelocity);
	GetIntOpt(dict, k.samesize, &o.samesize);
	GetRealOpt(dict, k.directionbase, &o.directionbase);
	GetRealOpt(dict, k.directionrange, &o.directionrange);
	GetIntOpt(dict, k.baseaccel, &o.baseaccel);
	GetIntOpt(dict, k.accelrange, &o.accelrange);
	GetIntOpt(dict, k.pause, &o.pause);
	GetIntOpt(dict, k.effect, &o.effect);
	GetIntOpt(dict, k.transtime, &o.transtime);
	if (o.transtime <= 0) o.transtime = 5000;   // 0x10002fba: absent or bad → 5000

	// `colortable`: the correctly spelled key first, the binary's own misspelling
	// as a fallback so a caller that copied the literal still works.
	{
		tTJSVariant val;
		bool found = !TJS_FAILED(dict->PropGet(0, k.colortable.c_str(), k.colortable.GetHint(), &val, dict));
		if (!found)
			found = !TJS_FAILED(dict->PropGet(0, k.colortableTypo.c_str(), k.colortableTypo.GetHint(), &val, dict));

		if (found)
		{
			const ttstr text(val);
			tjs_uint8 table[3 * 64];
			std::memset(table, 0, sizeof(table));
			const tjs_int n = ParseColorTable(text, table, (tjs_int)sizeof(table));
			if (n > 0)
			{
				std::memcpy(o.colors, table, (size_t)n * 3);
				o.ncolors = n;
			}
		}
	}
}

} // namespace

//---------------------------------------------------------------------------
// V2Link / V2Unlink — the plugin ABI (shim_abi.h).
//
// The four names are registered through the engine's own helpers, which take
// and drop the global dispatch reference correctly; unregistration uses the
// matching remover, so the pair is symmetric and the engine may link, unlink and
// link another shim in one run.
//
// extern "C" is stated explicitly. shim_abi.h renames the entry points per shim
// only in the built-in build, and in that build SHIM_EXPORT is empty, so without
// this the symbols would be C++-mangled and would not match BuiltinShims.cpp's
// `extern "C" int shim_firespark_V2Link(...)`. On the plugin path SHIM_EXPORT
// already carries extern "C"; nesting is legal and redundant.
//---------------------------------------------------------------------------
extern "C" {

SHIM_EXPORT SHIM_HIDDEN int V2Link(iTVPFunctionExporter *exporter)
{
	(void)exporter;

	// Registration is symmetric with V2Unlink below. A failure to register one
	// name is reported by the return code, which is what the engine's loader
	// and TVPInstallBuiltinShim() both check.
	tTJSNativeClassMethod *init = TJSCreateNativeClassMethod(&InitFireSpark);
	tTJSNativeClassMethod *finish = TJSCreateNativeClassMethod(&FinishFireSpark);
	tTJSNativeClassMethod *change = TJSCreateNativeClassMethod(&ChangeFireSpark);
	tTJSNativeClassMethod *draw = TJSCreateNativeClassMethod(&DrawFireSpark);
	if (!init || !finish || !change || !draw)
	{
		if (init) init->Release();
		if (finish) finish->Release();
		if (change) change->Release();
		if (draw) draw->Release();
		return TJS_E_FAIL;
	}

	bool ok = true;
	Keys &k = K();
	ok = TVPRegisterGlobalObject(k.initFireSpark.c_str(), init) && ok;
	ok = TVPRegisterGlobalObject(k.finishFireSpark.c_str(), finish) && ok;
	ok = TVPRegisterGlobalObject(k.changeFireSpark.c_str(), change) && ok;
	ok = TVPRegisterGlobalObject(k.drawFireSpark.c_str(), draw) && ok;

	// TVPRegisterGlobalObject keeps its own reference in the global object, so
	// the creator's reference is dropped here.
	init->Release();
	finish->Release();
	change->Release();
	draw->Release();

	return ok ? SHIM_TJS_S_OK : TJS_E_FAIL;
}

SHIM_EXPORT SHIM_HIDDEN int V2Unlink(void)
{
	Keys &k = K();
	TVPRemoveGlobalObject(k.initFireSpark.c_str());
	TVPRemoveGlobalObject(k.finishFireSpark.c_str());
	TVPRemoveGlobalObject(k.changeFireSpark.c_str());
	TVPRemoveGlobalObject(k.drawFireSpark.c_str());

	// Anything still live goes with the shim: the engine may unload a plugin
	// without the game having called finishFireSpark (the original's V2Unlink
	// released the particle array the same way, at 0x10003399).
	g_effect.Release();
	return SHIM_TJS_S_OK;
}

} // extern "C"
