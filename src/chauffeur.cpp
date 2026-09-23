#include "chauffeur.h"
#include "guards.h"
#include "vehicles.h"
#include "config.h"

#include <algorithm>
#include <cstring>

namespace
{
	constexpr float ENTER_RADIUS = 10.0f;         // F near the car -> player boards a rear seat
	constexpr float ARRIVE_RADIUS = 12.0f;        // car this close to the player -> waiting
	constexpr float CLOSE_ENOUGH_DIST = 40.0f;    // stopped this close to the player -> waiting
	constexpr float CURB_APPROACH_DIST = 45.0f;   // arriving: this close to the player, head for the curb next to him
	constexpr float CURB_MAX_FROM_PLAYER = 25.0f; // ...if that curb spot is at most this far from him
	constexpr float CURB_REACHED_DIST = 2.5f;
	constexpr DWORD CURB_STALL_MS = 4000;          // stopped this long while pulling over: done
	constexpr DWORD CURB_GIVEUP_MS = 15000;
	constexpr float LEAD_HANDOFF_DIST = 80.0f;   // convoy: closer than this to the destination, stop following the lead and do the curb approach
	constexpr float FINAL_APPROACH_SPEED = 12.0f;
	constexpr float NODE_REACHED_DIST = 30.0f;    // stopped this close to the player's nearest road node -> waiting
	constexpr float CALL_NEAR_DIST = 20.0f;       // car closer than this counts as "already here"
	constexpr float TELEPORT_DIST = 250.0f;       // arriving car still this far -> teleport closer
	constexpr float STUCK_TELEPORT_DIST = 90.0f;  // stuck arriving car farther than this -> teleport closer
	constexpr float DEST_ARRIVE_DIST = 20.0f;     // 2D distance to the (road-snapped) waypoint that counts as arrived
	constexpr float DEST_STOPPED_DIST = 100.0f;   // stopped this close to the destination for a while -> arrived
	constexpr float SNAP_MAX_DIST = 150.0f;       // road node farther than this from the waypoint -> drive to the raw point
	constexpr float WP_MOVED_DIST = 10.0f;
	constexpr float PROGRESS_DIST = 5.0f;         // distance to the target must shrink by this to count as progress
	constexpr float WENT_AWAY_DIST = 25.0f;       // player must go this far before "coming back" counts
	constexpr float RETURN_DIST = 15.0f;
	constexpr float DOOR_OPEN_DIST = 8.0f;        // player on foot this close to the waiting car -> open his rear door
	constexpr float DOOR_SHUT_DIST = 15.0f;
	constexpr float SEAT_WARP_DIST = 20.0f;       // driver this close (or off-screen) is warped to the wheel when the player boards
	constexpr float DISMISS_RELEASE_DIST = 150.0f;
	constexpr DWORD EXIT_MS = 1000;
	constexpr DWORD POLL_MS = 1000;
	constexpr DWORD TELEPORT_AFTER_MS = 20000;
	constexpr DWORD ARRIVE_STOPPED_MS = 5000;     // arriving car stopped this long near the road end -> waiting
	constexpr DWORD ARRIVE_STUCK_MS = 20000;      // arriving car not getting closer this long -> teleport / give up
	constexpr DWORD DEST_STOPPED_MS = 8000;
	constexpr DWORD REISSUE_MS = 10000;           // no progress this long (and at most this often) -> re-issue the drive
	constexpr DWORD BOARD_WARP_MS = 15000;
	constexpr DWORD WARP_THROTTLE_MS = 1000;
	constexpr DWORD DISMISS_RELEASE_MS = 30000;

	enum class State
	{
		None,
		Arriving,   // driving to the player
		Waiting,    // at the wheel, player on foot
		Aboard,     // player in a rear seat
		Leaving,    // driver getting out (GuardCar / FollowYou)
		Outside,    // driver on foot guarding the car or following the player
		Returning,  // driver getting back behind the wheel
	};

	enum ExitMode { EXIT_STAY = 0, EXIT_GUARD = 1, EXIT_FOLLOW = 2 };

	const std::vector<NamedItem> s_carList = {
		{ "Schafter LWB Armored", "schafter6" },
		{ "Cognoscenti Armored", "cog552" },
		{ "Stretch Limo", "stretch" },
		{ "Baller LE LWB Armored", "baller6" },
		{ "Washington", "washington" },
		{ "Granger", "granger" },
	};
	const char* s_exitNames[] = { "Stay in car", "Guard car", "Follow you" };

	// Menu / ini settings
	int s_carIndex = 0;
	int s_modelIndex = 0;
	int s_exitMode = EXIT_STAY;
	int s_styleIndex = 0;

	// Active driver
	State s_state = State::None;
	Ped s_driver = 0;
	Vehicle s_car = 0;
	Blip s_blip = 0;
	bool s_blipOn = false;       // blip wanted (active driver, player not in the car)
	DWORD s_stateSince = 0;
	DWORD s_lastPoll = 0;
	DWORD s_lastTask = 0;
	DWORD s_lastTeleport = 0;
	DWORD s_lastWarp = 0;
	DWORD s_stuckSince = 0;      // car stopped since (0 = moving)
	DWORD s_outSince = 0;
	bool s_tasked = false;
	Vector3 s_taskTarget = {};
	int s_taskStyle = 0;
	Vehicle s_lead = 0;          // motorcade lead car to follow (convoy), 0 = drive the route himself
	Vehicle s_taskLead = 0;      // lead the current task follows (0 = drive-to task)
	float s_taskSpeed = 0.0f;    // speed the current drive task was issued with

	// Progress towards the current target (best distance so far and when it last improved)
	float s_bestDist = 0.0f;
	DWORD s_progressSince = 0;

	// Arriving
	bool s_hasRoadNode = false;
	Vector3 s_roadNode = {};     // road node closest to the player

	// Waiting: hazards + rear door
	bool s_hazards = false;
	bool s_curbing = false;      // pulling over onto the curb (a short drive-to-spot)
	Vector3 s_curbPos;
	DWORD s_curbSince = 0;
	int s_door = -1;             // rear door we opened for the player, -1 none
	bool s_doorArmed = false;    // door may open (pickup arrival, or player went away and came back)

	// Aboard
	bool s_hasWp = false;
	Vector3 s_wp = {};           // raw waypoint
	Vector3 s_dest = {};         // road-snapped destination
	bool s_destGround = false;   // snapped with a real ground height (else re-snap once it streams in)
	bool s_arrived = false;
	bool s_stopped = false;
	bool s_wpNotified = false;

	// Outside
	bool s_footTasked = false;
	bool s_wentAway = false;
	bool s_wheelNotified = false;
	int s_lastMode = -1;

	// Dismissed driver + car driving away (kept apart so a new driver can be called right away)
	Ped s_dismissDriver = 0;
	Vehicle s_dismissCar = 0;
	DWORD s_dismissSince = 0;
	bool s_dismissWander = false;

