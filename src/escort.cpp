#include "escort.h"
#include "guards.h"
#include "config.h"
#include "vehicles.h"
#include "combat.h"
#include "chauffeur.h"
#include "motorcade.h"

#include <algorithm>

namespace
{
	constexpr DWORD GRACE_MS = 3000;          // time for group AI to seat guards in the player's car
	constexpr DWORD EXIT_MS = 1000;           // player must be out this long before escorts disband
	constexpr DWORD ASSIGN_INTERVAL_MS = 2000;
	constexpr DWORD SPAWN_BACKOFF_MS = 30000; // after a failed escort-car spawn (same player vehicle)
	constexpr float NEARBY_RADIUS = 50.0f;
	constexpr float PARKED_REUSE_RADIUS = 60.0f;
	constexpr float PARKED_RELEASE_DIST = 150.0f;
	constexpr int MAX_PER_CAR = 4;

	struct Rider
	{
		Ped ped = 0;
		int seat = -1;              // assigned seat; kept when other riders die
	};

	struct EscortCar
	{
		Vehicle veh = 0;
		bool spawned = false;       // created by the mod (deleted on dismiss) vs. commandeered
		std::vector<Rider> riders;  // riders[0] drives
		DWORD boardStart = 0;       // boarding deadline = boardStart + boardMs
		DWORD boardMs = 0;
		DWORD lastEnterTask = 0;
		DWORD lastCheck = 0;
		DWORD lastTeleport = 0;
		DWORD stuckSince = 0;
		bool tasked = false;
		Vehicle taskTarget = 0;
		Vehicle chaseVeh = 0;       // Combat target vehicle being chased, 0 when escorting
		Entity chaseEntity = 0;     // entity the chase task was issued against (driver or vehicle)
	};

	std::vector<EscortCar> s_cars;
	std::vector<Vehicle> s_parked; // spawned escort cars left behind after the player got out
	Vehicle s_playerVeh = 0;
	DWORD s_inVehSince = 0;
	DWORD s_outSince = 0;
	DWORD s_lastAssign = 0;
	DWORD s_lastParkedCheck = 0;
	DWORD s_spawnFailedAt = 0;
	bool s_spawnFailed = false;    // cleared after SPAWN_BACKOFF_MS or when the player changes vehicle

	bool IsEscortVehicle(Vehicle v)
	{
		for (auto& c : s_cars)
			if (c.veh == v)
				return true;
		return false;
	}

	bool IsParked(Vehicle v) { return std::find(s_parked.begin(), s_parked.end(), v) != s_parked.end(); }

	// Nearest unoccupied, driveable street car within radius (parked escort cars count too).
	// Skips other scripts' mission vehicles, the player's last car and other mod units' cars.
	Vehicle FindNearbyCar(Ped player, Vehicle exclude, float radius)
	{
		static int arr[1024];
		int n = worldGetAllVehicles(arr, 1024);
		Vector3 ppos = ENTITY::GET_ENTITY_COORDS(player, TRUE);
		Vehicle lastVeh = PED::GET_VEHICLE_PED_IS_IN(player, TRUE);
		Vehicle best = 0;
		float bestDist = radius;
		for (int i = 0; i < n; i++)
		{
			Vehicle v = arr[i];
			if (v == exclude || v == lastVeh || !ENTITY::DOES_ENTITY_EXIST(v) || IsEscortVehicle(v))
				continue;
			if (Chauffeur::OwnsVehicle(v) || Motorcade::OwnsVehicle(v))
				continue;
			if (!VEHICLE::IS_THIS_MODEL_A_CAR(ENTITY::GET_ENTITY_MODEL(v)) || !VEHICLE::IS_VEHICLE_DRIVEABLE(v, FALSE))
				continue;
			float d = Dist(ppos, ENTITY::GET_ENTITY_COORDS(v, TRUE));
			if (d >= bestDist || !Vehicles::IsEmpty(v))
				continue;
			if (ENTITY::IS_ENTITY_A_MISSION_ENTITY(v) && !IsParked(v))
				continue;
			best = v;
			bestDist = d;
		}
		return best;
	}

	Vehicle TakeParked(Ped player, float radius)
	{
		Vector3 ppos = ENTITY::GET_ENTITY_COORDS(player, TRUE);
		for (auto it = s_parked.begin(); it != s_parked.end(); ++it)
		{
			Vehicle v = *it;
			if (ENTITY::DOES_ENTITY_EXIST(v) && VEHICLE::IS_VEHICLE_DRIVEABLE(v, FALSE) && Vehicles::IsEmpty(v)
				&& Dist(ppos, ENTITY::GET_ENTITY_COORDS(v, TRUE)) < radius)
			{
				s_parked.erase(it);
				return v;
			}
		}
		return 0;
	}

