//---------------------------------------------------------------------------
/*
	Debug switches — implementation. See DebugFlags.h for why this exists and
	what belongs here (observation only, never behaviour).
*/
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "DebugFlags.h"

//---------------------------------------------------------------------------
/* The table. One row per bit, so a probe's name has exactly one definition and
 * "describe" and "parse" cannot drift apart. Descriptions are one line and say
 * what the probe answers, because a log reader wants to know that, not which
 * function prints. */
//---------------------------------------------------------------------------
struct KrkrDebugBit
{
	uint32_t    bit;
	const char *name;
	const char *description;
};

static const KrkrDebugBit krkrsdl2_debug_bits[] = {
	{ KRKRSDL2_DBG_PROVINCE_CONTOUR, "province",
	  "outline each province rectangle, with its rect/colour/origin" },
	{ KRKRSDL2_DBG_FILL, "fill",
	  "each fill/colourRect call: rect, colour, face" },
	{ KRKRSDL2_DBG_SCRIPT, "script",
	  "each scenario tag as it executes" },
	{ KRKRSDL2_DBG_INPUT, "input",
	  "per-poll input state as the engine sees it" },
	{ KRKRSDL2_DBG_DRAW, "draw",
	  "the transfer/composite branch each completed draw resolves to" },
	{ KRKRSDL2_DBG_SELECT, "select",
	  "pending-select refusal code: which guard declined the ordinal" },
	{ KRKRSDL2_DBG_SPLIT, "split",
	  "which layers reach the composite, their text, the artwork-only skip" },
};

static const size_t krkrsdl2_debug_bit_count =
	sizeof(krkrsdl2_debug_bits) / sizeof(krkrsdl2_debug_bits[0]);

//---------------------------------------------------------------------------
/* -1 = not yet resolved. Not 0, so "no probes" and "not asked yet" are
 * distinguishable while debugging this header itself. */
static int      g_flags_resolved = -1;
static uint32_t g_flags          = 0;

/* Set by krkrsdl2_debug_set, and whatever the environment says is then ignored:
 * the host's explicit choice outranks a variable that may have been inherited
 * from a shell. */
static int      g_flags_forced   = 0;

//---------------------------------------------------------------------------
const char *krkrsdl2_debug_name(uint32_t bit)
{
	for(size_t i = 0; i < krkrsdl2_debug_bit_count; i++)
		if(krkrsdl2_debug_bits[i].bit == bit) return krkrsdl2_debug_bits[i].name;
	return "(unknown)";
}

//---------------------------------------------------------------------------
/* Trim leading/trailing blanks in place; returns the start. */
static char *krkrsdl2_trim(char *s)
{
	while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
	char *e = s + strlen(s);
	while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
	                e[-1] == '\n'))
		*--e = 0;
	return s;
}

/* If `*item` carries a "krkr:" qualifier, drop it and return 1. If it carries
 * another module's qualifier, return 0 so the caller skips the item. No
 * qualifier returns 1. */
static int krkrsdl2_strip_prefix(char **item)
{
	char *colon = strchr(*item, ':');
	if(!colon) return 1;
	static const char prefix[] = "krkr";
	size_t n = (size_t)(colon - *item);
	if(n != sizeof(prefix) - 1 || strncmp(*item, prefix, n)) return 0;
	*item = colon + 1;
	return 1;
}

/* Match one name against the table. Returns the bit, or 0 with *matched=0. */
static uint32_t krkrsdl2_match_name(const char *s, int *matched)
{
	for(size_t i = 0; i < krkrsdl2_debug_bit_count; i++)
	{
		if(strcmp(s, krkrsdl2_debug_bits[i].name)) continue;
		*matched = 1;
		return krkrsdl2_debug_bits[i].bit;
	}
	*matched = 0;
	return 0;
}

/* Iterate the comma/space-separated items of `spec`, handing each trimmed item
 * to `apply`. Splits on commas and on runs of spaces/tabs (VNOVEL_DEBUG allows
 * either separator). */