	int Style() { return s_styleIndex == 1 ? Vehicles::STYLE_RUSHED : Vehicles::STYLE_NORMAL; }
	float CruiseSpeed() { return s_styleIndex == 1 ? 40.0f : 25.0f; }

	float Dist2D(const Vector3& a, const Vector3& b)
	{
		float dx = a.x - b.x, dy = a.y - b.y;
		return std::sqrt(dx * dx + dy * dy);
	}

	// GTA heading (0 = north, counter-clockwise) pointing from `from` to `to`.
	float HeadingTo(const Vector3& from, const Vector3& to)
	{
		return std::atan2(-(to.x - from.x), to.y - from.y) * 57.29578f;
	}

	void Speak(const char* line)
	{
		AUDIO::_PLAY_AMBIENT_SPEECH1(s_driver, const_cast<char*>(line), const_cast<char*>("SPEECH_PARAMS_FORCE"));
	}

	// "Street, Zone" (either part may be missing); game strings are copied right away.
	std::string PlaceName(const Vector3& p)
	{
		Hash street = 0, crossing = 0;
		PATHFIND::GET_STREET_NAME_AT_COORD(p.x, p.y, p.z, &street, &crossing);
		std::string streetName;
		if (street != 0)
		{
			const char* s = UI::GET_STREET_NAME_FROM_HASH_KEY(street);
			if (s && *s)
				streetName = s;
		}

		std::string zoneName;
		const char* code = ZONE::GET_NAME_OF_ZONE(p.x, p.y, p.z);
		if (code && *code)
		{
			std::string zoneCode = code;
			const char* label = UI::_GET_LABEL_TEXT(const_cast<char*>(zoneCode.c_str()));
			if (label && *label && std::strcmp(label, "NULL") != 0)
				zoneName = label;
		}

		if (!streetName.empty() && !zoneName.empty())
			return streetName + ", " + zoneName;
		if (!streetName.empty())
			return streetName;
		return zoneName;
	}

	void SetHazards(bool on)
	{
		if (s_hazards == on)
			return;
		s_hazards = on;
		if (!ENTITY::DOES_ENTITY_EXIST(s_car))
			return;
		VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(s_car, 0, on);
		VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(s_car, 1, on);
	}

	void ShutDoor()
	{
		if (s_door >= 0 && ENTITY::DOES_ENTITY_EXIST(s_car))
			VEHICLE::SET_VEHICLE_DOOR_SHUT(s_car, s_door, FALSE);
		s_door = -1;
	}

	void ResetProgress(float dist)
	{
		s_bestDist = dist;
		s_progressSince = Now();
	}

	// Records `dist` as progress when it improved enough on the best so far.
	void TrackProgress(float dist)
	{
		if (dist < s_bestDist - PROGRESS_DIST)
			ResetProgress(dist);
	}

	void SetState(State s)
	{
		s_state = s;
		s_stateSince = Now();
		s_tasked = false;
		s_footTasked = false;
		s_stuckSince = 0;
	}

	void RemoveCarBlip()
	{
		Vehicles::RemoveUnitBlip(s_blip);
	}

	void AddCarBlip()
	{
		RemoveCarBlip();
		s_blip = Vehicles::AddUnitBlip(s_car, -1, BLIP_COLOUR_WHITE, "Driver");
		if (s_blip != 0)
			UI::SET_BLIP_SCALE(s_blip, 0.8f);
	}

	// One car blip while there is a driver and the player isn't inside the car.
	void UpdateBlip(Ped player)
	{
		bool want = s_state != State::None && ENTITY::DOES_ENTITY_EXIST(s_car) && !PED::IS_PED_IN_VEHICLE(player, s_car, FALSE);
		if (want == s_blipOn)
			return;
		s_blipOn = want;
		if (want)
			AddCarBlip();
		else
			RemoveCarBlip();
	}

	// At the wheel: Combat leaves him alone and he ignores gunfire instead of bailing out.
	void SetDriving(bool driving)
	{
		Guard* g = Guards::Find(s_driver);
		if (g)
			g->driving = driving;
		PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(s_driver, driving);
		PED::SET_PED_COMBAT_ATTRIBUTES(s_driver, CA_LEAVE_VEHICLES, !driving);
	}

	// Resets the active-driver state (not the dismissed slot or the settings). No natives.
	void ResetActiveVars()
	{
		s_lead = 0;
		s_taskLead = 0;
		s_curbing = false;
		s_curbSince = 0;
		s_state = State::None;
		s_driver = 0;
		s_car = 0;
		s_blip = 0;
		s_blipOn = false;
		s_stateSince = 0;
		s_lastPoll = 0;
		s_lastTask = 0;
		s_lastTeleport = 0;
		s_lastWarp = 0;
		s_stuckSince = 0;
		s_outSince = 0;
		s_tasked = false;
		s_taskTarget = {};
		s_taskStyle = 0;
		s_bestDist = 0.0f;
		s_progressSince = 0;
		s_hasRoadNode = false;
		s_roadNode = {};
		s_hazards = false;
		s_door = -1;
		s_doorArmed = false;
		s_hasWp = false;
		s_wp = {};
		s_dest = {};
		s_destGround = false;
		s_arrived = false;
		s_stopped = false;
		s_wpNotified = false;
		s_footTasked = false;
		s_wentAway = false;
		s_wheelNotified = false;
		s_lastMode = -1;
	}

	void ResetDismissVars()
	{
		s_dismissDriver = 0;
		s_dismissCar = 0;
		s_dismissSince = 0;
		s_dismissWander = false;
	}

	void ClearState()
	{
		RemoveCarBlip();
		ResetActiveVars();
	}

	bool DriverSeated()
	{
		return PED::IS_PED_IN_VEHICLE(s_driver, s_car, FALSE) && VEHICLE::GET_PED_IN_VEHICLE_SEAT(s_car, -1) == s_driver;
	}

	// Player sitting in (or getting into, if atGetIn) the car, but not at the wheel.
	bool PlayerAboard(Ped player, bool atGetIn)
	{
		return PED::IS_PED_IN_VEHICLE(player, s_car, atGetIn) && VEHICLE::GET_PED_IN_VEHICLE_SEAT(s_car, -1) != player;
	}

	bool PlayerAtWheel(Ped player)
	{
		return PED::IS_PED_IN_VEHICLE(player, s_car, FALSE) && VEHICLE::GET_PED_IN_VEHICLE_SEAT(s_car, -1) == player;
	}

	// Rear seat the player should take: the one behind the door we opened, else 1, 2, then front passenger. -2 = none.
	int PlayerSeat()
	{
		if (s_door == 2 && VEHICLE::IS_VEHICLE_SEAT_FREE(s_car, 1))
			return 1;
		if (s_door == 3 && VEHICLE::IS_VEHICLE_SEAT_FREE(s_car, 2))
			return 2;
		for (int seat : { 1, 2, 0 })
			if (VEHICLE::IS_VEHICLE_SEAT_FREE(s_car, seat))
				return seat;
		return -2;
	}

