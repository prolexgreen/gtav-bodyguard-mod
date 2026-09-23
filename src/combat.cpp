#include "combat.h"
#include "guards.h"
#include "escort.h"
#include "config.h"
#include "chauffeur.h"
#include "motorcade.h"
#include "heli.h"

#include <algorithm>

namespace
{
	constexpr DWORD DEFEND_INTERVAL_MS = 500;
	constexpr DWORD REASSIGN_INTERVAL_MS = 500;
	constexpr DWORD REGROUP_PAUSE_MS = 5000;
	constexpr DWORD REENGAGE_INTERVAL_MS = 2000;
	constexpr float ORDER_MAX_DIST = 300.0f;
	constexpr int MAX_DEFEND_CANDIDATES = 64;
	constexpr int SCRIPT_TASK_NONE = 7;         // GET_SCRIPT_TASK_STATUS: ped has no such task

	// Player order: a ped, or a vehicle (ped = the aimed occupant, informational only).
	struct Order
	{
		Ped ped = 0;
		Vehicle veh = 0;
	};

	Order s_order;
	std::vector<Ped> s_occupants; // original occupants of a vehicle order, driver first
	Blip s_orderBlip = 0;         // red blip we added (0 if the target already had a blip of its own)
	Entity s_blipEntity = 0;      // entity the order blip belongs to (the vehicle, or a bailed-out occupant)
	DWORD s_lastDefendScan = 0;
	DWORD s_lastReassign = 0;
	DWORD s_lastReengage = 0;
	DWORD s_defendPausedAt = 0;
	bool s_defendPaused = false;  // auto-defend paused for REGROUP_PAUSE_MS after a regroup
	size_t s_nextOccupant = 0;    // round-robin cursor
	std::vector<Ped> s_handled;   // attackers whose damage to the player / a guard was already acted on

	bool HasOrder() { return s_order.ped != 0 || s_order.veh != 0; }

	void RemoveOrderBlip()
	{
		if (s_orderBlip != 0 && UI::DOES_BLIP_EXIST(s_orderBlip))
			UI::REMOVE_BLIP(&s_orderBlip);
		s_orderBlip = 0;
		s_blipEntity = 0;
	}

	// Puts the red order blip on `e`, unless it already carries a blip (mission target, another mod...).
	void SetOrderBlip(Entity e)
	{
		if (e == s_blipEntity)
			return;
		RemoveOrderBlip();
		if (e == 0 || !ENTITY::DOES_ENTITY_EXIST(e))
			return;
		s_blipEntity = e;
		Blip existing = UI::GET_BLIP_FROM_ENTITY(e);
		if (existing != 0 && UI::DOES_BLIP_EXIST(existing))
			return;
		s_orderBlip = UI::ADD_BLIP_FOR_ENTITY(e);
		UI::SET_BLIP_COLOUR(s_orderBlip, BLIP_COLOUR_RED);
	}

	void ClearOrderTarget()
	{
		RemoveOrderBlip();
		s_order = Order();
		s_occupants.clear();
		s_nextOccupant = 0;
	}

	void SetOrder(Ped ped, Vehicle veh)
	{
		ClearOrderTarget();
		s_order.ped = ped;
		s_order.veh = veh;
		SetOrderBlip(veh != 0 ? (Entity)veh : (Entity)ped);
	}

	void Engage(Guard& g, Ped target)
	{
		// Drivers keep driving; their passengers do the shooting.
		if (g.driving)
			return;
		// Seated guards shoot from the car instead of bailing out of a moving vehicle.
		if (PED::IS_PED_SITTING_IN_ANY_VEHICLE(g.ped))
			AI::TASK_DRIVE_BY(g.ped, target, 0, 0.0f, 0.0f, 0.0f, ORDER_MAX_DIST, 100, TRUE, FIRING_PATTERN_FULL_AUTO);
		else
			AI::TASK_COMBAT_PED(g.ped, target, 0, 16);
		g.target = target;
	}

