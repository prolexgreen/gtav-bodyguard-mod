#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cmath>

#include "../sdk/inc/main.h"
#include "../sdk/inc/types.h"
#include "../sdk/inc/natives.h"

// Ped types / relationship values used by the natives
constexpr int PED_TYPE_MISSION = 26;
constexpr int REL_COMPANION = 0;

// Combat attributes
constexpr int CA_USE_COVER = 0;
constexpr int CA_USE_VEHICLES = 1;
constexpr int CA_DO_DRIVEBYS = 2;
constexpr int CA_LEAVE_VEHICLES = 3;
constexpr int CA_FIGHT_ARMED_WHEN_UNARMED = 5;
constexpr int CA_ALWAYS_FIGHT = 46;

constexpr Hash FIRING_PATTERN_FULL_AUTO = 0xC6EE6B4C;

// Blip colours
constexpr int BLIP_COLOUR_WHITE = 0;
constexpr int BLIP_COLOUR_RED = 1;
constexpr int BLIP_COLOUR_BLUE = 3;

// Game time in ms: stops while the game is paused, so deadlines don't all expire during a pause.
inline DWORD Now() { return (DWORD)GAMEPLAY::GET_GAME_TIMER(); }

// Wrap-safe "has `ms` passed since `since`".
inline bool Elapsed(DWORD since, DWORD ms) { return Now() - since >= ms; }

inline Hash Joaat(const std::string& s) { return GAMEPLAY::GET_HASH_KEY(const_cast<char*>(s.c_str())); }

inline float Dist(const Vector3& a, const Vector3& b)
{
	float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline float EntityDist(Entity a, Entity b)
{
	return Dist(ENTITY::GET_ENTITY_COORDS(a, TRUE), ENTITY::GET_ENTITY_COORDS(b, TRUE));
}

inline bool IsAlive(Entity e)
{
	return e != 0 && ENTITY::DOES_ENTITY_EXIST(e) && !ENTITY::IS_ENTITY_DEAD(e);
}

inline void Notify(const std::string& text)
{
	UI::_SET_NOTIFICATION_TEXT_ENTRY(const_cast<char*>("STRING"));
	UI::_ADD_TEXT_COMPONENT_STRING(const_cast<char*>(text.c_str()));
	UI::_DRAW_NOTIFICATION(FALSE, FALSE);
}

// Requests a model and yields until it loads. Returns false on timeout or invalid model.
inline bool LoadModel(Hash model, DWORD timeoutMs = 5000)
{
	if (!STREAMING::IS_MODEL_IN_CDIMAGE(model))
		return false;
	STREAMING::REQUEST_MODEL(model);
	DWORD start = Now();
	while (!STREAMING::HAS_MODEL_LOADED(model))
	{
		if (Elapsed(start, timeoutMs))
			return false;
		WAIT(0);
	}
	return true;
}