	void DriveTo(const Vector3& pos, float speed, float stopRange)
	{
		SetHazards(false);
		ShutDoor();
		PED::SET_DRIVER_ABILITY(s_driver, 1.0f);
		PED::SET_DRIVER_AGGRESSIVENESS(s_driver, s_styleIndex == 1 ? 0.5f : 0.0f);
		s_curbing = false;
		AI::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(s_driver, s_car, pos.x, pos.y, pos.z, speed, Style(), stopRange);
		s_tasked = true;
		s_taskTarget = pos;
		s_taskStyle = s_styleIndex;
		s_taskLead = 0;
		s_taskSpeed = speed;
		s_lastTask = Now();
		s_stuckSince = 0;
	}

	// Convoy: tuck in behind the motorcade lead car (the escort AI brakes for it) instead of driving the route.
	void FollowLead()
	{
		SetHazards(false);
		ShutDoor();
		s_curbing = false;
		PED::SET_DRIVER_ABILITY(s_driver, 1.0f);
		PED::SET_DRIVER_AGGRESSIVENESS(s_driver, s_styleIndex == 1 ? 0.5f : 0.0f);
		AI::TASK_VEHICLE_ESCORT(s_driver, s_car, s_lead, Vehicles::ESCORT_BEHIND, CruiseSpeed(), Style(), 7.0f, 0, 20.0f);
		AI::SET_DRIVE_TASK_MAX_CRUISE_SPEED(s_driver, CruiseSpeed());
		s_tasked = true;
		s_taskLead = s_lead;
		s_taskStyle = s_styleIndex;
		s_lastTask = Now();
		s_stuckSince = 0;
	}

	// Stops the car and keeps it there (a seated ped stays seated after CLEAR_PED_TASKS).
	void Brake()
	{
		s_curbing = false;
		s_lead = 0;
		s_taskLead = 0;
		AI::CLEAR_PED_TASKS(s_driver);
		AI::TASK_VEHICLE_TEMP_ACTION(s_driver, s_car, 27, 3000);
		s_tasked = true;
		s_lastTask = Now();
	}

	// Pull onto the right-hand curb at `curb` (slow, last metres straight). Brake() when it gets there.
	void StartCurbAt(const Vector3& curb)
	{
		s_lead = 0; // final approach: on our own now
		s_taskLead = 0;
		float speed = std::clamp(ENTITY::GET_ENTITY_SPEED(s_car), 5.0f, 10.0f);
		Vehicles::DriveToSpot(s_driver, s_car, curb, speed);
		s_curbing = true;
		s_curbPos = curb;
		s_curbSince = Now();
		s_tasked = true;
		s_lastTask = Now();
	}

	// Pull over to the side of the road (never stop in the middle of it). Brakes in place if no curb is found.
	void StopAtCurb()
	{
		Vector3 carPos = ENTITY::GET_ENTITY_COORDS(s_car, TRUE);
		float heading = ENTITY::GET_ENTITY_HEADING(s_car);
		float speed = ENTITY::GET_ENTITY_SPEED(s_car);
		float ahead = speed > 3.0f ? std::min(25.0f, 8.0f + speed) : 0.0f; // moving: pull over a bit further up the road
		Vector3 probe = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(s_car, 0.0f, ahead, 0.0f);
		Vector3 curb;
		float curbHeading;
		if (!Vehicles::CurbPoint(probe, heading, curb, curbHeading) || Dist2D(curb, carPos) > 50.0f
			|| Dist2D(curb, carPos) < CURB_REACHED_DIST)
		{
			Brake(); // no road data, or already at the curb
			return;
		}
		StartCurbAt(curb);
	}

	// True while still pulling over. When it gets there (or stalls), brakes and holds.
	bool UpdateCurb()
	{
		if (!s_curbing)
			return false;
		float d = Dist2D(ENTITY::GET_ENTITY_COORDS(s_car, TRUE), s_curbPos);
		bool stalled = ENTITY::GET_ENTITY_SPEED(s_car) < 0.3f && Elapsed(s_curbSince, CURB_STALL_MS);
		if (d > CURB_REACHED_DIST && !stalled && !Elapsed(s_curbSince, CURB_GIVEUP_MS))
			return true;
		Brake();
		return false;
	}

	// Road node 40-90 m from the player, preferably off-screen; facing the player.
	void FindArrivalSpot(Ped player, Vector3& pos, float& heading)
	{
		Vector3 ppos = ENTITY::GET_ENTITY_COORDS(player, TRUE);
		bool found = false, hidden = false;
		for (int n = 1; n <= 60 && !hidden; n++)
		{
			Vector3 node;
			float nodeHeading = 0.0f;
			Any lanes = 0;
			if (!PATHFIND::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(ppos.x, ppos.y, ppos.z, n, &node, &nodeHeading, &lanes, 0, 3.0f, 0.0f))
				continue;
			float d = Dist(node, ppos);
			if (d < 40.0f || d > 90.0f)
				continue;
			bool visible = CAM::IS_SPHERE_VISIBLE(node.x, node.y, node.z, 4.0f) != FALSE;
			if (found && visible)
				continue;
			pos = node;
			heading = nodeHeading;
			found = true;
			hidden = !visible;
		}
		if (!found)
		{
			Vehicles::SpotRelative(player, -60.0f, pos, heading);
			if (CAM::IS_SPHERE_VISIBLE(pos.x, pos.y, pos.z, 4.0f))
			{
				Vector3 alt;
				float altHeading;
				Vehicles::SpotRelative(player, 60.0f, alt, altHeading);
				if (!CAM::IS_SPHERE_VISIBLE(alt.x, alt.y, alt.z, 4.0f))
				{
					pos = alt;
					heading = altHeading;
				}
			}
		}
		float toPlayer = HeadingTo(pos, ppos);
		if (Vehicles::HeadingDiff(heading, toPlayer) > 90.0f)
			heading += 180.0f;
	}

	void TeleportNearPlayer(Ped player)
	{
		Vector3 pos;
		float heading;
		FindArrivalSpot(player, pos, heading);
		ENTITY::SET_ENTITY_COORDS(s_car, pos.x, pos.y, pos.z, FALSE, FALSE, FALSE, TRUE);
		ENTITY::SET_ENTITY_HEADING(s_car, heading);
		VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(s_car);
		s_lastTeleport = Now();
		s_tasked = false;
		s_stuckSince = 0;
		ResetProgress(EntityDist(s_car, player));
	}

	void StartArriving()
	{
		SetState(State::Arriving);
		SetHazards(false);
		ShutDoor();
		s_hasRoadNode = false;
		ResetProgress(EntityDist(s_car, PLAYER::PLAYER_PED_ID()));
	}

