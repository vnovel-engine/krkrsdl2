//---------------------------------------------------------------------------
/*
	KiriKiri engine-compatibility switches — SDL2 backend implementation.
	See CompatProfile.h for the switches, why they are flags rather than a
	version number, and when each is read.
*/
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CompatProfile.h"

#include "DebugIntf.h"
#include "SysInitIntf.h"
#include "ScriptMgnIntf.h"

/* The globals each switch drives, taken from their own headers rather than
 * redeclared: the declarations live inside namespace TJS, so a bare extern here
 * would be a second, different entity. These are the same globals the engine's
 * own command-line handling assigns (base/ScriptMgnIntf.cpp,
 * base/sdl2/SysInitImpl.cpp); nothing new is introduced. */
#include "tjs.h"
#include "LayerIntf.h"
#include "CompositeLevel.h"
#include "StructFormat.h"

//---------------------------------------------------------------------------
// Mask construction
//---------------------------------------------------------------------------
static unsigned int TVPCompatBit(TVPCompatFlag flag)
{
	return 1u << (unsigned int)flag;
}

/* The mask last applied, so script can ask what was selected rather than
 * inferring it. Exposed to script by TVPExposeCompatToScript. */
static TVPCompatMask TVPCompatLastMask = { 0 };

unsigned int TVPCompatResolvedBits(void)
{
	return TVPCompatLastMask.bits;
}

TVPCompatMask TVPCompatResolvedMask(void)
{
	return TVPCompatLastMask;
}

int TVPCompatResolved(TVPCompatFlag flag)
{
	return TVPCompatHasFlag(TVPCompatLastMask, flag);
}

int TVPCompatHasFlag(TVPCompatMask mask, TVPCompatFlag flag)
{
	return (mask.bits & TVPCompatBit(flag)) != 0;
}

/* The 2.19-era preset.
 *
 * Each entry is here because the title that motivated it needs it, and its
 * evidence is named so a future reader does not have to re-derive it:
 *
 *   classvar     Fate/stay night declares `var face` on a class extending the
 *                native Layer and needs the accessor to survive (measured: the
 *                message layer's province fill composites over the backdrop as
 *                opaque black without it).
 *   evalcontext  the game's scripts use the post-! operator expecting TJS2's
 *                old global-context behaviour; the engine warns about exactly
 *                this on load ("Non-global post-! operator is used").
 *   holdalpha    Layer.holdAlpha defaulted true before 2.23 beta 5.
 *   unaryaster   unary '*' meant direct property access before 2.25 beta 1.
 *
 * arcdelim is deliberately NOT a switch here, for a measured reason: this port
 * and the games it runs are on '>' (the delimiter changed at 2.19 *beta 14*,
 * and Fate/stay night is 2.19 final). Selecting '#' makes the engine fail
 * outright with its own "path name must end with '>' or '/'" error, so it is
 * not a behaviour any game this port can reach actually wants, and carrying the
 * switch would only invite setting it.
 */
