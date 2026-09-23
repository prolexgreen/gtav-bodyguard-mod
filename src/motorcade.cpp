#include "motorcade.h"
#include "guards.h"
#include "vehicles.h"
#include "config.h"
#include "combat.h"
#include "chauffeur.h"

#include <algorithm>

namespace
{
	constexpr DWORD CHECK_MS = 500;            // per-car logic throttle
	constexpr DWORD EXIT_MS = 1500;            // player must be out this long before the crew dismounts
	constexpr DWORD BOARD_MS = 12000;          // remount: warp anyone still outside after this
	constexpr DWORD BOARD_GIVEUP_MS = 5000;    // ...and promote to bodyguard whoever is still outside this long after the warp
	constexpr DWORD ENTER_REISSUE_MS = 3000;   // re-issue enter / leave tasks at most this often
	constexpr DWORD DRIVER_WALK_MS = 10000;    // replacement driver walking to the car: warp after this
	constexpr DWORD PERIMETER_MS = 3000;       // perimeter slot check interval
	constexpr DWORD ATTACK_POLL_MS = 500;
	constexpr DWORD TELEPORT_COOLDOWN_MS = 5000;
	constexpr DWORD STUCK_MS = 4000;
	constexpr DWORD REJOIN_DRIVE_MS = 10000;   // foot mode: re-issue drive-to-player at most this often
	constexpr DWORD RANGE_CHECK_MS = 1000;
	constexpr DWORD GOVERN_MS = 500;           // gap governor tick (lead <-> centre)
	constexpr DWORD LEAD_BEHIND_MS = 3000;     // rearmost lead behind the centre this long = it fell behind (traffic)
	constexpr float PERIMETER_RADIUS = 5.0f;
	constexpr float SLOT_FAR = 10.0f;
	constexpr float REJOIN_DIST = 150.0f;      // parked cars drive closer when the player walks this far
	constexpr float DISMOUNT_DIST = 40.0f;     // passengers only get out when their car is this close to the player
	constexpr float LOST_DIST = 1000.0f;       // player this far from every car: disband
	constexpr float DRIVER_WALK_DIST = 15.0f;  // replacement driver farther than this walks to the car first
	constexpr float TWO_PI = 6.2831853f;
	constexpr int BLIP_SPRITE = -1;            // default dot

	// Follow chain (metres, m/s): escort-behind the link, cruise speed tuned every CHECK_MS
	constexpr float CEILING = 55.0f;           // task speed cap; the gap logic sets the real cruise speed
	constexpr float CEILING_HURRY = 80.0f;
	constexpr float GAP = 9.0f;                // target gap behind the link
	constexpr float GAP_BAND = 4.0f;           // within +-GAP_BAND of the target gap = in position
	constexpr float GAP_GAIN = 1.0f;           // cruise = pace speed + GAP_GAIN * gap error (in position)
	constexpr float BOOST = 12.0f;             // catch-up margin over the pace car's speed
	constexpr float BOOST_HURRY = 20.0f;
	constexpr float MIN_CATCHUP_SPEED = 18.0f; // catch-up never slower than this (pace car just pulling away)
	constexpr float REAR_EASE = 3.0f;          // too close: pace speed - this
	constexpr float LATERAL_MAX = 20.0f;       // farther off the link's centre line than this: along-track is unreliable
	constexpr float STOPPED_SPEED = 1.0f;
	constexpr float STOPPED_CREEP = 12.0f;     // pace car stopped: cap for chain cars still closing the gap
	constexpr float FOLLOW_MIN_DIST = 5.0f;    // TASK_VEHICLE_ESCORT min distance behind the centre / a follower
	constexpr float LEAD_MIN_DIST = 6.0f;      // ...behind the lead car ahead
	constexpr float POWER_BOOST = 30.0f;       // extra engine power (%) while a follower is behind its slot

	// Gap governor: cruise speed of the route-driving lead from the gap between the centre and the rearmost lead
	constexpr float GOV_NEAR = 8.0f;           // closer than this: desired + GOV_SPEEDUP
	constexpr float GOV_FAR = 20.0f;           // farther than this: max(desired - GOV_EASE, GOV_MIN)
	constexpr float GOV_CRAWL = 60.0f;         // farther than this: crawl (stop if the centre stopped)
	constexpr float GOV_SPEEDUP = 6.0f;
	constexpr float GOV_EASE = 6.0f;
	constexpr float GOV_MIN = 6.0f;
	constexpr float GOV_CRAWL_SPEED = 6.0f;
	constexpr float CATCHUP_MARGIN = 12.0f;    // lead that fell behind: desired + this until it is ahead again
	constexpr float LEAD_BEHIND = -5.0f;       // rearmost lead this far behind the centre (along track) = behind
	constexpr float LEAD_AHEAD_AGAIN = 15.0f;  // ...followed again once this far ahead
	constexpr float LEAD_FOLLOW_MIN = 5.0f;    // chauffeur only follows a lead at least this far ahead
	constexpr float LEAD_FOLLOW_LATERAL = 40.0f; // ...and not on a parallel street
	constexpr float DEST_MOVED = 10.0f;        // destination moved this far: re-issue the route
	constexpr float ARRIVE_RADIUS = 80.0f;     // lead this close to the destination parks at the curb ahead of it
	constexpr float ARRIVE_SPEED = 8.0f;
	constexpr float DEST_PASSED = 150.0f;      // own car: reached the waypoint, then drove this far off = ignore it
	constexpr float ROUTE_MOVING = 5.0f;       // route lead told to drive at least this fast counts as "should move"
	constexpr float STUCK_LINK_DIST = 25.0f;

	// Guard detail (parked own car, player walks off)
	constexpr float GUARD_WALK_DIST = 30.0f;   // player this far from his parked car on foot: guard detail
	constexpr float GUARD_RETURN_DIST = 20.0f; // ...back within this: normal parked formation (hysteresis)
	constexpr int GUARD_CARS = 2;              // at most this many cars stay with the car (rears first)

	constexpr float AUTO_HURRY_SPEED = 25.0f;  // Auto: player driving faster than this = hurry...
	constexpr float AUTO_CALM_SPEED = 15.0f;   // ...until slower than this (hysteresis)
	constexpr int HURRY_OFF = 0, HURRY_ON = 1, HURRY_AUTO = 2;
	constexpr float SPAWN_SPACING = 6.0f;      // spawn spot must be this far from other motorcade cars
	constexpr float VIP_SPEED = 25.0f;         // VIP car driving to the waypoint
	constexpr float PARK_GAP = 7.5f;           // parked formation: bumper-to-bumper spacing along the curb
	constexpr float PARK_REACHED = 2.5f;
	constexpr float PARK_CURB_SNAP = 15.0f;    // arrival slot: use the curb point when it is this close to the ideal spot
	constexpr DWORD PARK_STALL_MS = 4000;
	constexpr DWORD PARK_GIVEUP_MS = 20000;
	constexpr DWORD CALM_MS = 8000;            // no hostiles this long: crew who got out to fight get back in
	constexpr float HOSTILE_DIST = 80.0f;
	constexpr int NO_SEAT = -2;

	// What a car's driver was last told to do
	enum TaskKind { TASK_NONE, TASK_ROUTE, TASK_ESCORT, TASK_ARRIVE };

	// One mode per update; every car gets its role from it (see Motorcade::ModeName)
	enum Mode { MODE_NONE, MODE_CONVOY, MODE_LEAD_WP, MODE_FOLLOW, MODE_PICKUP, MODE_PARKED, MODE_GUARD };

	// "When you drive" setting
	enum DriveFormation { FOLLOW_BEHIND = 0, LEAD_TO_WAYPOINT = 1 };

	struct Car
	{
		Vehicle veh = 0;
		Blip blip = 0;
		bool lead = false;          // chain it belongs to (VIP car: chain it returns to)
		std::vector<Ped> crew;      // crew[0] drives
		bool tasked = false;        // driving task issued (VIP: drive / hold)
		int taskKind = TASK_NONE;
		Vehicle taskTarget = 0;     // escort link
		DWORD taskIssuedAt = 0;
		bool taskHurry = false;     // task was issued in hurry mode
		float lastCruise = -1.0f;
		bool boosted = false;       // engine power boost on (catching up)
		bool arriving = false;      // lead near the destination: parking at its curb slot ahead of it
		bool chasing = false;
		Vehicle chaseTarget = 0;
		Entity chaseEntity = 0;
		bool dismounted = false;    // passengers were told to get out (perimeter)
		bool parked = false;        // driver stopped after the player got out
		bool guardPost = false;     // guard detail: stays with the parked car, crew inside
		bool forceEnter = false;    // Board re-issues enter tasks on its next pass
		DWORD lastCheck = 0;
		DWORD lastTeleport = 0;
		DWORD stuckSince = 0;
		DWORD boardSince = 0;       // first time a rider was seen outside (0 = unset)
		DWORD lastEnterTask = 0;
		DWORD lastLeaveTask = 0;
		DWORD lastRejoin = 0;
		Ped walkingDriver = 0;      // replacement driver on his way to the wheel
		DWORD driverWalkSince = 0;
		DWORD lastDriverTask = 0;
		float destX = 0.0f, destY = 0.0f, destZ = 0.0f; // route / VIP drive destination the task was issued for
		bool parkApproach = false;  // driving to its slot at the curb
		Vector3 parkPos;
		DWORD parkSince = 0;
		DWORD lastHostile = 0;      // last time crew were fighting / a target was near
	};