	// Pulled up for the player: horn, hazards, door ready to open for him.
	void ArriveForPickup(bool cantGetCloser)
	{
		SetState(State::Waiting);
		VEHICLE::START_VEHICLE_HORN(s_car, 800, GAMEPLAY::GET_HASH_KEY(const_cast<char*>("HELDDOWN")), FALSE);
		SetHazards(true);
		s_doorArmed = true;
		Notify(cantGetCloser ? "~b~Driver:~s~ your driver can't get any closer." : "~b~Driver:~s~ your car is here.");
	}

	void SpawnDriver(Ped player)
	{
		Vector3 pos;
		float heading;
		FindArrivalSpot(player, pos, heading);

		Vehicle car = Vehicles::Spawn(s_carList[s_carIndex].name, pos, heading);
		if (!car)
			return;
		const std::string& model = g_guardModels[s_modelIndex].name;
		Vector3 side = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(car, -2.0f, 0.0f, 0.0f);
		Ped driver = Guards::Create(model, side, heading, Role::Chauffeur);
		if (!driver)
		{
			Vehicles::DeleteOrRelease(car);
			return;
		}
		s_driver = driver;
		s_car = car;
		PED::SET_PED_INTO_VEHICLE(driver, car, -1);
		PED::SET_PED_CAN_BE_DRAGGED_OUT(driver, FALSE);
		VEHICLE::SET_VEHICLE_DOORS_LOCKED(car, 1);
		VEHICLE::SET_VEHICLE_ENGINE_ON(car, TRUE, TRUE, FALSE);
		SetDriving(true);
		s_blipOn = false; // UpdateBlip adds it
		s_lastTeleport = Now();
		StartArriving();
		Notify("~b~Driver:~s~ on the way.");
	}

	void StartReturning()
	{
		SetState(State::Returning);
		SetDriving(true);
		if (!PED::IS_PED_IN_VEHICLE(s_driver, s_car, FALSE))
			AI::TASK_ENTER_VEHICLE(s_driver, s_car, -1, -1, 2.0f, 1, 0);
		s_lastTask = Now();
	}

	void StartOutside()
	{
		SetState(State::Leaving);
		ShutDoor();
		AI::TASK_LEAVE_VEHICLE(s_driver, s_car, 0);
		s_lastTask = Now();
		s_wentAway = false;
		s_lastMode = -1;
	}

	void EnterAboard()
	{
		SetState(State::Aboard);
		ShutDoor();
		s_doorArmed = false;
		s_hasWp = false;
		s_arrived = false;
		s_stopped = false;
		s_wpNotified = false;
		s_lastPoll = 0;
		Speak("GENERIC_HI");
	}

	// Player boarding: the driver's seat must be his. Moves a bodyguard out of it, then warps / tasks the driver in.
	// At most one warp per second.
	void EnsureDriverSeat(Ped player)
	{
		if (DriverSeated() || !Elapsed(s_lastWarp, WARP_THROTTLE_MS))
			return;
		Ped occupant = VEHICLE::GET_PED_IN_VEHICLE_SEAT(s_car, -1);
		if (occupant != 0 && occupant != s_driver && occupant != player)
		{
			if (!Guards::IsGuard(occupant))
				return;
			s_lastWarp = Now();
			int passengers = VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(s_car);
			for (int seat = 0; seat < passengers; seat++)
			{
				if (VEHICLE::IS_VEHICLE_SEAT_FREE(s_car, seat))
				{
					PED::SET_PED_INTO_VEHICLE(occupant, s_car, seat);
					return;
				}
			}
			AI::TASK_LEAVE_VEHICLE(occupant, s_car, 0);
			return;
		}
		if (occupant != 0)
			return; // player at the wheel, or the driver himself

		if (!ENTITY::IS_ENTITY_ON_SCREEN(s_driver) || EntityDist(s_driver, s_car) < SEAT_WARP_DIST)
		{
			s_lastWarp = Now();
			PED::SET_PED_INTO_VEHICLE(s_driver, s_car, -1);
		}
		else if (!PED::IS_PED_GETTING_INTO_A_VEHICLE(s_driver))
		{
			AI::TASK_ENTER_VEHICLE(s_driver, s_car, -1, -1, 2.0f, 1, 0);
			s_lastTask = Now();
		}
	}

	// Player on foot next to the car (and it's the closest vehicle): F boards a rear seat.
	void HandleEnterControl(Ped player)
	{
		if (!Vehicles::ShouldHandleEnter(s_car, ENTER_RADIUS))
			return;
		CONTROLS::DISABLE_CONTROL_ACTION(0, 23, TRUE);
		if (!CONTROLS::IS_DISABLED_CONTROL_JUST_PRESSED(0, 23))
			return;

		int seat = PlayerSeat();
		if (seat == -2)
		{
			Notify("~b~Driver:~s~ no free seat.");
			return;
		}
		if (s_state == State::Leaving || s_state == State::Outside)
			StartReturning();
		EnsureDriverSeat(player);
		AI::TASK_ENTER_VEHICLE(player, s_car, 10000, seat, 1.0f, 1, 0);
	}

	// Waiting car: open the rear door for the approaching player, shut it once he's in or walks off.
	void UpdateDoor(Ped player)
	{
		float dist = EntityDist(player, s_car);
		if (dist > DOOR_SHUT_DIST)
			s_doorArmed = true;

		if (s_door >= 0)
		{
			bool seated = PED::IS_PED_IN_VEHICLE(player, s_car, FALSE);
			bool entering = PED::IS_PED_GETTING_INTO_A_VEHICLE(player);
			bool moving = s_state == State::Arriving || (s_state == State::Aboard && s_hasWp && !s_arrived && !s_stopped);
			if (seated || moving || (!entering && dist > DOOR_SHUT_DIST))
			{
				ShutDoor();
				if (seated)
					s_doorArmed = false;
			}
			return;
		}

		if (s_state != State::Waiting || !s_doorArmed || dist > DOOR_OPEN_DIST || !PED::IS_PED_ON_FOOT(player))
			return;
		int seat = PlayerSeat();
		if (seat != 1 && seat != 2)
			return;
		s_door = seat + 1; // seat 1 -> door 2 (rear left), seat 2 -> door 3 (rear right)
		s_doorArmed = false;
		VEHICLE::SET_VEHICLE_DOOR_OPEN(s_car, s_door, FALSE, FALSE);
	}