static void krkrsdl2_for_each_item(const char *spec,
                                   void (*apply)(char *item, void *ctx),
                                   void *ctx)
{
	const char *p = spec;
	while(*p)
	{
		char buf[64];
		const char *item = p;
		while(*p && *p != ',' && *p != ' ' && *p != '\t') p++;
		size_t len = (size_t)(p - item);
		while(*p == ',' || *p == ' ' || *p == '\t') p++;
		if(!len) continue;
		if(len >= sizeof(buf)) len = sizeof(buf) - 1;
		memcpy(buf, item, len);
		buf[len] = 0;
		char *s = krkrsdl2_trim(buf);
		if(*s) apply(s, ctx);
	}
}

/* Context for the KRKRSDL2_DEBUG parser: names and raw words OR into `bits`,
 * "all"/"none"/"off" set or clear it. A raw word here is the LOCAL slice. */
struct krkrsdl2_parse_ctx { uint32_t bits; };

static void krkrsdl2_apply_local(char *s, void *ctx)
{
	struct krkrsdl2_parse_ctx *c = (struct krkrsdl2_parse_ctx *)ctx;

	if(!strcmp(s, "all"))     { c->bits = KRKRSDL2_DBG_ALL; return; }
	if(!strcmp(s, "none") ||
	   !strcmp(s, "off"))     { c->bits = 0;               return; }

	if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
	{
		c->bits |= (uint32_t)strtoul(s + 2, NULL, 16);
		return;
	}
	if(s[0] >= '0' && s[0] <= '9')
	{
		c->bits |= (uint32_t)strtoul(s, NULL, 10);
		return;
	}

	int matched = 0;
	uint32_t b = krkrsdl2_match_name(s, &matched);
	if(matched) { c->bits |= b; return; }

	/* Reported, not fatal - the one place a wrong probe name is noticed. */
	fprintf(stderr, "(warn) krkrsdl2: unknown debug probe '%s'; ignored\n", s);
	fflush(stderr);
}

//---------------------------------------------------------------------------
uint32_t krkrsdl2_debug_parse(const char *spec)
{
	if(!spec || !*spec) return 0;
	struct krkrsdl2_parse_ctx c = { 0 };
	krkrsdl2_for_each_item(spec, krkrsdl2_apply_local, &c);
	return c.bits;
}

/* Context for the VNOVEL_DEBUG parser. Names and "all" accumulate in `local`
 * (unshifted, slice-local bits); a raw word is the WHOLE 64-bit mask, kept in
 * `word` and reduced to this slice at the end. "none"/"off" clear both. */
struct krkrsdl2_vnovel_ctx { uint32_t local; uint64_t word; };

static void krkrsdl2_apply_vnovel(char *s, void *ctx)
{
	struct krkrsdl2_vnovel_ctx *c = (struct krkrsdl2_vnovel_ctx *)ctx;

	/* A qualifier naming another module is skipped without complaint: one
	 * VNOVEL_DEBUG drives every module, and this one owns only "krkr". */
	if(!krkrsdl2_strip_prefix(&s)) return;
	s = krkrsdl2_trim(s);
	if(!*s) return;

	if(!strcmp(s, "all"))
	{
		c->local = KRKRSDL2_DBG_ALL;
		return;
	}
	if(!strcmp(s, "none") || !strcmp(s, "off"))
	{
		c->local = 0;
		c->word  = 0;
		return;
	}

	/* A raw word is the whole 64-bit mask, not slice-local. */
	if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
	{
		c->word |= (uint64_t)strtoull(s + 2, NULL, 16);
		return;
	}
	if(s[0] >= '0' && s[0] <= '9')
	{
		c->word |= (uint64_t)strtoull(s, NULL, 10);
		return;
	}

	int matched = 0;
	uint32_t b = krkrsdl2_match_name(s, &matched);
	if(matched) c->local |= b;
	/* A name this module does not own is another module's: ignored without a
	 * warning, the way a foreign qualifier is. */
}