TVPCompatMask TVPCompatMaskForLevel(TVPCompatLevel level)
{
	TVPCompatMask mask;
	mask.bits = 0;

	switch(level)
	{
	case TVPCompatV219:
		mask.bits |= TVPCompatBit(TVPCompatFlagClassVar);
		mask.bits |= TVPCompatBit(TVPCompatFlagEvalContext);
		mask.bits |= TVPCompatBit(TVPCompatFlagHoldAlpha);
		mask.bits |= TVPCompatBit(TVPCompatFlagUnaryAsterisk);
		/* The pre-2006 compositing algorithm. The switch exists because the
		 * modern DrawCompleted's transparent fill of uncovered destination area
		 * overwrites what children drew (the observed loss of the message text
		 * on Fate's disclaimer); the 2.19-era titles need the children's output
		 * to stand. See CompositeLevel.h for the two algorithms and why this is
		 * a level chosen once rather than a branch in the loop. */
		mask.bits |= TVPCompatBit(TVPCompatFlagComposite219);
		/* The obsolete Layer API shims, for the reason and the temporary
		 * direction explained at their load site in base/ScriptMgnIntf.cpp:
		 * krkrsdl2 dropped the methods natively and has no native replacements
		 * yet, so an old-era game still needs the TJS files. When the native
		 * implementations are lifted from deps/kirikiri2, this bit belongs on
		 * the *modern* level instead and the approximate TJS shims retire. */
		mask.bits |= TVPCompatBit(TVPCompatFlagLayerShims);
		/* The save wire format of the era. `structconst` is load-bearing: a
		 * 2.19-era game eval's a save with Scripts.evalStorage, and the 2008+
		 * `(const) %[` prefix makes that eval throw, so without this bit every
		 * save this port writes is unreadable by the very game it was written
		 * for (measured against Fate.exe). `structcrlf` is what the reference
		 * binary emits and is not a blocker — the loader takes either — but it
		 * is set so a save we write is byte-comparable with one the reference
		 * writes. */
		mask.bits |= TVPCompatBit(TVPCompatFlagStructConst);
		mask.bits |= TVPCompatBit(TVPCompatFlagStructCRLF);
		break;

	case TVPCompatModern:
	case TVPCompatNone:
	default:
		/* Nothing: the engine's own defaults are the modern behaviour, and a
		 * level that named no switches is indistinguishable from them. Written
		 * out rather than left to fall through silently, so adding a level
		 * means revisiting this. */
		break;
	}

	return mask;
}

//---------------------------------------------------------------------------
// Parsing
//---------------------------------------------------------------------------
static bool TVPCompatParseBool(const char *v, bool *out)
{
	if(!v) return false;
	if(!strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "true"))
	{
		*out = true;
		return true;
	}
	if(!strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "0") || !strcmp(v, "false"))
	{
		*out = false;
		return true;
	}
	return false;
}

const char *TVPCompatFlagName(TVPCompatFlag flag)
{
	switch(flag)
	{
	case TVPCompatFlagClassVar:      return "classvar";
	case TVPCompatFlagEvalContext:   return "evalcontext";
	case TVPCompatFlagHoldAlpha:     return "holdalpha";
	case TVPCompatFlagUnaryAsterisk: return "unaryaster";
	case TVPCompatFlagLayerShims:    return "layershims";
	case TVPCompatFlagComposite219:  return "composite219";
	case TVPCompatFlagStructConst:   return "structconst";
	case TVPCompatFlagStructCRLF:    return "structcrlf";
	default:                         return "(unknown)";
	}
}

const char *TVPCompatFlagDescription(TVPCompatFlag flag)
{
	switch(flag)
	{
	case TVPCompatFlagClassVar:
		return "class-scope `var` keeps a same-named native accessor";
	case TVPCompatFlagEvalContext:
		return "post-! operator evaluates on the global context";
	case TVPCompatFlagHoldAlpha:
		return "Layer.holdAlpha defaults to true";
	case TVPCompatFlagUnaryAsterisk:
		return "unary '*' ignores property access";
	case TVPCompatFlagLayerShims:
		return "load the TJS shims supplying the obsolete Layer API";
	case TVPCompatFlagComposite219:
		return "pre-2006 layer compositing algorithm (children's output stands)";
	case TVPCompatFlagStructConst:
		return "saveStruct writes \"%[\" rather than \"(const) %[\" (pre-2008)";
	case TVPCompatFlagStructCRLF:
		return "saveStruct writes CRLF rather than LF (Windows builds)";
	default:
		return "(unknown)";
	}
}

/* Local copies of the names, so the parser does not depend on the name table
 * having a particular order. */
static const struct
{
	const char *name;
	TVPCompatFlag flag;
} TVPCompatFlagNames[] = {
	{ "classvar",    TVPCompatFlagClassVar },
	{ "evalcontext", TVPCompatFlagEvalContext },
	{ "holdalpha",   TVPCompatFlagHoldAlpha },
	{ "unaryaster",  TVPCompatFlagUnaryAsterisk },
	{ "layershims",  TVPCompatFlagLayerShims },
	{ "composite219", TVPCompatFlagComposite219 },
	{ "structconst", TVPCompatFlagStructConst },
	{ "structcrlf",  TVPCompatFlagStructCRLF },
};

