#pragma once

#include "common.h"

// Remembers the mod's vehicles outside the DLL (process environment variable), so leftovers from a
// previous script session / .asi reload can be cleaned up when the script starts again.
namespace Registry
{
	void Track(Entity e);
	void Untrack(Entity e);

	// Deletes vehicles tracked by a previous session and every ped in the mod's relationship group.
	// Call once at script start, after all modules have forgotten their state.
	void CleanupOrphans();
}