	void StandDown(Guard& g)
	{
		g.target = 0;
		// Following bodyguards are picked up by group AI; units and escort riders get their task
		// reissued by the owning module (escort.cpp for riders), which clears the flag.
		if (g.role != Role::Bodyguard || g.state == GuardState::Escorting)
			g.needsRetask = true;
		if (g.driving)
			return;
		// Seated peds stay seated with CLEAR_PED_TASKS.
		AI::CLEAR_PED_TASKS(g.ped);
	}

	// Whole-module retask at the end of an order / on regroup (e.g. drivers chasing the target vehicle
	// without a combat target of their own). Per-guard stand-downs go through Guard::needsRetask.
	void RetaskUnits()
	{
		Escort::RetaskAll();
		Chauffeur::RetaskAll();
		Motorcade::RetaskAll();
		Heli::RetaskAll();
	}

	bool IsOrderTarget(Ped p)
	{
		if (p == 0)
			return false;
		if (p == s_order.ped)
			return true;
		return std::find(s_occupants.begin(), s_occupants.end(), p) != s_occupants.end();
	}

	// Natural end of an order (target down / lost): stand down attackers, units resume their tasks.
	void EndOrder(const char* msg)
	{
		for (auto& g : Guards::All())
			if (IsOrderTarget(g.target))
				StandDown(g);
		ClearOrderTarget();
		RetaskUnits();
		Notify(std::string("~b~Bodyguards:~s~ ") + msg);
	}

	// Tracked occupant still worth fighting: alive and within order range.
	bool OccupantActive(Ped p, Ped player)
	{
		return IsAlive(p) && EntityDist(player, p) <= ORDER_MAX_DIST;
	}

	std::vector<Ped> ActiveOccupants(Ped player)
	{
		std::vector<Ped> out;
		for (Ped p : s_occupants)
			if (OccupantActive(p, player))
				out.push_back(p);
		return out;
	}

	// Gives every non-driving guard without a living target one of the living occupants, round-robin.
	// all = true: (re)assign every non-driving guard (order start).
	void EngageOccupants(Ped player, bool all)
	{
		std::vector<Ped> living = ActiveOccupants(player);
		if (living.empty())
			return;
		for (auto& g : Guards::All())
		{
			if (g.driving || !IsAlive(g.ped))
				continue;
			if (!all && g.target != 0 && IsAlive(g.target))
				continue;
			Engage(g, living[s_nextOccupant % living.size()]);
			s_nextOccupant++;
		}
	}