TVPCompatMask TVPCompatParse(const char *spec, TVPCompatMask mask)
{
	if(!spec || !*spec) return mask;

	/* One pass over a comma-separated list. Items are a level name, a flag
	 * name with a boolean, or a bare flag name (which turns it *on*, since
	 * writing the name is itself the request). */
	const char *p = spec;
	while(*p)
	{
		const char *item = p;
		while(*p && *p != ',') p++;
		size_t len = (size_t)(p - item);
		if(*p == ',') p++;

		/* Trim, into a local buffer; items are short. */
		char buf[64];
		if(len >= sizeof(buf)) len = sizeof(buf) - 1;
		memcpy(buf, item, len);
		buf[len] = 0;
		char *s = buf;
		while(*s == ' ' || *s == '\t') s++;
		char *e = s + strlen(s);
		while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
		if(!*s) continue;

		/* A level name in the spec. `modern` clears, because naming the modern
		 * level is an explicit request for this tree's own behaviour and must
		 * beat any preset the caller applied first. `auto` means "no explicit
		 * level": the default era stands, and the name is accepted here only so
		 * the frontend's own default value is not reported as an unknown flag.
		 * `none` is deliberately NOT a spelling of `modern`: "none" reads like
		 * "no spec", and a spec that silently zeroed the mask would turn a typo
		 * into a behavioural change. It falls through to the unknown-flag
		 * report. */
		if(!strcmp(s, "auto") || !strcmp(s, "default"))
		{
			continue;
		}
		if(!strcmp(s, "2.19") || !strcmp(s, "219") || !strcmp(s, "old"))
		{
			mask.bits |= TVPCompatMaskForLevel(TVPCompatV219).bits;
			continue;
		}
		if(!strcmp(s, "modern") || !strcmp(s, "new"))
		{
			mask.bits = 0;
			continue;
		}

		/* `name=value` or bare `name`. */
		char *eq = strchr(s, '=');
		bool on = true;
		if(eq)
		{
			*eq = 0;
			char *v = eq + 1;
			if(!TVPCompatParseBool(v, &on))
			{
				TVPAddImportantLog(ttstr(TJS_W("(warn) compat: unrecognised value for '"))
					+ ttstr(s) + TJS_W("' (use on|off); ignored"));
				continue;
			}
		}

		bool matched = false;
		for(size_t i = 0; i < sizeof(TVPCompatFlagNames) / sizeof(TVPCompatFlagNames[0]); i++)
		{
			if(strcmp(s, TVPCompatFlagNames[i].name)) continue;
			unsigned int bit = TVPCompatBit(TVPCompatFlagNames[i].flag);
			if(on) mask.bits |= bit; else mask.bits &= ~bit;
			matched = true;
			break;
		}
		if(!matched)
		{
			/* Reported, not fatal: a launcher's typo must not decide whether
			 * the game starts. */
			TVPAddImportantLog(ttstr(TJS_W("(warn) compat: unrecognised flag '"))
				+ ttstr(s) + TJS_W("'; ignored"));
		}
	}

	return mask;
}

//---------------------------------------------------------------------------
// Per-game selection
//
// The table keys a game by the identity the rest of the project already uses:
// the crc32 of its data file, as eight upper-case hex digits. That is what
// tools/hash-game.py prints and what dat/vnovel.dat records, and the core
// exports the id it computed in KRKRSDL2_GAME_ID, so the table, the database
// and the tools agree about which game is which without a second fingerprint
// scheme.
//
// Adding an entry: run `tools/hash-game.py <game folder>`, take the `crc` it
// prints, and add a row below naming the era the title shipped against. A row's
// spec is any KRKRSDL2_COMPAT string, so a title that needs one switch off its
// era is `"2.19,holdalpha=off"` rather than a new level.
//---------------------------------------------------------------------------
const char *TVPCompatSourceName(TVPCompatSource source)
{
	switch(source)
	{
	case TVPCompatSourceTable:    return "table";
	case TVPCompatSourceUserGame: return "user (per-game)";
	case TVPCompatSourceUserRun:  return "user (per-run)";
	case TVPCompatSourceDefault:  return "default";
	default:                      return "(unknown)";
	}
}

