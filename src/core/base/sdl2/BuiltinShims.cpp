//---------------------------------------------------------------------------
// BuiltinShims.cpp — compiled-in plugin shims, addressed by name.
//
// Gated behind KRKRSDL2_BUILTIN_SHIMS. Off, the engine behaves as before:
// overrides.txt names a file and the file is dlopen'd. On, a table entry of the
// form `@builtin:<name>` resolves to a function in this registry instead, so
// the shim travels inside the core and cannot be the wrong architecture, in the
// wrong directory, or missing.
//
// The gate is deliberate rather than automatic. It exists so the engine still
// builds standalone and so a downstream that wants the old external-file
// behaviour keeps it. It is not tied to KRKRSDL2_LIBRETRO: a standalone build
// may legitimately want the compiled-in shims too, and the choice is about how
// a shim is supplied, not about who drives the engine.
//
// See BuiltinShims.h for why compiled-in is preferred.
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "BuiltinShims.h"

#include <cstring>
#include <string>

#include "DebugIntf.h"
#include "PluginImpl.h"

//---------------------------------------------------------------------------
// The `@builtin:<name>` reference.
//
// Kept outside the feature gate: it is pure syntax, and the parser in
// BuiltinPlugins.cpp must recognise the form whether or not this build can
// resolve it — a table written for a shim-enabled build must not silently mean
// something else in a build without it.
//---------------------------------------------------------------------------
bool TVPParseBuiltinRef(const std::string &replacement, const char **name, size_t *len)
{
	static const char kPrefix[] = "@builtin:";
	const size_t kPrefixLen = sizeof(kPrefix) - 1;

	if (replacement.compare(0, kPrefixLen, kPrefix) != 0)
		return false;

	const char *rest = replacement.c_str() + kPrefixLen;
	size_t n = replacement.size() - kPrefixLen;
	// Trim a trailing separator run: the table is whitespace-bounded already,
	// but a stray CR survives on a file written with CRLF.
	while (n > 0 && (rest[n - 1] == '\r' || rest[n - 1] == ' ' || rest[n - 1] == '\t'))
		n--;
	if (n == 0)
		return false;

	*name = rest;
	*len = n;
	return true;
}

#ifdef KRKRSDL2_BUILTIN_SHIMS

// The shims are their own files under src/shims/, each building BOTH as a
// standalone Windows plugin and as part of this registry — one source, so the
// two paths cannot drift apart. They are registered under the name used by the
// override table's `@builtin:<name>` form.
//
// Each shim's entry points are renamed per shim when built in (see
// shims/shim_abi.h), because several shims in one binary would otherwise all
// define `V2Link` and only the first would be reachable. The names below must
// match the `SHIM_NAME` each shim defines; shim_abi.h builds them the same way,
// so the two cannot drift without a link error naming the mismatch.
#define SHIM_SYMBOL(name, suffix) shim_##name##_##suffix
#define SHIM_LINK(name) SHIM_SYMBOL(name, V2Link)
#define SHIM_UNLINK(name) SHIM_SYMBOL(name, V2Unlink)

extern "C" int SHIM_LINK(noop)(iTVPFunctionExporter *);
extern "C" int SHIM_UNLINK(noop)(void);
extern "C" int SHIM_LINK(drawer)(iTVPFunctionExporter *);
extern "C" int SHIM_UNLINK(drawer)(void);
extern "C" int SHIM_LINK(firespark)(iTVPFunctionExporter *);
extern "C" int SHIM_UNLINK(firespark)(void);
extern "C" int SHIM_LINK(extrans)(iTVPFunctionExporter *);
extern "C" int SHIM_UNLINK(extrans)(void);
extern "C" int SHIM_LINK(textstream)(iTVPFunctionExporter *);
extern "C" int SHIM_UNLINK(textstream)(void);

namespace {

// One entry per shim. The shim file supplies the functions; this table supplies
// the name and the description.
//
// The entries are in two groups: `noop` answers a link for a plugin whose
// behaviour the engine or the game's scripts already provide, and the named
// ones replace a plugin that drew or computed something the port must supply
// itself. Both are listed here so the override table has one place to point at.
const BuiltinShim g_builtin_shims[] = {
	{ "noop",
	  SHIM_LINK(noop),
	  SHIM_UNLINK(noop),
	  "answer a plugin link without providing anything (the game's scripts or "
	  "the engine already supply the behaviour)" },
	{ "drawer",
	  SHIM_LINK(drawer),
	  SHIM_UNLINK(drawer),
	  "Fate's line/anti-aliased-line/triangle rasteriser into a Layer buffer "
	  "(drawLine, drawAALine, drawAATriangle)" },
	{ "firespark",
	  SHIM_LINK(firespark),
	  SHIM_UNLINK(firespark),
	  "Fate's time- and RNG-driven particle effect drawn into a Layer buffer "
	  "(initFireSpark, changeFireSpark, drawFireSpark, finishFireSpark)" },
	{ "extrans",
	  SHIM_LINK(extrans),
	  SHIM_UNLINK(extrans),
	  "KiriKiri transition effects (mosaic, ripple, ...) via a transition "
	  "handler provider" },
	{ "textstream",
	  SHIM_LINK(textstream),
	  SHIM_UNLINK(textstream),
	  "the TJS -> core text channel: vnovelPublishTextRun / "
	  "vnovelPublishTextPage hand parsed text to the shell's run sink, for a "
	  "consumer that re-lays-out the text rather than viewing the engine's "
	  "pixels" },
	{ nullptr, nullptr, nullptr, nullptr },
};

} // namespace

const BuiltinShim *TVPGetBuiltinShims(void)
{
	return g_builtin_shims;
}

const BuiltinShim *TVPFindBuiltinShim(const char *name)
{
	if (!name || !*name)
		return nullptr;

	for (const BuiltinShim *s = g_builtin_shims; s->name; s++)
	{
		if (std::strcmp(s->name, name) == 0)
			return s;
	}
	return nullptr;
}

bool TVPInstallBuiltinShim(const char *name)
{
	const BuiltinShim *s = TVPFindBuiltinShim(name);
	if (!s)
		return false;

	const int r = s->link ? s->link(TVPGetFunctionExporter()) : TJS_S_OK;
	if (r == TJS_S_OK)
	{
		TVPAddImportantLog(ttstr(TJS_W("(info) builtin shim: '")) +
			ttstr(name) + TJS_W("' — ") + ttstr(s->description));
	}
	else
	{
		TVPAddImportantLog(ttstr(TJS_W("(warn) builtin shim '")) +
			ttstr(name) + TJS_W("' failed to link"));
	}
	return true;
}

#else // !KRKRSDL2_BUILTIN_SHIMS

// Feature compiled out: an empty registry. The table's `@builtin:` form is still
// recognised by the parser above, so the caller can report it as "compiled
// without builtin shims" rather than mistaking it for a file name.
namespace {
const BuiltinShim g_no_shims[] = {
	{ nullptr, nullptr, nullptr, nullptr },
};
} // namespace

const BuiltinShim *TVPGetBuiltinShims(void)
{
	return g_no_shims;
}

const BuiltinShim *TVPFindBuiltinShim(const char *)
{
	return nullptr;
}

bool TVPInstallBuiltinShim(const char *)
{
	return false;
}

#endif // KRKRSDL2_BUILTIN_SHIMS
