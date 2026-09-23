#include "guards.h"
#include "config.h"

#include <algorithm>

static std::vector<Guard> s_guards;
static Hash s_relGroup = 0;
static Ped s_lastPlayerPed = 0;
static DWORD s_lastCatchUp = 0;
static DWORD s_lastRejoin = 0;
static DWORD s_lastFullNotify = 0;

constexpr float CATCH_UP_DIST = 250.0f;

static Hash RelGroup()
{
	if (s_relGroup == 0)
	{
		PED::ADD_RELATIONSHIP_GROUP(const_cast<char*>("BG_GUARDS"), &s_relGroup);
		Hash player = Joaat("PLAYER");
		PED::SET_RELATIONSHIP_BETWEEN_GROUPS(REL_COMPANION, s_relGroup, player);
		PED::SET_RELATIONSHIP_BETWEEN_GROUPS(REL_COMPANION, player, s_relGroup);
		PED::SET_RELATIONSHIP_BETWEEN_GROUPS(REL_COMPANION, s_relGroup, s_relGroup);
	}
	return s_relGroup;
}

static int PlayerGroup()
{
	return PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID());
}

static void RemoveBlip(Guard& g)
{
	if (g.blip != 0 && UI::DOES_BLIP_EXIST(g.blip))
		UI::REMOVE_BLIP(&g.blip);
	g.blip = 0;
}

static void ApplyOptions(Guard& g)
{
	ENTITY::SET_ENTITY_INVINCIBLE(g.ped, g_cfg.invincible);
	WEAPON::SET_PED_INFINITE_AMMO_CLIP(g.ped, g_cfg.infiniteAmmo);
	PED::SET_PED_ACCURACY(g.ped, g_cfg.accuracy);

	// Only bodyguards get a ped blip; units (driver, motorcade, helis) show one blip on their vehicle.
	bool wantBlip = g_cfg.blips && g.role == Role::Bodyguard;
	bool hasBlip = g.blip != 0 && UI::DOES_BLIP_EXIST(g.blip);
	if (wantBlip && !hasBlip)
	{
		g.blip = UI::ADD_BLIP_FOR_ENTITY(g.ped);
		UI::SET_BLIP_AS_FRIENDLY(g.blip, TRUE);
		UI::SET_BLIP_SCALE(g.blip, 0.7f);
	}
	else if (!wantBlip && hasBlip)
	{
		RemoveBlip(g);
	}
	if (g.blip != 0)
		UI::SET_BLIP_COLOUR(g.blip, BLIP_COLOUR_BLUE);
}

// Undo our buffs before handing a ped back to the game, so no immortal ex-guards roam the city.
static void ResetForRelease(Ped ped)
{
	ENTITY::SET_ENTITY_INVINCIBLE(ped, FALSE);
	WEAPON::SET_PED_INFINITE_AMMO_CLIP(ped, FALSE);
	PED::SET_PED_CAN_BE_TARGETTED_BY_PLAYER(ped, PLAYER::PLAYER_ID(), TRUE);
	PED::SET_PED_RELATIONSHIP_GROUP_HASH(ped, Joaat("CIVMALE"));
	PED::SET_PED_KEEP_TASK(ped, FALSE);
	PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, FALSE);
}

static void GiveWeapon(Ped ped, const std::string& weapon)
{
	Hash hash = Joaat(weapon);
	WEAPON::REMOVE_ALL_PED_WEAPONS(ped, TRUE);
	WEAPON::GIVE_WEAPON_TO_PED(ped, hash, 9999, FALSE, TRUE);
	WEAPON::SET_CURRENT_PED_WEAPON(ped, hash, TRUE);
}

// Drivers and pilots carry a pistol; only combat roles get the selected weapon (no RPGs next to the limo).
static bool UsesSidearm(Role role)
{
	return role == Role::Chauffeur || role == Role::HeliPilot || role == Role::TransportPilot;
}

static std::string WeaponFor(Role role)
{
	return UsesSidearm(role) ? "WEAPON_PISTOL" : g_weapons[g_cfg.weaponIndex].name;
}

// Teleports far-away followers (e.g. left behind after a heli trip) to a safe spot behind the player.
static void CatchUpFollowers(Ped player)
{
	if (!Elapsed(s_lastCatchUp, 2000))
		return;
	s_lastCatchUp = Now();
	if (!PED::IS_PED_ON_FOOT(player))
		return;

	for (auto& g : s_guards)
	{
		if (g.role != Role::Bodyguard || g.state != GuardState::Following || !PED::IS_PED_ON_FOOT(g.ped))
			continue;
		if (EntityDist(player, g.ped) < CATCH_UP_DIST)
			continue;
		Vector3 behind = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(player, 0.0f, -20.0f, 0.0f);
		Vector3 safe;
		if (!PATHFIND::GET_SAFE_COORD_FOR_PED(behind.x, behind.y, behind.z, TRUE, &safe, 16))
			safe = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(player, 0.0f, -3.0f, 0.0f);
		ENTITY::SET_ENTITY_COORDS(g.ped, safe.x, safe.y, safe.z, FALSE, FALSE, FALSE, TRUE);
		AI::CLEAR_PED_TASKS(g.ped);
	}
}

