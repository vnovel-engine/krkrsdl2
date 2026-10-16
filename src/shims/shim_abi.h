//---------------------------------------------------------------------------
// shim_abi.h — the plugin ABI a shim presents, and how several can coexist.
//
// One source, two consumers
// -------------------------
// A shim file builds two ways from the same text, because it has to be
// indistinguishable from the Windows plugin it replaces:
//
//   * standalone: compiled to a PE plugin (`<name>.dll` / `.tpm`) and dropped
//     into the game directory, where the game's own TJS code links it exactly as
//     it would the original. The engine is not involved.
//   * built in: compiled into the core and reached through the registry in
//     BuiltinShims.cpp via the override table's `@builtin:<name>` form, because
//     a PE cannot be loaded where the core runs.
//
// The ABI below is therefore not ours to choose. A plugin's loader finds
// `V2Link`/`V2Unlink` by name from outside, so those are the names in the
// standalone build.
//
// Why the names are mangled when built in
// ---------------------------------------
// Built into one binary, several shims would each define `V2Link` — a duplicate
// C symbol, and only the first would be reachable. So in that build each shim's
// entry points are renamed to a per-shim symbol, selected by the `SHIM_NAME`
// macro the shim defines before including this header. The registry declares
// those same names.
//
// The rename is done by macro rather than by editing the functions, so a shim
// keeps spelling its entry points `V2Link`/`V2Unlink`: the text you read is the
// ABI, and only the linkage differs per build. That is the same reason the
// export/hide decision lives here rather than in each file.
//
// Usage, at the top of a shim:
//
//     #define SHIM_NAME drawer      // becomes shim_drawer_V2Link when built in
//     #include "shim_abi.h"
//
// and then define `SHIM_EXPORT SHIM_HIDDEN int V2Link(iTVPFunctionExporter *)`
// and `... V2Unlink(void)` as usual.
//
// State contract: see BuiltinShims.h. A shim owns its state and nothing else.
//---------------------------------------------------------------------------
#ifndef VNOVEL_SHIM_ABI_H
#define VNOVEL_SHIM_ABI_H

// The plugin ABI's return codes. TJS_S_OK is 0; a plugin returns it to say the
// link succeeded. Declared here rather than pulled from an engine header: a
// shim must build on both paths, and an engine header exists on only one.
#define SHIM_TJS_S_OK 0

// The exporter a plugin is handed in V2Link. Opaque here for the same reason —
// only the pointer crosses the boundary.
typedef struct iTVPFunctionExporter iTVPFunctionExporter;

// SHIM_NAME must be defined by the shim before this header is included. Checked
// rather than defaulted: a silent default would give two shims the same symbol
// again, which is the failure this header exists to prevent.
#ifndef SHIM_NAME
#  error "define SHIM_NAME (the shim's registry name) before including shim_abi.h"
#endif

// Two-level token pasting: one level would paste the *literal* SHIM_NAME rather
// than its expansion.
#define SHIM_JOIN2_(a, b) a##b
#define SHIM_JOIN2(a, b) SHIM_JOIN2_(a, b)
#define SHIM_JOIN3_(a, b, c) a##b##c
#define SHIM_JOIN3(a, b, c) SHIM_JOIN3_(a, b, c)

#if defined(KRKRSDL2_BUILTIN_SHIMS)

// Built into the engine. The entry points are reached through the registry
// table, never by name from outside, so they must not be exported: an exported
// `V2Link` would land in the engine's symbol table where it could shadow another
// shim or be picked up by an unrelated plugin looking for that name.
//
// Hiding is explicit rather than left to the surrounding build's
// -fvisibility=hidden, because this engine is not compiled with that flag.
#  define SHIM_EXPORT
#  if defined(__GNUC__) && (__GNUC__ >= 4)
#    define SHIM_HIDDEN __attribute__((visibility("hidden")))
#  else
#    define SHIM_HIDDEN
#  endif

// Per-shim symbols, so several shims link into one binary. The registry in
// BuiltinShims.cpp declares the same names.
#  define V2Link   SHIM_JOIN3(shim_, SHIM_NAME, _V2Link)
#  define V2Unlink SHIM_JOIN3(shim_, SHIM_NAME, _V2Unlink)

#else /* not built in: this file is a plugin */

// A plugin's loader resolves these by name, so they are the ABI names and they
// are exported.
#  if defined(_WIN32)
#    define SHIM_EXPORT extern "C" __declspec(dllexport)
#  else
#    define SHIM_EXPORT extern "C" __attribute__((visibility("default")))
#  endif
#  define SHIM_HIDDEN

#endif /* KRKRSDL2_BUILTIN_SHIMS */

#endif /* VNOVEL_SHIM_ABI_H */
//---------------------------------------------------------------------------