	// Settings
	int s_leadCount = 1;
	int s_rearCount = 1;
	int s_perCar = 3;
	int s_carModel = 0;
	int s_crewModel = 0;
	int s_hurryMode = HURRY_AUTO;
	int s_driveFormation = LEAD_TO_WAYPOINT;
	bool s_getOut = false;       // crew get out and form a perimeter when the player is on foot (else only for hostiles)
	const char* s_hurryNames[] = { "Off", "On", "Match driver" };
	const char* s_formationNames[] = { "Follow behind", "Lead to waypoint" };

	// State
	std::vector<Car> s_leads;   // s_leads[0] directly ahead of the centre; s_leads.back() is the front car (route head)
	std::vector<Car> s_rears;   // s_rears[0] directly behind the centre
	std::vector<Car> s_vip;     // 0 or 1 motorcade car the player is riding in (it is the centre)
	Vehicle s_centre = 0;
	int s_mode = MODE_NONE;
	bool s_onFoot = false;      // perimeter mode
	DWORD s_outSince = 0;
	DWORD s_lastPerimeter = 0;
	DWORD s_lastFarCheck = 0;
	bool s_perimeterDirty = false;
	std::vector<Ped> s_ringTasked;  // on-foot crew that already got a perimeter task
	size_t s_ringSize = 0;
	Vehicle s_attackVeh = 0;
	DWORD s_lastAttackPoll = 0;
	DWORD s_lastRangeCheck = 0;
	bool s_autoFast = false;        // Auto hurry: player driving the centre fast (with hysteresis)

	// Convoy / lead-to-waypoint
	Vector3 s_dest;                 // destination the leads drive to
	bool s_haveDest = false;        // ...valid for the current trip
	Vector3 s_lastDest;             // last destination seen (moved test survives mode changes)
	bool s_lastDestValid = false;
	bool s_destReached = false;     // own car: the centre came within ARRIVE_RADIUS of the waypoint...
	bool s_destPassed = false;      // ...and then drove DEST_PASSED away from it: follow behind instead
	float s_govCruise = 0.0f;       // governor output: cruise speed of the route head
	DWORD s_lastGovern = 0;
	DWORD s_leadBehindSince = 0;
	bool s_leadCatchingUp = false;  // rearmost lead fell behind the centre; nobody follows it until it is ahead again
	DWORD s_lastLeadSend = 0;
	int s_guardCars = 0;            // guard detail size
	bool s_guardFar = false;        // player walked off from his parked car (hysteresis)

	float Dist2D(const Vector3& a, const Vector3& b)
	{
		float dx = a.x - b.x, dy = a.y - b.y;
		return std::sqrt(dx * dx + dy * dy);
	}

	bool IsMotorcadeCar(Vehicle v)
	{
		if (v == 0)
			return false;
		for (auto* chain : { &s_leads, &s_rears, &s_vip })
			for (auto& c : *chain)
				if (c.veh == v)
					return true;
		return false;
	}

	int CountCars() { return (int)(s_leads.size() + s_rears.size() + s_vip.size()); }

	int CountGuards()
	{
		int n = 0;
		for (auto* chain : { &s_leads, &s_rears, &s_vip })
			for (auto& c : *chain)
				n += (int)c.crew.size();
		return n;
	}

	void SetDriving(Ped ped, bool driving)
	{
		if (Guard* g = Guards::Find(ped))
		{
			g->driving = driving;
			if (driving)
				g->target = 0;
		}
	}

	bool IsSeatReserved(const std::vector<int>& reserved, int seat)
	{
		return std::find(reserved.begin(), reserved.end(), seat) != reserved.end();
	}

	// Passenger seat for crew[i]: its usual seat if free and not reserved this pass, else any free one.
	int PickSeat(Vehicle v, size_t i, const std::vector<int>& reserved)
	{
		int seat = Vehicles::SeatFor(i);
		if (seat >= 0 && VEHICLE::IS_VEHICLE_SEAT_FREE(v, seat) && !IsSeatReserved(reserved, seat))
			return seat;
		int passengers = VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(v);
		for (int s = 0; s < passengers; s++)
			if (VEHICLE::IS_VEHICLE_SEAT_FREE(v, s) && !IsSeatReserved(reserved, s))
				return s;
		return NO_SEAT;
	}

	// Player's land vehicle that may become the centre (never a dismissed chauffeur car driving away).
	Vehicle CurrentLandVehicle(Ped player)
	{
		if (!PED::IS_PED_IN_ANY_VEHICLE(player, FALSE))
			return 0;
		Vehicle v = PED::GET_VEHICLE_PED_IS_IN(player, FALSE);
		if (!Vehicles::IsLandVehicle(v))
			return 0;
		if (Chauffeur::OwnsVehicle(v) && v != Chauffeur::Car())
			return 0;
		return v;
	}

	void AddBlip(Car& car)
	{
		Vehicles::RemoveUnitBlip(car.blip);
		car.blip = Vehicles::AddUnitBlip(car.veh, BLIP_SPRITE, BLIP_COLOUR_BLUE, "Motorcade");
	}

	// Road spot `forward` metres from the centre, nudged along the chain until it is clear of other motorcade cars.
	void FindSpawnSpot(Vehicle centre, float forward, Vector3& pos, float& heading)
	{
		float dir = forward >= 0.0f ? 1.0f : -1.0f;
		for (int attempt = 0; attempt < 4; attempt++)
		{
			Vehicles::SpotRelative(centre, forward, pos, heading);
			bool clear = Dist(pos, ENTITY::GET_ENTITY_COORDS(centre, TRUE)) >= SPAWN_SPACING;
			for (auto* chain : { &s_leads, &s_rears })
				for (auto& c : *chain)
					if (clear && Dist(pos, ENTITY::GET_ENTITY_COORDS(c.veh, TRUE)) < SPAWN_SPACING)
						clear = false;
			if (clear)
				return;
			forward += dir * SPAWN_SPACING;
		}
	}

	Vehicle SpawnCar(Vehicle centre, size_t index, int crewCount, bool lead, std::vector<Car>& out)
	{
		const std::string& carModel = g_escortModels[s_carModel].name;
		const std::string& crewModel = g_guardModels[s_crewModel].name;
		// Models first (may yield), then the spot from the centre's current position.
		if (!LoadModel(Joaat(carModel)) || !LoadModel(Joaat(crewModel)) || !ENTITY::DOES_ENTITY_EXIST(centre))
			return 0;

		Vector3 pos;
		float heading;
		FindSpawnSpot(centre, (lead ? GAP : -GAP) * (float)(index + 1), pos, heading);
		Vehicle v = Vehicles::Spawn(carModel, pos, heading);
		if (!v)
			return 0;
		Vehicles::TuneForPursuit(v);

		Car car;
		car.veh = v;
		car.lead = lead;
		int n = std::min(crewCount, Vehicles::Capacity(v, 4));
		for (int i = 0; i < n; i++)
		{
			Vector3 at = ENTITY::GET_ENTITY_COORDS(v, TRUE);
			Ped p = Guards::Create(crewModel, at, heading, Role::MotorcadeCrew);
			if (!p)
				break;
			PED::SET_PED_INTO_VEHICLE(p, v, Vehicles::SeatFor(car.crew.size()));
			PED::SET_PED_COMBAT_ATTRIBUTES(p, CA_LEAVE_VEHICLES, FALSE);
			if (Guard* g = Guards::Find(p))
				g->escortVeh = v;
			SetDriving(p, car.crew.empty());
			car.crew.push_back(p);
		}
		if (car.crew.empty())
		{
			Vehicles::DeleteOrRelease(v);
			return 0;
		}
		AddBlip(car);
		out.push_back(car);
		return v;
	}

	// Crew gets out and wanders off, the driver drives away; everything is released to the game.
	// If the player sits in the car, the whole crew gets out and leaves him the car.
	void ReleaseCar(Car& car)
	{
		Vehicles::RemoveUnitBlip(car.blip);
		Vehicles::SetPowerBoost(car.veh, 0.0f);
		bool exists = ENTITY::DOES_ENTITY_EXIST(car.veh);
		bool playerInside = exists && PED::IS_PED_IN_VEHICLE(PLAYER::PLAYER_PED_ID(), car.veh, FALSE);
		for (size_t i = 0; i < car.crew.size(); i++)
		{
			Ped p = car.crew[i];
			if (!Guards::Find(p))
				continue;
			bool alive = IsAlive(p);
			bool seated = alive && exists && PED::IS_PED_IN_VEHICLE(p, car.veh, FALSE);
			bool drives = seated && !playerInside && VEHICLE::GET_PED_IN_VEHICLE_SEAT(car.veh, -1) == p
				&& VEHICLE::IS_VEHICLE_DRIVEABLE(car.veh, FALSE);
			// Remove first: releasing resets keep-task, so the parting task is given afterwards.
			Guards::Remove(p, false);
			if (!alive)
				continue;
			if (drives)
				AI::TASK_VEHICLE_DRIVE_WANDER(p, car.veh, 20.0f, Vehicles::STYLE_NORMAL);
			else if (seated)
				AI::TASK_LEAVE_VEHICLE(p, car.veh, 0);
			else if (PED::IS_PED_ON_FOOT(p))
				AI::TASK_WANDER_STANDARD(p, 10.0f, 10);
			PED::SET_PED_KEEP_TASK(p, TRUE);
		}
		car.crew.clear();
		Vehicles::Release(car.veh);
	}

	// Survivors of a wrecked car become regular bodyguards.
	void AbandonCar(Car& car)
	{
		Vehicles::RemoveUnitBlip(car.blip);
		Vehicles::SetPowerBoost(car.veh, 0.0f);
		for (Ped p : car.crew)
			if (Guards::Find(p))
			{
				PED::SET_PED_COMBAT_ATTRIBUTES(p, CA_LEAVE_VEHICLES, TRUE);
				Guards::Promote(p);
			}
		car.crew.clear();
		Vehicles::Release(car.veh);
	}