namespace Guards
{
	std::vector<Guard>& All() { return s_guards; }

	Guard* Find(Ped ped)
	{
		for (auto& g : s_guards)
			if (g.ped == ped)
				return &g;
		return nullptr;
	}

	bool IsGuard(Ped ped) { return Find(ped) != nullptr; }

	int GroupCount()
	{
		int n = 0;
		for (auto& g : s_guards)
			if (g.role == Role::Bodyguard && (g.state == GuardState::Following || g.state == GuardState::Escorting))
				n++;
		return n;
	}

	void JoinGroup(Guard& g)
	{
		int group = PlayerGroup();
		PED::SET_PED_AS_GROUP_MEMBER(g.ped, group);
		PED::SET_PED_NEVER_LEAVES_GROUP(g.ped, TRUE);
		PED::SET_GROUP_SEPARATION_RANGE(group, 9999.0f);
		PED::SET_GROUP_FORMATION(group, 0);
		PED::SET_PED_COMBAT_ATTRIBUTES(g.ped, CA_LEAVE_VEHICLES, TRUE);
		g.state = GuardState::Following;
		g.escortVeh = 0;
		g.driving = false;
	}

	void LeaveGroup(Guard& g)
	{
		PED::SET_PED_NEVER_LEAVES_GROUP(g.ped, FALSE);
		PED::REMOVE_PED_FROM_GROUP(g.ped);
	}

	Ped Create(const std::string& model, const Vector3& pos, float heading, Role role)
	{
		Hash hash = Joaat(model);
		if (!LoadModel(hash))
		{
			Notify("~r~Bodyguards:~s~ couldn't load model " + model);
			return 0;
		}

		Guard g;
		g.role = role;
		g.state = role == Role::Bodyguard ? GuardState::Following : GuardState::Unit;
		g.ped = PED::CREATE_PED(PED_TYPE_MISSION, hash, pos.x, pos.y, pos.z, heading, FALSE, TRUE);
		STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(hash);
		if (!ENTITY::DOES_ENTITY_EXIST(g.ped))
			return 0;
		ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g.ped, TRUE, TRUE);

		PED::SET_PED_RELATIONSHIP_GROUP_HASH(g.ped, RelGroup());
		PED::SET_PED_CAN_BE_TARGETTED_BY_PLAYER(g.ped, PLAYER::PLAYER_ID(), FALSE);
		PED::SET_CAN_ATTACK_FRIENDLY(g.ped, FALSE, FALSE);

		PED::SET_PED_MAX_HEALTH(g.ped, g_cfg.health);
		ENTITY::SET_ENTITY_HEALTH(g.ped, g_cfg.health);
		PED::SET_PED_ARMOUR(g.ped, g_cfg.armour);

		PED::SET_PED_COMBAT_ABILITY(g.ped, 2);
		PED::SET_PED_COMBAT_RANGE(g.ped, 2);
		PED::SET_PED_COMBAT_MOVEMENT(g.ped, 2);
		PED::SET_PED_COMBAT_ATTRIBUTES(g.ped, CA_USE_COVER, TRUE);
		PED::SET_PED_COMBAT_ATTRIBUTES(g.ped, CA_USE_VEHICLES, TRUE);
		PED::SET_PED_COMBAT_ATTRIBUTES(g.ped, CA_DO_DRIVEBYS, TRUE);
		PED::SET_PED_COMBAT_ATTRIBUTES(g.ped, CA_FIGHT_ARMED_WHEN_UNARMED, TRUE);
		PED::SET_PED_COMBAT_ATTRIBUTES(g.ped, CA_ALWAYS_FIGHT, TRUE);
		PED::SET_PED_FLEE_ATTRIBUTES(g.ped, 0, FALSE);
		PED::SET_PED_SEEING_RANGE(g.ped, 100.0f);
		PED::SET_PED_HEARING_RANGE(g.ped, 100.0f);
		PED::SET_PED_CAN_BE_DRAGGED_OUT(g.ped, FALSE);
		WEAPON::SET_PED_DROPS_WEAPONS_WHEN_DEAD(g.ped, FALSE);

