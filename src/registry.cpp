#include "registry.h"

#include <sstream>
#include <utility>

namespace
{
	constexpr const char* ENV_NAME = "BG_MOD_ENTITIES";

	std::vector<std::pair<Entity, Hash>> s_tracked;

	void Save()
	{
		std::ostringstream out;
		for (auto& t : s_tracked)
			out << t.first << ':' << t.second << ';';
		SetEnvironmentVariableA(ENV_NAME, s_tracked.empty() ? nullptr : out.str().c_str());
	}

	std::vector<std::pair<Entity, Hash>> Load()
	{
		std::vector<std::pair<Entity, Hash>> list;
		DWORD len = GetEnvironmentVariableA(ENV_NAME, nullptr, 0);
		if (len == 0)
			return list;
		std::string buf(len, '\0');
		GetEnvironmentVariableA(ENV_NAME, &buf[0], len);

		std::istringstream in(buf.c_str());
		std::string item;
		while (std::getline(in, item, ';'))
		{
			size_t colon = item.find(':');
			if (colon == std::string::npos)
				continue;
			Entity e = (Entity)std::stol(item.substr(0, colon));
			Hash h = (Hash)std::stoul(item.substr(colon + 1));
			list.push_back({ e, h });
		}
		return list;
	}

	void RemoveEntityBlip(Entity e)
	{
		Blip b = UI::GET_BLIP_FROM_ENTITY(e);
		if (b != 0 && UI::DOES_BLIP_EXIST(b))
			UI::REMOVE_BLIP(&b);
	}
}

namespace Registry
{
	void Track(Entity e)
	{
		if (e == 0)
			return;
		for (auto& t : s_tracked)
			if (t.first == e)
				return;
		s_tracked.push_back({ e, ENTITY::GET_ENTITY_MODEL(e) });
		Save();
	}

	void Untrack(Entity e)
	{
		for (auto it = s_tracked.begin(); it != s_tracked.end(); ++it)
		{
			if (it->first == e)
			{
				s_tracked.erase(it);
				Save();
				return;
			}
		}
	}

	void CleanupOrphans()
	{
		int cleaned = 0;
		Ped player = PLAYER::PLAYER_PED_ID();

		// Peds first: everything in our relationship group is ours.
		Hash rel = Joaat("BG_GUARDS");
		static int peds[1024];
		int n = worldGetAllPeds(peds, 1024);
		for (int i = 0; i < n; i++)
		{
			Ped p = peds[i];
			if (p == player || !ENTITY::DOES_ENTITY_EXIST(p) || PED::IS_PED_A_PLAYER(p))
				continue;
			if (PED::GET_PED_RELATIONSHIP_GROUP_HASH(p) != rel)
				continue;
			RemoveEntityBlip(p);
			PED::REMOVE_PED_FROM_GROUP(p);
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, TRUE, TRUE);
			PED::DELETE_PED(&p);
			cleaned++;
		}

		// Then vehicles from the previous session, verified by model so reused handles are never touched.
		for (auto& t : Load())
		{
			Vehicle v = t.first;
			if (!ENTITY::DOES_ENTITY_EXIST(v) || ENTITY::GET_ENTITY_MODEL(v) != t.second || !ENTITY::IS_ENTITY_A_MISSION_ENTITY(v))
				continue;
			RemoveEntityBlip(v);
			if (PED::IS_PED_IN_VEHICLE(player, v, FALSE))
				ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&v);
			else
				VEHICLE::DELETE_VEHICLE(&v);
			cleaned++;
		}

		s_tracked.clear();
		Save();
		if (cleaned > 0)
			Notify("~b~Bodyguards:~s~ cleaned up " + std::to_string(cleaned) + " leftover entities.");
	}
}