struct TVPCompatGameEntry
{
	const char *game_id;   /* upper-case crc32 of the data file, as hash-game.py prints it */
	const char *spec;      /* the era, as a KRKRSDL2_COMPAT spec string */
	const char *title;     /* for the log, so a reader knows which game matched */
};

static const TVPCompatGameEntry TVPCompatGameTable[] = {
	/* Fate/stay night (English patch), data.xp3 — the title this port is
	 * developed against, and the reason the era machinery exists. Its scripts
	 * are 2.19-era: `var face` on a native-Layer subclass needs the accessor,
	 * the post-! operator expects the global context, Layer.holdAlpha defaults
	 * true and unary '*' takes direct property access (see the 2.19 preset's
	 * own notes above for the evidence). Identity from
	 * `tools/hash-game.py /home/matcha/Fate-Eng-Install`:
	 *   data.xp3 size 7960837 crc 64960AF9
	 * The crc is the same one dat/vnovel.dat carries for this release. */
	{ "64960AF9", "2.19", "Fate/stay night" },
};

static bool TVPCompatGameIdEqual(const char *a, const char *b)
{
	for(; *a && *b; a++, b++)
	{
		int ca = (unsigned char)*a, cb = (unsigned char)*b;
		if(ca >= 'a' && ca <= 'z') ca -= 'a' - 'A';
		if(cb >= 'a' && cb <= 'z') cb -= 'a' - 'A';
		if(ca != cb) return false;
	}
	return *a == *b;
}

static const TVPCompatGameEntry *TVPCompatLookupGame(const char *game_id)
{
	if(!game_id || !*game_id) return NULL;
	for(size_t i = 0; i < sizeof(TVPCompatGameTable) / sizeof(TVPCompatGameTable[0]); i++)
	{
		const TVPCompatGameEntry *e = &TVPCompatGameTable[i];
		if(!TVPCompatGameIdEqual(e->game_id, game_id)) continue;
		return e;
	}
	return NULL;
}

/* The per-game user override: KRKRSDL2_COMPAT_<ID>, e.g.
 * KRKRSDL2_COMPAT_64960AF9=modern. A launcher or a developer pins one title
 * without editing the table or rebuilding the engine. */
static const char *TVPCompatUserGameSpec(const char *game_id, char *name, size_t namelen)
{
	if(!game_id || !*game_id) return NULL;
	if(snprintf(name, namelen, "KRKRSDL2_COMPAT_%s", game_id) < 0) return NULL;
	return getenv(name);
}

/* The resolved set as one line, e.g. "classvar, evalcontext, holdalpha". Shared
 * by the provenance log (below) and the applied-set log in TVPApplyCompatMask,
 * so the two cannot describe the same mask differently. */
static void TVPCompatFormatFlags(TVPCompatMask mask, char *out, size_t outlen)
{
	bool any = false;
	out[0] = 0;
	for(int i = 0; i < TVPCompatFlagCount; i++)
	{
		TVPCompatFlag f = (TVPCompatFlag)i;
		if(!TVPCompatHasFlag(mask, f)) continue;
		size_t used = strlen(out);
		if(used + 1 >= outlen) break;
		snprintf(out + used, outlen - used, "%s%s", any ? ", " : "", TVPCompatFlagName(f));
		any = true;
	}
	if(!any && outlen) snprintf(out, outlen, "none (this tree's defaults)");
}

