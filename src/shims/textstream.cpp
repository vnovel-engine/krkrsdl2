//---------------------------------------------------------------------------
// shims/textstream.cpp — the TJS -> core channel for the structured text stream.
//
// What this is
// ------------
// A global function pair a TJS script calls to hand parsed text to the shell:
//
//     vnovelPublishTextRun(text)     one run of characters, in scenario order
//     vnovelPublishTextPage(text)    a whole page at once, newlines included
//
// Why a plugin surface rather than a new core hook
// ------------------------------------------------
// TJS has no route to the core today. `Debug.message` goes to TVPAddLog, the
// engine's own log, and the one existing C++ -> TJS precedent is a start-up env
// var setting a global. That is the wrong direction and the wrong lifetime for
// a per-page stream.
//
// A game already links plugins by name and the engine already answers those
// links (data/overrides.txt, BuiltinShims.h), so expressing this as one needs no
// new concept to explain and travels inside the core like the other shims. The
// consumer reaches it the same way it reaches `drawLine`: by calling a global
// that is there.
//
// The granularity is the caller's to state
// ----------------------------------------
// Two names rather than one, because a page and a run are different things and
// only the caller knows which it is emitting. Inferring it in the shell from
// timing would be a guess.
//
// Style and position are absent on purpose
// ----------------------------------------
// This stream comes from a parser with no layer: it has no coordinates and no
// resolved style, and its whole value is that the characters and their ORDER are
// known before the engine has drawn any of them. A consumer that re-lays-out the
// text (reflow, totalfit, Knuth-Plass — see private/notes/for-the-text-renderer-mode.md)
// wants exactly that and nothing more. The pixel path already carries position
// and style, for consumers that want the engine's own placement.
//
// State contract (BuiltinShims.h)
// -------------------------------
// All state is file-static in this translation unit. The registration is
// symmetric — V2Unlink removes what V2Link added — so the engine may link,
// unlink and link another shim in one run. Nothing here touches an engine global
// directly; registration goes through the engine's own helpers.
//
// Failure is never fatal. The game links this name unguarded, as it does
// drawer.dll, so V2Link returns success even when nothing could be registered;
// the consequence is only that the globals are absent, which a caller detects
// with `typeof vnovelPublishTextRun == "function"` before using it.
//---------------------------------------------------------------------------

#define SHIM_NAME textstream
#include "shim_abi.h"

#include "tp_stub.h"

#include <string.h>

//---------------------------------------------------------------------------
// The shell's text-run ABI, declared by hand.
//
// NOT by including src/engine_bridge.h: that is the core's header, and the
// engine is a separate project. A shim that includes a core header stops being
// buildable on the plugin path, which is the rule noop.c states — "a shim that
// needs a type it cannot see without engine headers should declare it locally"
// rather than including them. The layout must match VNovelTextRun in
// src/engine_bridge.h field for field; this is the same hand-declared contract
// src/engine_kirikiri_cabi.cpp keeps for the text plane.
//
// Only the fields this producer fills are listed. A consumer reads the struct
// as the header defines it; this side states what it sets and nothing else.
//
// The symbol resolves at the core's final link, where engine_bridge.c is linked
// alongside the engine archive — an undefined reference in the archive is the
// ordinary way for the engine to reach the shell.
//---------------------------------------------------------------------------
typedef struct VNovelTextRun
{
	const unsigned short *text;
	int text_length;
	int x, y;
	unsigned int color;
	int opacity;
	int antialias;
	int shadow_level;
	unsigned int shadow_color;
	int shadow_offset_x, shadow_offset_y;
	int kind;
	unsigned long long serial;
} VNovelTextRun;

/* The producer kind for runs from THIS shim: read by a look-ahead parser, not
 * drawn by the engine (see engine_bridge.h). Set explicitly so a consumer can
 * tell this stream from the draw-time one, which carries the same characters
 * again but paced by the game's own typewriter speed. */
#define VNOVEL_RUN_PARSE 1

/* C linkage, matching how src/engine_bridge.c defines it. Without the
 * `extern "C"` this declaration picks up C++ name mangling and the linker looks
 * for vnovel_publish_text_run(VNovelTextRun const*), which does not exist —
 * measured as exactly that undefined reference. */