	Vehicle SpawnEscortCar(Vehicle pv, int index)
	{
		Vector3 pos;
		float heading;
		Vehicles::SpotRelative(pv, -(12.0f + 9.0f * (float)index), pos, heading);
		Vehicle v = Vehicles::Spawn(g_escortModels[g_cfg.escortModelIndex].name, pos, heading);
		if (v != 0)
			Vehicles::TuneForPursuit(v); // stock SUVs can't keep up with a fast player
		return v;
	}

	bool SpawnBackedOff()
	{
		if (s_spawnFailed && Elapsed(s_spawnFailedAt, SPAWN_BACKOFF_MS))
			s_spawnFailed = false;
		return s_spawnFailed;
	}

	void StartEscorting(Guard& g, Vehicle v)
	{
		Guards::LeaveGroup(g);
		g.state = GuardState::Escorting;
		g.escortVeh = v;
		g.needsRetask = false;
		PED::SET_PED_COMBAT_ATTRIBUTES(g.ped, CA_LEAVE_VEHICLES, FALSE);
	}

	// Commandeered street cars are never registry-tracked: released straight back to the game.
	void ReleaseCommandeered(Vehicle v)
	{
		if (ENTITY::DOES_ENTITY_EXIST(v))
			ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&v);
	}

	void ReleaseVehicle(EscortCar& car)
	{
		if (!ENTITY::DOES_ENTITY_EXIST(car.veh))
			return;
		if (car.spawned)
			s_parked.push_back(car.veh);
		else
			ReleaseCommandeered(car.veh);
	}

	// Riders get out and rejoin the player's group on foot.
	void Disband(EscortCar& car)
	{
		for (const Rider& r : car.riders)
		{
			Guard* g = Guards::Find(r.ped);
			if (!g || !IsAlive(r.ped))
				continue;
			if (ENTITY::DOES_ENTITY_EXIST(car.veh) && PED::IS_PED_IN_VEHICLE(r.ped, car.veh, FALSE))
				AI::TASK_LEAVE_VEHICLE(r.ped, car.veh, 0);
			g->needsRetask = false;
			Guards::JoinGroup(*g); // also clears g->driving
		}
		ReleaseVehicle(car);
	}

	void AssignOverflow(Ped player, Vehicle pv)
	{
		// Ped handles, not Guard*: spawning a car may yield.
		std::vector<Ped> overflow;
		for (auto& g : Guards::All())
			if (g.role == Role::Bodyguard && g.state == GuardState::Following && g.target == 0
				&& !PED::IS_PED_IN_VEHICLE(g.ped, pv, TRUE))
				overflow.push_back(g.ped);

		FollowMode mode = g_cfg.followMode;
		while (!overflow.empty())
		{
			Vehicle v = 0;
			bool spawned = false;
			bool warp = false;

			if (mode != FollowMode::NearbyCars)
			{
				v = TakeParked(player, PARKED_REUSE_RADIUS);
				spawned = v != 0;
			}
			if (!v && mode != FollowMode::EscortCar)
			{
				v = FindNearbyCar(player, pv, NEARBY_RADIUS);
				auto it = std::find(s_parked.begin(), s_parked.end(), v);
				if (v && it != s_parked.end())
				{
					s_parked.erase(it);
					spawned = true;
				}
			}
			if (!v && mode != FollowMode::NearbyCars && !SpawnBackedOff())
			{
				v = SpawnEscortCar(pv, (int)s_cars.size());
				spawned = true;
				warp = true;
				if (!v)
				{
					s_spawnFailed = true;
					s_spawnFailedAt = Now();
				}
			}
			if (!v)
				return; // nothing available: they stay in the group and we retry later

			if (!spawned)
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, TRUE, TRUE);
			VEHICLE::SET_VEHICLE_DOORS_LOCKED(v, 1); // unlock so guards don't smash windows of parked cars

			EscortCar car;
			car.veh = v;
			car.spawned = spawned;
			car.boardStart = Now();
			car.boardMs = mode == FollowMode::Auto ? 8000 : 15000;
			car.lastEnterTask = Now();

			size_t take = std::min(overflow.size(), (size_t)Vehicles::Capacity(v, MAX_PER_CAR));
			for (size_t i = 0; i < take; i++)
			{
				Guard* g = Guards::Find(overflow[i]);
				if (!g || !IsAlive(g->ped) || g->state != GuardState::Following)
					continue;
				Rider r;
				r.ped = g->ped;
				r.seat = Vehicles::SeatFor(car.riders.size());
				StartEscorting(*g, v);
				g->driving = car.riders.empty();
				car.riders.push_back(r);
				if (warp)
					PED::SET_PED_INTO_VEHICLE(g->ped, v, r.seat);
				else
					AI::TASK_ENTER_VEHICLE(g->ped, v, 20000, r.seat, 2.0f, 1, 0);
			}
			overflow.erase(overflow.begin(), overflow.begin() + take);
			if (car.riders.empty())
			{
				ReleaseVehicle(car);
				continue;
			}
			s_cars.push_back(car);
		}
	}

	void EndEscort()
	{
		for (auto& car : s_cars)
			Disband(car);
		s_cars.clear();
	}

	void UpdateCar(EscortCar& car, int index, Vehicle pv)
	{
		Ped driver = car.riders[0].ped;

		// Boarding: re-issue enter tasks, warp anyone still outside after the deadline.
		// Riders fighting on foot are left alone (and the deadline re-armed) until their target is gone.
		bool allIn = true;
		bool reissue = Elapsed(car.lastEnterTask, 3000);
		for (const Rider& r : car.riders)
		{
			if (PED::IS_PED_IN_VEHICLE(r.ped, car.veh, FALSE))
				continue;
			Guard* g = Guards::Find(r.ped);
			if (g && g->target != 0)
			{
				car.boardStart = Now();
				continue;
			}
			allIn = false;
			if (Elapsed(car.boardStart, car.boardMs))
				PED::SET_PED_INTO_VEHICLE(r.ped, car.veh, r.seat);
			else if (!PED::IS_PED_GETTING_INTO_A_VEHICLE(r.ped) && reissue)
				AI::TASK_ENTER_VEHICLE(r.ped, car.veh, 20000, r.seat, 2.0f, 1, 0);
		}
		if (!allIn)
		{
			if (reissue)
				car.lastEnterTask = Now();
			return;
		}
		if (!PED::IS_PED_IN_VEHICLE(driver, car.veh, FALSE))
			return; // driver still fighting outside

		// If the original driver died, move the new lead rider into the driver seat.
		if (VEHICLE::GET_PED_IN_VEHICLE_SEAT(car.veh, -1) != driver)
		{
			PED::SET_PED_INTO_VEHICLE(driver, car.veh, -1);
			car.riders[0].seat = -1;
			car.tasked = false;
			car.chaseVeh = 0;
			car.chaseEntity = 0;
		}

		// Player ordered a vehicle attack: chase the target instead of escorting.
		Vehicle tv = Combat::TargetVehicle(); // 0 once the target vehicle is wrecked
		if (tv != 0 && IsAlive(tv))
		{
			Ped tdriver = Combat::TargetPed();
			bool driverAboard = IsAlive(tdriver) && PED::IS_PED_IN_VEHICLE(tdriver, tv, FALSE);
			Entity chaseEnt = driverAboard ? (Entity)tdriver : (Entity)tv;
			if (car.chaseVeh != tv || car.chaseEntity != chaseEnt)
			{
				PED::SET_DRIVER_ABILITY(driver, 1.0f);
				PED::SET_DRIVER_AGGRESSIVENESS(driver, 1.0f);
				AI::TASK_VEHICLE_CHASE(driver, chaseEnt);
				AI::SET_TASK_VEHICLE_CHASE_BEHAVIOR_FLAG(driver, 1, TRUE);
				AI::SET_TASK_VEHICLE_CHASE_IDEAL_PURSUIT_DISTANCE(driver, 15.0f);
				car.chaseVeh = tv;
				car.chaseEntity = chaseEnt;
			}
			car.stuckSince = 0;
			return;
		}
		if (car.chaseVeh != 0)
		{
			car.chaseVeh = 0;
			car.chaseEntity = 0;
			car.tasked = false; // resume escorting
		}

		if (pv == 0)
			return;

		float dist = EntityDist(car.veh, pv);
		if (g_cfg.catchUpTeleport && dist > g_cfg.catchUpDistance && Elapsed(car.lastTeleport, 5000))
		{
			Vehicles::TeleportBehind(car.veh, pv, 20.0f + 9.0f * (float)index);
			car.lastTeleport = Now();
			car.tasked = false;
		}

		// Stuck: barely moving while far behind for a few seconds -> re-issue the task.
		if (car.tasked && ENTITY::GET_ENTITY_SPEED(car.veh) < 1.0f && dist > 25.0f)
		{
			if (car.stuckSince == 0)
				car.stuckSince = Now();
			else if (Elapsed(car.stuckSince, 4000))
			{
				car.tasked = false;
				car.stuckSince = 0;
			}
		}
		else
		{
			car.stuckSince = 0;
		}

		if (!car.tasked || car.taskTarget != pv)
		{
			PED::SET_DRIVER_ABILITY(driver, 1.0f);
			PED::SET_DRIVER_AGGRESSIVENESS(driver, 0.75f);
			AI::TASK_VEHICLE_ESCORT(driver, car.veh, pv, -1, g_cfg.escortSpeed, g_cfg.drivingStyle, 10.0f, 0, 20.0f);
			car.tasked = true;
			car.taskTarget = pv;
		}
	}

	void UpdateCars(Vehicle pv)
	{
		for (size_t i = 0; i < s_cars.size();)
		{
			EscortCar& car = s_cars[i];

			car.riders.erase(std::remove_if(car.riders.begin(), car.riders.end(), [&](const Rider& r) {
				Guard* g = Guards::Find(r.ped);
				return !g || !IsAlive(r.ped) || g->state != GuardState::Escorting || g->escortVeh != car.veh;
			}), car.riders.end());

			if (car.riders.empty() || !ENTITY::DOES_ENTITY_EXIST(car.veh) || !VEHICLE::IS_VEHICLE_DRIVEABLE(car.veh, FALSE))
			{
				Disband(car);
				s_cars.erase(s_cars.begin() + i);
				continue;
			}

			for (const Rider& r : car.riders)
			{
				Guard* g = Guards::Find(r.ped);
				if (!g)
					continue;
				// Combat stood this rider down: the boarding / escort logic below reissues his task.
				if (g->needsRetask)
				{
					g->needsRetask = false;
					car.tasked = false;
					car.chaseVeh = 0;
					car.chaseEntity = 0;
					car.lastEnterTask = 0; // outside riders get a fresh enter task on the next check
				}
			}

			// The lead rider drives (changes when the driver dies).
			// A promoted passenger drops his drive-by target (drivers never get combat orders).
			if (Guard* lead = Guards::Find(car.riders[0].ped))
			{
				if (!lead->driving)
					lead->target = 0;
				lead->driving = true;
			}

			if (Elapsed(car.lastCheck, 500))
			{
				car.lastCheck = Now();
				UpdateCar(car, (int)i, pv);
			}
			i++;
		}
	}

	void ReleaseFarParked(Ped player)
	{
		if (!Elapsed(s_lastParkedCheck, 2000))
			return;
		s_lastParkedCheck = Now();
		for (auto it = s_parked.begin(); it != s_parked.end();)
		{
			Vehicle v = *it;
			if (!ENTITY::DOES_ENTITY_EXIST(v))
			{
				it = s_parked.erase(it);
			}
			else if (EntityDist(player, v) > PARKED_RELEASE_DIST)
			{
				Vehicles::Release(v); // spawned car: also untracks it
				it = s_parked.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}

namespace Escort
{
	void Update()
	{
		Ped player = PLAYER::PLAYER_PED_ID();
		Vehicle pv = 0;
		if (PED::IS_PED_IN_ANY_VEHICLE(player, FALSE))
		{
			Vehicle v = PED::GET_VEHICLE_PED_IS_IN(player, FALSE);
			if (Vehicles::IsLandVehicle(v) && !IsEscortVehicle(v) && !Motorcade::OwnsVehicle(v))
				pv = v;
		}

		if (pv != 0)
		{
			s_outSince = 0;
			if (pv != s_playerVeh)
			{
				s_playerVeh = pv;
				s_inVehSince = Now();
				s_spawnFailed = false;
			}
			// The motorcade already escorts this car (its rear cars take the spot behind): no escort cars.
			bool motorcadeCentre = pv == Motorcade::Centre();
			if (!motorcadeCentre && Elapsed(s_inVehSince, GRACE_MS) && Elapsed(s_lastAssign, ASSIGN_INTERVAL_MS))
			{
				s_lastAssign = Now();
				AssignOverflow(player, pv);
			}
		}
		else if (s_playerVeh != 0)
		{
			if (s_outSince == 0)
				s_outSince = Now();
			if (Elapsed(s_outSince, EXIT_MS))
			{
				EndEscort();
				s_playerVeh = 0;
				s_outSince = 0;
			}
		}

		UpdateCars(pv);
		ReleaseFarParked(player);
	}

	void Reset()
	{
		for (auto& car : s_cars)
		{
			if (car.spawned)
				Vehicles::DeleteOrRelease(car.veh);
			else
				ReleaseCommandeered(car.veh);
		}
		for (Vehicle v : s_parked)
			Vehicles::DeleteOrRelease(v);
		s_cars.clear();
		s_parked.clear();
		s_playerVeh = 0;
		s_outSince = 0;
		s_spawnFailed = false;
	}

	void RetaskAll()
	{
		for (auto& car : s_cars)
		{
			car.tasked = false;
			car.chaseVeh = 0;
			car.chaseEntity = 0;
		}
	}

	void Forget()
	{
		s_cars.clear();
		s_parked.clear();
		s_playerVeh = 0;
		s_inVehSince = 0;
		s_outSince = 0;
		s_lastAssign = 0;
		s_lastParkedCheck = 0;
		s_spawnFailedAt = 0;
		s_spawnFailed = false;
	}
}
