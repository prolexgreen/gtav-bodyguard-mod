#pragma once

#include "common.h"

namespace Combat
{
	void AttackAimedTarget(); // hotkey / menu: attack whoever (or whatever vehicle) the player is aiming at
	void Regroup();           // cancel all attacks, every unit returns to its normal task
	void Reset();             // clear target state (used by Dismiss all)
	void Update();
	void Forget();            // drop all state, no natives (script restart; handles may be stale)

	// Current player-ordered target, polled by escort / motorcade / heli modules to switch into
	// chase or attack mode. Both 0 when there is no order.
	Ped TargetPed();          // ped target (for a vehicle order: its current driver, may be 0)
	Vehicle TargetVehicle();  // vehicle target, 0 for a ped order
}