	void UpdateArriving(Ped player)
	{
		// Pulling onto the curb next to the player: when it gets there, it's a pickup.
		if (s_curbing)
		{
			if (!UpdateCurb())
				ArriveForPickup(false);
			return;
		}
		if (s_tasked && !Elapsed(s_lastPoll, 500))
			return;
		s_lastPoll = Now();

		Vector3 ppos = ENTITY::GET_ENTITY_COORDS(player, TRUE);
		float dist = EntityDist(s_car, player);
		bool onScreen = ENTITY::IS_ENTITY_ON_SCREEN(s_car) != FALSE;

		if (dist > TELEPORT_DIST && !onScreen && Elapsed(s_stateSince, TELEPORT_AFTER_MS) && Elapsed(s_lastTeleport, TELEPORT_AFTER_MS))
		{
			TeleportNearPlayer(player);
			dist = EntityDist(s_car, player);
		}

		// Close: pull onto the curb next to the player instead of stopping in the lane.
		if (dist < CURB_APPROACH_DIST)
		{
			Vector3 curb;
			float curbHeading;
			if (Vehicles::CurbPoint(ppos, ENTITY::GET_ENTITY_HEADING(s_car), curb, curbHeading) && Dist(curb, ppos) < CURB_MAX_FROM_PLAYER)
			{
				StartCurbAt(curb);
				return;
			}
		}

		if (dist < ARRIVE_RADIUS)
		{
			ArriveForPickup(false);
			return;
		}

		TrackProgress(dist);
		float speed = ENTITY::GET_ENTITY_SPEED(s_car);
		if (s_tasked && speed < 0.5f && Elapsed(s_lastTask, 3000))
		{
			if (s_stuckSince == 0)
				s_stuckSince = Now();
		}
		else
		{
			s_stuckSince = 0;
		}

		// Stopped at the end of the road nearest the player (player indoors, on a roof, off-road): that's as close as it gets.
		if (s_stuckSince != 0 && Elapsed(s_stuckSince, ARRIVE_STOPPED_MS))
		{
			Vector3 carPos = ENTITY::GET_ENTITY_COORDS(s_car, TRUE);
			bool atRoadEnd = s_hasRoadNode && Dist(carPos, s_roadNode) < NODE_REACHED_DIST;
			if (dist < CLOSE_ENOUGH_DIST || atRoadEnd)
			{
				ArriveForPickup(dist >= CLOSE_ENOUGH_DIST);
				return;
			}
		}

		// Not getting any closer (stuck, circling, wrong side of a barrier).
		if (s_tasked && Elapsed(s_progressSince, ARRIVE_STUCK_MS))
		{
			if (dist > STUCK_TELEPORT_DIST && !onScreen && Elapsed(s_lastTeleport, TELEPORT_AFTER_MS))
				TeleportNearPlayer(player);
			else
				ArriveForPickup(dist >= CLOSE_ENOUGH_DIST);
			return;
		}
		if (s_tasked && Elapsed(s_progressSince, REISSUE_MS) && Elapsed(s_lastTask, REISSUE_MS))
			s_tasked = false;

		bool targetMoved = s_tasked && Dist(ppos, s_taskTarget) > 20.0f;
		if (!s_tasked || targetMoved || s_taskStyle != s_styleIndex)
		{
			if (targetMoved || !s_hasRoadNode)
			{
				Vector3 node;
				s_hasRoadNode = PATHFIND::GET_CLOSEST_VEHICLE_NODE(ppos.x, ppos.y, ppos.z, &node, 1, 3.0f, 0.0f) != FALSE;
				if (s_hasRoadNode)
					s_roadNode = node;
			}
			if (targetMoved)
				ResetProgress(dist);
			DriveTo(ppos, 20.0f, 8.0f);
		}
	}

	// Road-snapped destination for the waypoint. Waypoint Z is unreliable: use the ground height when it's streamed in.
	void SnapDestination()
	{
		Vector3 probe = s_wp;
		float z = 0.0f;
		s_destGround = Vehicles::GroundZ(s_wp.x, s_wp.y, z);
		probe.z = s_destGround ? z : (s_wp.z != 0.0f ? s_wp.z : 100.0f);
		s_dest = probe;

		Vector3 node;
		if (PATHFIND::GET_CLOSEST_VEHICLE_NODE(probe.x, probe.y, probe.z, &node, 1, 3.0f, 0.0f) && Dist2D(node, s_wp) < SNAP_MAX_DIST)
			s_dest = node;
	}

	void SetDestination(const Vector3& wp)
	{
		s_hasWp = true;
		s_wp = wp;
		SnapDestination();
		s_arrived = false;
		s_stopped = false;
		s_tasked = false;
		s_wpNotified = false;
		s_stuckSince = 0;
		ResetProgress(Dist2D(ENTITY::GET_ENTITY_COORDS(s_car, TRUE), s_dest));

		std::string place = PlaceName(s_dest);
		Notify(place.empty() ? "~b~Driver:~s~ heading to the waypoint." : "~b~Driver:~s~ heading to " + place + ".");
	}

	void ArriveAtDestination()
	{
		// Curb AT the destination (not up the road: the motorcade's lead cars park in the slots ahead of it).
		Vector3 carPos = ENTITY::GET_ENTITY_COORDS(s_car, TRUE);
		Vector3 probe = Vehicles::AlongTrack(s_car, s_dest) < 0.0f ? carPos : s_dest; // already past it: pull in right here
		Vector3 curb;
		float curbHeading;
		if (Vehicles::CurbPoint(probe, ENTITY::GET_ENTITY_HEADING(s_car), curb, curbHeading) && Dist2D(curb, carPos) > CURB_REACHED_DIST && Dist2D(curb, carPos) < 50.0f)
			StartCurbAt(curb);
		else
			Brake();
		s_arrived = true;
		UI::SET_WAYPOINT_OFF();
		Speak(GAMEPLAY::GET_RANDOM_INT_IN_RANGE(0, 2) == 0 ? "GENERIC_THANKS" : "GENERIC_BYE");
		std::string place = PlaceName(ENTITY::GET_ENTITY_COORDS(s_car, TRUE));
		Notify(place.empty() ? "~b~Driver:~s~ arrived." : "~b~Driver:~s~ arrived at " + place + ".");
	}