	void ClearConvoyState()
	{
		s_haveDest = false;
		s_lastDestValid = false;
		s_destReached = false;
		s_destPassed = false;
		s_govCruise = 0.0f;
		s_lastGovern = 0;
		s_leadBehindSince = 0;
		s_leadCatchingUp = false;
		s_guardCars = 0;
		s_guardFar = false;
	}

	void ClearState()
	{
		s_mode = MODE_NONE;
		s_leads.clear();
		s_rears.clear();
		s_vip.clear();
		s_centre = 0;
		s_onFoot = false;
		s_outSince = 0;
		s_attackVeh = 0;
		s_perimeterDirty = false;
		s_ringTasked.clear();
		s_ringSize = 0;
		ClearConvoyState();
	}

	void Disband()
	{
		for (auto* chain : { &s_leads, &s_rears, &s_vip })
			for (auto& c : *chain)
				ReleaseCar(c);
		Chauffeur::SetLeadVehicle(0); // a released lead car is still alive: he must not keep following it
		ClearState();
	}

	void Form()
	{
		if (g_escortModels.empty() || g_guardModels.empty())
			return;
		if (s_leadCount + s_rearCount == 0)
		{
			Notify("~r~Motorcade:~s~ set at least one lead or rear car.");
			return;
		}

		Ped player = PLAYER::PLAYER_PED_ID();
		Vehicle inside = PED::IS_PED_IN_ANY_VEHICLE(player, FALSE) ? PED::GET_VEHICLE_PED_IS_IN(player, FALSE) : 0;
		if (IsMotorcadeCar(inside))
		{
			// Re-forming would release the car the player sits in.
			Notify("~r~Motorcade:~s~ get out of the motorcade car first.");
			return;
		}
		Vehicle centre = CurrentLandVehicle(player);
		if (!centre && PED::IS_PED_ON_FOOT(player) && IsAlive(Chauffeur::Car()))
			centre = Chauffeur::Car();
		if (!centre)
		{
			Notify("~r~Motorcade:~s~ get in a car first.");
			return;
		}

		if (!LoadModel(Joaat(g_escortModels[s_carModel].name)) || !LoadModel(Joaat(g_guardModels[s_crewModel].name)))
		{
			Notify("~r~Motorcade:~s~ couldn't load the car / crew model.");
			return;
		}

		if (Motorcade::IsActive())
			Disband();

		for (int i = 0; i < s_leadCount; i++)
			if (!SpawnCar(centre, (size_t)i, s_perCar, true, s_leads))
				break;
		for (int i = 0; i < s_rearCount; i++)
			if (!SpawnCar(centre, (size_t)i, s_perCar, false, s_rears))
				break;

		s_centre = centre;
		s_mode = MODE_NONE; // first Update picks the mode
		s_onFoot = false;
		s_outSince = 0;
		s_attackVeh = 0;
		ClearConvoyState();
		if (!Motorcade::IsActive())
		{
			Notify("~r~Motorcade:~s~ couldn't spawn any cars.");
			return;
		}
		Notify("~b~Motorcade:~s~ " + std::to_string(CountCars()) + " cars, " + std::to_string(CountGuards()) + " guards.");
	}

	// Drops dead / missing crew and wrecked cars. Returns false when the car should be removed.
	bool PruneCar(Car& car)
	{
		car.crew.erase(std::remove_if(car.crew.begin(), car.crew.end(), [](Ped p) {
			Guard* g = Guards::Find(p);
			return !g || !IsAlive(p) || g->role != Role::MotorcadeCrew;
		}), car.crew.end());

		if (!ENTITY::DOES_ENTITY_EXIST(car.veh) || !VEHICLE::IS_VEHICLE_DRIVEABLE(car.veh, FALSE))
		{
			AbandonCar(car);
			return false;
		}
		if (car.crew.empty())
		{
			Vehicles::RemoveUnitBlip(car.blip);
			Vehicles::SetPowerBoost(car.veh, 0.0f);
			Vehicles::Release(car.veh);
			return false;
		}
		return true;
	}

	void PruneChain(std::vector<Car>& chain)
	{
		for (size_t i = 0; i < chain.size();)
		{
			if (PruneCar(chain[i]))
				i++;
			else
				chain.erase(chain.begin() + i);
		}
	}

	// Forget tasks so the next pass re-issues whatever the car should be doing (an arriving lead keeps its slot).
	void ResetCarTask(Car& car)
	{
		car.tasked = false;
		car.taskKind = TASK_NONE;
		car.chasing = false;
		car.chaseEntity = 0;
		car.parked = false;
		car.stuckSince = 0;
		car.lastCruise = -1.0f;
	}

	// Mode change: every car starts its new role from scratch (dismounted crews are handled by the remount logic).
	void ResetCarRole(Car& car)
	{
		ResetCarTask(car);
		car.parkApproach = false;
		car.arriving = false;
		car.guardPost = false;
		car.lastCheck = 0;
	}

	void SetTask(Car& car, int kind, Vehicle target, bool hurry)
	{
		car.tasked = true;
		car.taskKind = kind;
		car.taskTarget = target;
		car.taskHurry = hurry;
		car.taskIssuedAt = Now();
		car.parked = false;
		car.stuckSince = 0;
		car.lastCruise = -1.0f; // the task isn't active yet: tune its speed from the next pass on
	}

	void SetBoost(Car& car, bool on)
	{
		if (car.boosted == on)
			return;
		Vehicles::SetPowerBoost(car.veh, on ? POWER_BOOST : 0.0f);
		car.boosted = on;
	}

	void DeleteCorpse(Ped corpse)
	{
		if (corpse == PLAYER::PLAYER_PED_ID())
			return;
		if (Guards::Find(corpse))
		{
			Guards::Remove(corpse, true);
			return;
		}
		ENTITY::SET_ENTITY_AS_MISSION_ENTITY(corpse, TRUE, TRUE);
		PED::DELETE_PED(&corpse);
	}

	// Makes sure crew[0] sits at the wheel (the original driver may have died). A replacement far from the car
	// walks to it first and is warped in after DRIVER_WALK_MS. Returns false while the seat is still empty.
	bool EnsureDriver(Car& car)
	{
		Ped driver = car.crew[0];
		SetDriving(driver, true);
		Ped occupant = VEHICLE::GET_PED_IN_VEHICLE_SEAT(car.veh, -1);
		if (occupant == driver)
		{
			if (car.walkingDriver != 0)
			{
				// Replacement arrived on foot: re-issue the car's task.
				car.walkingDriver = 0;
				ResetCarTask(car);
			}
			return true;
		}
		auto it = std::find(car.crew.begin(), car.crew.end(), occupant);
		if (occupant != 0 && it != car.crew.end() && IsAlive(occupant))
		{
			// Another crew member shuffled over to the wheel: he is the driver now.
			std::iter_swap(car.crew.begin(), it);
			SetDriving(driver, false);
			SetDriving(occupant, true);
			car.walkingDriver = 0;
			ResetCarTask(car);
			return true;
		}
		if (occupant != 0 && ENTITY::DOES_ENTITY_EXIST(occupant) && ENTITY::IS_ENTITY_DEAD(occupant))
			DeleteCorpse(occupant);

		bool inCar = PED::IS_PED_IN_VEHICLE(driver, car.veh, FALSE);
		if (!inCar && (car.walkingDriver == driver || EntityDist(driver, car.veh) > DRIVER_WALK_DIST))
		{
			if (car.walkingDriver != driver)
			{
				car.walkingDriver = driver;
				car.driverWalkSince = Now();
				car.lastDriverTask = Now();
				PED::SET_PED_COMBAT_ATTRIBUTES(driver, CA_LEAVE_VEHICLES, FALSE);
				AI::TASK_ENTER_VEHICLE(driver, car.veh, 20000, -1, 2.0f, 1, 0);
			}
			if (!Elapsed(car.driverWalkSince, DRIVER_WALK_MS))
			{
				if (Elapsed(car.lastDriverTask, ENTER_REISSUE_MS) && !PED::IS_PED_GETTING_INTO_A_VEHICLE(driver))
				{
					AI::TASK_ENTER_VEHICLE(driver, car.veh, 20000, -1, 2.0f, 1, 0);
					car.lastDriverTask = Now();
				}
				return false;
			}
		}

		PED::SET_PED_INTO_VEHICLE(driver, car.veh, -1);
		PED::SET_PED_COMBAT_ATTRIBUTES(driver, CA_LEAVE_VEHICLES, FALSE);
		car.walkingDriver = 0;
		ResetCarTask(car);
		return VEHICLE::GET_PED_IN_VEHICLE_SEAT(car.veh, -1) == driver;
	}

	// Riders crew[first..] get (back) in. Seats are reserved within the pass so two riders never head for the
	// same one; riders without a free seat, or still outside long after the warp, become regular bodyguards
	// instead of blocking the car. Returns true when every remaining rider is seated.
	bool Board(Car& car, size_t first)
	{
		bool allIn = true;
		bool reissue = car.forceEnter || Elapsed(car.lastEnterTask, ENTER_REISSUE_MS);
		std::vector<int> reserved;
		std::vector<Ped> dropped;
		for (size_t i = first; i < car.crew.size(); i++)
		{
			Ped p = car.crew[i];
			if (PED::IS_PED_IN_VEHICLE(p, car.veh, FALSE))
				continue;
			int seat = PickSeat(car.veh, i, reserved);
			if (car.boardSince == 0)
				car.boardSince = Now();
			if (seat == NO_SEAT || Elapsed(car.boardSince, BOARD_MS + BOARD_GIVEUP_MS))
			{
				dropped.push_back(p);
				continue;
			}
			reserved.push_back(seat);
			allIn = false;
			PED::SET_PED_COMBAT_ATTRIBUTES(p, CA_LEAVE_VEHICLES, FALSE);
			if (Elapsed(car.boardSince, BOARD_MS))
				PED::SET_PED_INTO_VEHICLE(p, car.veh, seat);
			else if (reissue && !PED::IS_PED_GETTING_INTO_A_VEHICLE(p))
				AI::TASK_ENTER_VEHICLE(p, car.veh, 20000, seat, 2.0f, 1, 0);
		}
		if (reissue)
		{
			car.lastEnterTask = Now();
			car.forceEnter = false;
		}
		for (Ped p : dropped)
		{
			car.crew.erase(std::remove(car.crew.begin(), car.crew.end(), p), car.crew.end());
			PED::SET_PED_COMBAT_ATTRIBUTES(p, CA_LEAVE_VEHICLES, TRUE);
			Guards::Promote(p);
		}
		if (allIn)
			car.boardSince = 0;
		else
			car.tasked = false;
		return allIn;
	}

