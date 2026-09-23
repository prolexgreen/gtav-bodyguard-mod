#pragma once

#include "common.h"

enum class FollowMode { EscortCar = 0, NearbyCars = 1, Auto = 2 };

struct NamedItem
{
	std::string label;
	std::string name; // model or weapon name
};

struct Config
{
	// Keys
	DWORD menuKey = VK_F7;
	DWORD attackKey = 'J';
	DWORD regroupKey = 'K';
	DWORD driverKey = 'L';
	DWORD rappelKey = 'U';

	// Guards
	int modelIndex = 0;
	int weaponIndex = 3;
	int spawnCount = 1;
	int accuracy = 60;
	int health = 300;
	int armour = 100;
	bool invincible = false;
	bool infiniteAmmo = true;
	bool blips = true;
	bool dismissOnDeath = true;

	// Vehicles
	FollowMode followMode = FollowMode::Auto;
	int escortModelIndex = 0;
	bool catchUpTeleport = true;
	float catchUpDistance = 200.0f;
	int drivingStyle = 1074528293; // "rushed": ignores lights, swerves around traffic
	float escortSpeed = 35.0f;     // max speed in m/s (task cap; motorcade gap control does the rest)

	// Combat
	bool autoDefend = true;
	float defendRadius = 80.0f;
};

extern Config g_cfg;
extern std::vector<NamedItem> g_guardModels;
extern std::vector<NamedItem> g_weapons;
extern std::vector<NamedItem> g_escortModels;

void LoadConfig(HMODULE module);

// ini helpers (valid after LoadConfig) for modules that parse their own sections
std::string ReadString(const char* section, const char* key, const char* def);
int ReadInt(const char* section, const char* key, int def);
bool ReadBool(const char* section, const char* key, bool def);
std::string Upper(std::string s);
DWORD ParseKey(const std::string& raw, DWORD def);
int FindIndex(const std::vector<NamedItem>& list, const std::string& name, int def);

const char* FollowModeName(FollowMode mode);