extern "C" void vnovel_publish_text_run(const VNovelTextRun *run);

/* Page-flow edge: 1 = a reader text wait is armed (page on screen, reader must
 * click), 0 = it ended. Same hand-declared, C-linkage contract as the text-run
 * symbol above, for the same reason (the engine is a separate project and does
 * not include src/engine_bridge.h). The definition lives in engine_bridge.c. */
// Without VNOVEL_DEVWIRE there is no wire protocol to publish to, so the calls
// become local no-ops instead of references to symbols the build does not define.
#ifdef VNOVEL_DEVWIRE
extern "C" void vnovel_publish_page_event(int event);
#else
static inline void vnovel_publish_page_event(int) {}
#endif

// The page-event vocabulary (src/engine_bridge.h, VNOVEL_EV_*). ONE list shared
// with the client's wire tokens (src/devsock.h) and with the other engines:
// boundaries and waits are different events and are not interchangeable.
#define VNOVEL_EV_PAGE_START 0
#define VNOVEL_EV_PAGE_END   1
#define VNOVEL_EV_LINE_PAUSE 2
#define VNOVEL_EV_PAGE_PAUSE 3
#define VNOVEL_EV_RESUME     4

namespace
{

//---------------------------------------------------------------------------
// The TJS result codes this shim returns.
//
// Not from an engine header: on the plugin path there is none, which is why
// drawer.cpp carries the same two defines (drawer.cpp:103-104). The values are
// TJS_S_OK and TJS_E_BADPARAMCOUNT; spelled numerically here so the file builds
// on both paths from the same text.
//---------------------------------------------------------------------------
const tjs_error kESucceed = 0;          // TJS_S_OK
const tjs_error kEBadParamCount = -1004; // TJS_E_BADPARAMCOUNT (0xfffffc14)

//---------------------------------------------------------------------------
// Keys
//
// Built once. tjs_char is char16_t on this platform, so TJS_W is the right
// constructor.
//---------------------------------------------------------------------------
struct Keys
{
	ttstr textRun;
	ttstr textPage;
	ttstr pageEvent;

	Keys() :
		textRun(TJS_W("vnovelPublishTextRun")),
		textPage(TJS_W("vnovelPublishTextPage")),
		pageEvent(TJS_W("vnovelPublishPageEvent"))
	{
	}
};

inline Keys &K()
{
	static Keys keys;
	return keys;
}

//---------------------------------------------------------------------------
// Hand one string to the shell.
//
// The bridge does not copy: its contract states the text is valid for the
// duration of the call only, which is exactly the lifetime of `text` here — the
// shell copies what it keeps. UTF-16 crosses unchanged, since tjs_char is
// char16_t here and the bridge's field already states UTF-16 code units, so
// there is no re-encoding step to get wrong.
//
// An empty string is dropped rather than published: it carries no information,
// and the shell would have to filter it anyway.
//---------------------------------------------------------------------------
inline void PublishToShell(const ttstr &text)
{
	if (text.IsEmpty())
		return;

	VNovelTextRun run;
	memset(&run, 0, sizeof(run));
	run.text        = reinterpret_cast<const unsigned short *>(text.c_str());
	run.text_length = (int)text.GetLen();
	// Read, not drawn: see the field's note in engine_bridge.h. This is what
	// separates this stream from the draw-time one, which carries the same
	// characters again at the game's own pace.
	run.kind = VNOVEL_RUN_PARSE;
	// The producer keeps no serial of its own; the shell counts publications.
	run.serial = 0;

	vnovel_publish_text_run(&run);
}

//---------------------------------------------------------------------------
// vnovelPublishTextRun(text)
//
// One run of characters: a glyph, a word or a whole line, whichever granularity
// the caller walks at. The shell concatenates runs and does not infer
// boundaries from them.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD PublishTextRunFunc(tTJSVariant *result,
	tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis;
	if (result) result->Clear();

	if (numparams < 1 || !param || !param[0])
		return kEBadParamCount;

	ttstr text = *param[0];
	PublishToShell(text);
	return kESucceed;
}

