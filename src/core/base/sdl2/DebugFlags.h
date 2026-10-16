//---------------------------------------------------------------------------
/*
	Debug switches for krkrsdl2 — one word, queried in one place.

	Diagnostic output in this port grew as a scatter of ad-hoc environment
	checks: each probe test-guessed its own variable on first use
	(KRKR_PROBE_FILL, KRKR_PROVINCE_CONTOUR, KRKR_SCRIPT_DEBUG, ...). That has
	three costs, all of which have already been paid here:

	  - a probe's state cannot be inspected. Nothing can say which probes are on,
	    so "why did I get no output" has no answer short of grepping the source.
	  - naming is inconsistent, so a probe is easy to switch on and forget, and
	    easy to switch on under the wrong name and believe it is running.
	  - a probe added behind TVP_DEBUG_PROBES is only in the build when the macro
	    is defined, which is a *second* axis an operator has to know about.

	This header replaces the first two with a single 32-bit word and a table.
	Callers ask for a bit; the table says what each bit is called and what it
	does, so the set can be listed, and one call site decides what "all" means.

	How it is set
	-------------
	KRKRSDL2_DEBUG is a comma-separated list of names, or a hex/decimal word:

	    KRKRSDL2_DEBUG=province,fill        # two probes
	    KRKRSDL2_DEBUG=all                  # every bit
	    KRKRSDL2_DEBUG=0x8000000c           # raw word, for the core
	    KRKRSDL2_DEBUG=province,0x0c        # names and words mix

	A libretro frontend cannot easily set the environment, so the core sets this
	variable from a core option before the engine starts (see
	krkr_set_debug_flags); the string form exists so a developer and a launcher
	can use the same mechanism without the frontend.

	Ordering
	--------
	The word is read once, at first use, and cached. A probe registered after the
	first query is still honoured because the cache only feeds the *bit test*, not
	the table - but a probe cannot be turned on mid-run, which is deliberate:
	diagnostics that can change under a run make two halves of one log
	incomparable.

	What NOT to put here
	--------------------
	Behavioural switches belong in CompatProfile (they change what the engine
	does); this is only for observation. A bit here must not alter output.
*/
//---------------------------------------------------------------------------
#ifndef KRKRSDL2_DEBUG_H
#define KRKRSDL2_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One bit per probe. The numbering is the ABI for KRKRSDL2_DEBUG=<word>, so a
 * value is stable: append here, never renumber. */
enum
{
	/* Print every write to a province plane, with the rectangle, colour and the
	 * layer's primary-coordinate origin, and outline the rectangle on the main
	 * image. Establishes whether the hit-test plane is stamped where the game
	 * intends - the question a clickability bug turns on, given that a province
	 * plane is invisible by nature. */
	KRKRSDL2_DBG_PROVINCE_CONTOUR = 1u << 0,

	/* Print each fill/colour-rect call with its rectangle, colour and face, for
	 * tracing what draws over what. */
	KRKRSDL2_DBG_FILL = 1u << 1,

	/* Trace scenario tags as they execute (KAGParser reads the flag). Tells a
	 * script bug from a rendering bug. */
	KRKRSDL2_DBG_SCRIPT = 1u << 2,

	/* Per-poll input state: the RetroPad/MOUSE values the engine sees. Reports
	 * what the engine receives rather than what the frontend sent, which is the
	 * distinction a "click did nothing" report needs. */
	KRKRSDL2_DBG_INPUT = 1u << 3,

	/* Draw-branch trace: which compositing/transfer path each completed draw
	 * resolved to. */
	KRKRSDL2_DBG_DRAW = 1u << 4,

	/* Reserved space for the next probes. Bits are cheap; a probe that needs a
	 * name gets one here rather than inventing another environment variable. */
	KRKRSDL2_DBG_RESERVED_5 = 1u << 5,
	KRKRSDL2_DBG_RESERVED_6 = 1u << 6,
	KRKRSDL2_DBG_RESERVED_7 = 1u << 7,
};

/* Every defined bit, so "all" has one definition rather than a literal at each
 * site. */
#define KRKRSDL2_DBG_ALL \
	(KRKRSDL2_DBG_PROVINCE_CONTOUR | KRKRSDL2_DBG_FILL | KRKRSDL2_DBG_SCRIPT | \
	 KRKRSDL2_DBG_INPUT | KRKRSDL2_DBG_DRAW)

/* The resolved word. Parsed once from KRKRSDL2_DEBUG on first call. */
uint32_t krkrsdl2_debug_flags(void);

/* True when `bit` is set. The form call sites should use. */
int krkrsdl2_debug_on(uint32_t bit);

/* Override the word directly (the core does this from a core option, because a
 * frontend cannot set the environment cheaply). Must be called before the
 * engine's first krkrsdl2_debug_flags(), i.e. during start-up. */
void krkrsdl2_debug_set(uint32_t bits);

/* Parse a spec ("province,fill", "all", "0x0c") into a word. Exposed so the core
 * can build a word from a frontend string without duplicating the names. Unknown
 * names are reported to stderr and skipped rather than failing: a typo in a
 * launcher's config must not decide whether the game starts. */
uint32_t krkrsdl2_debug_parse(const char *spec);

/* Write the resolved set to stderr, one probe per line, so a log states which
 * diagnostics were live. Called by the core at start-up. */
void krkrsdl2_debug_describe(void);

/* The name of `bit`, or "(unknown)" - for logs and the description. */
const char *krkrsdl2_debug_name(uint32_t bit);

#ifdef __cplusplus
}
#endif

#endif /* KRKRSDL2_DEBUG_H */
