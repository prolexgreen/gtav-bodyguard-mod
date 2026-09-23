#include "heli.h"
#include "guards.h"
#include "vehicles.h"
#include "config.h"
#include "combat.h"

#include <algorithm>

namespace
{
	constexpr DWORD POLL_MS = 500;              // guard heli logic / target polling
	constexpr DWORD TRANSPORT_POLL_MS = 250;
	constexpr DWORD REFRESH_MS = 10000;         // periodic task re-issue
	constexpr DWORD WAYPOINT_POLL_MS = 1000;
	constexpr DWORD CATCH_UP_COOLDOWN_MS = 5000;
	constexpr DWORD ROPE_TIMEOUT_MS = 25000;    // rappel ends this long after the ropes drop
	constexpr DWORD ROPE_RETRY_MS = 6000;       // re-issue the rappel task to anyone still seated
	constexpr DWORD APPROACH_TIMEOUT_MS = 45000;
	constexpr DWORD LEAVE_TIMEOUT_MS = 40000;
	constexpr DWORD LAND_TIMEOUT_MS = 30000;    // time near the landing spot without touching down
	constexpr DWORD LAND_HARD_TIMEOUT_MS = 90000; // total time on one landing spot
	constexpr DWORD RAPPELLER_STALE_MS = 60000;

	constexpr float SPAWN_BACK = 150.0f;
	constexpr float SPAWN_UP = 60.0f;
	constexpr float CATCH_UP_DIST = 400.0f;
	constexpr float LEAVE_DIST = 800.0f;
	constexpr float RELEASE_DIST = 400.0f;
	constexpr float RAPPEL_HOVER = 25.0f;
	constexpr float BOARD_DIST = 8.0f;
	constexpr float BOARD_NOTICE_RESET_DIST = 15.0f;
	constexpr float LAND_APPROACH_DIST = 150.0f;
	constexpr float LAND_NEAR_DIST = 40.0f;     // landing timeout only runs this close to the spot
	constexpr float LAND_RETRY_OFFSET = 20.0f;
	constexpr float CRUISE_HEIGHT = 80.0f;

	constexpr int MAX_CREW = 4;
	constexpr int MAX_GUARD_HELIS = 2;
	constexpr int BLIP_SPRITE_HELI = 64;
	constexpr int BLIP_COLOUR_YELLOW = 5;
	constexpr int HELI_FLAG_LAND_ON_ARRIVAL = 32;
	constexpr int CONTROL_ENTER = 23;
	constexpr int DOORS_UNLOCKED = 1;
	constexpr int DOORS_LOCKED = 2;

	const std::vector<NamedItem> kGuardHeliModels = {
		{ "Maverick", "maverick" },
		{ "Police Maverick", "polmav" },
		{ "Annihilator", "annihilator" },
	};

	const std::vector<NamedItem> kTransportModels = {
		{ "Swift Deluxe", "swift2" },
		{ "Volatus", "volatus" },
		{ "SuperVolito Carbon", "supervolito2" },
		{ "SuperVolito", "supervolito" },
	};

	enum class HeliMode { None, Circle, Formation, Attack, RappelGoto, Rappelling, Leaving };

	struct GuardHeli
	{
		Vehicle veh = 0;
		Blip blip = 0;
		Ped pilot = 0;
		std::vector<Ped> crew;      // seated crew (rear seats)
		HeliMode mode = HeliMode::None;
		Entity taskTarget = 0;
		DWORD lastTask = 0;
		DWORD lastPoll = 0;
		DWORD lastTeleport = 0;
		DWORD modeSince = 0;
		bool rappel = false;        // rappel order active
		bool ropeRetried = false;
		Vector3 gotoPos;
		bool gotoFast = false;
	};

	enum class TPhase { None, Inbound, Landed, Flying, Landing, Leaving };

	struct Transport
	{
		Vehicle veh = 0;
		Blip blip = 0;
		Ped pilot = 0;
		TPhase phase = TPhase::None;
		bool tasked = false;
		Vector3 spot;               // pickup landing spot / current landing target
		Vector3 dest;               // flight destination (waypoint)
		bool hasDest = false;       // dest is valid (a waypoint was flown to this trip)
		bool arrived = false;       // landed at a drop-off (waypoint or "Land now")
		bool wasAboard = false;
		bool wpNotified = false;
		bool boardNotified = false;
		int landFails = 0;          // landing attempts that timed out on the current landing
		DWORD phaseSince = 0;
		DWORD landSince = 0;        // restarts while the heli is still far from the spot
		DWORD lastWp = 0;
		DWORD lastPoll = 0;
	};

	struct Rappeller
	{
		Ped ped = 0;
		DWORD since = 0;
		Vehicle heli = 0;
	};

	std::vector<GuardHeli> s_helis;
	std::vector<Rappeller> s_rappellers;
	Transport s_tr;
	DWORD s_rappelCheck = 0;

	// Settings
	int s_guardModel = 0;
	int s_guardCountIdx = 0;   // count = idx + 1
	int s_trModel = 0;
	int s_escortCount = 1;     // 0..2
	int s_afterDrop = 0;       // 0 = Wait, 1 = Leave
	float s_rappelHeight = 10.0f;

	// ---- helpers ----

	Vector3 V3(float x, float y, float z)
	{
		Vector3 v;
		v.x = x;
		v.y = y;
		v.z = z;
		return v;
	}

	float HDist(const Vector3& a, const Vector3& b)
	{
		float dx = a.x - b.x, dy = a.y - b.y;
		return std::sqrt(dx * dx + dy * dy);
	}

	float HeadingTo(const Vector3& from, const Vector3& to)
	{
		return std::atan2(-(to.x - from.x), to.y - from.y) * 57.2957795f;
	}

	Vector3 Pos(Entity e) { return ENTITY::GET_ENTITY_COORDS(e, TRUE); }

	// The real native takes a float min height; the SDK stub declares an int.
	void TaskRappel(Ped ped, float minHeight)
	{
		invoke<Void>(0x09693B0312F91649, ped, minHeight);
	}

	void HeliMission(Ped pilot, Vehicle veh, Vehicle targetVeh, Ped targetPed, const Vector3& pos, int mission,
		float speed, float radius, int maxHeight, int minHeight, int flags = 0)
	{
		AI::TASK_HELI_MISSION(pilot, veh, (Any)targetVeh, targetPed, pos.x, pos.y, pos.z, mission, speed, radius, -1.0f,
			maxHeight, minHeight, -1.0f, flags);
	}

	// Respects the Map Blips option (returns 0 when blips are off).
	Blip AddHeliBlip(Vehicle v, int colour, const char* name)
	{
		return Vehicles::AddUnitBlip(v, BLIP_SPRITE_HELI, colour, name);
	}

	void KillBlip(Blip& b)
	{
		Vehicles::RemoveUnitBlip(b);
	}

	Ped Player() { return PLAYER::PLAYER_PED_ID(); }

