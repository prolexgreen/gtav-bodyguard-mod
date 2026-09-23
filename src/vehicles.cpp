#include "vehicles.h"

#include "registry.h"
#include "config.h"

#include <algorithm>

namespace Vehicles
{
	float HeadingDiff(float a, float b)
	{
		float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
		return std::fabs(d);
	}

	bool IsLandVehicle(Vehicle v)
	{
		Hash m = ENTITY::GET_ENTITY_MODEL(v);
		return VEHICLE::IS_THIS_MODEL_A_CAR(m) || VEHICLE::IS_THIS_MODEL_A_BIKE(m) || VEHICLE::IS_THIS_MODEL_A_QUADBIKE(m);
	}

	bool IsEmpty(Vehicle v)
	{
		if (!VEHICLE::IS_VEHICLE_SEAT_FREE(v, -1))
			return false;
		int passengers = VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(v);
		for (int s = 0; s < passengers; s++)
			if (!VEHICLE::IS_VEHICLE_SEAT_FREE(v, s))
				return false;
		return true;
	}

	int Capacity(Vehicle v, int max)
	{
		return std::min(VEHICLE::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(v) + 1, max);
	}

	int SeatFor(size_t riderIndex) { return riderIndex == 0 ? -1 : (int)riderIndex - 1; }

	void SpotRelative(Entity e, float forward, Vector3& pos, float& heading)
	{
		Vector3 want = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(e, 0.0f, forward, 0.0f);
		float entityHeading = ENTITY::GET_ENTITY_HEADING(e);
		pos = want;
		heading = entityHeading;

		Vector3 node;
		float nodeHeading = 0.0f;
		if (PATHFIND::GET_CLOSEST_VEHICLE_NODE_WITH_HEADING(want.x, want.y, want.z, &node, &nodeHeading, 1, 3.0f, 0)
			&& Dist(node, want) < 30.0f)
		{
			pos = node;
			heading = HeadingDiff(nodeHeading, entityHeading) > 90.0f ? nodeHeading + 180.0f : nodeHeading;
		}
	}

