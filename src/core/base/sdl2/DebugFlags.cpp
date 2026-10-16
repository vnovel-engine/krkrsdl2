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
uint32_t krkrsdl2_debug_parse(const char *spec)
{
	if(!spec || !*spec) return 0;

	uint32_t bits = 0;
	const char *p = spec;
	while(*p)
	{
		const char *item = p;
		while(*p && *p != ',') p++;
		size_t len = (size_t)(p - item);
		if(*p == ',') p++;

		char buf[64];
		if(len >= sizeof(buf)) len = sizeof(buf) - 1;
		memcpy(buf, item, len);
		buf[len] = 0;

		char *s = buf;
		while(*s == ' ' || *s == '\t') s++;
		char *e = s + strlen(s);
		while(e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
		if(!*s) continue;

		if(!strcmp(s, "all"))
		{
			bits = KRKRSDL2_DBG_ALL;
			continue;
		}
		if(!strcmp(s, "none") || !strcmp(s, "off"))
		{
			bits = 0;
			continue;
		}

		/* A raw word, for the core and for a launcher that already has one. */
		if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		{
			bits |= (uint32_t)strtoul(s + 2, NULL, 16);
			continue;
		}
		if(s[0] >= '0' && s[0] <= '9')
		{
			bits |= (uint32_t)strtoul(s, NULL, 10);
			continue;
		}

		bool matched = false;
		for(size_t i = 0; i < krkrsdl2_debug_bit_count; i++)
		{
			if(strcmp(s, krkrsdl2_debug_bits[i].name)) continue;
			bits |= krkrsdl2_debug_bits[i].bit;
			matched = true;
			break;
		}
		if(!matched)
		{
			/* Reported, not fatal - and this is the one place a wrong probe name
			 * can be noticed, which is half the reason the table exists. */
			fprintf(stderr, "(warn) krkrsdl2: unknown debug probe '%s'; ignored\n", s);
			fflush(stderr);
		}
	}

	return bits;
}

//---------------------------------------------------------------------------
void krkrsdl2_debug_set(uint32_t bits)
{
	g_flags          = bits;
	g_flags_resolved = 1;
	g_flags_forced   = 1;
}

//---------------------------------------------------------------------------
uint32_t krkrsdl2_debug_flags(void)
{
	if(g_flags_resolved < 0)
	{
		g_flags_resolved = 1;
		if(!g_flags_forced)
			g_flags = krkrsdl2_debug_parse(getenv("KRKRSDL2_DEBUG"));
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