	// Player's vehicle if driving/riding, otherwise the player ped.
	Entity PlayerAnchor()
	{
		Ped p = Player();
		if (PED::IS_PED_IN_ANY_VEHICLE(p, FALSE))
			return PED::GET_VEHICLE_PED_IS_IN(p, FALSE);
		return p;
	}

	Vector3 BehindPlayer(float back, float side, float up)
	{
		Entity a = PlayerAnchor();
		Vector3 p = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(a, side, -back, 0.0f);
		p.z = Pos(a).z + up;
		return p;
	}

	const std::string& CrewModel() { return g_guardModels[g_cfg.modelIndex].name; }

	// Consumes Combat's "stood down, reissue your task" flag.
	bool TakeRetask(Ped p)
	{
		Guard* g = Guards::Find(p);
		if (!g || !g->needsRetask)
			return false;
		g->needsRetask = false;
		return true;
	}

	void SetupPilot(Ped pilot, Vehicle veh)
	{
		PED::SET_PED_INTO_VEHICLE(pilot, veh, -1);
		if (Guard* g = Guards::Find(pilot))
		{
			g->driving = true;
			g->escortVeh = veh;
		}
		PED::SET_PED_COMBAT_ATTRIBUTES(pilot, CA_LEAVE_VEHICLES, FALSE);
		PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(pilot, TRUE);
		PED::SET_PED_KEEP_TASK(pilot, TRUE); // keeps flying away after release
	}

	void ReleasePed(Ped p)
	{
		if (Guards::Find(p))
			Guards::Remove(p, false);
	}

	bool IsRappeller(Ped p)
	{
		for (auto& r : s_rappellers)
			if (r.ped == p)
				return true;
		return false;
	}

	// Someone from this heli is still on the rope.
	bool RopeBusy(Vehicle v)
	{
		for (auto& r : s_rappellers)
			if (r.heli == v)
				return true;
		return false;
	}

	void DropRappeller(Ped p)
	{
		s_rappellers.erase(std::remove_if(s_rappellers.begin(), s_rappellers.end(),
			[p](const Rappeller& r) { return r.ped == p; }), s_rappellers.end());
	}