	// Combat stood a crew member down: give him his current task back.
	void HandleRetask(Car& car)
	{
		for (size_t i = 0; i < car.crew.size(); i++)
		{
			Ped p = car.crew[i];
			Guard* g = Guards::Find(p);
			if (!g || !g->needsRetask)
				continue;
			g->needsRetask = false;
			if (i == 0)
			{
				ResetCarTask(car); // escort / chase / park re-issued this pass
				continue;
			}
			if (car.dismounted)
			{
				// Perimeter passenger: re-slot him, or get him out if he is still seated.
				s_ringTasked.erase(std::remove(s_ringTasked.begin(), s_ringTasked.end(), p), s_ringTasked.end());
				car.lastLeaveTask = 0;
			}
			else if (!PED::IS_PED_IN_VEHICLE(p, car.veh, FALSE))
			{
				car.forceEnter = true;
			}
		}
	}

	bool TeleportClear(const Vector3& pos)
	{
		return !CAM::IS_SPHERE_VISIBLE(pos.x, pos.y, pos.z, 3.0f);
	}

	// Moves a car that fell far behind back behind the centre, only where the player can't see it pop in.
	// Leads land behind the rear cars and work their way forward again.
	bool CatchUpTeleport(Car& car, size_t index)
	{
		float base = GAP * (float)((car.lead ? s_rears.size() : 0) + index + 1);
		const float tries[] = { base, base + 25.0f, base + 50.0f };
		for (float t : tries)
		{
			Vector3 pos;
			float heading;
			Vehicles::SpotRelative(s_centre, -t, pos, heading);
			if (!TeleportClear(pos))
				continue;
			ENTITY::SET_ENTITY_COORDS(car.veh, pos.x, pos.y, pos.z, FALSE, FALSE, FALSE, TRUE);
			ENTITY::SET_ENTITY_HEADING(car.veh, heading);
			VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(car.veh);
			VEHICLE::SET_VEHICLE_FORWARD_SPEED(car.veh, ENTITY::GET_ENTITY_SPEED(s_centre));
			return true;
		}
		return false;
	}

	// Rear cars: chase the ordered vehicle target. Returns true while chasing.
	bool UpdateChase(Car& car)
	{
		if (car.lead || s_attackVeh == 0)
		{
			if (car.chasing)
			{
				car.chasing = false;
				car.chaseEntity = 0;
				car.tasked = false;
			}
			return false;
		}
		// Chase the driver while he is at the wheel; the vehicle itself once he is dead or bailed out.
		Ped targetDriver = VEHICLE::GET_PED_IN_VEHICLE_SEAT(s_attackVeh, -1);
		Entity want = (IsAlive(targetDriver) && PED::IS_PED_IN_VEHICLE(targetDriver, s_attackVeh, FALSE))
			? (Entity)targetDriver : (Entity)s_attackVeh;
		if (!car.chasing || car.chaseTarget != s_attackVeh || car.chaseEntity != want)
		{
			Ped driver = car.crew[0];
			PED::SET_DRIVER_ABILITY(driver, 1.0f);
			PED::SET_DRIVER_AGGRESSIVENESS(driver, 1.0f);
			AI::TASK_VEHICLE_CHASE(driver, want);
			AI::SET_TASK_VEHICLE_CHASE_BEHAVIOR_FLAG(driver, 1, TRUE);
			AI::SET_TASK_VEHICLE_CHASE_IDEAL_PURSUIT_DISTANCE(driver, 15.0f);
			car.chasing = true;
			car.chaseTarget = s_attackVeh;
			car.chaseEntity = want;
			car.tasked = false;
			car.taskKind = TASK_NONE;
			car.parked = false;
			car.arriving = false;
		}
		return true;
	}

	// Hurry: On, or Auto while the chauffeur is hurrying / the player is driving fast.
	bool Hurrying()
	{
		if (s_hurryMode == HURRY_ON)
			return true;
		if (s_hurryMode == HURRY_OFF || s_centre == 0 || !ENTITY::DOES_ENTITY_EXIST(s_centre))
			return false;
		if (s_centre == Chauffeur::Car())
			return Chauffeur::IsHurrying();
		return s_autoFast;
	}

	float Ceiling(bool hurry) { return hurry ? CEILING_HURRY : CEILING; }

	// Auto hurry: sample the centre's speed (with hysteresis).
	void SampleCentreSpeed()
	{
		if (s_centre == 0 || !ENTITY::DOES_ENTITY_EXIST(s_centre))
			return;
		float speed = ENTITY::GET_ENTITY_SPEED(s_centre);
		if (speed > AUTO_HURRY_SPEED)
			s_autoFast = true;
		else if (speed < AUTO_CALM_SPEED)
			s_autoFast = false;
	}

	// Every motorcade driver: skilled, and only fully aggressive when hurrying (so they don't ram).
	void PrepDriver(Ped driver, bool hurry)
	{
		PED::SET_DRIVER_ABILITY(driver, 1.0f);
		PED::SET_DRIVER_AGGRESSIVENESS(driver, hurry ? 1.0f : 0.9f);
	}

	void SetCruise(Car& car, Ped driver, float cruise, float ceiling)
	{
		cruise = std::clamp(cruise, 0.0f, ceiling);
		if (std::fabs(cruise - car.lastCruise) < 0.25f)
			return;
		AI::SET_DRIVE_TASK_CRUISE_SPEED(driver, cruise);
		car.lastCruise = cruise;
	}

	// Barely moving for a few seconds while it should be driving -> re-issue the task. Not while the car it
	// paces itself by is stopped (red lights): the whole chain is supposed to wait.
	void StuckCheck(Car& car, bool shouldMove)
	{
		if (shouldMove && car.tasked && ENTITY::GET_ENTITY_SPEED(car.veh) < 1.0f)
		{
			if (car.stuckSince == 0)
				car.stuckSince = Now();
			else if (Elapsed(car.stuckSince, STUCK_MS))
			{
				car.tasked = false;
				car.stuckSince = 0;
			}
		}
		else
		{
			car.stuckSince = 0;
		}
	}

	// Follow `link` (centre or the car ahead in the chain) with escort-behind, cruise speed retuned every pass
	// from the gap. `pace` is the car whose speed the chain moves at (the centre, or the route head for leads).
	// A car behind its slot always gets a big margin over the pace speed (a car told to drive at exactly that
	// speed can never close a gap) plus extra engine power; only a car that is too close is slowed.
	void FollowChain(Car& car, Ped driver, Vehicle link, Vehicle pace, float minDist)
	{
		bool hurry = Hurrying();
		float ceiling = Ceiling(hurry);
		if (!car.tasked || car.taskKind != TASK_ESCORT || car.taskTarget != link || car.taskHurry != hurry)
		{
			PrepDriver(driver, hurry);
			AI::TASK_VEHICLE_ESCORT(driver, car.veh, link, Vehicles::ESCORT_BEHIND, ceiling, Vehicles::STYLE_RUSHED, minDist, 0, 20.0f);
			AI::SET_DRIVE_TASK_MAX_CRUISE_SPEED(driver, ceiling);
			SetTask(car, TASK_ESCORT, link, hurry);
			return;
		}
		if (!Elapsed(car.taskIssuedAt, CHECK_MS))
			return;

		Vector3 carPos = ENTITY::GET_ENTITY_COORDS(car.veh, TRUE);
		float cs = ENTITY::GET_ENTITY_SPEED(pace);
		float along = Vehicles::AlongTrack(link, carPos);   // negative: behind the link
		float lateral = Vehicles::OffTrack(link, carPos);
		StuckCheck(car, cs >= STOPPED_SPEED && EntityDist(car.veh, link) > STUCK_LINK_DIST);

		float boost = hurry ? BOOST_HURRY : BOOST;
		float catchUp = std::max(cs + boost, MIN_CATCHUP_SPEED);
		float gapErr = -GAP - along; // > 0: needs to move up, < 0: needs to drop back
		bool behind = gapErr > GAP_BAND || lateral > LATERAL_MAX;
		bool inPosition = std::fabs(gapErr) <= GAP_BAND && lateral <= LATERAL_MAX;

		if (cs < STOPPED_SPEED)
		{
			// Pace car stopped: the escort task stops the chain; only let a car still closing the gap creep up.
			if (gapErr > GAP_BAND)
				SetCruise(car, driver, STOPPED_CREEP, ceiling);
			SetBoost(car, false);
			return;
		}
		if (inPosition)
		{
			float proportional = std::clamp(cs + GAP_GAIN * gapErr, std::max(cs - REAR_EASE, 0.0f), cs + boost);
			SetCruise(car, driver, proportional, ceiling);
			SetBoost(car, false);
		}
		else if (behind)
		{
			SetCruise(car, driver, catchUp, ceiling);
			SetBoost(car, true);
		}
		else
		{
			SetCruise(car, driver, std::max(cs - REAR_EASE, 0.0f), ceiling); // too close
			SetBoost(car, false);
		}
	}

