#pragma once

#include "common.h"

// Shared vehicle helpers used by escort, chauffeur, motorcade and heli modules.
namespace Vehicles
{
	// Driving styles
	constexpr int STYLE_NORMAL = 786603;
	constexpr int STYLE_RUSHED = 1074528293;

	// TASK_HELI_MISSION mission types
	constexpr int HELI_GOTO = 4;
	constexpr int HELI_ATTACK = 6;
	constexpr int HELI_FOLLOW = 7;
	constexpr int HELI_CIRCLE = 9;
	constexpr int HELI_ESCORT_LEFT = 10;
	constexpr int HELI_ESCORT_RIGHT = 11;
	constexpr int HELI_ESCORT_REAR = 12;
	constexpr int HELI_ESCORT_FRONT = 13;
	constexpr int HELI_LAND = 20; // land and wait

	// TASK_VEHICLE_ESCORT modes
	constexpr int ESCORT_BEHIND = -1;
	constexpr int ESCORT_AHEAD = 0;

	float HeadingDiff(float a, float b);           // absolute angle difference in degrees (0..180)
	bool IsLandVehicle(Vehicle v);                 // car, bike or quad
	bool IsEmpty(Vehicle v);                       // no driver and no passengers
	int Capacity(Vehicle v, int max = 4);          // seats incl. driver, capped at max
	int SeatFor(size_t riderIndex);                // 0 -> -1 (driver), 1 -> 0, 2 -> 1 ...

	// Road position `forward` metres ahead of (positive) or behind (negative) an entity,
	// snapped to the nearest road node when one is close, facing the entity's direction.
	void SpotRelative(Entity e, float forward, Vector3& pos, float& heading);

	// Loads + creates a vehicle as a mission entity, on the ground with the engine running.
	// Returns 0 on failure (and notifies).
	Vehicle Spawn(const std::string& model, const Vector3& pos, float heading);

	// Spawns a helicopter in the air, blades at full speed, moving forward.
	Vehicle SpawnHeliInAir(const std::string& model, const Vector3& pos, float heading, float forwardSpeed = 20.0f);

	// Moves a vehicle to `back` metres behind `target` on the road, matching its speed.
	void TeleportBehind(Vehicle v, Entity target, float back);

	// Deletes a mod vehicle unless the player is inside it (then it is only released).
	void DeleteOrRelease(Vehicle v);
	void Release(Vehicle v);

	// One map blip for a whole unit (car / heli). Respects the Map Blips option (returns 0 when off).
	// sprite -1 keeps the default dot.
	Blip AddUnitBlip(Entity e, int sprite, int colour, const char* name);
	void RemoveUnitBlip(Blip& b);

	// F-key (control 23) arbiter: true only if the player is on foot and not already entering a vehicle,
	// `v` is nearly stopped, within maxDist, and the closest vehicle of all to the player. Only one
	// vehicle can be closest, so two modules never both hijack the same key press.
	bool ShouldHandleEnter(Vehicle v, float maxDist);

	// Right-hand curb position on the road nearest `at`, for traffic heading `travelHeading`. Built from the
	// road's lane data (GET_CLOSEST_ROAD). outHeading faces the direction of travel. False if no road found.
	bool CurbPoint(const Vector3& at, float travelHeading, Vector3& out, float& outHeading);

	// Slow, precise drive to a spot (last metres in a straight line), for parking at the curb.
	void DriveToSpot(Ped driver, Vehicle v, const Vector3& pos, float speed);

	// Signed metres of `pos` ahead (+) / behind (-) of `ref`, along ref's forward vector (2D).
	float AlongTrack(Entity ref, const Vector3& pos);
	// Lateral (2D) distance of `pos` from ref's centre line.
	float OffTrack(Entity ref, const Vector3& pos);

	// Full performance mods + strong body so a pursuit / motorcade car can keep up with anything.
	void TuneForPursuit(Vehicle v);
	// Temporary extra engine power (0 = stock) while a follower is catching up.
	void SetPowerBoost(Vehicle v, float percent);

	bool GetWaypoint(Vector3& out);                // false if no waypoint is set
	bool GroundZ(float x, float y, float& z);      // ground height at x,y (area must be streamed in)
}
