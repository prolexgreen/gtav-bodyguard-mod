#pragma once

#include "common.h"
#include "menu.h"

// Dedicated driver who takes the player (in a rear seat) to the map waypoint.
namespace Chauffeur
{
	void LoadConfig();                          // parse [Driver] ini section
	void Update();                              // every frame
	void Reset();                               // Dismiss all: guards are already deleted; delete/release the car, clear state
	void RetaskAll();                           // re-issue driving task (after regroup / attack end)
	void BuildMenu(std::vector<MenuItem>& items);
	void OnHotkey();                            // DriverKey: call driver, or drive to waypoint if already aboard
	Vehicle Car();                              // chauffeur car, 0 if none
	bool OwnsVehicle(Vehicle v);                // active chauffeur car OR a dismissed one still driving away
	bool IsHurrying();                          // hurry toggle is on (motorcade matches it)
	bool IsParked();                            // car is stopped at the curb on purpose (waiting, arrived, pulled over, driver outside)

	// Convoy link with the motorcade:
	bool HasDestination(Vector3& dest);         // road-snapped destination while driving the player to a waypoint
	float DesiredSpeed();                       // cruise speed he is aiming for (normal / hurry)
	void SetLeadVehicle(Vehicle v);             // follow this car (escort-behind) instead of driving the route himself; 0 = drive himself
	Vehicle LeadVehicle();                      // current lead (0 = none)
	void Forget();                              // drop all state, no natives (script restart; handles may be stale)
	void RefreshBlips();                        // re-create / remove the car blip after the Map Blips option changed
}
