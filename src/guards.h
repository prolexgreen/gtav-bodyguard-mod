#pragma once

#include "common.h"

enum class Role
{
	Bodyguard,      // player's ped-group guards (spawned from the menu, or promoted heli crew)
	Chauffeur,      // dedicated driver
	MotorcadeCrew,  // drivers + passengers of motorcade cars
	HeliPilot,      // guard heli pilot
	HeliCrew,       // guard heli passengers (can rappel)
	TransportPilot, // luxury transport heli pilot
};

enum class GuardState
{
	Following, // in the player's ped group (on foot or in the player's vehicle)
	Escorting, // removed from the group, riding in an escort vehicle
	Holding,   // bodyguard outside the group (group was full); guards his position
	Unit,      // non-bodyguard role, fully controlled by its owning module
};

struct Guard
{
	Ped ped = 0;
	Blip blip = 0;
	Role role = Role::Bodyguard;
	GuardState state = GuardState::Following;
	bool driving = false;   // at the wheel of a mod vehicle: never given combat orders
	Vehicle escortVeh = 0;
	Ped target = 0;         // current combat target, 0 if none
	bool needsRetask = false; // set by Combat when it stands a unit ped down; the owning module reissues its task and clears it
};

// NOTE: never keep a Guard* across frames or across calls that spawn/remove guards;
// store the Ped handle and look it up with Guards::Find().
namespace Guards
{
	constexpr int MAX_GUARDS = 7; // ped group limit: leader + 7

	std::vector<Guard>& All();
	Guard* Find(Ped ped);
	bool IsGuard(Ped ped);   // any role
	int GroupCount();        // bodyguards occupying group slots (Following + Escorting)

	// Creates a fully configured guard ped (relationships, combat, weapon, options, blip).
	// Loads the model if needed (may yield). Bodyguards join the player group.
	// Returns the ped handle, or 0 on failure.
	Ped Create(const std::string& model, const Vector3& pos, float heading, Role role);

	// Removes a guard from tracking; deletes the ped, or releases it to the game.
	void Remove(Ped ped, bool deletePed);

	// Turns a unit ped (e.g. landed heli crew) into a bodyguard: joins the group if there is room,
	// otherwise holds position until a slot frees up.
	void Promote(Ped ped);

	void Spawn(const std::string& model, int count); // menu spawn of bodyguards
	void GiveWeaponAll(const std::string& weapon);
	void ApplyOptionsAll(); // invincible / infinite ammo / blips / accuracy
	void DismissAll();      // deletes every guard of every role

	void JoinGroup(Guard& g);
	void LeaveGroup(Guard& g);

	void Forget(); // drop all state without calling natives (script restart; handles may be stale)

	void Update();
}
