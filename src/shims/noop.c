//---------------------------------------------------------------------------
// shims/noop.c — answer a plugin link without providing anything.
//
// One source, two consumers
// -------------------------
// This file is built both ways from the same text, because a plugin shim has
// to be indistinguishable from the Windows plugin it replaces:
//
//   * Windows: compiled to a PE plugin (`<name>.dll` / `.tpm`) and dropped into
//     the game directory, where the game's own TJS code links it exactly as it
//     would the original. The engine is not involved.
//   * Elsewhere: compiled into the core and reached through the override
//     table's `@builtin:noop` form (BuiltinShims.h), because a PE cannot be
//     loaded there.
//
// So the ABI below is not ours to choose. It is the plugin ABI:
//
//     __declspec(dllexport) int V2Link(iTVPFunctionExporter *);
//     __declspec(dllexport) int V2Unlink(void);
//
// and it must stay byte-for-byte compatible with it. In particular the exporter
// parameter is a real iTVPFunctionExporter* when Windows loads this, and the
// engine passes the same pointer through the registry — the signature is
// identical on both paths on purpose, so a shim that registers classes behaves
// the same whether the game loaded it or the engine did.
//
// A shim that needs a type it cannot see without engine headers should declare
// it locally (as fate_xp3dec.c does for tTVPXP3ExtractionFilterInfo) rather than
// including engine headers: an engine header would make this file buildable on
// only one of the two paths.
//---------------------------------------------------------------------------

// The plugin ABI and the build-dependent linkage come from the shared header
// (src/shims/shim_abi.h), which is also what keeps several shims from colliding
// on one `V2Link` when they are compiled into the same binary.
#define SHIM_NAME noop
#include "shim_abi.h"

// Answer the link without providing anything.
//
// This is the shim for a plugin a game links for its own side effects, where
// the behaviour it needs is already carried by the game's scripts or by the
// engine. The whole job is to succeed: Plugins.link() then sees a plugin that
// loaded, which is what the script checks, instead of an exception that aborts
// the scene.
//
// Nothing is registered with the exporter, deliberately. A shim that needs to
// register something is its own file with its own V2Link, not a mode of this
// one.
//
// State: none. Nothing here is written, so there is nothing to share, nothing
// to reset between loads, and no global to reach for.
SHIM_EXPORT SHIM_HIDDEN int V2Link(iTVPFunctionExporter *exporter)
{
	(void)exporter;
	return SHIM_TJS_S_OK;
}

// Symmetric with V2Link: it installed nothing, so it removes nothing. The
// engine may load a plugin, unload it, and load another in the same run.
SHIM_EXPORT SHIM_HIDDEN int V2Unlink(void)
{
	return SHIM_TJS_S_OK;
}
