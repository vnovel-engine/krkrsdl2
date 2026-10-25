//---------------------------------------------------------------------------
// shims/noop.c — answer a plugin link without providing anything.
//
// Built two ways from the same text (see shim_abi.h): as a PE plugin the game
// links, or built into the core via the override table's `@builtin:noop` form.
// The exporter parameter is a real iTVPFunctionExporter* on both paths, so the
// signature is identical whether the game loaded this or the engine did.
//
// A shim that needs a type it cannot see without engine headers declares it
// locally rather than including engine headers: an engine header would make
// this file buildable on only one of the two paths.
//---------------------------------------------------------------------------

// The plugin ABI and the build-dependent linkage come from shim_abi.h.
#define SHIM_NAME noop
#include "shim_abi.h"

// Succeed without registering anything. For a plugin a game links for side
// effects already carried by its scripts or the engine: Plugins.link() sees a
// loaded plugin instead of an exception that aborts the scene. State: none.
SHIM_EXPORT SHIM_HIDDEN int V2Link(iTVPFunctionExporter *exporter)
{
	(void)exporter;
	return SHIM_TJS_S_OK;
}

// Symmetric with V2Link: installs nothing, removes nothing.
SHIM_EXPORT SHIM_HIDDEN int V2Unlink(void)
{
	return SHIM_TJS_S_OK;
}
