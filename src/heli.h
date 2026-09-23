#pragma once

#include "common.h"
#include "menu.h"

// Guard helicopters (circle / attack / rappel) and the luxury transport helicopter.
namespace Heli
{
	void LoadConfig();                          // parse [Heli] ini section
	void Update();                              // every frame
	void Reset();                               // Dismiss all: guards are already deleted; delete/release helis, clear state
	void RetaskAll();                           // back to circling / formation (after regroup / attack end)
	void BuildMenu(std::vector<MenuItem>& items);
	void Rappel();                              // RappelKey / Orders menu: guard heli crews rappel down to the player
	Vehicle TransportHeli();                    // transport heli, 0 if none
	void Forget();                              // drop all state, no natives (script restart; handles may be stale)
	void RefreshBlips();                        // re-create / remove heli blips after the Map Blips option changed
}