//---------------------------------------------------------------------------
// vnovelPublishTextPage(text)
//
// A whole page in one call, its newlines included. Its own name so the caller
// states its granularity instead of the shell inferring it.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD PublishTextPageFunc(tTJSVariant *result,
	tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis;
	if (result) result->Clear();

	if (numparams < 1 || !param || !param[0])
		return kEBadParamCount;

	ttstr text = *param[0];
	PublishToShell(text);
	return kESucceed;
}

//---------------------------------------------------------------------------
// vnovelPublishPageFlow(paused)
//
// A page-flow edge, not text. The TJS look-ahead calls this the instant the
// live game arms a reader text pause (the page is on screen, the reader must
// click) and again the instant that wait ends. The bridge keeps no payload;
// the value is the edge (1 then 0), published ONCE per pause, exactly as
// src/engine_bridge.h / engine_bridge.c define for the C side. A no-op when no
// shell installed a callback, so a caller that publishes unconditionally is
// still correct.
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// vnovelPublishPageEvent(event)
//
// ONE page event (VNOVEL_EV_*, src/engine_bridge.h). It replaced two functions
// -- a pause/resume one and a boundary one -- because they were two halves of a
// single vocabulary: the client speaks the same five events as wire tokens
// (src/devsock.h), so the engine side is its event half rather than a second
// list that has to be kept in step (which is how VNOVEL_FLOW_LP and
// VNOVEL_EDGE_START came to share the value 0 and mean different things).
//
// A no-op when no shell installed a callback, so a caller that publishes
// unconditionally is still correct.
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD PublishPageEventFunc(tTJSVariant *result,
	tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis)
{
	(void)objthis;
	if (result) result->Clear();

	if (numparams < 1 || !param || !param[0])
		return kEBadParamCount;

	vnovel_publish_page_event((int)*param[0]);
	return kESucceed;
}

} // namespace

//---------------------------------------------------------------------------
// V2Link / V2Unlink — the plugin ABI (shim_abi.h).
//
// The exporter is unused: this shim needs only the engine's own registration
// helpers, which are linked directly on the built-in path exactly as
// drawer.cpp's are, and reached the same way on the plugin path. Nothing is
// resolved by name from the exporter, so a missing exporter cannot fail the
// link.
//---------------------------------------------------------------------------
extern "C"
{

SHIM_EXPORT SHIM_HIDDEN int V2Link(iTVPFunctionExporter *exporter)
{
	(void)exporter;

	tTJSNativeClassMethod *runFunc  = TJSCreateNativeClassMethod(&PublishTextRunFunc);
	tTJSNativeClassMethod *pageFunc = TJSCreateNativeClassMethod(&PublishTextPageFunc);
	tTJSNativeClassMethod *evFunc = TJSCreateNativeClassMethod(&PublishPageEventFunc);
	if (!runFunc || !pageFunc || !evFunc)
	{
		// A half-registered surface is worse than none: a caller that finds one
		// name present and the other absent would have to special-case it.
		if (runFunc) runFunc->Release();
		if (pageFunc) pageFunc->Release();
		if (evFunc) evFunc->Release();
		return SHIM_TJS_S_OK;
	}

	Keys &k = K();
	bool ok = true;
	ok = TVPRegisterGlobalObject(k.textRun.c_str(), runFunc) && ok;
	ok = TVPRegisterGlobalObject(k.textPage.c_str(), pageFunc) && ok;
	ok = TVPRegisterGlobalObject(k.pageEvent.c_str(), evFunc) && ok;

	// TVPRegisterGlobalObject keeps its own reference in the global object, so
	// the creator's reference is dropped here (the same contract drawer.cpp
	// documents).
	runFunc->Release();
	pageFunc->Release();
	evFunc->Release();

	return ok ? SHIM_TJS_S_OK : TJS_E_FAIL;
}

SHIM_EXPORT SHIM_HIDDEN int V2Unlink(void)
{
	Keys &k = K();
	TVPRemoveGlobalObject(k.textRun.c_str());
	TVPRemoveGlobalObject(k.textPage.c_str());
	TVPRemoveGlobalObject(k.pageEvent.c_str());
	return SHIM_TJS_S_OK;
}

} // extern "C"