	// Seat index the ped occupies in `v`, or -2 if none.
	int SeatOf(Vehicle v, Ped p)
	{
		int passengers = VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(v);
		for (int s = -1; s < passengers; s++)
			if (VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, s) == p)
				return s;
		return -2;
	}

	bool PlayerInside(Vehicle v)
	{
		return v != 0 && PED::IS_PED_IN_VEHICLE(Player(), v, FALSE);
	}

	Vector3 AwayPoint(Vehicle veh)
	{
		Vector3 hp = Pos(veh), pp = Pos(Player());
		float dx = hp.x - pp.x, dy = hp.y - pp.y;
		float len = std::sqrt(dx * dx + dy * dy);
		if (len < 1.0f)
		{
			Vector3 f = ENTITY::GET_ENTITY_FORWARD_VECTOR(veh);
			dx = f.x;
			dy = f.y;
			len = std::max(std::sqrt(dx * dx + dy * dy), 0.01f);
		}
		return V3(hp.x + dx / len * LEAVE_DIST, hp.y + dy / len * LEAVE_DIST, pp.z + 100.0f);
	}

	bool IsLanded(Vehicle v)
	{
		return ENTITY::GET_ENTITY_SPEED(v) < 1.0f && ENTITY::GET_ENTITY_HEIGHT_ABOVE_GROUND(v) < 3.0f && !ENTITY::IS_ENTITY_IN_AIR(v);
	}

	float GroundBelow(Vehicle v)
	{
		Vector3 p = Pos(v);
		float z;
		if (Vehicles::GroundZ(p.x, p.y, z) && z < p.z + 1.0f)
			return z;
		return p.z - ENTITY::GET_ENTITY_HEIGHT_ABOVE_GROUND(v);
	}

	// Transport is cruising (not in its final approach / on the ground): escorts fly formation.
	bool TransportCruising()
	{
		if (s_tr.phase == TPhase::Flying)
			return true;
		if (s_tr.phase == TPhase::Inbound)
			return HDist(Pos(s_tr.veh), s_tr.spot) > LAND_APPROACH_DIST;
		return false;
	}

	// Helis on station. Leaving helis don't count, so at most MAX_GUARD_HELIS are ever active.
	int ActiveHeliCount()
	{
		int n = 0;
		for (auto& h : s_helis)
			if (h.mode != HeliMode::Leaving)
				n++;
		return n;
	}

	// ---- guard helis ----

	// Fills empty rear seats with new crew. May yield while loading the model.
	int FillCrew(GuardHeli& h)
	{
		const std::string model = CrewModel();
		if (!LoadModel(Joaat(model)))
			return 0;
		int passengers = VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(h.veh);
		Vector3 pos = Pos(h.veh);
		pos.z -= 5.0f;
		float heading = ENTITY::GET_ENTITY_HEADING(h.veh);
		int added = 0;
		for (int seat = 1; seat < passengers && (int)h.crew.size() < MAX_CREW; seat++)
		{
			if (!VEHICLE::IS_VEHICLE_SEAT_FREE(h.veh, seat))
				continue;
			Ped p = Guards::Create(model, pos, heading, Role::HeliCrew);
			if (!p)
				break;
			PED::SET_PED_INTO_VEHICLE(p, h.veh, seat);
			PED::SET_PED_COMBAT_ATTRIBUTES(p, CA_LEAVE_VEHICLES, FALSE);
			if (Guard* g = Guards::Find(p))
				g->escortVeh = h.veh;
			h.crew.push_back(p);
			added++;
		}
		return added;
	}

	bool SpawnGuardHeli(const Vector3& pos, float heading)
	{
		// Load the ped model first so the heli isn't left falling pilotless while it streams.
		if (!LoadModel(Joaat(CrewModel())))
		{
			Notify("~r~Bodyguards:~s~ couldn't load model " + CrewModel());
			return false;
		}
		Vehicle veh = Vehicles::SpawnHeliInAir(kGuardHeliModels[s_guardModel].name, pos, heading, 30.0f);
		if (!veh)
			return false;
		Vector3 below = pos;
		below.z -= 5.0f;
		Ped pilot = Guards::Create(CrewModel(), below, heading, Role::HeliPilot);
		if (!pilot)
		{
			Vehicles::DeleteOrRelease(veh);
			return false;
		}
		SetupPilot(pilot, veh);

		GuardHeli h;
		h.veh = veh;
		h.pilot = pilot;
		h.blip = AddHeliBlip(veh, BLIP_COLOUR_BLUE, "Guard Heli");
		FillCrew(h);
		VEHICLE::SET_HELI_BLADES_FULL_SPEED(veh);
		s_helis.push_back(h);
		return true;
	}

	// Spawns guard helis until `count` are on station. `origin` = spawn point of the first one.
	int EnsureGuardHelis(int count, const Vector3& origin, float heading)
	{
		int spawned = 0;
		count = std::min(count, MAX_GUARD_HELIS);
		float rad = heading / 57.2957795f;
		float rx = std::cos(rad), ry = std::sin(rad); // right-ish lateral vector for spacing
		while (ActiveHeliCount() < count)
		{
			float side = ActiveHeliCount() == 0 ? -30.0f : 30.0f;
			Vector3 p = V3(origin.x + rx * side, origin.y + ry * side, origin.z);
			if (!SpawnGuardHeli(p, heading))
				break;
			spawned++;
		}
		return spawned;
	}

	void EndRappel(GuardHeli& h)
	{
		h.rappel = false;
		h.mode = HeliMode::None;
		bool alive = ENTITY::DOES_ENTITY_EXIST(h.veh);
		for (Ped p : h.crew)
		{
			// Anyone already out on the rope stays watched until he lands.
			if (!IsRappeller(p) || !alive || !PED::IS_PED_IN_VEHICLE(p, h.veh, FALSE))
				continue;
			// Still seated with a rappel task: cancel it and put him back in his seat.
			DropRappeller(p);
			int seat = SeatOf(h.veh, p);
			if (seat != -2)
			{
				AI::CLEAR_PED_TASKS_IMMEDIATELY(p); // pulls him out of the seat...
				PED::SET_PED_INTO_VEHICLE(p, h.veh, seat); // ...so put him straight back
			}
			if (Guard* g = Guards::Find(p))
				g->driving = false;
		}
	}

	void StartLeaving(GuardHeli& h)
	{
		if (h.rappel)
			EndRappel(h);
		VEHICLE::SET_VEHICLE_DOORS_LOCKED(h.veh, DOORS_LOCKED);
		HeliMission(h.pilot, h.veh, 0, 0, AwayPoint(h.veh), Vehicles::HELI_GOTO, 40.0f, 20.0f, 100, 50);
		h.mode = HeliMode::Leaving;
		h.modeSince = Now();
		KillBlip(h.blip);
	}

	void ReleaseUnit(GuardHeli& h)
	{
		KillBlip(h.blip);
		for (Ped p : h.crew)
		{
			if (IsRappeller(p) && ENTITY::DOES_ENTITY_EXIST(h.veh) && !PED::IS_PED_IN_VEHICLE(p, h.veh, FALSE))
				continue; // on the rope: promoted when he lands
			DropRappeller(p);
			ReleasePed(p);
		}
		h.crew.clear();
		ReleasePed(h.pilot);
		Vehicles::Release(h.veh);
	}

	// Drops dead / promoted crew. Crew that left the heli become bodyguards: right away if they're
	// already on the ground, otherwise once they land (rappeller watch list).
	void PruneCrew(GuardHeli& h)
	{
		for (size_t i = 0; i < h.crew.size();)
		{
			Ped p = h.crew[i];
			Guard* g = Guards::Find(p);
			if (!g || !IsAlive(p) || g->role != Role::HeliCrew)
			{
				DropRappeller(p);
				h.crew.erase(h.crew.begin() + i);
				continue;
			}
			if (!PED::IS_PED_IN_VEHICLE(p, h.veh, FALSE))
			{
				if (!IsRappeller(p))
				{
					if (PED::IS_PED_ON_FOOT(p) && !ENTITY::IS_ENTITY_IN_AIR(p))
						Guards::Promote(p);
					else
						s_rappellers.push_back({ p, Now(), h.veh }); // falling / ragdolled: promote once down
				}
				h.crew.erase(h.crew.begin() + i);
				continue;
			}
			if (g->needsRetask)
			{
				g->needsRetask = false;
				// Seated crew have no standing task; only a pending rappel needs re-issuing.
				if (h.mode == HeliMode::Rappelling && IsRappeller(p))
					TaskRappel(p, s_rappelHeight);
			}
			i++;
		}
	}

	void UpdateRappel(GuardHeli& h)
	{
		if (h.mode == HeliMode::Rappelling)
		{
			// Hold the hover until everyone is down (or timeout).
			if ((h.crew.empty() && !RopeBusy(h.veh)) || Elapsed(h.modeSince, ROPE_TIMEOUT_MS))
			{
				EndRappel(h);
				return;
			}
			if (!h.ropeRetried && Elapsed(h.modeSince, ROPE_RETRY_MS))
			{
				h.ropeRetried = true;
				for (Ped p : h.crew)
					TaskRappel(p, s_rappelHeight);
			}
			return;
		}

		if (h.crew.empty())
		{
			EndRappel(h);
			return;
		}

		Ped player = Player();
		if (PED::IS_PED_IN_FLYING_VEHICLE(player))
		{
			Notify("~r~Bodyguards:~s~ rappel cancelled - you're flying.");
			EndRappel(h);
			return;
		}

		// Approach: hover ~25 m above the player.
		if (h.mode == HeliMode::RappelGoto && Elapsed(h.modeSince, APPROACH_TIMEOUT_MS))
		{
			Notify("~r~Bodyguards:~s~ heli couldn't get into rappel position.");
			EndRappel(h);
			return;
		}
		Vector3 pp = Pos(player), hp = Pos(h.veh);
		float hd = HDist(hp, pp);
		bool fast = hd > 60.0f;
		Vector3 want = V3(pp.x, pp.y, pp.z + RAPPEL_HOVER);
		if (h.mode != HeliMode::RappelGoto || fast != h.gotoFast || Dist(want, h.gotoPos) > 8.0f || Elapsed(h.lastTask, REFRESH_MS))
		{
			if (h.mode != HeliMode::RappelGoto)
				h.modeSince = Now();
			HeliMission(h.pilot, h.veh, 0, 0, want, Vehicles::HELI_GOTO, fast ? 30.0f : 10.0f, 4.0f, (int)RAPPEL_HOVER, 20); // min 20 m so the pilot stays inside the trigger window below
			h.mode = HeliMode::RappelGoto;
			h.gotoPos = want;
			h.gotoFast = fast;
			h.lastTask = Now();
		}

		float dz = hp.z - pp.z;
		bool heightOk = dz > RAPPEL_HOVER - 10.0f && dz < RAPPEL_HOVER + 12.0f
			&& ENTITY::GET_ENTITY_HEIGHT_ABOVE_GROUND(h.veh) > s_rappelHeight;
		if (hd < 15.0f && ENTITY::GET_ENTITY_SPEED(h.veh) < 3.0f && heightOk)
		{
			for (Ped p : h.crew)
			{
				if (Guard* g = Guards::Find(p))
				{
					g->driving = true; // keeps Combat from overriding the rappel task
					g->target = 0;
				}
				TaskRappel(p, s_rappelHeight);
				if (!IsRappeller(p))
					s_rappellers.push_back({ p, Now(), h.veh });
			}
			h.mode = HeliMode::Rappelling;
			h.modeSince = Now();
			h.ropeRetried = false;
		}
	}

	void TeleportCatchUp(GuardHeli& h, int slot)
	{
		Vector3 p = BehindPlayer(SPAWN_BACK, slot == 0 ? -25.0f : 25.0f, 0.0f);
		float gz;
		if (Vehicles::GroundZ(p.x, p.y, gz))
			p.z = std::max(p.z, gz);
		p.z += SPAWN_UP;
		ENTITY::SET_ENTITY_COORDS(h.veh, p.x, p.y, p.z, FALSE, FALSE, FALSE, TRUE);
		ENTITY::SET_ENTITY_HEADING(h.veh, ENTITY::GET_ENTITY_HEADING(PlayerAnchor()));
		VEHICLE::SET_HELI_BLADES_FULL_SPEED(h.veh);
		VEHICLE::SET_VEHICLE_FORWARD_SPEED(h.veh, 25.0f);
		h.lastTeleport = Now();
		h.mode = HeliMode::None;
	}

	// Returns false when the unit should be removed.
	bool UpdateGuardHeli(GuardHeli& h, int slot)
	{
		bool dead = !ENTITY::DOES_ENTITY_EXIST(h.veh) || ENTITY::IS_ENTITY_DEAD(h.veh) || !VEHICLE::IS_VEHICLE_DRIVEABLE(h.veh, FALSE)
			|| !Guards::Find(h.pilot) || !IsAlive(h.pilot);
		if (dead)
		{
			if (h.mode != HeliMode::Leaving)
				Notify("~r~Bodyguards:~s~ guard heli lost.");
			ReleaseUnit(h);
			return false;
		}

		if (!Elapsed(h.lastPoll, POLL_MS))
			return true;
		h.lastPoll = Now();

		PruneCrew(h);
		Ped player = Player();

		if (h.mode == HeliMode::Leaving)
		{
			// Never cut the pilot loose with the player on board.
			if ((EntityDist(h.veh, player) > RELEASE_DIST || Elapsed(h.modeSince, LEAVE_TIMEOUT_MS)) && !PlayerInside(h.veh))
			{
				ReleaseUnit(h);
				return false;
			}
			return true;
		}

		bool retask = TakeRetask(h.pilot);
		if (!PED::IS_PED_IN_VEHICLE(h.pilot, h.veh, FALSE) && VEHICLE::IS_VEHICLE_SEAT_FREE(h.veh, -1))
		{
			PED::SET_PED_INTO_VEHICLE(h.pilot, h.veh, -1);
			retask = true;
		}
		if (retask)
		{
			// Re-issue without restarting the rappel approach timer.
			if (h.rappel)
				h.lastTask = 0;
			else
				h.mode = HeliMode::None;
		}

		if (h.rappel)
		{
			UpdateRappel(h);
			return true;
		}

		Vehicle tv = Combat::TargetVehicle();
		Ped tp = Combat::TargetPed();
		bool attacking = tv != 0 || (tp != 0 && IsAlive(tp));
		bool cruising = TransportCruising();

		if (!attacking && !cruising && h.mode != HeliMode::Attack && h.mode != HeliMode::Formation
			&& EntityDist(h.veh, player) > CATCH_UP_DIST && Elapsed(h.lastTeleport, CATCH_UP_COOLDOWN_MS)
			&& !ENTITY::IS_ENTITY_ON_SCREEN(h.veh))
			TeleportCatchUp(h, slot);

		bool refresh = Elapsed(h.lastTask, REFRESH_MS);
		Vector3 zero = V3(0.0f, 0.0f, 0.0f);

		// Attack the player-ordered target
		if (attacking)
		{
			Entity target = tv != 0 ? tv : tp;
			if (h.mode != HeliMode::Attack || h.taskTarget != target || refresh)
			{
				HeliMission(h.pilot, h.veh, tv, tv != 0 ? 0 : tp, zero, Vehicles::HELI_ATTACK, 40.0f, 40.0f, 60, 25);
				h.mode = HeliMode::Attack;
				h.taskTarget = target;
				h.lastTask = Now();
			}
			return true;
		}

		// Formation with the transport while it cruises
		if (cruising)
		{
			if (h.mode != HeliMode::Formation || h.taskTarget != s_tr.veh || refresh)
			{
				int mission = slot == 0 ? Vehicles::HELI_ESCORT_LEFT : Vehicles::HELI_ESCORT_RIGHT;
				HeliMission(h.pilot, h.veh, s_tr.veh, 0, zero, mission, 60.0f, 25.0f, 40, 20);
				h.mode = HeliMode::Formation;
				h.taskTarget = s_tr.veh;
				h.lastTask = Now();
			}
			return true;
		}

		// Default: circle the player (or the vehicle he's in)
		Ped cp = player;
		Vehicle cv = 0;
		if (PED::IS_PED_IN_ANY_VEHICLE(player, FALSE))
		{
			cv = PED::GET_VEHICLE_PED_IS_IN(player, FALSE);
			cp = 0;
		}
		Entity target = cv != 0 ? cv : cp;
		if (h.mode != HeliMode::Circle || h.taskTarget != target || refresh)
		{
			// Wider circle while the transport is landing / parked nearby.
			bool transportNear = s_tr.phase != TPhase::None && s_tr.phase != TPhase::Leaving && EntityDist(s_tr.veh, player) < 120.0f;
			float radius = transportNear ? 50.0f : 30.0f;
			HeliMission(h.pilot, h.veh, cv, cp, zero, Vehicles::HELI_CIRCLE, 30.0f, radius, 50, 35);
			h.mode = HeliMode::Circle;
			h.taskTarget = target;
			h.lastTask = Now();
		}
		return true;
	}

	void UpdateGuardHelis()
	{
		int slot = 0;
		for (size_t i = 0; i < s_helis.size();)
		{
			bool active = s_helis[i].mode != HeliMode::Leaving;
			if (!UpdateGuardHeli(s_helis[i], active ? slot : 0))
			{
				s_helis.erase(s_helis.begin() + i);
				continue;
			}
			if (active)
				slot++;
			i++;
		}
	}

	// Rappellers become bodyguards once they're on the ground.
	void UpdateRappellers()
	{
		if (!Elapsed(s_rappelCheck, POLL_MS))
			return;
		s_rappelCheck = Now();

		for (size_t i = 0; i < s_rappellers.size();)
		{
			Ped p = s_rappellers[i].ped;
			Guard* g = Guards::Find(p);
			if (!g || !IsAlive(p) || g->role != Role::HeliCrew)
			{
				s_rappellers.erase(s_rappellers.begin() + i);
				continue;
			}
			g->needsRetask = false; // on the rope: nothing to re-issue, he's promoted on landing
			bool grounded = !PED::IS_PED_IN_ANY_VEHICLE(p, FALSE) && PED::IS_PED_ON_FOOT(p) && !ENTITY::IS_ENTITY_IN_AIR(p)
				&& ENTITY::GET_ENTITY_HEIGHT_ABOVE_GROUND(p) < 2.0f;
			bool stale = Elapsed(s_rappellers[i].since, RAPPELLER_STALE_MS) && !PED::IS_PED_IN_ANY_VEHICLE(p, FALSE);
			if (grounded || stale)
			{
				s_rappellers.erase(s_rappellers.begin() + i);
				Guards::Promote(p);
				continue;
			}
			i++;
		}
	}

	// ---- transport ----

	void ReleaseTransport()
	{
		KillBlip(s_tr.blip);
		ReleasePed(s_tr.pilot);
		Vehicles::Release(s_tr.veh);
		s_tr = Transport();
	}

	void SetPhase(TPhase phase)
	{
		s_tr.phase = phase;
		s_tr.phaseSince = Now();
		s_tr.landSince = Now();
		s_tr.tasked = false;
	}

	// Returns false (and stays put) if the player is aboard or getting in.
	bool StartTransportLeaving()
	{
		if (PED::IS_PED_IN_VEHICLE(Player(), s_tr.veh, TRUE))
			return false;
		SetPhase(TPhase::Leaving);
		s_tr.arrived = false;
		VEHICLE::SET_VEHICLE_DOORS_LOCKED(s_tr.veh, DOORS_LOCKED);
		KillBlip(s_tr.blip);
		return true;
	}

	void LandAt(const Vector3& pos)
	{
		HeliMission(s_tr.pilot, s_tr.veh, 0, 0, pos, Vehicles::HELI_LAND, 25.0f, 5.0f, 40, 0, HELI_FLAG_LAND_ON_ARRIVAL);
	}

	void IssueTransportTask()
	{
		switch (s_tr.phase)
		{
		case TPhase::Inbound:
			LandAt(s_tr.spot);
			break;
		case TPhase::Landing:
			LandAt(s_tr.spot);
			break;
		case TPhase::Flying:
		{
			Vector3 hp = Pos(s_tr.veh);
			float ground = GroundBelow(s_tr.veh);
			float z = std::max(ground, Pos(Player()).z) + CRUISE_HEIGHT;
			z = std::max(z, hp.z);
			HeliMission(s_tr.pilot, s_tr.veh, 0, 0, V3(s_tr.dest.x, s_tr.dest.y, z), Vehicles::HELI_GOTO, 50.0f, 20.0f,
				(int)CRUISE_HEIGHT, 40);
			break;
		}
		case TPhase::Leaving:
			HeliMission(s_tr.pilot, s_tr.veh, 0, 0, AwayPoint(s_tr.veh), Vehicles::HELI_GOTO, 40.0f, 20.0f, 100, 50);
			break;
		default:
			break; // Landed: the land mission keeps it on the ground
		}
		s_tr.tasked = true;
	}

	int FreeBoardingSeat(Vehicle v)
	{
		int passengers = VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(v);
		const int prefer[] = { 1, 2, 0 };
		for (int s : prefer)
			if (s < passengers && VEHICLE::IS_VEHICLE_SEAT_FREE(v, s))
				return s;
		for (int s = 3; s < passengers; s++)
			if (VEHICLE::IS_VEHICLE_SEAT_FREE(v, s))
				return s;
		return -2;
	}

	void HandleBoarding(bool aboard)
	{
		if (s_tr.phase != TPhase::Landed || aboard || !Vehicles::ShouldHandleEnter(s_tr.veh, BOARD_DIST))
			return;
		if (!s_tr.boardNotified)
		{
			s_tr.boardNotified = true;
			Notify("~b~Bodyguards:~s~ press ~INPUT_ENTER~ to board the transport.");
		}
		CONTROLS::DISABLE_CONTROL_ACTION(0, CONTROL_ENTER, TRUE);
		if (!CONTROLS::IS_DISABLED_CONTROL_JUST_PRESSED(0, CONTROL_ENTER))
			return;
		int seat = FreeBoardingSeat(s_tr.veh);
		if (seat == -2)
		{
			Notify("~r~Bodyguards:~s~ no free seat in the transport.");
			return;
		}
		AI::TASK_ENTER_VEHICLE(Player(), s_tr.veh, 10000, seat, 1.0f, 1, 0);
	}

	// Waypoint far enough from the heli to be worth flying to, and not the one we just gave up on.
	bool UsableWaypoint(Vector3& wp)
	{
		if (!Vehicles::GetWaypoint(wp) || HDist(wp, Pos(s_tr.veh)) <= 60.0f)
			return false;
		return !s_tr.hasDest || HDist(wp, s_tr.dest) > 5.0f;
	}

	void FlyTo(const Vector3& wp)
	{
		s_tr.dest = wp;
		s_tr.hasDest = true;
		s_tr.arrived = false;
		s_tr.wpNotified = false;
		s_tr.landFails = 0;
		SetPhase(TPhase::Flying);
	}

	// Landing attempt timed out: try a spot a little further along.
	void RetryLanding(bool aboard)
	{
		s_tr.landFails++;
		float ang = s_tr.landFails * 2.39996f; // golden angle: successive spots spread around
		float x = s_tr.spot.x + std::cos(ang) * LAND_RETRY_OFFSET;
		float y = s_tr.spot.y + std::sin(ang) * LAND_RETRY_OFFSET;
		float gz;
		float z = Vehicles::GroundZ(x, y, gz) ? gz : s_tr.spot.z;
		s_tr.spot = V3(x, y, z);
		SetPhase(s_tr.phase); // restart timers, re-task
		if (s_tr.landFails == 2)
		{
			if (aboard)
				Notify("~r~Bodyguards:~s~ can't land here - set a new waypoint or use Land Now.");
			else if (s_tr.phase == TPhase::Inbound)
				Notify("~r~Bodyguards:~s~ transport can't land here - move to an open area and call the pickup again.");
		}
	}

	void UpdateTransport()
	{
		if (s_tr.phase == TPhase::None)
			return;
		Ped player = Player();

		bool dead = !ENTITY::DOES_ENTITY_EXIST(s_tr.veh) || ENTITY::IS_ENTITY_DEAD(s_tr.veh) || !VEHICLE::IS_VEHICLE_DRIVEABLE(s_tr.veh, FALSE)
			|| !Guards::Find(s_tr.pilot) || !IsAlive(s_tr.pilot);
		if (dead)
		{
			if (s_tr.phase != TPhase::Leaving)
				Notify("~r~Bodyguards:~s~ transport heli lost.");
			ReleaseTransport();
			return;
		}

		bool aboard = PED::IS_PED_IN_VEHICLE(player, s_tr.veh, FALSE);
		HandleBoarding(aboard); // every frame (control disabling)

		if (!Elapsed(s_tr.lastPoll, TRANSPORT_POLL_MS))
			return;
		s_tr.lastPoll = Now();

		if (s_tr.phase == TPhase::Leaving)
		{
			if (aboard)
			{
				// Got in anyway: cancel the departure and set down.
				VEHICLE::SET_VEHICLE_DOORS_LOCKED(s_tr.veh, DOORS_UNLOCKED);
				KillBlip(s_tr.blip);
				s_tr.blip = AddHeliBlip(s_tr.veh, BLIP_COLOUR_YELLOW, "Transport Heli");
				Vector3 hp = Pos(s_tr.veh);
				s_tr.spot = V3(hp.x, hp.y, GroundBelow(s_tr.veh));
				s_tr.arrived = false;
				s_tr.landFails = 0;
				SetPhase(TPhase::Landing);
				IssueTransportTask();
				return;
			}
			if (!s_tr.tasked)
				IssueTransportTask();
			if (EntityDist(s_tr.veh, player) > RELEASE_DIST || Elapsed(s_tr.phaseSince, LEAVE_TIMEOUT_MS))
				ReleaseTransport();
			return;
		}

		bool retask = TakeRetask(s_tr.pilot);
		if (!PED::IS_PED_IN_VEHICLE(s_tr.pilot, s_tr.veh, FALSE) && VEHICLE::IS_VEHICLE_SEAT_FREE(s_tr.veh, -1))
		{
			PED::SET_PED_INTO_VEHICLE(s_tr.pilot, s_tr.veh, -1);
			retask = true;
		}
		if (retask)
			s_tr.tasked = false;

		switch (s_tr.phase)
		{
		case TPhase::Inbound:
		case TPhase::Landing:
		{
			if (Elapsed(s_tr.phaseSince, 3000) && IsLanded(s_tr.veh))
			{
				bool dropOff = s_tr.phase == TPhase::Landing;
				SetPhase(TPhase::Landed);
				s_tr.tasked = true;
				s_tr.landFails = 0;
				if (dropOff)
					Notify("~b~Bodyguards:~s~ transport has landed.");
				else
					Notify("~b~Bodyguards:~s~ transport has landed - walk up and press ~INPUT_ENTER~ to board.");
				s_tr.boardNotified = !dropOff;
				break;
			}
			// A new waypoint while coming down re-routes the flight.
			if (s_tr.phase == TPhase::Landing && aboard && Elapsed(s_tr.lastWp, WAYPOINT_POLL_MS))
			{
				s_tr.lastWp = Now();
				Vector3 wp;
				if (UsableWaypoint(wp))
				{
					FlyTo(wp);
					break;
				}
			}
			if (HDist(Pos(s_tr.veh), s_tr.spot) > LAND_NEAR_DIST)
				s_tr.landSince = Now();
			if (Elapsed(s_tr.landSince, LAND_TIMEOUT_MS) || Elapsed(s_tr.phaseSince, LAND_HARD_TIMEOUT_MS))
				RetryLanding(aboard);
			break;
		}

		case TPhase::Landed:
			if (aboard)
			{
				s_tr.wasAboard = true;
				if (Elapsed(s_tr.lastWp, WAYPOINT_POLL_MS))
				{
					s_tr.lastWp = Now();
					Vector3 wp;
					if (UsableWaypoint(wp))
						FlyTo(wp);
					else if (!s_tr.wpNotified)
					{
						s_tr.wpNotified = true;
						Notify(s_tr.hasDest ? "~b~Bodyguards:~s~ set a new waypoint for the transport."
							: "~b~Bodyguards:~s~ set a waypoint for the transport.");
					}
				}
			}
			else if (s_tr.wasAboard && PED::IS_PED_ON_FOOT(player))
			{
				// Player got out. He's right next to the heli: no "press F" notice until he walks away and back.
				s_tr.wasAboard = false;
				s_tr.wpNotified = false;
				s_tr.boardNotified = true;
				s_tr.hasDest = false; // re-boarding may fly to the same waypoint again
				if (s_tr.arrived)
				{
					s_tr.arrived = false;
					if (s_afterDrop == 1 && StartTransportLeaving())
					{
						Notify("~b~Bodyguards:~s~ transport is leaving.");
						return;
					}
				}
			}
			else if (s_tr.boardNotified && EntityDist(player, s_tr.veh) > BOARD_NOTICE_RESET_DIST)
			{
				s_tr.boardNotified = false;
			}
			break;

		case TPhase::Flying:
		{
			if (!aboard)
			{
				// Player left mid-flight: set down right here.
				Vector3 hp = Pos(s_tr.veh);
				s_tr.spot = V3(hp.x, hp.y, GroundBelow(s_tr.veh));
				s_tr.arrived = false;
				s_tr.wasAboard = false;
				s_tr.landFails = 0;
				SetPhase(TPhase::Landing);
				break;
			}
			if (Elapsed(s_tr.lastWp, WAYPOINT_POLL_MS))
			{
				s_tr.lastWp = Now();
				Vector3 wp;
				if (Vehicles::GetWaypoint(wp) && HDist(wp, s_tr.dest) > 5.0f)
				{
					s_tr.dest = wp;
					s_tr.tasked = false;
				}
			}
			Vector3 hp = Pos(s_tr.veh);
			float hd = HDist(hp, s_tr.dest);
			if (hd < LAND_APPROACH_DIST)
			{
				// Ground height at the destination streams in as we approach; retry until it resolves.
				float gz;
				bool haveZ = Vehicles::GroundZ(s_tr.dest.x, s_tr.dest.y, gz);
				if (!haveZ && hd < 40.0f)
				{
					gz = GroundBelow(s_tr.veh);
					haveZ = true;
				}
				if (haveZ)
				{
					s_tr.spot = V3(s_tr.dest.x, s_tr.dest.y, gz);
					s_tr.arrived = true;
					s_tr.landFails = 0;
					SetPhase(TPhase::Landing);
					break;
				}
			}
			break;
		}

		default:
			break;
		}

		if (!s_tr.tasked)
			IssueTransportTask();
	}

	// ---- menu actions ----

	void CallGuardHelis()
	{
		int want = s_guardCountIdx + 1;
		if (ActiveHeliCount() >= want)
		{
			Notify("~b~Bodyguards:~s~ guard helis already on station.");
			return;
		}
		float heading = ENTITY::GET_ENTITY_HEADING(PlayerAnchor());
		int n = EnsureGuardHelis(want, BehindPlayer(SPAWN_BACK, 0.0f, SPAWN_UP), heading);
		if (n > 0)
			Notify("~b~Bodyguards:~s~ " + std::to_string(n) + " guard heli(s) inbound.");
	}

	void Reinforce()
	{
		if (ActiveHeliCount() == 0)
		{
			Notify("~r~Bodyguards:~s~ no guard helis.");
			return;
		}
		Ped player = Player();
		int added = 0;
		bool blocked = false, full = true;
		for (size_t i = 0; i < s_helis.size(); i++)
		{
			GuardHeli& h = s_helis[i];
			if (h.mode == HeliMode::Leaving || h.rappel)
				continue;
			if (EntityDist(h.veh, player) <= 150.0f && ENTITY::IS_ENTITY_ON_SCREEN(h.veh))
			{
				blocked = true;
				continue;
			}
			int n = FillCrew(h);
			if (n > 0)
				full = false;
			added += n;
		}
		if (added > 0)
			Notify("~b~Bodyguards:~s~ " + std::to_string(added) + " crew reinforced.");
		else if (blocked)
			Notify("~r~Bodyguards:~s~ heli must be out of sight or over 150 m away to reinforce.");
		else if (full)
			Notify("~b~Bodyguards:~s~ guard helis are fully crewed.");
	}

	void DismissGuardHelis()
	{
		int n = 0;
		for (auto& h : s_helis)
		{
			if (h.mode == HeliMode::Leaving)
				continue;
			StartLeaving(h);
			n++;
		}
		Notify(n > 0 ? "~b~Bodyguards:~s~ guard helis leaving." : "~r~Bodyguards:~s~ no guard helis.");
	}

	void CallPickup()
	{
		Ped player = Player();
		if (s_tr.phase != TPhase::None && s_tr.phase != TPhase::Leaving && PED::IS_PED_IN_VEHICLE(player, s_tr.veh, FALSE))
		{
			Notify("~r~Bodyguards:~s~ you're already aboard.");
			return;
		}
		if (PED::IS_PED_IN_ANY_VEHICLE(player, FALSE))
		{
			Notify("~r~Bodyguards:~s~ get out of the vehicle first.");
			return;
		}
		bool needEscort = s_escortCount > ActiveHeliCount();

		if (s_tr.phase == TPhase::None || s_tr.phase == TPhase::Leaving)
		{
			// Stream every model first, so nothing spawned sits untasked while the rest loads.
			const std::string& trModel = kTransportModels[s_trModel].name;
			const std::string& escModel = kGuardHeliModels[s_guardModel].name;
			if (!LoadModel(Joaat(CrewModel())))
			{
				Notify("~r~Bodyguards:~s~ couldn't load model " + CrewModel());
				return;
			}
			if (!LoadModel(Joaat(trModel)))
			{
				Notify("~r~Bodyguards:~s~ couldn't load helicopter " + trModel);
				return;
			}
			if (needEscort && !LoadModel(Joaat(escModel)))
			{
				Notify("~r~Bodyguards:~s~ couldn't load helicopter " + escModel);
				needEscort = false;
			}
		}

		// Model loading may have yielded: re-read the world.
		player = Player();
		if (PED::IS_PED_IN_ANY_VEHICLE(player, FALSE))
		{
			Notify("~r~Bodyguards:~s~ get out of the vehicle first.");
			return;
		}
		if (s_tr.phase == TPhase::Leaving)
			ReleaseTransport();

		Vector3 pp = Pos(player);
		Vector3 spot = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(player, 0.0f, 25.0f, 0.0f);
		float gz;
		spot.z = Vehicles::GroundZ(spot.x, spot.y, gz) ? gz : pp.z;

		Vector3 escortFrom;
		float escortHeading;
		if (s_tr.phase == TPhase::None)
		{
			Vector3 from = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(player, 0.0f, -250.0f, 0.0f);
			from.z = pp.z + 80.0f;
			float heading = HeadingTo(from, spot);

			Vehicle veh = Vehicles::SpawnHeliInAir(kTransportModels[s_trModel].name, from, heading, 30.0f);
			if (!veh)
				return;
			Vector3 below = from;
			below.z -= 5.0f;
			Ped pilot = Guards::Create(CrewModel(), below, heading, Role::TransportPilot);
			if (!pilot)
			{
				Vehicles::DeleteOrRelease(veh);
				return;
			}
			SetupPilot(pilot, veh);
			VEHICLE::SET_VEHICLE_DOORS_LOCKED(veh, DOORS_UNLOCKED);
			VEHICLE::SET_HELI_BLADES_FULL_SPEED(veh);

			s_tr = Transport();
			s_tr.veh = veh;
			s_tr.pilot = pilot;
			s_tr.blip = AddHeliBlip(veh, BLIP_COLOUR_YELLOW, "Transport Heli");

			// Escort helis spawn beside the transport so the formation forms up right away.
			escortFrom = from;
			escortFrom.z += 10.0f;
			escortHeading = heading;
		}
		else
		{
			if (s_tr.blip == 0)
				s_tr.blip = AddHeliBlip(s_tr.veh, BLIP_COLOUR_YELLOW, "Transport Heli");
			escortFrom = BehindPlayer(SPAWN_BACK, 0.0f, SPAWN_UP);
			escortHeading = ENTITY::GET_ENTITY_HEADING(player);
		}

		s_tr.spot = spot;
		s_tr.arrived = false;
		s_tr.wasAboard = false;
		s_tr.wpNotified = false;
		s_tr.boardNotified = false;
		s_tr.landFails = 0;
		SetPhase(TPhase::Inbound);
		IssueTransportTask(); // task the pilot before anything else can yield
		Notify("~b~Bodyguards:~s~ transport inbound - stand in an open area.");

		if (needEscort)
			EnsureGuardHelis(s_escortCount, escortFrom, escortHeading);
	}

	void LandNow()
	{
		if (s_tr.phase != TPhase::Flying && s_tr.phase != TPhase::Inbound && s_tr.phase != TPhase::Landing)
		{
			Notify("~r~Bodyguards:~s~ transport is not flying.");
			return;
		}
		Vector3 hp = Pos(s_tr.veh);
		s_tr.spot = V3(hp.x, hp.y, GroundBelow(s_tr.veh));
		s_tr.landFails = 0;
		if (s_tr.phase == TPhase::Flying)
		{
			s_tr.arrived = true;
			SetPhase(TPhase::Landing);
		}
		else
		{
			SetPhase(s_tr.phase); // Inbound stays a pickup, Landing keeps its drop-off flag
		}
		Notify("~b~Bodyguards:~s~ transport landing.");
	}

	void DismissTransport()
	{
		if (s_tr.phase == TPhase::None || s_tr.phase == TPhase::Leaving)
		{
			Notify("~r~Bodyguards:~s~ no transport heli.");
			return;
		}
		if (!StartTransportLeaving())
		{
			Notify("~r~Bodyguards:~s~ get out of the transport first.");
			return;
		}
		Notify("~b~Bodyguards:~s~ transport dismissed.");
	}

	const char* ModeName(const GuardHeli& h)
	{
		switch (h.mode)
		{
		case HeliMode::Circle: return "Circling";
		case HeliMode::Formation: return "Formation";
		case HeliMode::Attack: return "Attacking";
		case HeliMode::RappelGoto: return "Moving to rappel";
		case HeliMode::Rappelling: return "Rappelling";
		case HeliMode::Leaving: return "Leaving";
		default: return "Standby";
		}
	}

	const char* PhaseName(TPhase p)
	{
		switch (p)
		{
		case TPhase::Inbound: return "Inbound";
		case TPhase::Landed: return "Landed";
		case TPhase::Flying: return "Flying to waypoint";
		case TPhase::Landing: return "Landing";
		case TPhase::Leaving: return "Leaving";
		default: return "None";
		}
	}
}