//---------------------------------------------------------------------------
uint32_t krkrsdl2_debug_parse_vnovel(const char *spec)
{
	if(!spec || !*spec) return 0;
	struct krkrsdl2_vnovel_ctx c = { 0, 0 };
	krkrsdl2_for_each_item(spec, krkrsdl2_apply_vnovel, &c);
	/* Local names land in `local`; the raw word's slice is shifted back to the
	 * local bit space. The two are ORed, so a mixed spec works. */
	uint64_t slice = (c.word >> KRKRSDL2_DEBUG_SHIFT) &
	                 (((uint64_t)1 << KRKRSDL2_DEBUG_WIDTH) - 1);
	return c.local | (uint32_t)slice;
}

//---------------------------------------------------------------------------
void krkrsdl2_debug_set(uint32_t bits)
{
	g_flags          = bits;
	g_flags_resolved = 1;
	g_flags_forced   = 1;
}

//---------------------------------------------------------------------------
/* The per-probe environment variables that pre-date this table. Each is an
 * alias for its bit: a set (non-NULL) variable turns the bit on, regardless of
 * its value, matching the original `getenv(...) ? 1 : 0` probes. ORed into the
 * word so a launcher that still sets the old name works alongside the table.
 * KRKR_INPUT_DEBUG is read by the core, not here, and has no alias. */
static uint32_t krkrsdl2_legacy_aliases(void)
{
	uint32_t bits = 0;
	if(getenv("KRKR_PROBE_FILL"))   bits |= KRKRSDL2_DBG_FILL;
	if(getenv("KRKR_SCRIPT_DEBUG")) bits |= KRKRSDL2_DBG_SCRIPT;
	if(getenv("KRKR_SELECT_DEBUG")) bits |= KRKRSDL2_DBG_SELECT;
	if(getenv("KRKR_SPLIT_DEBUG"))  bits |= KRKRSDL2_DBG_SPLIT;
	return bits;
}

//---------------------------------------------------------------------------
uint32_t krkrsdl2_debug_flags(void)
{
	if(g_flags_resolved < 0)
	{
		g_flags_resolved = 1;
		if(!g_flags_forced)
		{
			/* KRKRSDL2_DEBUG is the engine's own channel and wins over the
			 * shared mask: the core sets it from a frontend option that a
			 * libretro frontend cannot express as an environment variable. When
			 * it is absent, fall back to VNOVEL_DEBUG and read this module's
			 * slice. Legacy per-probe variables are ORed in either way. */
			const char *krkr = getenv("KRKRSDL2_DEBUG");
			if(krkr && *krkr)
				g_flags = krkrsdl2_debug_parse(krkr);
			else
				g_flags = krkrsdl2_debug_parse_vnovel(getenv("VNOVEL_DEBUG"));
			g_flags |= krkrsdl2_legacy_aliases();
		}
	}
	return g_flags;
}

//---------------------------------------------------------------------------
int krkrsdl2_debug_on(uint32_t bit)
{
	return (krkrsdl2_debug_flags() & bit) != 0;
}

//---------------------------------------------------------------------------
void krkrsdl2_debug_describe(void)
{
	const uint32_t on = krkrsdl2_debug_flags();
	if(!on)
	{
		fprintf(stderr, "(info) krkrsdl2: no debug probes\n");
		fflush(stderr);
		return;
	}
	for(size_t i = 0; i < krkrsdl2_debug_bit_count; i++)
	{
		if(!(on & krkrsdl2_debug_bits[i].bit)) continue;
		fprintf(stderr, "(info) krkrsdl2: debug probe '%s' (%s)\n",
			krkrsdl2_debug_bits[i].name, krkrsdl2_debug_bits[i].description);
	}
	fflush(stderr);
}
//---------------------------------------------------------------------------
