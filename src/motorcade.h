#pragma once

#include "common.h"
#include "menu.h"

// Lead and rear cars full of guards around the player's (or chauffeur's) car.
namespace Motorcade
{
	void LoadConfig();                          // parse [Motorcade] ini section
	void Update();                              // every frame
	void Reset();                               // Dismiss all: guards are already deleted; delete/release cars, clear state
	void RetaskAll();                           // re-issue formation tasks (after regroup / attack end)
	void BuildMenu(std::vector<MenuItem>& items);
	bool IsActive();
	const char* ModeName();                     // current convoy mode for status text ("Convoy", "Follow behind", "Parked", ...)
	bool OwnsVehicle(Vehicle v);                // true for motorcade cars
	Vehicle Centre();                           // car the motorcade is escorting (0 if none)
	void Forget();                              // drop all state, no natives (script restart; handles may be stale)
	void RefreshBlips();                        // re-create / remove car blips after the Map Blips option changed
}
