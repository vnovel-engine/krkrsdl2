//---------------------------------------------------------------------------
/*
	KiriKiri engine-compatibility switches — SDL2 backend.

	KiriKiri's scripting engine and drawing path changed observable behaviour
	several times, and each change was a documented break for applications
	written before it. A game therefore targets the *set of behaviours* of the
	engine version it shipped against, and a port that wants to run releases
	from more than one era has to be able to select them.

	Why flags and not just versions
	------------------------------
	KiriKiri is open source and publishers edited it, so a title can carry a
	backport of one changed behaviour without the rest of the newer engine.
	Selecting by version number alone cannot express that. Every switch is
	therefore settable on its own, and a "level" is nothing more than a named
	preset that fills several of them at once — a starting point to build a
	per-game mask from, which is then tested against that game.

	The switches, with the change each reverts to its old behaviour:
	  classvar        <= 2007-01-20 : class-scope `var` uses SPDE
	  evalcontext     <= 2.21 beta 9           : post-! evaluates on global
	  holdalpha       <= 2.23 beta 5           : Layer.holdAlpha defaults true
	  unaryaster      <= 2.25 beta 1           : unary '*' ignores property access
	  composite219    <= 2006-10               : pre-2006 layer compositing algorithm
	  structconst     <= 2008-04-08 : saveStruct writes "%[" (no "(const)")
	  structcrlf      <= (Windows builds)      : saveStruct writes CRLF, not LF

	The two save-format switches are a *wire format*, not a behaviour, and they
	are the only switches here that a game can observe as data rather than as
	drawing or script semantics — a save one era writes, the other must be able
	to read.

	`structconst`: the 2008 line made Array.saveStruct and Dictionary.saveStruct
	emit `(const) %[` instead of `%[`. Measured against the real thing: Fate/stay
	night's engine is 2.19.15.783, and its scripts eval a save with
	Scripts.evalStorage. Feeding it a save written with `(const) %[` makes that
	eval THROW ("the data or its backup is corrupt"), so a 2.19-era title cannot
	load a modern save at all — it is not a cosmetic difference. Our engine
	vendors krkrz, which carries the 2008+ line, so without this switch every
	save we write is unreadable by the game that motivated the port. Format: see
	tTJSDictionaryNI::SaveStructuredData and tTJSArrayNI::SaveStructuredData.

	`structcrlf`: the `(const)` change was hard-coded behind TJS_TEXT_OUT_CRLF,
	a *build* define — so the same engine emits LF on a Unix build and CRLF on a
	Windows one. Making it a flag is what removes save output's dependence on
	how the engine was compiled; the reference (a Windows binary) writes CRLF.
	Note that the load side accepts either, so this one is not a compatibility
	blocker, unlike `structconst`.

	Both are read when a save is serialised, so a change takes effect on the
	next save with no engine restart.

	`classvar` is expressed as a KiriKiri revision rather than a version because
	that is what it is: the opcode choice changed inside the 2.29 development
	line, roughly two and a half years after 2.23, so any level named after
	2.23 would be wrong about it in both directions.

	Timing
	------
	Some switches are read while the interpreter compiles script, so changing
	them mid-run would mean two different code shapes in one process; those are
	applied once, before the scripting engine initialises, and a later change
	takes effect on the next engine start. That is a property of *where the
	value is read*, not of the switch's name, and it is stated per switch below
	so a future one is not guessed at.

	Game identity
	-------------
	A per-game selection is keyed by the identity this project already uses for
	a game everywhere else: the crc32 of the data file the engine reads
	(`data.xp3`), as eight upper-case hex digits. That is exactly the value
	`tools/hash-game.py` writes into a `dat/vnovel.dat` record and the one
	`vnovel.rdb` is indexed on, so one game has one id across the tools, the
	database and this table. The core computes it while it still holds the game
	information (`retro_load_game`) and passes it in `KRKRSDL2_GAME_ID`; a table
	key is written the way hash-game.py prints it.

	Where the spec comes from
	-------------------------
	Four sources, first match wins:

	  1. the per-run spec in `KRKRSDL2_COMPAT` — the user overriding for this
	     session. A libretro frontend cannot set the environment, so the core
	     exports this from its `vnovel_kirikiri_compat` option (see
	     krkr_set_engine_compat); a launcher may set it directly.
	  2. a per-game user override in `KRKRSDL2_COMPAT_<ID>` (e.g.
	     `KRKRSDL2_COMPAT_64960AF9=modern`) — a launcher or a developer pinning
	     one title, without touching the table or rebuilding.
	  3. the per-game table, keyed by the identity above. This is the "auto"
	     path: the game says what it is, and the table says what era that is.
	  4. `TVP_COMPAT_DEFAULT_LEVEL`, the last resort for a game the table does
	     not know.
	Whichever produced it, the spec is parsed by the same parser and the run
	logs which source it was, because "which era did this game actually get" is
	the first question when a title misbehaves.

	The spec format is a comma-separated list, where each item is either a level
	name or `name=on|off`:

	    KRKRSDL2_COMPAT=2.19
	    KRKRSDL2_COMPAT=2.19,classvar=off
	    KRKRSDL2_COMPAT=classvar=on,holdalpha=off

	A level sets the switches it knows; an explicit `name=` later in the list
	overrides it. Nothing here invents a second mechanism for the four switches
	that already exist on the engine's command line: where such a switch is
	present, it is set through the engine's own option stock (see the .cpp), so
	a command-line argument and a flag cannot disagree.
*/
//---------------------------------------------------------------------------
#ifndef TVP_COMPAT_PROFILE_H
#define TVP_COMPAT_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* A named preset of switch values. `TVPCompatNone` applies nothing, which
 * leaves the engine on its own defaults (this tree's behaviour). */
typedef enum
{
	TVPCompatNone = 0,     /* no preset: flags only */
	TVPCompatV219,         /* the 2.19-era set: Type-Moon-era titles */
	TVPCompatModern,       /* this tree's own behaviour (all flags off) */
	TVPCompatLevelCount
} TVPCompatLevel;

/* The era assumed when nothing selects one: the last resort, after the per-run
 * spec, the per-game user override and the per-game table have all declined.
 *
 * It is what an unknown game gets, so it is deliberately the empty set
 * (`Modern`) rather than any one title's era: a title the table has not been
 * taught must not be given another game's switches, and `TVP_COMPAT_DEFAULT_LEVEL`
 * now names the engine's own behaviour, which is where an unidentified game
 * belongs. Fate/stay night gets its 2.19 set from the table entry keyed by its
 * data.xp3 identity, not from here. */
#define TVP_COMPAT_DEFAULT_LEVEL TVPCompatModern

/* The switch a bit names, for logging and for `name=on|off` parsing. */
typedef enum
{
	TVPCompatFlagClassVar = 0,    /* class-scope `var` keeps a native accessor */
	TVPCompatFlagEvalContext,     /* post-! evaluates on the global context   */
	TVPCompatFlagHoldAlpha,       /* Layer.holdAlpha defaults to true         */
	TVPCompatFlagUnaryAsterisk,   /* unary '*' ignores property access        */
	TVPCompatFlagLayerShims,      /* load the obsolete-Layer TJS shim set     */
	TVPCompatFlagComposite219,    /* pre-2006 layer compositing algorithm     */
	TVPCompatFlagStructConst,     /* saveStruct: write `%[`, not `(const) %[` */
	TVPCompatFlagStructCRLF,      /* saveStruct: CRLF line endings            */
	TVPCompatFlagCount
} TVPCompatFlag;

/* A resolved mask: one bit per switch. Built from a level and/or explicit
 * flags, then applied once. Kept as a value the caller can log, so a run states
 * exactly which behaviours were selected rather than implying them. */
typedef struct
{
	unsigned int bits;
} TVPCompatMask;

/* Parse `spec` (NULL or empty leaves `mask` empty) and OR the result with
 * `mask`. Unrecognised items are reported and ignored rather than failing the
 * run: a typo in a launcher's config must not be the difference between a game
 * starting and not. Returns `mask` with the parsed bits added. */
TVPCompatMask TVPCompatParse(const char *spec, TVPCompatMask mask);

/* The mask selected by the environment, starting from `level`. */
TVPCompatMask TVPCompatMaskForLevel(TVPCompatLevel level);

/* Where a resolved spec came from. Logged, so a run says which source decided
 * the era rather than implying it. */
typedef enum
{
	TVPCompatSourceDefault = 0, /* TVP_COMPAT_DEFAULT_LEVEL: last resort     */
	TVPCompatSourceTable,       /* the per-game table, by game identity      */
	TVPCompatSourceUserGame,    /* KRKRSDL2_COMPAT_<id>: per-game override   */
	TVPCompatSourceUserRun      /* KRKRSDL2_COMPAT: per-run override         */
} TVPCompatSource;

/* Resolve the mask for `game_id`, applying the sources in order: the per-run
 * spec, the per-game user override, the per-game table, then the default level.
 * `game_id` is the identity the table is keyed by (the eight hex digits of the
 * data file's crc32, upper case; NULL or empty when the game has none, which
 * simply skips the two per-game sources). Returns the resolved mask and, when
 * `source` is non-NULL, the source that supplied the winning spec. The spec
 * text itself goes through TVPCompatParse unchanged, so this adds a source for
 * the string, not a second mechanism. */
TVPCompatMask TVPCompatResolve(const char *game_id, TVPCompatSource *source);

/* Human-readable name of `source`, for logs. Never NULL. */
const char *TVPCompatSourceName(TVPCompatSource source);

/* True when `flag` is selected. */
int TVPCompatHasFlag(TVPCompatMask mask, TVPCompatFlag flag);

/* Apply `mask`. Must be called before the scripting engine initialises; the
 * code-generation switches are read while compiling. Each switch is applied
 * through the engine's own option stock where one exists, so an explicit
 * command-line option still wins. Logs the resolved set. */
void TVPApplyCompatMask(TVPCompatMask mask);

/* Publish the resolved switches as TJS globals (`__krkrCompat_<flag>` and
 * `__krkrCompatMask`), so a TJS shim or user patch branches on what the engine
 * actually resolved rather than re-deriving it. Safe to call when no scripting
 * engine is up; does nothing then. */
void TVPExposeCompatToScript(void);

/* The bits last applied, for a host or a test that wants the value without
 * going through script. */
unsigned int TVPCompatResolvedBits(void);

/* The mask last applied, as a value. Read by anything that has to route on the
 * resolved switches after start-up - the polyfill loader is the first such
 * consumer - so the decision is taken once, from the mask, rather than by
 * re-reading the environment and possibly disagreeing with it. */
TVPCompatMask TVPCompatResolvedMask(void);

/* True when `flag` is selected in the resolved mask. Shorthand for the common
 * case, so a caller does not have to hold the struct. */
int TVPCompatResolved(TVPCompatFlag flag);
/* Human-readable names, for logs. Never NULL. */
const char *TVPCompatFlagName(TVPCompatFlag flag);
const char *TVPCompatFlagDescription(TVPCompatFlag flag);

#ifdef __cplusplus
}
#endif

#endif /* TVP_COMPAT_PROFILE_H */