TVPCompatMask TVPCompatResolve(const char *game_id, TVPCompatSource *source)
{
	TVPCompatMask mask;
	TVPCompatSource src = TVPCompatSourceDefault;
	const char *matched_title = NULL;

	/* 1. the per-run spec: the user overriding for this session. */
	const char *spec = getenv("KRKRSDL2_COMPAT");
	if(spec && *spec)
	{
		mask = TVPCompatParse(spec, TVPCompatMaskForLevel(TVP_COMPAT_DEFAULT_LEVEL));
		src = TVPCompatSourceUserRun;
	}
	else
	{
		/* 2. a per-game user override, then 3. the table. Both key on the
		 * identity the core computed; either may be absent. */
		bool resolved = false;
		if(game_id && *game_id)
		{
			char name[64];
			const char *game_spec = TVPCompatUserGameSpec(game_id, name, sizeof(name));
			if(game_spec && *game_spec)
			{
				mask = TVPCompatParse(game_spec, TVPCompatMaskForLevel(TVP_COMPAT_DEFAULT_LEVEL));
				src = TVPCompatSourceUserGame;
				resolved = true;
			}
			if(!resolved)
			{
				const TVPCompatGameEntry *entry = TVPCompatLookupGame(game_id);
				if(entry)
				{
					mask = TVPCompatParse(entry->spec, TVPCompatMaskForLevel(TVP_COMPAT_DEFAULT_LEVEL));
					src = TVPCompatSourceTable;
					matched_title = entry->title;
					resolved = true;
				}
			}
		}
		if(!resolved)
		{
			/* 4. nothing knew the game: the documented last resort. */
			mask = TVPCompatMaskForLevel(TVP_COMPAT_DEFAULT_LEVEL);
			src = TVPCompatSourceDefault;
		}
	}

	if(source) *source = src;

	/* Say which source chose the era, and what it resolved to. This is the
	 * first question a misbehaving title raises, and it is not derivable from
	 * the mask alone. Written to stderr as well, because in this tree
	 * TVPAddImportantLog goes to a deque that nothing drains (see the note in
	 * TVPApplyCompatMask). */
	{
		char flags[256];
		char title[128];
		TVPCompatFormatFlags(mask, flags, sizeof(flags));
		if(matched_title) snprintf(title, sizeof(title), " (%s)", matched_title);
		else title[0] = 0;
		const char *id = (game_id && *game_id) ? game_id : "-";

		ttstr line(ttstr(TJS_W("(info) compat: mask from ")) + ttstr(TVPCompatSourceName(src))
			+ TJS_W(" for ") + ttstr(id) + ttstr(title) + TJS_W(": ") + ttstr(flags));
		TVPAddImportantLog(line);

		fprintf(stderr, "(info) compat: mask from %s for %s%s: %s\n",
			TVPCompatSourceName(src), id, title, flags);
	}

	return mask;
}

//---------------------------------------------------------------------------
// Applying
//---------------------------------------------------------------------------
/* True when the engine was given this option on its command line, in which case
 * the explicit request wins over the mask. */
static bool TVPCompatOptionGiven(const tjs_char *name)
{
	tTJSVariant v;
	return TVPGetCommandLine(name, &v);
}