	// Front lead: drives the route to the destination; the governor sets its cruise speed (no re-tasking).
	// Convoy: obeys traffic like the chauffeur unless hurrying. Own car: rushed, like the player usually drives.
	void DriveRoute(Car& car, Ped driver)
	{
		bool hurry = Hurrying();
		float ceiling = Ceiling(hurry);
		float dx = s_dest.x - car.destX, dy = s_dest.y - car.destY;
		bool destMoved = dx * dx + dy * dy > DEST_MOVED * DEST_MOVED;
		if (!car.tasked || car.taskKind != TASK_ROUTE || car.taskHurry != hurry || destMoved)
		{
			PrepDriver(driver, hurry);
			int style = (hurry || s_mode == MODE_LEAD_WP) ? Vehicles::STYLE_RUSHED : Vehicles::STYLE_NORMAL;
			AI::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(driver, car.veh, s_dest.x, s_dest.y, s_dest.z, ceiling, style, 10.0f);
			AI::SET_DRIVE_TASK_MAX_CRUISE_SPEED(driver, ceiling);
			SetTask(car, TASK_ROUTE, 0, hurry);
			car.destX = s_dest.x;
			car.destY = s_dest.y;
			car.destZ = s_dest.z;
			return;
		}
		if (!Elapsed(car.taskIssuedAt, CHECK_MS))
			return;
		bool centreMoving = ENTITY::GET_ENTITY_SPEED(s_centre) >= STOPPED_SPEED;
		StuckCheck(car, centreMoving && s_govCruise >= ROUTE_MOVING);
		SetCruise(car, driver, s_govCruise, ceiling);
		SetBoost(car, s_leadCatchingUp);
	}