	bool IsModVehicle(Vehicle v, Ped player)
	{
		if (Chauffeur::OwnsVehicle(v) || Motorcade::OwnsVehicle(v) || v == Heli::TransportHeli())
			return true;
		if (PED::IS_PED_IN_VEHICLE(player, v, FALSE))
			return true;
		int passengers = VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(v);
		for (int s = -1; s < passengers; s++)
		{
			Ped p = VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, s);
			if (p != 0 && (p == player || Guards::IsGuard(p)))
				return true;
		}
		return false;
	}

	std::vector<Ped> LivingOccupants(Vehicle v)
	{
		std::vector<Ped> out;
		int passengers = VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(v);
		for (int s = -1; s < passengers; s++)
		{
			Ped p = VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, s);
			if (IsAlive(p))
				out.push_back(p);
		}
		return out;
	}

	// Returns true if the order ended this frame.
	bool UpdateOrder(Ped player)
	{
		if (s_order.veh != 0)
		{
			if (!Elapsed(s_lastReassign, REASSIGN_INTERVAL_MS))
				return false;
			s_lastReassign = Now();

			bool vehGone = !IsAlive(s_order.veh);
			bool vehExists = ENTITY::DOES_ENTITY_EXIST(s_order.veh);
			// Range is measured to the nearest living occupant, not to a possibly abandoned car.
			bool anyLiving = false;
			bool anyInside = false;
			float nearest = 0.0f;
			for (Ped p : s_occupants)
			{
				if (!IsAlive(p))
					continue;
				float d = EntityDist(player, p);
				if (!anyLiving || d < nearest)
					nearest = d;
				anyLiving = true;
				if (vehExists && PED::IS_PED_IN_VEHICLE(p, s_order.veh, FALSE))
					anyInside = true;
			}
			// Everyone inside is dead: done, even if the car itself survived (no one left to shoot).
			if (!anyLiving)
			{
				EndOrder(vehGone ? "target vehicle destroyed." : "target neutralized.");
				return true;
			}
			if (nearest > ORDER_MAX_DIST)
			{
				EndOrder("target out of range.");
				return true;
			}
			// Car abandoned: the blip moves to the occupant everyone is hunting.
			SetOrderBlip(anyInside ? (Entity)s_order.veh : (Entity)Combat::TargetPed());
			EngageOccupants(player, false);
			return false;
		}

		if (s_order.ped != 0)
		{
			if (!IsAlive(s_order.ped))
			{
				EndOrder("target down.");
				return true;
			}
			if (EntityDist(player, s_order.ped) > ORDER_MAX_DIST)
			{
				EndOrder("target out of range.");
				return true;
			}
		}
		return false;
	}

	bool IsHandled(Ped p) { return std::find(s_handled.begin(), s_handled.end(), p) != s_handled.end(); }

	// In combat with the player / a guard, or damaged one of them and not acted on yet. The player's
	// last-damage record is never cleared (mission scripts read it), hence the handled set.
	bool IsHostile(Ped ped, Ped player)
	{
		if (PED::IS_PED_IN_COMBAT(ped, player))
			return true;
		for (auto& g : Guards::All())
			if (PED::IS_PED_IN_COMBAT(ped, g.ped))
				return true;
		if (IsHandled(ped))
			return false;
		bool damaged = ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(player, ped, TRUE) != FALSE;
		for (auto& g : Guards::All())
		{
			if (damaged)
				break;
			damaged = ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(g.ped, ped, TRUE) != FALSE;
		}
		if (damaged)
			s_handled.push_back(ped);
		return damaged;
	}

	void DefendScan(Ped player)
	{
		s_handled.erase(std::remove_if(s_handled.begin(), s_handled.end(), [](Ped p) { return !IsAlive(p); }), s_handled.end());

		static int arr[1024];
		int n = worldGetAllPeds(arr, 1024);
		Vector3 ppos = ENTITY::GET_ENTITY_COORDS(player, TRUE);

		// Cheap distance filter first: the guards x peds hostility checks only run on nearby peds.
		std::vector<Ped> candidates;
		for (int i = 0; i < n && (int)candidates.size() < MAX_DEFEND_CANDIDATES; i++)
		{
			Ped p = arr[i];
			if (p == player || !IsAlive(p) || Guards::IsGuard(p))
				continue;
			if (Dist(ppos, ENTITY::GET_ENTITY_COORDS(p, TRUE)) > g_cfg.defendRadius)
				continue;
			candidates.push_back(p);
		}

		std::vector<Ped> hostiles;
		for (Ped p : candidates)
			if (IsHostile(p, player))
				hostiles.push_back(p);

		// Our own guards' damage records are ours to clear.
		for (auto& g : Guards::All())
			ENTITY::CLEAR_ENTITY_LAST_DAMAGE_ENTITY(g.ped);

		if (hostiles.empty())
			return;

		// Spread idle guards across the hostiles round-robin.
		size_t next = 0;
		for (auto& g : Guards::All())
		{
			if (g.target != 0 || g.driving)
				continue;
			Engage(g, hostiles[next % hostiles.size()]);
			next++;
		}
	}

	// Seated shooters whose drive-by ended (target moved, lost sight...) while the target is still valid.
	void ReengageSeated()
	{
		static Hash driveBy = Joaat("SCRIPT_TASK_DRIVE_BY");
		for (auto& g : Guards::All())
		{
			if (g.target == 0 || g.driving || !IsAlive(g.ped) || !PED::IS_PED_SITTING_IN_ANY_VEHICLE(g.ped))
				continue;
			if (AI::GET_SCRIPT_TASK_STATUS(g.ped, driveBy) == SCRIPT_TASK_NONE)
				Engage(g, g.target);
		}
	}
}