void TVPApplyCompatMask(TVPCompatMask mask)
{
	TVPCompatLastMask = mask;
	const bool classvar    = TVPCompatHasFlag(mask, TVPCompatFlagClassVar) != 0;
	const bool evalcontext = TVPCompatHasFlag(mask, TVPCompatFlagEvalContext) != 0;
	const bool holdalpha   = TVPCompatHasFlag(mask, TVPCompatFlagHoldAlpha) != 0;
	const bool unaryaster  = TVPCompatHasFlag(mask, TVPCompatFlagUnaryAsterisk) != 0;

	/* class-scope `var`: read by the code generator, so it is applied once and
	 * affects only scripts compiled after this point. */
	if(!TVPCompatOptionGiven(TJS_W("-classvar")))
		TJS::TJSClassVarHonoursNativeProperty = classvar;

	/* eval-context and unary-'*': read when the interpreter resolves these
	 * options, i.e. during TVPInitScriptEngine. Applying them through the same
	 * option stock the command line uses means the engine reads them from one
	 * place, and an explicit -evalcontext / -unaryaster still wins because
	 * TVPGetCommandLine is consulted first, there. */
	if(!TVPCompatOptionGiven(TJS_W("-evalcontext")))
	{
		TJS::TJSEvalOperatorIsOnGlobal = evalcontext;
		TJS::TJSWarnOnNonGlobalEvalOperator = evalcontext;
	}
	if(!TVPCompatOptionGiven(TJS_W("-unaryaster")))
		TJS::TJSUnaryAsteriskIgnoresPropAccess = unaryaster;

	/* hold-alpha: read when each Layer is constructed, so a layer built after
	 * this call sees it. Late changes would affect later layers only, which is
	 * why it is applied here with the rest rather than exposed as a live one. */
	if(!TVPCompatOptionGiven(TJS_W("-holdalpha")))
		TVPDefaultHoldAlpha = holdalpha;

	/* Layer-compositing algorithm.
	 *
	 * Bound here, at the one point the resolved mask is applied, and therefore
	 * before TVPApplyCompatMask's caller starts the scripting engine and long
	 * before any layer is composited. The level is derived from the mask rather
	 * than read as a flag inside the compositing loop: the two eras are different
	 * algorithms, and CompositeLevel refuses a re-bind so a live layer tree can
	 * never see both. See CompositeLevel.h. */
	TVPBindCompositeLevel(TVPCompatHasFlag(mask, TVPCompatFlagComposite219)
		? TVPCompositeLegacy219
		: TVPCompositeModern);

	/* Structured-save wire format. Bound here with the rest, but unlike the
	 * compile-time switches it is read when a save is serialised, so a change
	 * takes effect on the next save. The two save bits move together because
	 * they describe one format: a file with the 2.19 prefix and this build's
	 * newline, or the reverse, is readable by neither era. */
	TVPBindStructFormat(TVPCompatHasFlag(mask, TVPCompatFlagStructConst)
		? TVPStructFormatLegacy219
		: TVPStructFormatModern);

	/* Say what was resolved, flag by flag. A run's log then states which
	 * behaviours were selected instead of implying them from a version, which
	 * is the whole point of building a mask per game. */
	{
		char flags[256];
		TVPCompatFormatFlags(mask, flags, sizeof(flags));
		TVPAddImportantLog(ttstr(TJS_W("(info) engine compatibility: ")) + ttstr(flags));

		/* Also to stderr, because in this tree TVPAddImportantLog appends to an
		 * in-memory deque that nothing drains (the KiriKiri 2 fork installed a
		 * TVPSDLOnLog handler; krkrsdl2 does not), so a mask logged only through
		 * it is invisible in every run — which is how a resolved-switch line came
		 * to be written and never once observed. stderr is what this tree's other
		 * start-up diagnostics use, so it is where a reader looks. */
		fprintf(stderr, "(info) engine compatibility: %s\n", flags);
	}
}

//---------------------------------------------------------------------------
// Script-visible view
//---------------------------------------------------------------------------
/* Publish the resolved switches as globals, so a TJS shim or a user patch can
 * branch on exactly the values the engine resolved instead of re-deriving them
 * - which is the failure mode that makes a shim behave differently from the
 * engine it is standing in for.
 *
 * One boolean per switch, named `__krkrCompat_<flag>`, plus the resolved set as
 * a comma-separated string for logging from script. Plain values rather than a
 * native class: nothing here needs a method, and a global that always exists is
 * what a shim's `if` needs (in TJS, *naming* an undefined identifier throws, so
 * a shim cannot test for absence and needs the value present).
 *
 * Called after the scripting engine initialises and before any script runs, so
 * the polyfill's own shims can read it. */
void TVPExposeCompatToScript(void)
{
	iTJSDispatch2 *global = TVPGetScriptDispatch();
	if(!global) return;

	for(int i = 0; i < TVPCompatFlagCount; i++)
	{
		TVPCompatFlag f = (TVPCompatFlag)i;
		tjs_string name(TJS_W("__krkrCompat_"));
		const char *n = TVPCompatFlagName(f);
		for(const char *p = n; *p; p++) name += (tjs_char)*p;

		tTJSVariant v(TVPCompatHasFlag(TVPCompatLastMask, f) ? 1 : 0);
		global->PropSet(TJS_MEMBERENSURE, name.c_str(), NULL, &v, global);
	}

	/* The mask itself, for a script that wants to log or compare it. */
	{
		ttstr spec;
		bool any = false;
		for(int i = 0; i < TVPCompatFlagCount; i++)
		{
			TVPCompatFlag f = (TVPCompatFlag)i;
			if(!TVPCompatHasFlag(TVPCompatLastMask, f)) continue;
			if(any) spec += TJS_W(",");
			spec += ttstr(TVPCompatFlagName(f));
			any = true;
		}
		tTJSVariant v(spec);
		global->PropSet(TJS_MEMBERENSURE, TJS_W("__krkrCompatMask"), NULL, &v, global);
	}

	global->Release();
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