	// Lead near the destination: park at the curb ahead of it, slot (index + 1) car lengths along the lead's
	// heading (the chauffeur / player pulls in behind). Brakes when reached or stalled; Parked mode tidies up later.
	void UpdateArrival(Car& car, Ped driver, size_t index)
	{
		if (!car.tasked || car.taskKind != TASK_ARRIVE)
		{
			if (!car.arriving)
			{
				Vector3 fwd = ENTITY::GET_ENTITY_FORWARD_VECTOR(car.veh);
				float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y);
				float fx = len > 0.01f ? fwd.x / len : 0.0f;
				float fy = len > 0.01f ? fwd.y / len : 1.0f;
				float ahead = PARK_GAP * (float)(index + 1);
				Vector3 slot;
				slot.x = s_dest.x + fx * ahead;
				slot.y = s_dest.y + fy * ahead;
				slot.z = s_dest.z;
				Vector3 curb;
				float curbHeading;
				if (Vehicles::CurbPoint(slot, ENTITY::GET_ENTITY_HEADING(car.veh), curb, curbHeading) && Dist2D(curb, slot) < PARK_CURB_SNAP)
					slot = curb;
				car.parkPos = slot;
				car.arriving = true;
				car.destX = s_dest.x;
				car.destY = s_dest.y;
				car.destZ = s_dest.z;
			}
			PrepDriver(driver, false);
			Vehicles::DriveToSpot(driver, car.veh, car.parkPos, ARRIVE_SPEED);
			SetTask(car, TASK_ARRIVE, 0, Hurrying());
			car.parkApproach = true;
			car.parkSince = Now();
			SetBoost(car, false);
			return;
		}
		if (car.parkApproach)
		{
			float d = Dist2D(ENTITY::GET_ENTITY_COORDS(car.veh, TRUE), car.parkPos);
			bool stalled = ENTITY::GET_ENTITY_SPEED(car.veh) < 0.3f && Elapsed(car.parkSince, PARK_STALL_MS);
			if (d < PARK_REACHED || stalled || Elapsed(car.parkSince, PARK_GIVEUP_MS))
			{
				AI::CLEAR_PED_TASKS(driver);
				AI::TASK_VEHICLE_TEMP_ACTION(driver, car.veh, 27, 5000); // brake until stopped, then wait
				car.parkApproach = false;
			}
		}
		VEHICLE::SET_VEHICLE_ENGINE_ON(car.veh, TRUE, TRUE, FALSE);
	}

	// Driving roles. `link` is the car to follow; 0 for the route head. `routing` = this lead drives / escorts
	// toward the destination (Convoy / Lead to waypoint) rather than chaining behind the centre.
	void UpdateDriving(Car& car, size_t index, Vehicle link, bool routing)
	{
		bool driverReady = EnsureDriver(car);
		bool boarded = Board(car, 1);
		if (!driverReady || !boarded)
			return;
		if (UpdateChase(car) || s_centre == 0 || !ENTITY::DOES_ENTITY_EXIST(s_centre))
			return;

		Ped driver = car.crew[0];
		Vector3 carPos = ENTITY::GET_ENTITY_COORDS(car.veh, TRUE);
		Vector3 centrePos = ENTITY::GET_ENTITY_COORDS(s_centre, TRUE);

		// Catch-up teleport: only a car that is actually behind, and only while nobody can see it.
		if (g_cfg.catchUpTeleport && Dist(carPos, centrePos) > g_cfg.catchUpDistance
			&& Elapsed(car.lastTeleport, TELEPORT_COOLDOWN_MS) && !ENTITY::IS_ENTITY_ON_SCREEN(car.veh)
			&& Vehicles::AlongTrack(s_centre, carPos) < 0.0f && !car.arriving)
		{
			car.lastTeleport = Now();
			if (CatchUpTeleport(car, index))
			{
				ResetCarTask(car);
				SetBoost(car, false);
				return;
			}
		}

		if (routing)
		{
			// New destination: a lead parked at the old one drives again.
			float dx = s_dest.x - car.destX, dy = s_dest.y - car.destY;
			if (car.arriving && dx * dx + dy * dy > DEST_MOVED * DEST_MOVED)
			{
				car.arriving = false;
				car.parkApproach = false;
				car.tasked = false;
			}
			if (car.arriving || Dist2D(carPos, s_dest) < ARRIVE_RADIUS)
			{
				UpdateArrival(car, driver, index);
				return;
			}
			if (link == 0)
				DriveRoute(car, driver);
			else
				FollowChain(car, driver, link, s_leads.back().veh, LEAD_MIN_DIST);
			return;
		}
		FollowChain(car, driver, link, s_centre, FOLLOW_MIN_DIST);
	}

	// Formation slot at the curb: leads bumper-to-bumper in front of the parked centre, rears behind it.
	// Returns false (just brake where it is) when there is no usable slot or the car is already there.
	bool StartFormationPark(Car& car, Ped driver, size_t index)
	{
		if (s_centre == 0 || !ENTITY::DOES_ENTITY_EXIST(s_centre))
			return false;
		float offset = (car.lead ? 1.0f : -1.0f) * PARK_GAP * (float)(index + 1);
		Vector3 slot = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(s_centre, 0.0f, offset, 0.0f);
		Vector3 curb;
		float curbHeading;
		// Follow the curb line on bends; fall back to the centre's own line (it is already at the curb).
		if (Vehicles::CurbPoint(slot, ENTITY::GET_ENTITY_HEADING(s_centre), curb, curbHeading) && Dist(curb, slot) < 4.0f)
			slot = curb;
		float d = Dist(ENTITY::GET_ENTITY_COORDS(car.veh, TRUE), slot);
		if (d < PARK_REACHED || d > 150.0f)
			return false;
		Vehicles::DriveToSpot(driver, car.veh, slot, 6.0f);
		car.parkPos = slot;
		return true;
	}

	void Brake(Car& car, Ped driver)
	{
		AI::CLEAR_PED_TASKS(driver);
		AI::TASK_VEHICLE_TEMP_ACTION(driver, car.veh, 27, 5000); // brake until stopped, then wait
	}

	// Crew fighting, or the player's attack target close by.
	bool CrewHostile(const Car& car)
	{
		for (Ped p : car.crew)
		{
			Guard* g = Guards::Find(p);
			if (g && g->target != 0 && IsAlive(g->target) && EntityDist(g->target, car.veh) < HOSTILE_DIST)
				return true;
		}
		Ped t = Combat::TargetPed();
		return IsAlive(t) && EntityDist(t, car.veh) < HOSTILE_DIST;
	}

	// Parked (player on foot, or the chauffeur parked the centre): drive into formation at the curb and wait.
	// Passengers stay inside unless there are hostiles, or "Get out when you stop" is on and the player is on foot.
	// Guard detail: cars on guard post keep their crews inside and stay put; the others' crews get out and walk
	// with the player, their cars only drive closer when he is far away.
	void UpdateParked(Car& car, Ped player, size_t index)
	{
		if (!EnsureDriver(car))
			return;
		if (UpdateChase(car))
			return;

		Ped driver = car.crew[0];
		if (!car.parked)
		{
			car.parked = true;
			car.tasked = false;
			car.taskKind = TASK_NONE;
			car.parkSince = Now();
			car.parkApproach = StartFormationPark(car, driver, index);
			if (!car.parkApproach)
				Brake(car, driver);
			SetBoost(car, false);
		}
		else if (car.parkApproach)
		{
			float d = Dist(ENTITY::GET_ENTITY_COORDS(car.veh, TRUE), car.parkPos);
			bool stalled = ENTITY::GET_ENTITY_SPEED(car.veh) < 0.3f && Elapsed(car.parkSince, PARK_STALL_MS);
			if (d < PARK_REACHED || stalled || Elapsed(car.parkSince, PARK_GIVEUP_MS))
			{
				Brake(car, driver);
				car.parkApproach = false;
			}
		}
		VEHICLE::SET_VEHICLE_ENGINE_ON(car.veh, TRUE, TRUE, FALSE);

		bool guardMode = s_mode == MODE_GUARD;
		bool playerOnFoot = PED::IS_PED_ON_FOOT(player) != FALSE; // not while the player flies / sails off
		float playerDist = EntityDist(car.veh, player);
		bool slow = ENTITY::GET_ENTITY_SPEED(car.veh) < 3.0f;
		if (CrewHostile(car))
			car.lastHostile = Now();
		bool threat = car.lastHostile != 0 && !Elapsed(car.lastHostile, CALM_MS);
		bool wantOut = threat
			|| (!guardMode && s_getOut && s_onFoot && playerOnFoot && playerDist <= DISMOUNT_DIST)
			|| (guardMode && !car.guardPost && playerOnFoot);

		if (wantOut && slow && (!car.dismounted || Elapsed(car.lastLeaveTask, ENTER_REISSUE_MS)))
		{
			// Out they go: straight into the fight if they have a target, else out of the car (perimeter / look around).
			bool issued = false;
			for (size_t i = 1; i < car.crew.size(); i++)
			{
				Ped p = car.crew[i];
				Guard* g = Guards::Find(p);
				if (!g || !PED::IS_PED_IN_VEHICLE(p, car.veh, FALSE))
					continue;
				PED::SET_PED_COMBAT_ATTRIBUTES(p, CA_LEAVE_VEHICLES, TRUE);
				if (g->target != 0 && IsAlive(g->target))
					AI::TASK_COMBAT_PED(p, g->target, 0, 16);
				else if (threat)
					AI::TASK_COMBAT_HATED_TARGETS_AROUND_PED(p, HOSTILE_DIST, 0);
				else
					AI::TASK_LEAVE_VEHICLE(p, car.veh, 0);
				issued = true;
			}
			if (!car.dismounted)
				s_perimeterDirty = true;
			car.dismounted = true;
			if (issued)
				car.lastLeaveTask = Now();
		}
		else if (!wantOut)
		{
			// Calm (or the toggle is off): everyone back in the car.
			if (car.dismounted)
			{
				car.dismounted = false;
				car.forceEnter = true;
				car.boardSince = 0;
				s_perimeterDirty = true;
			}
			Board(car, 1);
		}

		// Drive closer when the player walked far away on foot (never a car on guard post).
		bool mayRejoin = guardMode ? !car.guardPost : !car.dismounted;
		if (s_onFoot && playerOnFoot && playerDist > REJOIN_DIST && mayRejoin && Elapsed(car.lastRejoin, REJOIN_DRIVE_MS))
		{
			Vector3 p = ENTITY::GET_ENTITY_COORDS(player, TRUE);
			AI::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(driver, car.veh, p.x, p.y, p.z, 20.0f, Vehicles::STYLE_NORMAL, 20.0f);
			car.lastRejoin = Now();
			car.parkApproach = false;
		}
	}

	// Motorcade car the player rides in: it is the centre. Crew boards; the driver takes the player to the
	// waypoint if one is set, otherwise holds. If the player drives it himself, the whole crew rides along.
	void UpdateVip(Car& car, Ped player)
	{
		if (!Elapsed(car.lastCheck, CHECK_MS))
			return;
		car.lastCheck = Now();
		HandleRetask(car);

		if (VEHICLE::GET_PED_IN_VEHICLE_SEAT(car.veh, -1) == player)
		{
			SetDriving(car.crew[0], false);
			Board(car, 0);
			return;
		}
		bool driverReady = EnsureDriver(car);
		bool boarded = Board(car, 1);
		if (!driverReady || !boarded || car.crew.empty())
			return;

		Ped driver = car.crew[0];
		Vector3 wp;
		if (Vehicles::GetWaypoint(wp))
		{
			float dx = wp.x - car.destX, dy = wp.y - car.destY;
			if (!car.tasked || car.parked || dx * dx + dy * dy > 25.0f)
			{
				PrepDriver(driver, false);
				AI::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(driver, car.veh, wp.x, wp.y, wp.z,
					std::min(VIP_SPEED, g_cfg.escortSpeed), g_cfg.drivingStyle, 15.0f);
				car.destX = wp.x;
				car.destY = wp.y;
				car.destZ = wp.z;
				car.tasked = true;
				car.taskKind = TASK_ROUTE;
				car.parked = false;
			}
		}
		else if (!car.parked)
		{
			Brake(car, driver);
			car.parked = true;
			car.tasked = false;
			car.taskKind = TASK_NONE;
		}
		VEHICLE::SET_VEHICLE_ENGINE_ON(car.veh, TRUE, TRUE, FALSE);
	}

	// Player climbed into a motorcade car: it leaves its chain and becomes the centre.
	void TakeVip(Vehicle v)
	{
		for (auto* chain : { &s_leads, &s_rears })
			for (size_t i = 0; i < chain->size(); i++)
			{
				if ((*chain)[i].veh != v)
					continue;
				Car car = (*chain)[i];
				chain->erase(chain->begin() + i);
				ResetCarRole(car);
				SetBoost(car, false);
				car.dismounted = false;
				car.forceEnter = true;
				car.boardSince = 0;
				s_vip.clear();
				s_vip.push_back(car);
				return;
			}
	}

	// Player left the VIP car: it goes back to the end of its chain.
	void ReturnVip()
	{
		Car car = s_vip[0];
		s_vip.clear();
		ResetCarRole(car);
		car.dismounted = false;
		(car.lead ? s_leads : s_rears).push_back(car);
		if (s_centre == car.veh)
			s_centre = 0;
		Motorcade::RetaskAll();
	}

	// Dismounted passengers form a ring around the player.
	void UpdatePerimeter(Ped player)
	{
		if (!PED::IS_PED_ON_FOOT(player))
			return;
		if (!s_perimeterDirty && !Elapsed(s_lastPerimeter, 1000))
			return;
		s_lastPerimeter = Now();
		bool farCheck = Elapsed(s_lastFarCheck, PERIMETER_MS);
		if (farCheck)
			s_lastFarCheck = Now();

		std::vector<Ped> ring;
		for (auto* chain : { &s_leads, &s_rears })
			for (auto& car : *chain)
				if (car.dismounted)
					for (size_t i = 1; i < car.crew.size(); i++)
						ring.push_back(car.crew[i]);

		// Slots shift when the ring changes size: re-issue everyone.
		if (s_perimeterDirty || ring.size() != s_ringSize)
			s_ringTasked.clear();
		s_perimeterDirty = false;
		s_ringSize = ring.size();

		for (size_t k = 0; k < ring.size(); k++)
		{
			Ped p = ring[k];
			Guard* g = Guards::Find(p);
			if (!g || !PED::IS_PED_ON_FOOT(p))
				continue;
			if (g->target != 0)
			{
				// In combat: re-slot as soon as Combat stands him down.
				s_ringTasked.erase(std::remove(s_ringTasked.begin(), s_ringTasked.end(), p), s_ringTasked.end());
				continue;
			}
			float angle = TWO_PI * (float)k / (float)ring.size();
			float ox = PERIMETER_RADIUS * std::cos(angle);
			float oy = PERIMETER_RADIUS * std::sin(angle);
			bool tasked = std::find(s_ringTasked.begin(), s_ringTasked.end(), p) != s_ringTasked.end();
			bool idleFar = false;
			if (tasked && farCheck)
			{
				Vector3 slot = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(player, ox, oy, 0.0f);
				idleFar = Dist(ENTITY::GET_ENTITY_COORDS(p, TRUE), slot) > SLOT_FAR && ENTITY::GET_ENTITY_SPEED(p) < 0.5f;
			}
			if (!tasked || idleFar)
			{
				AI::TASK_FOLLOW_TO_OFFSET_OF_ENTITY(p, player, ox, oy, 0.0f, 2.0f, -1, 1.5f, TRUE);
				if (!tasked)
					s_ringTasked.push_back(p);
			}
		}
	}

	void PollAttack()
	{
		if (!Elapsed(s_lastAttackPoll, ATTACK_POLL_MS))
			return;
		s_lastAttackPoll = Now();
		Vehicle v = Combat::TargetVehicle();
		s_attackVeh = IsAlive(v) ? v : 0;
	}

	// True when the player is more than LOST_DIST from every motorcade car (e.g. after a respawn).
	bool PlayerLost(Ped player)
	{
		if (!Elapsed(s_lastRangeCheck, RANGE_CHECK_MS))
			return false;
		s_lastRangeCheck = Now();
		// While driving without catch-up teleport a fast player may legitimately leave the cars behind.
		if (!s_onFoot && s_vip.empty() && !g_cfg.catchUpTeleport)
			return false;
		for (auto* chain : { &s_leads, &s_rears, &s_vip })
			for (auto& c : *chain)
				if (EntityDist(c.veh, player) <= LOST_DIST)
					return false;
		return true;
	}

	// ---- Mode machine ----

	// Remembers the destination the leads drive to. `groundZ`: a map waypoint (z is 0) gets the ground height.
	void SetDest(const Vector3& d, bool groundZ)
	{
		Vector3 p;
		p.x = d.x;
		p.y = d.y;
		p.z = d.z;
		if (groundZ)
		{
			float z;
			if (Vehicles::GroundZ(p.x, p.y, z))
				p.z = z;
			else if (s_centre != 0 && ENTITY::DOES_ENTITY_EXIST(s_centre))
				p.z = ENTITY::GET_ENTITY_COORDS(s_centre, TRUE).z;
		}
		if (!s_lastDestValid || Dist2D(p, s_lastDest) > DEST_MOVED)
		{
			// New trip.
			s_destReached = false;
			s_destPassed = false;
			s_lastDest = p;
			s_lastDestValid = true;
		}
		s_dest = p;
		s_haveDest = true;
	}

	// Own car: the player reached his waypoint and drove on without removing it -> stop leading him back to it.
	void TrackDestPassed()
	{
		if (s_centre == 0 || !ENTITY::DOES_ENTITY_EXIST(s_centre))
			return;
		float d = Dist2D(ENTITY::GET_ENTITY_COORDS(s_centre, TRUE), s_dest);
		if (d < ARRIVE_RADIUS)
			s_destReached = true;
		if (s_destReached && d > DEST_PASSED)
			s_destPassed = true;
	}

	int DecideMode(Ped player, Vehicle pv, Vehicle driverCar)
	{
		bool centreExists = s_centre != 0 && ENTITY::DOES_ENTITY_EXIST(s_centre);
		bool centreStill = centreExists && ENTITY::GET_ENTITY_SPEED(s_centre) < 0.5f;

		if (pv != 0)
		{
			s_guardFar = false;
			if (pv == driverCar)
			{
				// Riding with the chauffeur. The destination is remembered while he is still rolling (curb
				// approach), so leads already parking at it keep their slots until he has parked too.
				Vector3 dest;
				if (Chauffeur::HasDestination(dest))
					SetDest(dest, false);
				else if (s_mode != MODE_CONVOY)
					s_haveDest = false; // only a Convoy trip's destination may be remembered here
				if (Chauffeur::IsParked() && centreStill)
					return MODE_PARKED;
				return s_haveDest ? MODE_CONVOY : MODE_FOLLOW;
			}
			Vector3 wp;
			if (!s_vip.empty() && pv == s_vip[0].veh && VEHICLE::GET_PED_IN_VEHICLE_SEAT(pv, -1) != player)
			{
				// Crew-driven VIP car: it drives the waypoint itself (UpdateVip); leads route ahead of it.
				if (Vehicles::GetWaypoint(wp))
				{
					SetDest(wp, true);
					return MODE_CONVOY;
				}
				return centreStill ? MODE_PARKED : MODE_FOLLOW;
			}
			// Own car (or the player at the wheel of a motorcade car).
			if (s_driveFormation == LEAD_TO_WAYPOINT && !s_leads.empty() && Vehicles::GetWaypoint(wp))
			{
				SetDest(wp, true);
				TrackDestPassed();
				if (!s_destPassed)
					return MODE_LEAD_WP;
			}
			return MODE_FOLLOW;
		}

		// Not in a land vehicle of his own: grace period before "on foot", or riding in a dismissed chauffeur car.
		if (!s_onFoot)
			return s_mode == MODE_NONE ? MODE_FOLLOW : s_mode;

		if (IsAlive(driverCar) && s_centre == driverCar)
			return (Chauffeur::IsParked() && centreStill) ? MODE_PARKED : MODE_PICKUP;

		// On foot near his own parked car: walk off and part of the motorcade stays to guard it.
		if (centreExists && PED::IS_PED_ON_FOOT(player))
		{
			float d = EntityDist(player, s_centre);
			if (d > GUARD_WALK_DIST)
				s_guardFar = true;
			else if (d < GUARD_RETURN_DIST)
				s_guardFar = false;
			if (s_guardFar)
				return MODE_GUARD;
		}
		else
		{
			s_guardFar = false;
		}
		return MODE_PARKED;
	}

	// Guard detail: min(GUARD_CARS, cars - 1) cars stay with the parked car, rear cars first.
	void AssignGuardPosts()
	{
		int guards = s_mode == MODE_GUARD ? std::max(0, std::min(GUARD_CARS, CountCars() - 1)) : 0;
		s_guardCars = guards;
		int k = 0;
		for (auto* chain : { &s_rears, &s_leads })
			for (auto& car : *chain)
				car.guardPost = k++ < guards;
	}

	void EnterMode(int mode)
	{
		s_mode = mode;
		for (auto* chain : { &s_leads, &s_rears })
			for (auto& car : *chain)
				ResetCarRole(car);
		if (mode != MODE_CONVOY && mode != MODE_LEAD_WP)
			s_haveDest = false; // a stale destination must never pull the leads back to it
		s_lastGovern = 0;
		s_leadBehindSince = 0;
		s_leadCatchingUp = false;
		AssignGuardPosts();
		if (s_onFoot)
			s_perimeterDirty = true;
	}

	// Gap governor (every GOVERN_MS): cruise speed of the route head from the along-track gap between the centre
	// and the rearmost lead, so the lead column never stops dead in the road while the centre is moving, and
	// never runs away from it. A lead that fell behind (traffic) gets catch-up speed and nobody follows it.
	void UpdateGovernor()
	{
		if (s_leads.empty() || s_centre == 0 || !ENTITY::DOES_ENTITY_EXIST(s_centre))
			return;
		if (!Elapsed(s_lastGovern, GOVERN_MS))
			return;
		s_lastGovern = Now();

		float cs = ENTITY::GET_ENTITY_SPEED(s_centre);
		bool centreStopped = cs < STOPPED_SPEED;
		float desired = (s_mode == MODE_CONVOY && s_centre == Chauffeur::Car()) ? Chauffeur::DesiredSpeed() : cs;
		float along = Vehicles::AlongTrack(s_centre, ENTITY::GET_ENTITY_COORDS(s_leads[0].veh, TRUE));

		if (along < LEAD_BEHIND)
		{
			if (s_leadBehindSince == 0)
				s_leadBehindSince = Now();
			else if (Elapsed(s_leadBehindSince, LEAD_BEHIND_MS))
				s_leadCatchingUp = true;
		}
		else
		{
			s_leadBehindSince = 0;
			if (along >= LEAD_AHEAD_AGAIN)
				s_leadCatchingUp = false;
		}

		if (s_leadCatchingUp)
			s_govCruise = desired + CATCHUP_MARGIN;
		else if (along > GOV_CRAWL)
			s_govCruise = centreStopped ? 0.0f : GOV_CRAWL_SPEED;
		else if (along > GOV_FAR)
			s_govCruise = std::max(desired - GOV_EASE, GOV_MIN);
		else if (along < GOV_NEAR)
			s_govCruise = desired + GOV_SPEEDUP;
		else
			s_govCruise = desired;
	}

	// Convoy: the chauffeur follows the rearmost lead while it is ahead of him, has its driver at the wheel and
	// is not parking at the destination; otherwise (and in every other mode) he drives the route himself.
	void WireChauffeur()
	{
		Vehicle want = 0;
		if (s_mode == MODE_CONVOY && s_centre == Chauffeur::Car() && !s_leads.empty() && !s_leadCatchingUp)
		{
			const Car& lead = s_leads[0];
			if (IsAlive(lead.veh) && !lead.arriving && !lead.crew.empty()
				&& VEHICLE::GET_PED_IN_VEHICLE_SEAT(lead.veh, -1) == lead.crew[0])
			{
				Vector3 leadPos = ENTITY::GET_ENTITY_COORDS(lead.veh, TRUE);
				if (Vehicles::AlongTrack(s_centre, leadPos) > LEAD_FOLLOW_MIN && Vehicles::OffTrack(s_centre, leadPos) < LEAD_FOLLOW_LATERAL)
					want = lead.veh;
			}
		}
		if (Chauffeur::LeadVehicle() == want)
			return;
		// He clears the lead himself near the destination: don't fight him about it every frame.
		if (want != 0 && !Elapsed(s_lastLeadSend, GOVERN_MS))
			return;
		Chauffeur::SetLeadVehicle(want);
		s_lastLeadSend = Now();
	}

	// Gives every car its role for the current mode.
	void UpdateCars(Ped player)
	{
		bool parkedMode = s_mode == MODE_PARKED || s_mode == MODE_GUARD;
		bool routing = (s_mode == MODE_CONVOY || s_mode == MODE_LEAD_WP) && s_haveDest && !s_leads.empty();

		// Leads: routing -> s_leads[i] escorts s_leads[i + 1], the front car drives the route.
		// Otherwise they head one chain behind the centre, continued by the rear cars.
		Vehicle link = s_centre;
		for (size_t i = 0; i < s_leads.size(); i++)
		{
			Car& car = s_leads[i];
			Vehicle myLink;
			if (routing)
				myLink = i + 1 < s_leads.size() ? s_leads[i + 1].veh : 0;
			else
			{
				myLink = link;
				link = car.veh;
			}
			if (!Elapsed(car.lastCheck, CHECK_MS))
				continue;
			car.lastCheck = Now();
			HandleRetask(car);
			if (parkedMode)
				UpdateParked(car, player, i);
			else
				UpdateDriving(car, i, myLink, routing);
		}
		if (routing)
			link = s_centre;
		for (size_t i = 0; i < s_rears.size(); i++)
		{
			Car& car = s_rears[i];
			Vehicle myLink = link;
			link = car.veh;
			if (!Elapsed(car.lastCheck, CHECK_MS))
				continue;
			car.lastCheck = Now();
			HandleRetask(car);
			if (parkedMode)
				UpdateParked(car, player, i);
			else
				UpdateDriving(car, i, myLink, false);
		}
	}
}