namespace Combat
{
	void AttackAimedTarget()
	{
		if (Guards::All().empty())
		{
			Notify("~r~Bodyguards:~s~ no guards spawned.");
			return;
		}

		Player pl = PLAYER::PLAYER_ID();
		Ped player = PLAYER::PLAYER_PED_ID();
		Entity e = 0;
		if (!PLAYER::GET_ENTITY_PLAYER_IS_FREE_AIMING_AT(pl, &e) || e == 0)
			PLAYER::GET_PLAYER_TARGET_ENTITY(pl, &e);

		if (e == 0 || !ENTITY::DOES_ENTITY_EXIST(e) || e == player)
		{
			Notify("~r~Bodyguards:~s~ aim at someone first.");
			return;
		}

		// Resolve: an aimed vehicle, or a ped sitting in one, is a vehicle attack.
		Ped ped = 0;
		Vehicle veh = 0;
		if (ENTITY::IS_ENTITY_A_VEHICLE(e))
		{
			veh = e;
		}
		else if (ENTITY::IS_ENTITY_A_PED(e))
		{
			ped = e;
			if (Guards::IsGuard(ped) || !IsAlive(ped))
			{
				Notify("~r~Bodyguards:~s~ aim at someone first.");
				return;
			}
			if (PED::IS_PED_IN_ANY_VEHICLE(ped, FALSE))
				veh = PED::GET_VEHICLE_PED_IS_IN(ped, FALSE);
		}
		else
		{
			Notify("~r~Bodyguards:~s~ aim at someone first.");
			return;
		}

		if (veh != 0)
		{
			if (!IsAlive(veh) || IsModVehicle(veh, player))
			{
				Notify("~r~Bodyguards:~s~ can't attack that vehicle.");
				return;
			}
			std::vector<Ped> occ = LivingOccupants(veh);
			if (occ.empty())
			{
				Notify("~r~Bodyguards:~s~ that vehicle is empty.");
				return;
			}
			SetOrder(ped, veh);
			s_occupants = occ;
			s_lastReassign = Now();
			EngageOccupants(player, true);
			Notify("~b~Bodyguards:~s~ attacking target vehicle.");
			return;
		}

		SetOrder(ped, 0);
		for (auto& g : Guards::All())
			Engage(g, ped);
		Notify("~b~Bodyguards:~s~ attacking target.");
	}

	void Regroup()
	{
		ClearOrderTarget();
		for (auto& g : Guards::All())
			StandDown(g);
		RetaskUnits();
		s_defendPaused = true;
		s_defendPausedAt = Now();
		Notify("~b~Bodyguards:~s~ regrouping.");
	}

	void Reset()
	{
		ClearOrderTarget();
		s_handled.clear();
	}

	void Forget()
	{
		s_order = Order();
		s_occupants.clear();
		s_orderBlip = 0;
		s_blipEntity = 0;
		s_lastDefendScan = 0;
		s_lastReassign = 0;
		s_lastReengage = 0;
		s_defendPausedAt = 0;
		s_defendPaused = false;
		s_nextOccupant = 0;
		s_handled.clear();
	}

	Ped TargetPed()
	{
		if (s_order.veh != 0)
		{
			// First living occupant (driver first), so units keep hunting bailed-out occupants too.
			std::vector<Ped> occ = ActiveOccupants(PLAYER::PLAYER_PED_ID());
			return occ.empty() ? 0 : occ[0];
		}
		return s_order.ped;
	}

	// 0 once the vehicle is wrecked, so nobody keeps chasing or attacking the wreck.
	Vehicle TargetVehicle() { return IsAlive(s_order.veh) ? s_order.veh : 0; }

	void Update()
	{
		Ped player = PLAYER::PLAYER_PED_ID();

		if (HasOrder())
			UpdateOrder(player);

		for (auto& g : Guards::All())
		{
			if (g.target == 0)
				continue;
			if (!IsAlive(g.target) || EntityDist(player, g.target) > ORDER_MAX_DIST)
				StandDown(g);
		}

		if (Elapsed(s_lastReengage, REENGAGE_INTERVAL_MS))
		{
			s_lastReengage = Now();
			ReengageSeated();
		}

		if (s_defendPaused && Elapsed(s_defendPausedAt, REGROUP_PAUSE_MS))
			s_defendPaused = false;
		if (g_cfg.autoDefend && !s_defendPaused && Elapsed(s_lastDefendScan, DEFEND_INTERVAL_MS))
		{
			s_lastDefendScan = Now();
			DefendScan(player);
		}
	}
}
