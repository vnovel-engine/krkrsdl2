//---------------------------------------------------------------------------
// shims/textstream.cpp — the TJS -> core channel for the structured text stream.
//
// A global function pair a TJS script calls to hand parsed text to the shell:
//
//     vnovelPublishTextRun(text)     one run of characters, in scenario order
//     vnovelPublishTextPage(text)    a whole page at once, newlines included
//
// Two names rather than one: a page and a run are different things and only
// the caller knows which it is emitting. This stream comes from a parser with
// no layer — it has no coordinates and no resolved style, and its whole value
// is that the characters and their order are known before the engine has
// drawn any of them. The pixel path already carries position and style.
//
// State contract (BuiltinShims.h): all state is file-static. Registration is
// symmetric — V2Unlink removes what V2Link added. Failure is never fatal:
// V2Link returns success even when nothing could be registered; a caller
// detects absence with `typeof vnovelPublishTextRun == "function"`.

#define SHIM_NAME textstream
#include "shim_abi.h"

#include "tp_stub.h"

#include <string.h>

//---------------------------------------------------------------------------
// The shell's text-run ABI, declared by hand (not by including
// src/engine_bridge.h: the engine is a separate project, and a shim that
// includes a core header stops being buildable on the plugin path). The
// layout must match VNovelTextRun in src/engine_bridge.h field for field.
// Only the fields this producer fills are listed. The symbol resolves at the
// core's final link, where engine_bridge.c is linked alongside the engine
// archive.
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

/* Page-flow edge: 1 = a reader text wait is armed, 0 = it ended. Same
 * hand-declared, C-linkage contract as the text-run symbol; the definition
 * lives in engine_bridge.c. Without VNOVEL_DEVWIRE the calls are local no-ops. */
#ifdef VNOVEL_DEVWIRE
extern "C" void vnovel_publish_page_event(int event);
#else
static inline void vnovel_publish_page_event(int) {}
#endif

// The page-event vocabulary (src/engine_bridge.h, VNOVEL_EV_*): boundaries
// and waits are different events and are not interchangeable.
#define VNOVEL_EV_PAGE_START 0
#define VNOVEL_EV_PAGE_END   1
#define VNOVEL_EV_LINE_PAUSE 2
#define VNOVEL_EV_PAGE_PAUSE 3
#define VNOVEL_EV_RESUME     4

namespace
{

//---------------------------------------------------------------------------
// TJS result codes, spelled numerically so the file builds on both paths
// (no engine header on the plugin path).
const tjs_error kESucceed = 0;          // TJS_S_OK
const tjs_error kEBadParamCount = -1004; // TJS_E_BADPARAMCOUNT (0xfffffc14)

//---------------------------------------------------------------------------
// Keys, built once.
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
// Hand one string to the shell. The bridge does not copy — text is valid for
// the duration of the call only, which is the lifetime of `text`. UTF-16
// crosses unchanged (tjs_char is char16_t here). Empty strings are dropped.
inline void PublishToShell(const ttstr &text)
{
	if (text.IsEmpty())
		return;

	VNovelTextRun run;
	memset(&run, 0, sizeof(run));
	run.text        = reinterpret_cast<const unsigned short *>(text.c_str());
	run.text_length = (int)text.GetLen();
	// Read, not drawn: separates this stream from the draw-time one.
	run.kind = VNOVEL_RUN_PARSE;
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
// vnovelPublishPageEvent(event)
//
// One page event (VNOVEL_EV_*, src/engine_bridge.h). A no-op when no shell
// installed a callback, so a caller that publishes unconditionally is correct.
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
// V2Link / V2Unlink — the plugin ABI (shim_abi.h). The exporter is unused:
// this shim needs only the engine's own registration helpers.
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

	// TVPRegisterGlobalObject keeps its own reference; drop the creator's.
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
