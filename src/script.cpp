#include "script.h"
#include "common.h"
#include "config.h"
#include "guards.h"
#include "escort.h"
#include "combat.h"
#include "chauffeur.h"
#include "motorcade.h"
#include "heli.h"
#include "menu.h"
#include "registry.h"
#include "keyboard.h"

static HMODULE s_module = nullptr;
static bool s_wasDead = false;

void SetModule(HMODULE module)
{
	s_module = module;
}

void DismissAll(bool force)
{
	// Deleting the transport pilot mid-flight would drop the player out of the sky.
	Vehicle transport = Heli::TransportHeli();
	if (!force && transport != 0 && PED::IS_PED_IN_VEHICLE(PLAYER::PLAYER_PED_ID(), transport, FALSE) && ENTITY::IS_ENTITY_IN_AIR(transport))
	{
		Notify("~r~Bodyguards:~s~ land the transport heli first.");
		return;
	}

	// Peds first (Guards owns every mod ped), then each module deletes its vehicles and clears state.
	Combat::Reset();
	Guards::DismissAll();
	Escort::Reset();
	Chauffeur::Reset();
	Motorcade::Reset();
	Heli::Reset();
	Notify("~b~Bodyguards:~s~ dismissed.");
}

// False whenever a key press shouldn't reach the mod: menus, phone, cutscenes, loading, death, typing.
static bool GameAcceptsInput(bool requireControl)
{
	Player pl = PLAYER::PLAYER_ID();
	Ped player = PLAYER::PLAYER_PED_ID();
	if (UI::IS_PAUSE_MENU_ACTIVE() || !PLAYER::IS_PLAYER_PLAYING(pl) || ENTITY::IS_ENTITY_DEAD(player))
		return false;
	if (CUTSCENE::IS_CUTSCENE_ACTIVE() || DLC2::GET_IS_LOADING_SCREEN_ACTIVE() || CAM::IS_SCREEN_FADED_OUT())
		return false;
	if (STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS())
		return false;
	if (requireControl && !PLAYER::IS_PLAYER_CONTROL_ON(pl))
		return false;
	return true;
}

static bool CanUseHotkeys()
{
	if (!GameAcceptsInput(true))
		return false;
	Ped player = PLAYER::PLAYER_PED_ID();
	return !PED::IS_PED_RUNNING_MOBILE_PHONE_TASK(player) && !UI::IS_HUD_COMPONENT_ACTIVE(19); // phone, weapon wheel
}

static void HandlePlayerDeath()
{
	Player pl = PLAYER::PLAYER_ID();
	bool dead = PLAYER::IS_PLAYER_DEAD(pl) || PLAYER::IS_PLAYER_BEING_ARRESTED(pl, TRUE);
	if (dead && !s_wasDead && g_cfg.dismissOnDeath && !Guards::All().empty())
		DismissAll(true);
	s_wasDead = dead;
}

static void HandleHotkeys()
{
	// The menu (and its key) only works while the game is actually playable; it closes itself otherwise.
	if (!GameAcceptsInput(false))
	{
		if (Menu::IsOpen())
			Menu::Close();
		// Swallow presses made while blocked, so they don't fire on the first playable frame.
		for (DWORD key : { g_cfg.menuKey, g_cfg.attackKey, g_cfg.regroupKey, g_cfg.driverKey, g_cfg.rappelKey })
			ResetKeyState(key);
		return;
	}
	if (IsKeyJustUp(g_cfg.menuKey))
		Menu::Toggle();

	if (!CanUseHotkeys())
	{
		for (DWORD key : { g_cfg.attackKey, g_cfg.regroupKey, g_cfg.driverKey, g_cfg.rappelKey })
			ResetKeyState(key);
		return;
	}
	if (IsKeyJustUp(g_cfg.attackKey))
		Combat::AttackAimedTarget();
	if (IsKeyJustUp(g_cfg.regroupKey))
		Combat::Regroup();
	if (IsKeyJustUp(g_cfg.driverKey))
		Chauffeur::OnHotkey();
	if (IsKeyJustUp(g_cfg.rappelKey))
		Heli::Rappel();
}

static void Update()
{
	HandlePlayerDeath();
	HandleHotkeys();

	Guards::Update();
	Escort::Update();
	Chauffeur::Update();
	Motorcade::Update();
	Heli::Update();
	Combat::Update();
	Menu::Update();
}

void ScriptMain()
{
	// ScriptHookV re-enters ScriptMain after a session / save reload with the old static state still in
	// memory. Those handles may now belong to unrelated entities, so forget them without touching the
	// world, then clean up our leftovers by relationship group / tracked vehicle models.
	Combat::Forget();
	Escort::Forget();
	Chauffeur::Forget();
	Motorcade::Forget();
	Heli::Forget();
	Guards::Forget();
	Menu::Close();
	s_wasDead = false;

	LoadConfig(s_module);
	Chauffeur::LoadConfig();
	Motorcade::LoadConfig();
	Heli::LoadConfig();

	Registry::CleanupOrphans();

	while (true)
	{
		Update();
		WAIT(0);
	}
}