	void UpdateAboard(Ped player)
	{
		if (UpdateCurb())
			return;
		if (Elapsed(s_lastPoll, POLL_MS))
		{
			s_lastPoll = Now();
			Vector3 wp;
			if (Vehicles::GetWaypoint(wp))
			{
				if (!s_hasWp || Dist2D(wp, s_wp) > WP_MOVED_DIST)
				{
					SetDestination(wp);
				}
				else if (!s_destGround && !s_arrived)
				{
					// Ground wasn't streamed in when the waypoint was set: re-snap now that it may be.
					Vector3 old = s_dest;
					SnapDestination();
					if (s_destGround && Dist(old, s_dest) > 5.0f && !s_stopped)
						s_tasked = false;
				}
			}
			else if (s_hasWp)
			{
				s_hasWp = false;
				if (!s_arrived)
				{
					StopAtCurb();
					Notify("~b~Driver:~s~ waypoint removed, pulling over.");
				}
			}
			else if (!s_wpNotified && !s_arrived)
			{
				s_wpNotified = true;
				Brake();
				Notify("~b~Driver:~s~ set a waypoint.");
			}
		}

		if (!s_hasWp || s_arrived || s_stopped)
		{
			if (!s_tasked)
				Brake(); // hold position (also after a retask)
			return;
		}

		Vector3 carPos = ENTITY::GET_ENTITY_COORDS(s_car, TRUE);
		float dist = Dist2D(carPos, s_dest);
		if (dist < DEST_ARRIVE_DIST)
		{
			ArriveAtDestination();
			return;
		}
		TrackProgress(dist);

		// Stopped close to an off-road waypoint: as close as the roads go.
		if (s_tasked && ENTITY::GET_ENTITY_SPEED(s_car) < 0.5f)
		{
			if (s_stuckSince == 0)
				s_stuckSince = Now();
			else if (Elapsed(s_stuckSince, DEST_STOPPED_MS) && std::min(dist, Dist2D(carPos, s_wp)) < DEST_STOPPED_DIST)
			{
				ArriveAtDestination();
				return;
			}
		}
		else
		{
			s_stuckSince = 0;
		}

		// Far away and not getting closer (not just a red light): re-issue, at most every REISSUE_MS.
		if (s_tasked && dist > DEST_STOPPED_DIST && Elapsed(s_progressSince, REISSUE_MS) && Elapsed(s_lastTask, REISSUE_MS))
			s_tasked = false;

		// Convoy: follow the lead car while it is ahead and we are not yet on the final approach.
		bool leadAhead = IsAlive(s_lead) && dist > LEAD_HANDOFF_DIST
			&& Vehicles::AlongTrack(s_car, ENTITY::GET_ENTITY_COORDS(s_lead, TRUE)) > 5.0f;
		if (leadAhead)
		{
			if (!s_tasked || s_taskStyle != s_styleIndex || s_taskLead != s_lead)
				FollowLead();
		}
		else
		{
			// Final approach: ease off so he never charges at lead cars parking ahead of the destination.
			float speed = dist < LEAD_HANDOFF_DIST ? std::min(CruiseSpeed(), FINAL_APPROACH_SPEED) : CruiseSpeed();
			if (!s_tasked || s_taskStyle != s_styleIndex || s_taskLead != 0 || s_taskSpeed != speed)
				DriveTo(s_dest, speed, 15.0f);
		}
	}

	void UpdateOutside(Ped player)
	{
		float carDist = EntityDist(player, s_car);
		if (carDist > WENT_AWAY_DIST)
			s_wentAway = true;
		if (s_wentAway && carDist < RETURN_DIST && PED::IS_PED_ON_FOOT(player) && !PlayerAtWheel(player))
		{
			StartReturning();
			return;
		}

		Guard* g = Guards::Find(s_driver);
		if (!g || g->target != 0)
			return; // Combat is using him; it flags needsRetask when it stands him down

		if (s_exitMode != s_lastMode) // menu changed Guard <-> Follow while he's outside
		{
			s_lastMode = s_exitMode;
			s_footTasked = false;
		}

		if (s_exitMode == EXIT_GUARD)
		{
			if (!s_footTasked)
			{
				AI::TASK_GUARD_CURRENT_POSITION(s_driver, 10.0f, 10.0f, TRUE);
				s_footTasked = true;
				s_lastTask = Now();
			}
			return;
		}

		// Follow: re-issue when he has stopped short of the player, or periodically.
		bool lagging = EntityDist(s_driver, player) > 5.0f && ENTITY::GET_ENTITY_SPEED(s_driver) < 0.5f;
		if (!s_footTasked || (lagging && Elapsed(s_lastTask, 2000)) || Elapsed(s_lastTask, 8000))
		{
			AI::TASK_FOLLOW_TO_OFFSET_OF_ENTITY(s_driver, player, 1.5f, -1.5f, 0.0f, 2.0f, -1, 2.0f, TRUE);
			s_footTasked = true;
			s_lastTask = Now();
		}
	}

	void UpdateReturning(Ped player)
	{
		if (PlayerAtWheel(player))
		{
			// Player grabbed the wheel: don't fight over it.
			SetState(State::Outside);
			SetDriving(false);
			return;
		}
		if (DriverSeated())
		{
			if (PlayerAboard(player, FALSE))
				EnterAboard();
			else if (EntityDist(s_car, player) > CALL_NEAR_DIST)
				StartArriving();
			else
				SetState(State::Waiting);
			return;
		}
		// Player is (getting) in: the wheel must be taken now.
		if (PED::IS_PED_IN_VEHICLE(player, s_car, TRUE))
		{
			EnsureDriverSeat(player);
			return;
		}
		if ((Elapsed(s_stateSince, BOARD_WARP_MS) || EntityDist(s_driver, s_car) > 60.0f) && Elapsed(s_lastWarp, WARP_THROTTLE_MS)
			&& VEHICLE::IS_VEHICLE_SEAT_FREE(s_car, -1))
		{
			s_lastWarp = Now();
			PED::SET_PED_INTO_VEHICLE(s_driver, s_car, -1);
			return;
		}
		if (!PED::IS_PED_GETTING_INTO_A_VEHICLE(s_driver) && Elapsed(s_lastTask, 4000))
		{
			AI::TASK_ENTER_VEHICLE(s_driver, s_car, -1, -1, 2.0f, 1, 0);
			s_lastTask = Now();
		}
	}

	// Re-issues whatever the driver should be doing right now (after Combat stood him down, regroup, ...).
	void ReissueTask()
	{
		switch (s_state)
		{
		case State::Arriving:
		case State::Waiting:
		case State::Aboard:
			s_tasked = false;
			break;
		case State::Outside:
			s_footTasked = false;
			break;
		case State::Leaving:
			AI::TASK_LEAVE_VEHICLE(s_driver, s_car, 0);
			s_lastTask = Now();
			break;
		case State::Returning:
			if (!PED::IS_PED_IN_VEHICLE(s_driver, s_car, FALSE))
				AI::TASK_ENTER_VEHICLE(s_driver, s_car, -1, -1, 2.0f, 1, 0);
			s_lastTask = Now();
			break;
		default:
			break;
		}
	}