namespace Heli
{
	void LoadConfig()
	{
		s_guardModel = FindIndex(kGuardHeliModels, ReadString("Heli", "GuardHeliModel", "maverick"), 0);
		s_guardCountIdx = std::clamp(ReadInt("Heli", "GuardHeliCount", 1), 1, MAX_GUARD_HELIS) - 1;
		s_trModel = FindIndex(kTransportModels, ReadString("Heli", "TransportModel", "swift2"), 0);
		s_escortCount = std::clamp(ReadInt("Heli", "EscortHelis", 1), 0, MAX_GUARD_HELIS);
		s_afterDrop = Upper(ReadString("Heli", "AfterDropOff", "Wait")) == "LEAVE" ? 1 : 0;
		// The rope must end above the ground: keep it well below the hover height.
		s_rappelHeight = (float)std::clamp(ReadInt("Heli", "RappelHeight", 10), 1, (int)RAPPEL_HOVER - 5);
	}

	void Update()
	{
		UpdateTransport();
		UpdateGuardHelis();
		UpdateRappellers();
	}

	void Reset()
	{
		for (auto& h : s_helis)
		{
			KillBlip(h.blip);
			Vehicles::DeleteOrRelease(h.veh);
		}
		s_helis.clear();
		s_rappellers.clear();
		KillBlip(s_tr.blip);
		if (s_tr.phase != TPhase::None)
			Vehicles::DeleteOrRelease(s_tr.veh);
		s_tr = Transport();
	}