namespace Motorcade
{
	void LoadConfig()
	{
		s_leadCount = std::clamp(ReadInt("Motorcade", "LeadCars", 1), 0, 2);
		s_rearCount = std::clamp(ReadInt("Motorcade", "RearCars", 1), 0, 2);
		s_perCar = std::clamp(ReadInt("Motorcade", "GuardsPerCar", 3), 2, 4);
		s_carModel = FindIndex(g_escortModels, ReadString("Motorcade", "CarModel", "granger"), 0);
		std::string hurry = Upper(ReadString("Motorcade", "Hurry", "Auto"));
		s_hurryMode = hurry == "ON" ? HURRY_ON : hurry == "OFF" ? HURRY_OFF : HURRY_AUTO;
		std::string formation = Upper(ReadString("Motorcade", "DriveFormation", "LeadToWaypoint"));
		s_driveFormation = formation == "FOLLOWBEHIND" ? FOLLOW_BEHIND : LEAD_TO_WAYPOINT;
		s_getOut = ReadBool("Motorcade", "GetOut", false);
		s_crewModel = g_cfg.modelIndex;
		std::string crew = ReadString("Motorcade", "CrewModel", "");
		if (!crew.empty())
			s_crewModel = FindIndex(g_guardModels, crew, s_crewModel);
	}

