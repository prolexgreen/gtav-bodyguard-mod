#include "config.h"

#include <algorithm>
#include <cctype>

Config g_cfg;

std::vector<NamedItem> g_guardModels = { // suit-wearing models only
	{ "Suit Security 1", "s_m_m_highsec_01" },
	{ "Suit Security 2", "s_m_m_highsec_02" },
	{ "Jewel Store Security", "u_m_m_jewelsec_01" },
	{ "Devin's Security", "s_m_y_devinsec_01" },
	{ "IAA Agent", "s_m_m_ciasec_01" },
	{ "FIB Office Agent", "s_m_m_fiboffice_01" },
	{ "FIB Suit", "ig_fbisuit_01" },
};

std::vector<NamedItem> g_weapons = {
	{ "Pistol", "WEAPON_PISTOL" },
	{ "Combat Pistol", "WEAPON_COMBATPISTOL" },
	{ "SMG", "WEAPON_SMG" },
	{ "Carbine Rifle", "WEAPON_CARBINERIFLE" },
	{ "Assault Rifle", "WEAPON_ASSAULTRIFLE" },
	{ "Pump Shotgun", "WEAPON_PUMPSHOTGUN" },
	{ "Combat MG", "WEAPON_COMBATMG" },
	{ "Sniper Rifle", "WEAPON_SNIPERRIFLE" },
	{ "RPG", "WEAPON_RPG" },
	{ "Minigun", "WEAPON_MINIGUN" },
};

std::vector<NamedItem> g_escortModels = {
	{ "Granger", "granger" },
	{ "Baller", "baller" },
	{ "Cavalcade", "cavalcade2" },
	{ "FIB SUV", "fbi2" },
};

static std::string s_iniPath;

std::string ReadString(const char* section, const char* key, const char* def)
{
	char buf[256];
	GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), s_iniPath.c_str());
	return buf;
}

int ReadInt(const char* section, const char* key, int def)
{
	return GetPrivateProfileIntA(section, key, def, s_iniPath.c_str());
}

bool ReadBool(const char* section, const char* key, bool def)
{
	return ReadInt(section, key, def ? 1 : 0) != 0;
}

std::string Upper(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
	return s;
}

// Accepts "F7", "J", "NUMPAD5", or a raw virtual-key number like "118".
DWORD ParseKey(const std::string& raw, DWORD def)
{
	std::string s = Upper(raw);
	if (s.empty())
		return def;
	if (s.size() == 1 && std::isalnum((unsigned char)s[0]))
		return (DWORD)s[0];
	if (s[0] == 'F' && s.size() <= 3 && std::isdigit((unsigned char)s[1]))
	{
		int n = std::atoi(s.c_str() + 1);
		if (n >= 1 && n <= 24)
			return VK_F1 + n - 1;
	}
	if (s.rfind("NUMPAD", 0) == 0 && s.size() == 7 && std::isdigit((unsigned char)s[6]))
		return VK_NUMPAD0 + (s[6] - '0');
	if (std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }))
		return (DWORD)std::atoi(s.c_str());
	return def;
}

int FindIndex(const std::vector<NamedItem>& list, const std::string& name, int def)
{
	std::string n = Upper(name);
	for (size_t i = 0; i < list.size(); i++)
		if (Upper(list[i].name) == n || Upper(list[i].label) == n)
			return (int)i;
	return def;
}

void LoadConfig(HMODULE module)
{
	char path[MAX_PATH];
	GetModuleFileNameA(module, path, MAX_PATH);
	s_iniPath = path;
	size_t dot = s_iniPath.find_last_of('.');
	s_iniPath = (dot == std::string::npos ? s_iniPath : s_iniPath.substr(0, dot)) + ".ini";

	// Idempotent: ScriptMain can run again (session reload), so drop previously appended custom entries.
	static const size_t baseGuardModels = g_guardModels.size();
	static const size_t baseEscortModels = g_escortModels.size();
	g_guardModels.resize(baseGuardModels);
	g_escortModels.resize(baseEscortModels);

	Config& c = g_cfg;

	c.menuKey = ParseKey(ReadString("Keys", "MenuKey", "F7"), VK_F7);
	c.attackKey = ParseKey(ReadString("Keys", "AttackKey", "J"), 'J');
	c.regroupKey = ParseKey(ReadString("Keys", "RegroupKey", "K"), 'K');
	c.driverKey = ParseKey(ReadString("Keys", "DriverKey", "L"), 'L');
	c.rappelKey = ParseKey(ReadString("Keys", "RappelKey", "U"), 'U');

	std::string customModel = ReadString("Guards", "CustomModel", "");
	if (!customModel.empty())
		g_guardModels.push_back({ "Custom (" + customModel + ")", customModel });
	c.modelIndex = FindIndex(g_guardModels, ReadString("Guards", "DefaultModel", "s_m_m_highsec_01"), 0);
	c.weaponIndex = FindIndex(g_weapons, ReadString("Guards", "DefaultWeapon", "WEAPON_CARBINERIFLE"), 3);
	c.accuracy = std::clamp(ReadInt("Guards", "Accuracy", 60), 0, 100);
	c.health = std::max(ReadInt("Guards", "Health", 300), 101);
	c.armour = std::clamp(ReadInt("Guards", "Armour", 100), 0, 100);
	c.invincible = ReadBool("Guards", "Invincible", false);
	c.infiniteAmmo = ReadBool("Guards", "InfiniteAmmo", true);
	c.blips = ReadBool("Guards", "Blips", true);
	c.dismissOnDeath = ReadBool("Guards", "DismissOnDeath", true);

	std::string mode = Upper(ReadString("Vehicles", "FollowMode", "Auto"));
	c.followMode = mode == "ESCORTCAR" ? FollowMode::EscortCar : mode == "NEARBYCARS" ? FollowMode::NearbyCars : FollowMode::Auto;
	std::string customEscort = ReadString("Vehicles", "CustomEscortModel", "");
	if (!customEscort.empty())
		g_escortModels.push_back({ "Custom (" + customEscort + ")", customEscort });
	c.escortModelIndex = FindIndex(g_escortModels, ReadString("Vehicles", "EscortModel", "granger"), 0);
	c.catchUpTeleport = ReadBool("Vehicles", "CatchUpTeleport", true);
	c.catchUpDistance = (float)std::max(ReadInt("Vehicles", "CatchUpDistance", 200), 50);
	c.drivingStyle = ReadInt("Vehicles", "DrivingStyle", 1074528293);
	c.escortSpeed = (float)std::max(ReadInt("Vehicles", "EscortSpeed", 35), 10);

	c.autoDefend = ReadBool("Combat", "AutoDefend", true);
	c.defendRadius = (float)std::max(ReadInt("Combat", "DefendRadius", 80), 10);
}

const char* FollowModeName(FollowMode mode)
{
	switch (mode)
	{
	case FollowMode::EscortCar: return "Escort Car";
	case FollowMode::NearbyCars: return "Nearby Cars";
	default: return "Auto";
	}
}