	void UpdateActive(Ped player)
	{
		// Robustness: driver dead / gone (Guards prunes dead guards), car destroyed.
		if (!Guards::Find(s_driver) || !IsAlive(s_driver))
		{
			Notify("~r~Driver:~s~ your driver is dead.");
			SetHazards(false);
			ShutDoor();
			Vehicles::Release(s_car);
			ClearState();
			return;
		}
		if (!ENTITY::DOES_ENTITY_EXIST(s_car) || !VEHICLE::IS_VEHICLE_DRIVEABLE(s_car, FALSE))
		{
			Notify("~r~Driver:~s~ your car was destroyed.");
			Vehicles::Release(s_car);
			Guards::Remove(s_driver, false);
			ClearState();
			return;
		}

		Guard* g = Guards::Find(s_driver);
		if (g && g->needsRetask)
		{
			g->needsRetask = false;
			ReissueTask();
		}

		HandleEnterControl(player);

		// Driver should be at the wheel but isn't (dragged out, bailed) -> get back in.
		bool wheelState = s_state == State::Arriving || s_state == State::Waiting || s_state == State::Aboard;
		if (wheelState && !PED::IS_PED_IN_VEHICLE(s_driver, s_car, FALSE) && !PED::IS_PED_GETTING_INTO_A_VEHICLE(s_driver))
		{
			StartReturning();
			return;
		}

		bool aboard = PlayerAboard(player, FALSE);
		if (aboard)
			s_outSince = 0;

		switch (s_state)
		{
		case State::Arriving:
			if (aboard)
				EnterAboard();
			else
				UpdateArriving(player);
			break;

		case State::Waiting:
			if (aboard)
				EnterAboard();
			else if (!s_tasked)
				Brake(); // hold position (also re-applied after a retask)
			break;

		case State::Aboard:
			if (!PlayerAboard(player, TRUE))
			{
				if (s_outSince == 0)
					s_outSince = Now();
				if (Elapsed(s_outSince, EXIT_MS))
				{
					s_outSince = 0;
					if (s_exitMode == EXIT_STAY)
						SetState(State::Waiting);
					else
						StartOutside();
				}
				break;
			}
			s_outSince = 0;
			UpdateAboard(player);
			break;

		case State::Leaving:
			if (s_exitMode == EXIT_STAY)
				StartReturning(); // menu switched to "Stay in car"
			else if (PED::IS_PED_ON_FOOT(s_driver))
			{
				SetState(State::Outside);
				SetDriving(false); // only now: he stays a "driver" (no combat orders) until he's actually out
			}
			else if (Elapsed(s_lastTask, 4000))
			{
				AI::TASK_LEAVE_VEHICLE(s_driver, s_car, 0);
				s_lastTask = Now();
			}
			break;

		case State::Outside:
			if (PlayerAtWheel(player))
			{
				if (!s_wheelNotified)
				{
					s_wheelNotified = true;
					Notify("~b~Driver:~s~ you took the wheel; your driver waits outside.");
				}
				break;
			}
			s_wheelNotified = false;
			if (aboard || s_exitMode == EXIT_STAY)
			{
				StartReturning();
				break;
			}
			UpdateOutside(player);
			break;

		case State::Returning:
			UpdateReturning(player);
			break;

		default:
			break;
		}

		if (s_state != State::None)
			UpdateDoor(player);
	}

	// Dismissed pair: drives off, released once far away / after a while, or right away with force.
	void UpdateDismissed(Ped player, bool force)
	{
		if (!s_dismissDriver && !s_dismissCar)
			return;
		bool driverOk = Guards::Find(s_dismissDriver) && IsAlive(s_dismissDriver);
		bool carOk = ENTITY::DOES_ENTITY_EXIST(s_dismissCar);
		bool release = force || !driverOk || !carOk || Elapsed(s_dismissSince, DISMISS_RELEASE_MS)
			|| EntityDist(player, s_dismissCar) > DISMISS_RELEASE_DIST;

		if (!release && !s_dismissWander && PED::IS_PED_IN_VEHICLE(s_dismissDriver, s_dismissCar, FALSE))
		{
			AI::TASK_VEHICLE_DRIVE_WANDER(s_dismissDriver, s_dismissCar, 20.0f, Vehicles::STYLE_NORMAL);
			s_dismissWander = true;
		}
		if (!release)
			return;

		Ped driver = s_dismissDriver;
		if (Guards::Find(driver))
			Guards::Remove(driver, false); // resets his buffs and releases him
		if (driverOk && ENTITY::DOES_ENTITY_EXIST(driver) && s_dismissWander)
			PED::SET_PED_KEEP_TASK(driver, TRUE); // after Remove (which clears it): keep wandering as an ambient driver
		Vehicles::Release(s_dismissCar);
		ResetDismissVars();
	}

	// Hands the active pair over to the "leaving" slot (drives off, released later).
	void Dismiss()
	{
		if (s_state == State::None)
		{
			Notify("~r~Driver:~s~ no driver.");
			return;
		}
		Ped player = PLAYER::PLAYER_PED_ID();
		if (PED::IS_PED_IN_VEHICLE(player, s_car, TRUE))
		{
			Notify("~r~Driver:~s~ get out of the car first.");
			return;
		}

		// Only one leaving pair at a time: release any previous one now.
		UpdateDismissed(player, true);

		SetHazards(false);
		ShutDoor();
		SetDriving(true);
		if (!PED::IS_PED_IN_VEHICLE(s_driver, s_car, FALSE))
		{
			if (EntityDist(s_driver, s_car) > 30.0f || !ENTITY::IS_ENTITY_ON_SCREEN(s_driver))
				PED::SET_PED_INTO_VEHICLE(s_driver, s_car, -1);
			else
				AI::TASK_ENTER_VEHICLE(s_driver, s_car, -1, -1, 2.0f, 1, 0);
		}
		RemoveCarBlip();
		s_dismissDriver = s_driver;
		s_dismissCar = s_car;
		s_dismissSince = Now();
		s_dismissWander = false;
		ClearState();
		Notify("~b~Driver:~s~ dismissed.");
	}

	void CallDriver()
	{
		Ped player = PLAYER::PLAYER_PED_ID();
		switch (s_state)
		{
		case State::None:
			SpawnDriver(player);
			break;

		case State::Arriving:
			Notify("~b~Driver:~s~ already on the way.");
			break;

		case State::Aboard:
			Notify("~b~Driver:~s~ you're in the car.");
			break;

		case State::Waiting:
			if (EntityDist(s_car, player) > CALL_NEAR_DIST)
			{
				StartArriving();
				Notify("~b~Driver:~s~ on the way.");
			}
			else
			{
				Notify("~b~Driver:~s~ your car is here.");
			}
			break;

		case State::Returning:
			Notify("~b~Driver:~s~ getting back in the car.");
			break;

		case State::Leaving:
		case State::Outside:
			// Far from the car (e.g. followed you on foot): put him straight back at the wheel.
			if (EntityDist(s_driver, s_car) > 60.0f && VEHICLE::IS_VEHICLE_SEAT_FREE(s_car, -1))
			{
				SetDriving(true);
				PED::SET_PED_INTO_VEHICLE(s_driver, s_car, -1);
				s_lastWarp = Now();
				if (EntityDist(s_car, player) > TELEPORT_DIST && !ENTITY::IS_ENTITY_ON_SCREEN(s_car))
					TeleportNearPlayer(player);
				StartArriving();
			}
			else
			{
				StartReturning(); // Returning -> Arriving once seated if the car is far
			}
			Notify("~b~Driver:~s~ on the way.");
			break;
		}
	}

