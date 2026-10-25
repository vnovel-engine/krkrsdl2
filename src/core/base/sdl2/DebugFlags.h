//---------------------------------------------------------------------------
/*
	Debug switches for krkrsdl2 — one word, queried in one place.

	The word is the KiriKiri slice (bits 24-39) of the shared 64-bit debug
	mask defined by src/vnovel_debug.h in the core repo. krkrsdl2 builds
	standalone from its own checkout and cannot include the core's headers,
	so the shift/width constants below are the local copy of that layout
	contract; they must match vnovel_debug.h (shift 24, width 16).

	Callers ask for a bit; the table says what each bit is called and what
	it answers, so the set can be listed, and one call site decides what
	"all" means.

	How it is set
	-------------
	KRKRSDL2_DEBUG is a comma-separated list of names, or a hex/decimal
	word. A raw word is the LOCAL slice (bits 0-15), because the core
	hands the already-shifted slice through this channel (see
	krkr_set_debug_flags in the core):

	    KRKRSDL2_DEBUG=province,fill        # two probes
	    KRKRSDL2_DEBUG=all                  # every defined bit
	    KRKRSDL2_DEBUG=0x000c               # raw local-slice word
	    KRKRSDL2_DEBUG=province,0x0c        # names and words mix

	When KRKRSDL2_DEBUG is absent, VNOVEL_DEBUG is read instead. That word
	is the whole 64-bit mask; this module applies only its own slice and
	its own name table. Names in VNOVEL_DEBUG may carry a "krkr:" qualifier;
	a name without a qualifier is treated as this module's, and a name
	qualified by another module's prefix is ignored without complaint (one
	VNOVEL_DEBUG drives every module, each reading its own slice).

	A libretro frontend cannot set the environment, so the core sets
	KRKRSDL2_DEBUG from a core option before the engine starts; the string
	form exists so a developer and a launcher use the same mechanism.

	Legacy aliases
	-------------
	Each probe that pre-dates this table had its own environment variable.
	Those names still turn their bit on (an OR of the legacy variable and
	the table word), so existing launchers and tests do not break:

	    KRKR_PROBE_FILL    -> fill
	    KRKR_SCRIPT_DEBUG  -> script
	    KRKR_SELECT_DEBUG  -> select
	    KRKR_SPLIT_DEBUG   -> split

	KRKR_INPUT_DEBUG is read by the core (src/engine_kirikiri_cabi.cpp),
	not here. No new environment variables are added beyond those.

	Ordering
	--------
	The word is read once, at first use, and cached. A probe cannot be
	turned on mid-run: diagnostics that change under a run make two halves
	of one log incomparable.

	What NOT to put here
	--------------------
	Behavioural switches belong in CompatProfile (they change what the
	engine does); this is only for observation. A bit here must not alter
	output.
*/
//---------------------------------------------------------------------------
#ifndef KRKRSDL2_DEBUG_H
#define KRKRSDL2_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where this module's bits live in the shared 64-bit mask, and how many it
 * owns. These mirror src/vnovel_debug.h (VNOVEL_DEBUG_SHIFT_KRKR /
 * VNOVEL_DEBUG_WIDTH_KRKR) and must not drift from it; the core and this
 * engine agree on them without sharing a header. */
#define KRKRSDL2_DEBUG_SHIFT 24u
#define KRKRSDL2_DEBUG_WIDTH 16u

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
	 * tracing what draws over what. Alias: KRKR_PROBE_FILL. */
	KRKRSDL2_DBG_FILL = 1u << 1,

	/* Trace scenario tags as they execute (KAGParser reads the flag). Tells a
	 * script bug from a rendering bug. Alias: KRKR_SCRIPT_DEBUG. */
	KRKRSDL2_DBG_SCRIPT = 1u << 2,

	/* Per-poll input state: the RetroPad/MOUSE values the engine sees. Reports
	 * what the engine receives rather than what the frontend sent, which is the
	 * distinction a "click did nothing" report needs. */
	KRKRSDL2_DBG_INPUT = 1u << 3,

	/* Draw-branch trace: which compositing/transfer path each completed draw
	 * resolved to. */
	KRKRSDL2_DBG_DRAW = 1u << 4,

	/* Refusal code from the pending-select path: which guard declined a choice
	 * ordinal (no kag, no links yet, ordinal out of range). Alias:
	 * KRKR_SELECT_DEBUG. */
	KRKRSDL2_DBG_SELECT = 1u << 5,

	/* Which layers reach the composite, whether they carry text, and whether
	 * the artwork-only skip takes them. Alias: KRKR_SPLIT_DEBUG. */
	KRKRSDL2_DBG_SPLIT = 1u << 6,

	/* Reserved for the next probe that needs a name. */
	KRKRSDL2_DBG_RESERVED_7 = 1u << 7,
};

/* Every defined bit, so "all" has one definition rather than a literal at each
 * site. Reserved bits are excluded: "all" means every probe that does
 * something. */
#define KRKRSDL2_DBG_ALL \
	(KRKRSDL2_DBG_PROVINCE_CONTOUR | KRKRSDL2_DBG_FILL | KRKRSDL2_DBG_SCRIPT | \
	 KRKRSDL2_DBG_INPUT | KRKRSDL2_DBG_DRAW | KRKRSDL2_DBG_SELECT | \
	 KRKRSDL2_DBG_SPLIT)

/* The resolved word. Parsed once from KRKRSDL2_DEBUG (or VNOVEL_DEBUG when that
 * is absent) on first call. */
uint32_t krkrsdl2_debug_flags(void);

/* True when `bit` is set. The form call sites should use. */
int krkrsdl2_debug_on(uint32_t bit);

/* Override the word directly (the core does this from a core option, because a
 * frontend cannot set the environment cheaply). Must be called before the
 * engine's first krkrsdl2_debug_flags(), i.e. during start-up. */
void krkrsdl2_debug_set(uint32_t bits);

/* Parse a spec ("province,fill", "all", "0x0c") into a word. A raw number is
 * the whole local slice; names are slice-local. Exposed so the core can build
 * a word from a frontend string without duplicating the names. Unknown names
 * are reported to stderr and skipped rather than failing: a typo in a
 * launcher's config must not decide whether the game starts. */
uint32_t krkrsdl2_debug_parse(const char *spec);

/* Parse VNOVEL_DEBUG for this module's slice. A raw number in the spec is the
 * whole 64-bit word; only this module's slice (shifted back to local bits) is
 * returned. Names may carry a "krkr:" qualifier; other modules' qualifiers are
 * ignored. */
uint32_t krkrsdl2_debug_parse_vnovel(const char *spec);

/* Write the resolved set to stderr, one probe per line, so a log states which
 * diagnostics were live. Called by the core at start-up. */
void krkrsdl2_debug_describe(void);

/* The name of `bit`, or "(unknown)" - for logs and the description. */
const char *krkrsdl2_debug_name(uint32_t bit);

#ifdef __cplusplus
}
#endif

#endif /* KRKRSDL2_DEBUG_H */