		GiveWeapon(g.ped, WeaponFor(role));
		if (role == Role::Bodyguard)
			JoinGroup(g);
		ApplyOptions(g);
		s_guards.push_back(g);
		return g.ped;
	}

	static void ReleaseOrDelete(Guard& g, bool deletePed)
	{
		RemoveBlip(g);
		if (!ENTITY::DOES_ENTITY_EXIST(g.ped))
			return;
		LeaveGroup(g);
		if (deletePed)
		{
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g.ped, TRUE, TRUE);
			PED::DELETE_PED(&g.ped);
		}
		else
		{
			if (!ENTITY::IS_ENTITY_DEAD(g.ped))
				ResetForRelease(g.ped);
			ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&g.ped);
		}
	}

	void Remove(Ped ped, bool deletePed)
	{
		for (auto it = s_guards.begin(); it != s_guards.end(); ++it)
		{
			if (it->ped != ped)
				continue;
			ReleaseOrDelete(*it, deletePed);
			s_guards.erase(it);
			return;
		}
	}

	void Promote(Ped ped)
	{
		Guard* g = Find(ped);
		if (!g)
			return;
		g->role = Role::Bodyguard;
		g->driving = false;
		g->escortVeh = 0;
		g->needsRetask = false;
		if (PED::IS_PED_IN_ANY_VEHICLE(g->ped, FALSE))
			AI::TASK_LEAVE_ANY_VEHICLE(g->ped, 0, 0);
		if (GroupCount() < MAX_GUARDS)
		{
			JoinGroup(*g);
		}
		else
		{
			g->state = GuardState::Holding;
			AI::TASK_GUARD_CURRENT_POSITION(g->ped, 15.0f, 10.0f, TRUE);
			if (Elapsed(s_lastFullNotify, 10000))
			{
				s_lastFullNotify = Now();
				Notify("~b~Bodyguards:~s~ group is full, extra guards are holding position.");
			}
		}
		ApplyOptions(*g);
	}

	void Spawn(const std::string& model, int count)
	{
		int room = MAX_GUARDS - GroupCount();
		if (room <= 0)
		{
			Notify("~r~Bodyguards:~s~ already at the limit of 7.");
			return;
		}
		count = std::min(count, room);

		Ped player = PLAYER::PLAYER_PED_ID();
		float heading = ENTITY::GET_ENTITY_HEADING(player);
		for (int i = 0; i < count; i++)
		{
			int slot = GroupCount();
			float x = (float)(slot % 3 - 1) * 1.5f;
			float y = -2.5f - (float)(slot / 3) * 1.5f;
			Vector3 pos = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(player, x, y, 0.0f);
			if (Create(model, pos, heading, Role::Bodyguard) == 0)
				break;
		}
		Notify("~b~Bodyguards:~s~ " + std::to_string(GroupCount()) + "/7 guards.");
	}

	void GiveWeaponAll(const std::string& weapon)
	{
		for (auto& g : s_guards)
			if (!UsesSidearm(g.role))
				GiveWeapon(g.ped, weapon);
	}

	void ApplyOptionsAll()
	{
		for (auto& g : s_guards)
			ApplyOptions(g);
	}

	void DismissAll()
	{
		for (auto& g : s_guards)
			ReleaseOrDelete(g, true);
		s_guards.clear();
	}

	void Forget()
	{
		s_guards.clear();
		s_relGroup = 0;
		s_lastPlayerPed = 0;
		s_lastCatchUp = 0;
		s_lastRejoin = 0;
		s_lastFullNotify = 0;
	}

	void Update()
	{
		// Prune dead / despawned guards; let corpses despawn naturally.
		for (size_t i = 0; i < s_guards.size();)
		{
			Ped p = s_guards[i].ped;
			if (!ENTITY::DOES_ENTITY_EXIST(p) || ENTITY::IS_ENTITY_DEAD(p))
				Remove(p, false);
			else
				i++;
		}

		// Character switch (or respawn with a new ped): the player group changes, so re-join.
		Ped player = PLAYER::PLAYER_PED_ID();
		bool playerChanged = player != s_lastPlayerPed;
		s_lastPlayerPed = player;

		// Group maintenance at most once a second (unless the player changed); a full group would
		// otherwise make every guard re-join every frame.
		if (playerChanged || Elapsed(s_lastRejoin, 1000))
		{
			s_lastRejoin = Now();
			int group = PlayerGroup();
			for (auto& g : s_guards)
			{
				if (g.role != Role::Bodyguard)
					continue;
				if (g.state == GuardState::Holding && GroupCount() < MAX_GUARDS)
				{
					AI::CLEAR_PED_TASKS(g.ped);
					JoinGroup(g);
				}
				if (g.state != GuardState::Following)
					continue;
				if (playerChanged || !PED::IS_PED_GROUP_MEMBER(g.ped, group))
				{
					JoinGroup(g);
					PED::SET_PED_CAN_BE_TARGETTED_BY_PLAYER(g.ped, PLAYER::PLAYER_ID(), FALSE);
					// Group refused him (full with story buddies / other mods): hold position instead.
					if (!PED::IS_PED_GROUP_MEMBER(g.ped, group))
					{
						LeaveGroup(g);
						g.state = GuardState::Holding;
						AI::TASK_GUARD_CURRENT_POSITION(g.ped, 15.0f, 10.0f, TRUE);
					}
				}
			}
		}

		CatchUpFollowers(player);
	}
}