	void GoToWaypoint()
	{
		if (s_state != State::Aboard)
		{
			Notify(s_state == State::None ? "~r~Driver:~s~ call a driver first." : "~r~Driver:~s~ get in the car first.");
			return;
		}
		Vector3 wp;
		if (!Vehicles::GetWaypoint(wp))
		{
			Notify("~r~Driver:~s~ set a waypoint first.");
			return;
		}
		SetDestination(wp);
		s_lastPoll = Now();
	}

	// Taxi-style "hurry up": rushed driving (the game's own reckless-but-avoiding AI), higher speed.
	// A style change makes the drive task re-issue on the next update, so it applies mid-trip.
	void SetHurry(bool on)
	{
		s_styleIndex = on ? 1 : 0;
		if (s_state == State::None)
			return;
		Notify(on ? "~b~Driver:~s~ hurrying." : "~b~Driver:~s~ taking it easy.");
		if (IsAlive(s_driver))
			AUDIO::_PLAY_AMBIENT_SPEECH1(s_driver, const_cast<char*>("GENERIC_YES"), const_cast<char*>("SPEECH_PARAMS_FORCE"));
	}

	void PullOver()
	{
		if (s_state == State::Aboard || s_state == State::Arriving)
		{
			if (s_state == State::Arriving)
				SetState(State::Waiting); // Waiting brakes on its first update
			else
				StopAtCurb();
			s_stopped = true;
			Notify("~b~Driver:~s~ pulling over.");
		}
		else
		{
			Notify("~r~Driver:~s~ not driving.");
		}
	}

	std::string StatusText()
	{
		switch (s_state)
		{
		case State::None: return s_dismissDriver ? "Driving away (dismissed)" : "No driver";
		case State::Arriving: return "Coming to you";
		case State::Waiting: return "Waiting for you";
		case State::Aboard:
			if (s_arrived) return "Arrived";
			if (s_stopped) return "Pulled over";
			if (s_hasWp) return "Driving to waypoint";
			return "Waiting for a waypoint";
		case State::Leaving: return "Getting out";
		case State::Outside: return s_exitMode == EXIT_FOLLOW ? "Following you" : "Guarding the car";
		case State::Returning: return "Getting back in";
		}
		return "";
	}
}

namespace Chauffeur
{
	void LoadConfig()
	{
		s_carIndex = FindIndex(s_carList, ReadString("Driver", "CarModel", "schafter6"), 0);
		s_modelIndex = FindIndex(g_guardModels, ReadString("Driver", "DriverModel", "s_m_m_highsec_01"), 0);

		std::string exit = Upper(ReadString("Driver", "ExitBehaviour", "StayInCar"));
		s_exitMode = exit == "GUARDCAR" ? EXIT_GUARD : exit == "FOLLOWYOU" ? EXIT_FOLLOW : EXIT_STAY;

		std::string style = Upper(ReadString("Driver", "DrivingStyle", "Normal"));
		s_styleIndex = (style == "FAST" || style == "HURRY") ? 1 : 0;
	}

	void Update()
	{
		Ped player = PLAYER::PLAYER_PED_ID();
		UpdateDismissed(player, false);
		if (s_state != State::None)
			UpdateActive(player);
		UpdateBlip(player);
	}

	void Reset()
	{
		// Guards::DismissAll already deleted the peds.
		RemoveCarBlip();
		Vehicles::DeleteOrRelease(s_car);
		Vehicles::DeleteOrRelease(s_dismissCar);
		ClearState();
		ResetDismissVars();
	}

	void RetaskAll()
	{
		if (s_state != State::None)
			ReissueTask();
	}

	void BuildMenu(std::vector<MenuItem>& items)
	{
		items.push_back(Menu::Header("Status: " + StatusText()));
		items.push_back(Menu::List("Car", s_carIndex, (int)s_carList.size(), s_carList[s_carIndex].label,
			"Car your next driver brings. Applies the next time you call a driver."));
		items.push_back(Menu::List("Driver model", s_modelIndex, (int)g_guardModels.size(), g_guardModels[s_modelIndex].label,
			"Who drives. Applies the next time you call a driver."));
		items.push_back(Menu::List("When you get out", s_exitMode, 3, s_exitNames[s_exitMode],
			"Stay in car: he waits at the wheel. Guard car: he gets out and guards it. Follow you: he walks with you."));
		items.push_back(MenuItem{ "Hurry", Menu::OnOff(s_styleIndex == 1), [] { SetHurry(s_styleIndex != 1); }, [](int) { SetHurry(s_styleIndex != 1); },
			"Like telling a taxi to hurry: he drives faster and more recklessly, weaving through traffic. Off = obeys traffic. Works mid-trip (or press the driver key while he's driving)." });
		items.push_back(Menu::Action("Call driver", [] { CallDriver(); },
			"Spawns a driver who comes to you, or calls your current one back. Press F near the car to get in the back."));
		items.push_back(Menu::Action("Go to waypoint", [] { GoToWaypoint(); },
			"While in the back: drives to the map waypoint (snapped to the nearest road). Setting a waypoint also starts the trip."));
		items.push_back(Menu::Action("Stop / pull over", [] { PullOver(); },
			"Stops the car where it is. Set or re-select a waypoint to continue."));
		items.push_back(Menu::Action("Dismiss driver", [] { Dismiss(); },
			"Sends the driver away; he drives off and is released. You must be out of the car."));
	}

	void OnHotkey()
	{
		// While he's driving you somewhere, the key toggles hurry (like a taxi); otherwise start the trip or call him.
		if (s_state == State::Aboard && s_hasWp && !s_arrived && !s_stopped)
			SetHurry(s_styleIndex != 1);
		else if (s_state == State::Aboard)
			GoToWaypoint();
		else
			CallDriver();
	}

	Vehicle Car()
	{
		return s_state != State::None && ENTITY::DOES_ENTITY_EXIST(s_car) ? s_car : 0;
	}

	bool IsHurrying() { return s_styleIndex == 1; }

	bool HasDestination(Vector3& dest)
	{
		if (s_state != State::Aboard || !s_hasWp || s_arrived || s_stopped)
			return false;
		dest = s_dest;
		return true;
	}

	float DesiredSpeed() { return CruiseSpeed(); }

	void SetLeadVehicle(Vehicle v) { s_lead = v; }

	Vehicle LeadVehicle() { return IsAlive(s_lead) ? s_lead : 0; }

	bool IsParked()
	{
		if (s_state == State::None || s_curbing)
			return false;
		switch (s_state)
		{
		case State::Waiting:
		case State::Leaving:
		case State::Outside:
		case State::Returning:
			return true;
		case State::Aboard:
			return s_arrived || s_stopped || !s_hasWp;
		default:
			return false;
		}
	}

	bool OwnsVehicle(Vehicle v)
	{
		return v != 0 && ((s_state != State::None && v == s_car) || v == s_dismissCar);
	}

	void Forget()
	{
		ResetActiveVars();
		ResetDismissVars();
	}

	void RefreshBlips()
	{
		RemoveCarBlip();
		if (s_blipOn)
			AddCarBlip();
	}
}