	void Forget()
	{
		s_helis.clear();
		s_rappellers.clear();
		s_tr = Transport();
		s_rappelCheck = 0;
	}

	void RefreshBlips()
	{
		for (auto& h : s_helis)
		{
			KillBlip(h.blip);
			if (h.mode != HeliMode::Leaving)
				h.blip = AddHeliBlip(h.veh, BLIP_COLOUR_BLUE, "Guard Heli");
		}
		KillBlip(s_tr.blip);
		if (s_tr.phase != TPhase::None && s_tr.phase != TPhase::Leaving)
			s_tr.blip = AddHeliBlip(s_tr.veh, BLIP_COLOUR_YELLOW, "Transport Heli");
	}

	void RetaskAll()
	{
		for (auto& h : s_helis)
		{
			if (h.mode == HeliMode::Leaving)
				continue;
			if (h.rappel)
			{
				h.lastTask = 0; // keep the rappel; just re-issue the approach
				continue;
			}
			h.mode = HeliMode::None;
		}
		if (s_tr.phase != TPhase::None)
			s_tr.tasked = false;
	}

	void BuildMenu(std::vector<MenuItem>& items)
	{
		items.push_back(Menu::Header("Guard Helis"));
		int idx = 0;
		for (auto& h : s_helis)
		{
			if (h.mode == HeliMode::Leaving)
				continue;
			idx++;
			items.push_back(Menu::Header("  Heli " + std::to_string(idx) + ": " + ModeName(h) + " (" + std::to_string(h.crew.size()) + " crew)"));
		}
		items.push_back(Menu::List("Model", s_guardModel, (int)kGuardHeliModels.size(), kGuardHeliModels[s_guardModel].label,
			"Helicopter used for new guard helis. The Annihilator is armed."));
		items.push_back(Menu::List("Count", s_guardCountIdx, MAX_GUARD_HELIS, std::to_string(s_guardCountIdx + 1),
			"How many guard helis to keep on station when you call them (max 2)."));
		items.push_back(Menu::Action("Call Guard Helis", [] { CallGuardHelis(); },
			"Guard helis fly in and circle you, attack your targets and escort the transport."));
		items.push_back(Menu::Action("Rappel Guards", [] { Heli::Rappel(); },
			"Guard helis hover over you and their crews rope down to join you. Not while you're flying."));
		items.push_back(Menu::Action("Reinforce Crew", [] { Reinforce(); },
			"Refill empty heli seats with fresh crew. The heli must be out of sight or over 150 m away."));
		items.push_back(Menu::Action("Dismiss Guard Helis", [] { DismissGuardHelis(); },
			"Guard helis fly away with their crews. Guards already on the ground stay with you."));

		items.push_back(Menu::Header("Transport"));
		if (s_tr.phase != TPhase::None)
			items.push_back(Menu::Header(std::string("  Status: ") + PhaseName(s_tr.phase)));
		items.push_back(Menu::List("Model", s_trModel, (int)kTransportModels.size(), kTransportModels[s_trModel].label,
			"Helicopter used for the next transport pickup."));
		items.push_back(Menu::List("Escort Helis", s_escortCount, MAX_GUARD_HELIS + 1, std::to_string(s_escortCount),
			"Guard helis called along with the transport; they fly in formation while it cruises."));
		items.push_back(Menu::List("After Drop-off", s_afterDrop, 2, s_afterDrop == 0 ? "Wait" : "Leave",
			"Wait: the transport stays parked after you get out. Leave: it flies away."));
		items.push_back(Menu::Action("Call Pickup", [] { CallPickup(); },
			"A transport heli lands near you. Board with the enter key, then set a waypoint to fly there."));
		items.push_back(Menu::Action("Land Now", [] { LandNow(); },
			"The transport sets down right where it is instead of flying on."));
		items.push_back(Menu::Action("Dismiss Transport", [] { DismissTransport(); },
			"Send the transport away. You must be out of it first."));
	}

	void Rappel()
	{
		Ped player = PLAYER::PLAYER_PED_ID();
		bool airborne = ENTITY::IS_ENTITY_IN_AIR(player)
			|| (PED::IS_PED_IN_ANY_VEHICLE(player, FALSE) && ENTITY::IS_ENTITY_IN_AIR(PED::GET_VEHICLE_PED_IS_IN(player, FALSE)));
		if (PED::IS_PED_IN_FLYING_VEHICLE(player) || airborne)
		{
			Notify("~r~Bodyguards:~s~ can't rappel while you're in the air.");
			return;
		}
		int n = 0;
		for (auto& h : s_helis)
		{
			if (h.mode == HeliMode::Leaving || h.rappel || h.crew.empty())
				continue;
			h.rappel = true;
			h.mode = HeliMode::None;
			h.modeSince = Now();
			h.lastPoll = 0;
			n++;
		}
		if (n == 0)
			Notify("~r~Bodyguards:~s~ no heli guards to rappel.");
		else
			Notify("~b~Bodyguards:~s~ heli guards moving in to rappel.");
	}

	Vehicle TransportHeli()
	{
		return s_tr.phase != TPhase::None ? s_tr.veh : 0;
	}
}