	void Update()
	{
		if (!IsActive())
			return;

		PruneChain(s_leads);
		PruneChain(s_rears);
		PruneChain(s_vip);
		if (!IsActive())
		{
			Disband();
			Notify("~b~Motorcade:~s~ lost.");
			return;
		}

		Ped player = PLAYER::PLAYER_PED_ID();
		if (PlayerLost(player))
		{
			Disband();
			Notify("~b~Motorcade:~s~ too far away, disbanded.");
			return;
		}

		PollAttack();

		Vehicle inside = PED::IS_PED_IN_ANY_VEHICLE(player, FALSE) ? PED::GET_VEHICLE_PED_IS_IN(player, FALSE) : 0;
		if (!s_vip.empty() && s_vip[0].veh != inside)
			ReturnVip();
		if (s_vip.empty() && inside != 0 && IsMotorcadeCar(inside))
			TakeVip(inside);

		Vehicle pv = !s_vip.empty() ? s_vip[0].veh : CurrentLandVehicle(player);
		bool keepMode = pv == 0 && inside != 0 && Chauffeur::OwnsVehicle(inside) && inside != Chauffeur::Car();

		if (pv != 0)
		{
			s_outSince = 0;
			if (pv != s_centre)
			{
				s_centre = pv;
				RetaskAll();
			}
			if (s_onFoot)
			{
				s_onFoot = false;
				for (auto* chain : { &s_leads, &s_rears, &s_vip })
					for (auto& car : *chain)
					{
						// Remount: crew heads back to its own car (warped in after BOARD_MS).
						car.dismounted = false;
						car.parked = false;
						car.tasked = false;
						car.forceEnter = true;
						car.boardSince = 0;
						car.lastCheck = 0;
					}
				s_ringTasked.clear();
				s_ringSize = 0;
			}
		}
		else if (keepMode)
		{
			s_outSince = 0; // riding in a dismissed chauffeur car: keep the current mode
		}
		else if (!s_onFoot)
		{
			if (s_outSince == 0)
				s_outSince = Now();
			if (Elapsed(s_outSince, EXIT_MS))
			{
				s_onFoot = true;
				s_outSince = 0;
				for (auto* chain : { &s_leads, &s_rears })
					for (auto& car : *chain)
					{
						car.parked = false;
						car.dismounted = false;
						car.lastCheck = 0;
					}
			}
		}

		// On foot with a driver: the motorcade belongs with the driver's car (escorts him while he comes to
		// pick you up, parks around him when he stops), not with the car you last drove.
		Vehicle driverCar = Chauffeur::Car();
		if (pv == 0 && !keepMode && s_onFoot && IsAlive(driverCar) && driverCar != s_centre
			&& (!Chauffeur::IsParked() || EntityDist(player, driverCar) < 40.0f)) // not a driver parked far away: guard detail needs the own car as centre
		{
			s_centre = driverCar;
			RetaskAll();
		}

		SampleCentreSpeed();
		int mode = DecideMode(player, pv, driverCar);
		if (mode != s_mode)
			EnterMode(mode);
		else if (mode == MODE_GUARD)
			AssignGuardPosts(); // a car may have been lost
		if (mode == MODE_CONVOY || mode == MODE_LEAD_WP)
			UpdateGovernor();
		WireChauffeur();

		if (!s_vip.empty())
			UpdateVip(s_vip[0], player);
		UpdateCars(player);
		if (s_onFoot)
			UpdatePerimeter(player);
	}

	void Reset()
	{
		for (auto* chain : { &s_leads, &s_rears, &s_vip })
			for (auto& c : *chain)
			{
				Vehicles::RemoveUnitBlip(c.blip);
				Vehicles::DeleteOrRelease(c.veh);
			}
		Chauffeur::SetLeadVehicle(0);
		ClearState();
	}

	void Forget()
	{
		ClearState();
		s_autoFast = false;
		s_lastPerimeter = 0;
		s_lastFarCheck = 0;
		s_lastAttackPoll = 0;
		s_lastRangeCheck = 0;
		s_lastLeadSend = 0;
	}

	void RetaskAll()
	{
		for (auto* chain : { &s_leads, &s_rears, &s_vip })
			for (auto& car : *chain)
			{
				ResetCarTask(car);
				car.lastCheck = 0;
			}
		s_lastGovern = 0;
		s_lastAttackPoll = 0; // Regroup clears the order: re-poll before any chase is re-issued
		if (s_onFoot)
			s_perimeterDirty = true;
	}

	void RefreshBlips()
	{
		for (auto* chain : { &s_leads, &s_rears, &s_vip })
			for (auto& car : *chain)
				AddBlip(car);
	}

	void BuildMenu(std::vector<MenuItem>& items)
	{
		if (IsActive())
			items.push_back(Menu::Header("Status: " + std::to_string(CountCars()) + " cars, "
				+ std::to_string(CountGuards()) + " guards - " + ModeName()));
		else
			items.push_back(Menu::Header("Status: inactive"));

		items.push_back(Menu::List("Lead cars", s_leadCount, 3, std::to_string(s_leadCount),
			"Cars ahead of you. With your driver (or 'Lead to waypoint' in your own car) the front car drives the "
			"route to the waypoint, pacing itself to your gap, and parks at the curb when it gets there. Otherwise "
			"they chain behind you."));
		items.push_back(Menu::List("Rear cars", s_rearCount, 3, std::to_string(s_rearCount),
			"Cars following ~9 m behind your car. They speed up when they fall back and chase the vehicle "
			"you order an attack on."));
		items.push_back(Menu::List("When you drive", s_driveFormation, 2, s_formationNames[s_driveFormation],
			"Follow behind: every motorcade car chains behind you. Lead to waypoint: with a map waypoint set, a "
			"lead car drives the route ahead of you (slows when you lag, speeds up when you close in) and parks at "
			"the curb there; the rest follow you. Without a waypoint they all follow behind."));
		items.push_back(MenuItem{ "Hurry", Menu::Choice(s_hurryNames[s_hurryMode]), nullptr,
			[](int d) { s_hurryMode = Menu::Wrap(s_hurryMode + d, 3); },
			"On: the motorcade drives aggressively to keep up at high speed. Match driver: hurries whenever your "
			"driver is hurrying (or you drive fast yourself). Off: calmer driving." });
		items.push_back(Menu::Toggle("Get out when you stop", s_getOut,
			"OFF: guards stay in their cars and only get out to fight hostiles (then get back in). "
			"ON: when you get out on foot, they get out too and form a walking perimeter around you. "
			"Walk more than 30 m from your parked car and up to 2 cars stay to guard it while the other crews walk with you."));
		items.push_back(MenuItem{ "Guards per car", Menu::Choice(std::to_string(s_perCar)), nullptr,
			[](int d) { s_perCar = 2 + Menu::Wrap(s_perCar - 2 + d, 3); },
			"Crew per car, driver included. When you get out, passengers form a ring around you." });
		if (!g_escortModels.empty())
			items.push_back(Menu::List("Car model", s_carModel, (int)g_escortModels.size(), g_escortModels[s_carModel].label,
				"Vehicle used for new motorcade cars (applies on the next Form)."));
		if (!g_guardModels.empty())
			items.push_back(Menu::List("Crew model", s_crewModel, (int)g_guardModels.size(), g_guardModels[s_crewModel].label,
				"Ped model for new motorcade crews (applies on the next Form)."));
		items.push_back(Menu::Action(IsActive() ? "Re-form motorcade" : "Form motorcade", [] { Form(); },
			"Spawns the lead and rear cars around the car you are in (or your chauffeur's car). Get in a motorcade "
			"car and it becomes the protected car."));
		items.push_back(Menu::Action("Disband motorcade", [] {
			if (!IsActive())
			{
				Notify("~r~Motorcade:~s~ no motorcade.");
				return;
			}
			Disband();
			Notify("~b~Motorcade:~s~ disbanded.");
		}, "Sends every motorcade car and crew away. If you sit in one, the crew gets out and leaves it to you."));
	}

	bool IsActive() { return !s_leads.empty() || !s_rears.empty() || !s_vip.empty(); }

	const char* ModeName()
	{
		if (!IsActive())
			return "Inactive";
		switch (s_mode)
		{
		case MODE_CONVOY:
			if (s_leads.empty())
				return "Convoy: rear cars trailing";
			return s_leadCatchingUp ? "Convoy: lead car catching up" : "Convoy: lead car leading";
		case MODE_LEAD_WP:
			return s_leadCatchingUp ? "Lead to waypoint: lead car catching up" : "Lead to waypoint: lead car ahead";
		case MODE_FOLLOW:
			return "Follow behind";
		case MODE_PICKUP:
			return "Pickup: trailing your driver";
		case MODE_PARKED:
			return "Parked at the curb";
		case MODE_GUARD:
			if (s_guardCars <= 0)
				return "Guard detail: crews walking with you";
			return s_guardCars == 1 ? "Guard detail: 1 car at your car" : "Guard detail: 2 cars at your car";
		default:
			return "Forming up";
		}
	}

	bool OwnsVehicle(Vehicle v) { return IsMotorcadeCar(v); }

	Vehicle Centre() { return IsActive() ? s_centre : 0; }
}