	Vehicle Spawn(const std::string& model, const Vector3& pos, float heading)
	{
		Hash hash = Joaat(model);
		if (!LoadModel(hash))
		{
			Notify("~r~Bodyguards:~s~ couldn't load vehicle " + model);
			return 0;
		}
		Vehicle v = VEHICLE::CREATE_VEHICLE(hash, pos.x, pos.y, pos.z, heading, FALSE, TRUE);
		STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(hash);
		if (!ENTITY::DOES_ENTITY_EXIST(v))
			return 0;
		ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, TRUE, TRUE);
		Registry::Track(v);
		VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(v);
		VEHICLE::SET_VEHICLE_ENGINE_ON(v, TRUE, TRUE, FALSE);
		return v;
	}

	Vehicle SpawnHeliInAir(const std::string& model, const Vector3& pos, float heading, float forwardSpeed)
	{
		Hash hash = Joaat(model);
		if (!LoadModel(hash))
		{
			Notify("~r~Bodyguards:~s~ couldn't load helicopter " + model);
			return 0;
		}
		Vehicle v = VEHICLE::CREATE_VEHICLE(hash, pos.x, pos.y, pos.z, heading, FALSE, TRUE);
		STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(hash);
		if (!ENTITY::DOES_ENTITY_EXIST(v))
			return 0;
		ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, TRUE, TRUE);
		Registry::Track(v);
		VEHICLE::SET_VEHICLE_ENGINE_ON(v, TRUE, TRUE, FALSE);
		VEHICLE::SET_HELI_BLADES_FULL_SPEED(v);
		VEHICLE::SET_VEHICLE_FORWARD_SPEED(v, forwardSpeed);
		return v;
	}

	void TeleportBehind(Vehicle v, Entity target, float back)
	{
		Vector3 pos;
		float heading;
		SpotRelative(target, -back, pos, heading);
		ENTITY::SET_ENTITY_COORDS(v, pos.x, pos.y, pos.z, FALSE, FALSE, FALSE, TRUE);
		ENTITY::SET_ENTITY_HEADING(v, heading);
		VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(v);
		VEHICLE::SET_VEHICLE_FORWARD_SPEED(v, ENTITY::GET_ENTITY_SPEED(target));
	}

	void DeleteOrRelease(Vehicle v)
	{
		if (!ENTITY::DOES_ENTITY_EXIST(v))
			return;
		Registry::Untrack(v);
		if (!PED::IS_PED_IN_VEHICLE(PLAYER::PLAYER_PED_ID(), v, FALSE))
			VEHICLE::DELETE_VEHICLE(&v);
		else
			ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&v);
	}

	void Release(Vehicle v)
	{
		Registry::Untrack(v);
		if (ENTITY::DOES_ENTITY_EXIST(v))
			ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&v);
	}

	Blip AddUnitBlip(Entity e, int sprite, int colour, const char* name)
	{
		if (!g_cfg.blips || !ENTITY::DOES_ENTITY_EXIST(e))
			return 0;
		Blip b = UI::ADD_BLIP_FOR_ENTITY(e);
		if (sprite >= 0)
			UI::SET_BLIP_SPRITE(b, sprite);
		UI::SET_BLIP_COLOUR(b, colour);
		UI::SET_BLIP_AS_FRIENDLY(b, TRUE);
		UI::BEGIN_TEXT_COMMAND_SET_BLIP_NAME(const_cast<char*>("STRING"));
		UI::_ADD_TEXT_COMPONENT_STRING(const_cast<char*>(name));
		UI::END_TEXT_COMMAND_SET_BLIP_NAME(b);
		return b;
	}

	void RemoveUnitBlip(Blip& b)
	{
		if (b != 0 && UI::DOES_BLIP_EXIST(b))
			UI::REMOVE_BLIP(&b);
		b = 0;
	}

	bool ShouldHandleEnter(Vehicle v, float maxDist)
	{
		Ped player = PLAYER::PLAYER_PED_ID();
		if (v == 0 || !ENTITY::DOES_ENTITY_EXIST(v) || !PED::IS_PED_ON_FOOT(player) || PED::IS_PED_GETTING_INTO_A_VEHICLE(player))
			return false;
		if (ENTITY::GET_ENTITY_SPEED(v) > 2.0f)
			return false;
		Vector3 ppos = ENTITY::GET_ENTITY_COORDS(player, TRUE);
		float mine = Dist(ppos, ENTITY::GET_ENTITY_COORDS(v, TRUE));
		if (mine > maxDist)
			return false;

		// Closest vehicle of all, cached per frame (several modules may ask in the same frame).
		static DWORD cachedAt = 0xFFFFFFFF;
		static Vehicle closest = 0;
		DWORD now = Now();
		if (now != cachedAt)
		{
			cachedAt = now;
			closest = 0;
			float best = 1e9f;
			static int arr[1024];
			int n = worldGetAllVehicles(arr, 1024);
			for (int i = 0; i < n; i++)
			{
				float d = Dist(ppos, ENTITY::GET_ENTITY_COORDS(arr[i], TRUE));
				if (d < best)
				{
					best = d;
					closest = arr[i];
				}
			}
		}
		return closest == v;
	}

	bool CurbPoint(const Vector3& at, float travelHeading, Vector3& out, float& outHeading)
	{
		constexpr float LANE_WIDTH = 5.0f;  // approximate GTA lane width
		constexpr float CURB_MARGIN = 2.2f; // keep the car's centre this far inside the road edge
		constexpr float DEG = 3.14159265f / 180.0f;

		// GET_CLOSEST_ROAD with its real parameter types (the SDK stub types them as Any).
		Vector3 src, dst;
		int lanesFwd = 0, lanesBack = 0;
		float median = 0.0f;
		if (!invoke<BOOL>(0x132F52BBA570FE92, at.x, at.y, at.z, 1.0f, 1, &src, &dst, &lanesFwd, &lanesBack, &median, FALSE))
			return false;

		float dx = dst.x - src.x, dy = dst.y - src.y;
		float len = std::sqrt(dx * dx + dy * dy);
		if (len < 0.5f)
			return false;
		dx /= len;
		dy /= len;

		// Which way along the road we travel (GTA heading: forward = (-sin h, cos h)).
		float tx = -std::sin(travelHeading * DEG), ty = std::cos(travelHeading * DEG);
		bool withRoad = dx * tx + dy * ty >= 0.0f;
		float ux = withRoad ? dx : -dx, uy = withRoad ? dy : -dy;
		int ours = withRoad ? lanesFwd : lanesBack;
		int theirs = withRoad ? lanesBack : lanesFwd;
		if (ours <= 0)
			ours = 1;

		// Project onto the road line through the nodes.
		float t = (at.x - src.x) * dx + (at.y - src.y) * dy;
		Vector3 c = at;
		c.x = src.x + dx * t;
		c.y = src.y + dy * t;
		c.z = src.z + (dst.z - src.z) * std::clamp(t / len, 0.0f, 1.0f);

		// Two-way road: nodes sit on the centre line, our lanes are to its right.
		// One-way road: nodes sit in the middle of all lanes.
		float offset = theirs > 0
			? median * 0.5f + (float)ours * LANE_WIDTH - CURB_MARGIN
			: (float)ours * LANE_WIDTH * 0.5f - CURB_MARGIN;
		offset = std::max(offset, 0.0f);

		// Right of the travel direction is (uy, -ux).
		out = c;
		out.x = c.x + uy * offset;
		out.y = c.y - ux * offset;
		float z = 0.0f;
		if (GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(out.x, out.y, c.z + 3.0f, &z, 0))
			out.z = z;
		outHeading = std::atan2(-ux, uy) / DEG;
		if (outHeading < 0.0f)
			outHeading += 360.0f;
		return true;
	}

	void DriveToSpot(Ped driver, Vehicle v, const Vector3& pos, float speed)
	{
		AI::TASK_VEHICLE_DRIVE_TO_COORD(driver, v, pos.x, pos.y, pos.z, speed, 1, ENTITY::GET_ENTITY_MODEL(v),
			STYLE_NORMAL, 1.5f, 15.0f);
	}

	float AlongTrack(Entity ref, const Vector3& pos)
	{
		Vector3 fwd = ENTITY::GET_ENTITY_FORWARD_VECTOR(ref);
		Vector3 rp = ENTITY::GET_ENTITY_COORDS(ref, TRUE);
		float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y);
		if (len < 0.01f)
			return 0.0f;
		return ((pos.x - rp.x) * fwd.x + (pos.y - rp.y) * fwd.y) / len;
	}

	float OffTrack(Entity ref, const Vector3& pos)
	{
		Vector3 fwd = ENTITY::GET_ENTITY_FORWARD_VECTOR(ref);
		Vector3 rp = ENTITY::GET_ENTITY_COORDS(ref, TRUE);
		float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y);
		if (len < 0.01f)
			return 0.0f;
		return std::fabs((pos.x - rp.x) * fwd.y - (pos.y - rp.y) * fwd.x) / len;
	}

	void TuneForPursuit(Vehicle v)
	{
		if (!ENTITY::DOES_ENTITY_EXIST(v))
			return;
		VEHICLE::SET_VEHICLE_MOD_KIT(v, 0);
		for (int modType : { 11, 12, 13, 15, 16 }) // engine, brakes, transmission, suspension, armour
		{
			int n = VEHICLE::GET_NUM_VEHICLE_MODS(v, modType);
			if (n > 0)
				VEHICLE::SET_VEHICLE_MOD(v, modType, n - 1, FALSE);
		}
		VEHICLE::TOGGLE_VEHICLE_MOD(v, 18, TRUE); // turbo
		VEHICLE::SET_VEHICLE_TYRES_CAN_BURST(v, FALSE);
		VEHICLE::SET_VEHICLE_HAS_STRONG_AXLES(v, TRUE);
		VEHICLE::SET_VEHICLE_STRONG(v, TRUE);
	}

	void SetPowerBoost(Vehicle v, float percent)
	{
		if (ENTITY::DOES_ENTITY_EXIST(v))
			VEHICLE::_SET_VEHICLE_ENGINE_POWER_MULTIPLIER(v, percent);
	}

	bool GetWaypoint(Vector3& out)
	{
		if (!UI::IS_WAYPOINT_ACTIVE())
			return false;
		Blip b = UI::GET_FIRST_BLIP_INFO_ID(8);
		if (!UI::DOES_BLIP_EXIST(b))
			return false;
		out = UI::GET_BLIP_INFO_ID_COORD(b);
		return true;
	}

	bool GroundZ(float x, float y, float& z)
	{
		return GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(x, y, 1000.0f, &z, 0) != FALSE;
	}
}
