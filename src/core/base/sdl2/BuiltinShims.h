//---------------------------------------------------------------------------
// BuiltinShims.h — compiled-in plugin shims, addressed by name.
//
// A KiriKiri game links Windows PE plugins (@loadplugin / Plugins.link). Those
// cannot be loaded here, so the engine answers the link itself. What to answer
// is described by data/overrides.txt (see BuiltinPlugins.cpp); this header is
// the other half — how a replacement is *supplied*.
//
// Two sources of replacement, in order of preference:
//
//   1. A compiled-in shim, named in the table as `@builtin:<name>`.
//   2. A shared object beside overrides.txt, named as a plain path.
//
// Compiled-in is preferred, and for the shims we ship it is the only form we
// use. A shim is code the core already contains; loading an external object for
// it adds three failure modes for no gain:
//
//   * the object must match the core's architecture, and a mismatch can only be
//     reported at run time (an x86-64 file silently cannot load on an aarch64
//     device);
//   * it must be installed next to overrides.txt, which is a directory the core
//     does not own — on a packaged system it defaults to /usr/share/krkrsdl2;
//   * it makes the core non-self-contained, which single-file packaging wants
//     to avoid.
//
// The registry is also the home for a genuinely platform-dependent shim: the
// table entry stays the same and only the compiled-in implementation is chosen
// at build time, so the data never has to know about the platform.
//
// Gated behind KRKRSDL2_BUILTIN_SHIMS (see the .cpp). Internal to the engine:
// nothing here is part of a C ABI, and no shim is looked up outside the core.
//
// State contract
// --------------
// A shim is a guest in the engine, so its reach is bounded deliberately:
//
//   1. Its own state is its own. A shim keeps state file-static in its own
//      translation unit, or allocates it in link() and releases it in
//      unlink(). Nothing it owns is a global, and nothing it owns is visible
//      from another shim. Two shims must be able to coexist without agreeing on
//      anything.
//
//   2. It does not touch engine globals directly. Where a shim must change
//      engine behaviour it calls a declared function for that purpose — e.g.
//      TVPSetXP3ArchiveExtractionFilter() for the archive filter, rather than
//      assigning TVPXP3ArchiveExtractionFilter. Reaching for the variable
//      directly couples the shim to a definition it does not own and breaks
//      silently when that global moves or gains a second writer.
//
//   3. Anything it registers with the engine is symmetric. If link() installs
//      something, unlink() removes it; the engine may load a plugin, unload it,
//      and load another in the same run.
//
//   4. It assumes nothing about lifetime beyond link/unlink. The engine may
//      call link() once per process, or once per archive; a shim that needs to
//      know the difference must be told through its entry rather than inferring
//      it from how often a global is observed changing.
//
// A shim that cannot satisfy these belongs behind a purpose-built surface in
// the engine, not in the registry.
//---------------------------------------------------------------------------
#ifndef BUILTIN_SHIMS_H
#define BUILTIN_SHIMS_H

#include <string>

#include "PluginImpl.h" /* iTVPFunctionExporter */

// A compiled-in shim presents the same surface as the plugin it replaces:
// V2Link is handed the engine's function exporter and does whatever the plugin
// would have done (register classes, install a filter); V2Unlink reverses it.
// Both return TJS_S_OK on success, matching what the engine's loader expects
// from a real plugin.
typedef int (*BuiltinShimLinkProc)(iTVPFunctionExporter *);
typedef int (*BuiltinShimUnlinkProc)(void);

struct BuiltinShim {
	const char *name;             // the name used in `@builtin:<name>`
	BuiltinShimLinkProc link;
	BuiltinShimUnlinkProc unlink; // may be null
	const char *description;      // one line, for the log
};

// True when `replacement` is the `@builtin:<name>` form, and if so points
// `name` at the name part (not NUL-terminated; `len` is its length). Kept here
// so the table parser and the installer agree on the syntax.
bool TVPParseBuiltinRef(const std::string &replacement, const char **name, size_t *len);

// The registry, terminated by an entry with name == nullptr. Never null; an
// empty registry (feature compiled out) yields a table whose first entry has a
// null name.
const BuiltinShim *TVPGetBuiltinShims(void);

// Look up by name; null when absent, including when the feature is compiled out.
const BuiltinShim *TVPFindBuiltinShim(const char *name);

// Install by name: call its V2Link with the engine's exporter, as a real
// plugin's loader would. Returns false when the name is not in the registry, so
// the caller can fall back to the file path and say why.
bool TVPInstallBuiltinShim(const char *name);

#endif // BUILTIN_SHIMS_H
