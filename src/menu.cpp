#include "menu.h"
#include "config.h"
#include "guards.h"
#include "combat.h"
#include "chauffeur.h"
#include "motorcade.h"
#include "heli.h"
#include "script.h"
#include "keyboard.h"

namespace
{
	using Menu::Choice;
	using Menu::OnOff;
	using Menu::Wrap;

	enum class Page { Main, Spawn, Weapons, Vehicles, Driver, Motorcade, Helicopters, Orders, Options };

	struct Level
	{
		Page page;
		int index;
		std::string selectedText; // keeps the cursor on the same row when a page's rows change
	};

	bool s_open = false;
	std::vector<Level> s_stack = { { Page::Main, 0, "" } };
	std::function<void()> s_pending; // selected action, run next frame (may spawn and yield)

	// Layout (normalized screen coords)
	constexpr float X = 0.015f;
	constexpr float W = 0.23f;
	constexpr float TOP = 0.08f;
	constexpr float TITLE_H = 0.05f;
	constexpr float SUB_H = 0.03f;
	constexpr float ROW_H = 0.034f;
	constexpr float HELP_LINE_H = 0.024f;
	constexpr int MAX_ROWS = 10;
	constexpr size_t HELP_CHARS_PER_LINE = 46;

	void Beep(const char* sound)
	{
		AUDIO::PLAY_SOUND_FRONTEND(-1, const_cast<char*>(sound), const_cast<char*>("HUD_FRONTEND_DEFAULT_SOUNDSET"), FALSE);
	}

	void Rect(float x, float y, float w, float h, int r, int g, int b, int a)
	{
		GRAPHICS::DRAW_RECT(x + w * 0.5f, y + h * 0.5f, w, h, r, g, b, a);
	}

	enum class Align { Left, Center, Right };

	void Text(const std::string& s, float x, float y, float scale, int font, int r, int g, int b, Align align = Align::Left)
	{
		UI::SET_TEXT_FONT(font);
		UI::SET_TEXT_SCALE(0.0f, scale);
		UI::SET_TEXT_COLOUR(r, g, b, 255);
		if (align == Align::Center)
			UI::SET_TEXT_CENTRE(TRUE);
		else if (align == Align::Right)
		{
			UI::SET_TEXT_WRAP(0.0f, x);
			UI::SET_TEXT_RIGHT_JUSTIFY(TRUE);
		}
		UI::_SET_TEXT_ENTRY(const_cast<char*>("STRING"));
		UI::_ADD_TEXT_COMPONENT_STRING(const_cast<char*>(s.c_str()));
		UI::_DRAW_TEXT(align == Align::Right ? 0.0f : x, y);
	}

	// Greedy word wrap for the help box (text natives cap a component at ~99 chars anyway).
	std::vector<std::string> WrapText(const std::string& s, size_t width)
	{
		std::vector<std::string> lines;
		std::string line, word;
		auto flush = [&] {
			if (line.empty())
				line = word;
			else if (line.size() + 1 + word.size() <= width)
				line += " " + word;
			else
			{
				lines.push_back(line);
				line = word;
			}
			word.clear();
		};
		for (char c : s)
		{
			if (c == ' ')
			{
				if (!word.empty())
					flush();
			}
			else
				word += c;
		}
		if (!word.empty())
			flush();
		if (!line.empty())
			lines.push_back(line);
		return lines;
	}

	bool IsSelectable(const MenuItem& item) { return item.select || item.change; }

	void Open(Page page)
	{
		s_stack.push_back({ page, 0, "" });
	}

	const char* PageTitle(Page p)
	{
		switch (p)
		{
		case Page::Spawn: return "BODYGUARDS";
		case Page::Weapons: return "WEAPONS";
		case Page::Vehicles: return "VEHICLE FOLLOW";
		case Page::Driver: return "DRIVER";
		case Page::Motorcade: return "MOTORCADE";
		case Page::Helicopters: return "HELICOPTERS";
		case Page::Orders: return "ORDERS";
		case Page::Options: return "OPTIONS";
		default: return "MAIN MENU";
		}
	}

	MenuItem Sub(const char* text, Page page, const char* help)
	{
		return MenuItem{ text, ">", [page] { Open(page); }, nullptr, help };
	}

	std::vector<MenuItem> BuildItems(Page page)
	{
		Config& c = g_cfg;
		std::vector<MenuItem> items;
		switch (page)
		{
		case Page::Main:
			items.push_back(Sub("Bodyguards", Page::Spawn, "Spawn up to 7 suited bodyguards who follow you, defend you and ride with you."));
			items.push_back(Sub("Weapons", Page::Weapons, "Choose the weapon your bodyguards carry."));
			items.push_back(Sub("Vehicle Follow", Page::Vehicles, "How guards who don't fit in your car follow you."));
			items.push_back(Sub("Driver", Page::Driver, "A personal chauffeur who drives you to your map waypoint."));
			items.push_back(Sub("Motorcade", Page::Motorcade, "Lead and rear cars full of guards around your car."));
			items.push_back(Sub("Helicopters", Page::Helicopters, "Guard helicopters (circle, attack, rappel) and a luxury transport helicopter."));
			items.push_back(Sub("Orders", Page::Orders, "Attack, regroup, rappel and dismiss."));
			items.push_back(Sub("Options", Page::Options, "Invincibility, ammo, map blips and auto-defend."));
			break;

		case Page::Spawn:
			items.push_back(Menu::List("Model", c.modelIndex, (int)g_guardModels.size(), g_guardModels[c.modelIndex].label,
				"Which suited guard model to spawn."));
			items.push_back({ "Count", Choice(std::to_string(c.spawnCount)), nullptr,
				[&c](int d) { c.spawnCount = Wrap(c.spawnCount - 1 + d, Guards::MAX_GUARDS) + 1; },
				"How many guards to spawn at once (the group holds 7 at most)." });
			items.push_back(Menu::Action("Spawn", [&c] { Guards::Spawn(g_guardModels[c.modelIndex].name, c.spawnCount); },
				"Spawn the guards behind you. They join your group and follow you."));
			items.push_back(Menu::Action("Dismiss All", [] { DismissAll(); },
				"Remove every guard, driver, motorcade and helicopter the mod spawned."));
			break;

		case Page::Weapons:
			items.push_back(Menu::List("Weapon", c.weaponIndex, (int)g_weapons.size(), g_weapons[c.weaponIndex].label,
				"Weapon for bodyguards, motorcade and heli crews. Drivers and pilots keep a pistol."));
			items.push_back(Menu::Action("Give To All Guards", [&c] {
				Guards::GiveWeaponAll(g_weapons[c.weaponIndex].name);
				Notify("~b~Bodyguards:~s~ weapon set to " + g_weapons[c.weaponIndex].label + ".");
			}, "Hand the selected weapon to every guard right now. New guards get it automatically."));
			break;

		case Page::Vehicles:
			items.push_back({ "Follow Mode", Choice(FollowModeName(c.followMode)), nullptr,
				[&c](int d) { c.followMode = (FollowMode)Wrap((int)c.followMode + d, 3); },
				"Escort Car: spawn an SUV. Nearby Cars: take the closest parked car. Auto: nearby car first, else spawn." });
			items.push_back(Menu::List("Escort Vehicle", c.escortModelIndex, (int)g_escortModels.size(), g_escortModels[c.escortModelIndex].label,
				"Model used when an escort car is spawned."));
			items.push_back(Menu::Toggle("Catch-up Teleport", c.catchUpTeleport,
				"Escort cars that fall far behind are moved up behind you."));
			break;

		case Page::Driver:
			Chauffeur::BuildMenu(items);
			break;

		case Page::Motorcade:
			Motorcade::BuildMenu(items);
			break;

		case Page::Helicopters:
			Heli::BuildMenu(items);
			break;

		case Page::Orders:
			items.push_back(Menu::Action("Attack Aimed Target", [] { Combat::AttackAimedTarget(); },
				"Hold aim on a person or vehicle and select this (or press the attack key) to send everyone after it."));
			items.push_back(Menu::Action("Regroup / Cease Fire", [] { Combat::Regroup(); },
				"Cancel all attacks. Everyone returns to you, their cars and their formations."));
			items.push_back(Menu::Action("Rappel Heli Guards", [] { Heli::Rappel(); },
				"Guard helicopters hover over you and their crews rappel down to join you."));
			items.push_back(Menu::Action("Dismiss All", [] { DismissAll(); },
				"Remove every guard, driver, motorcade and helicopter the mod spawned."));
			break;

		case Page::Options:
		{
			auto toggle = [](const char* name, bool& flag, const char* help) {
				bool* f = &flag;
				auto flip = [f] {
					*f = !*f;
					Guards::ApplyOptionsAll();
					Chauffeur::RefreshBlips();
					Motorcade::RefreshBlips();
					Heli::RefreshBlips();
				};
				return MenuItem{ name, OnOff(flag), flip, [flip](int) { flip(); }, help };
			};
			items.push_back(toggle("Invincible", c.invincible, "Guards, drivers and pilots can't be killed."));
			items.push_back(toggle("Infinite Ammo", c.infiniteAmmo, "Guards never run out of ammo."));
			items.push_back(toggle("Map Blips", c.blips, "Show guards and mod vehicles on the map and radar."));
			items.push_back(toggle("Auto-Defend", c.autoDefend, "Guards attack anyone who attacks you or them without being ordered."));
			break;
		}
		}
		if (items.empty())
			items.push_back(Menu::Header("(nothing here)"));
		return items;
	}

	bool Pressed(DWORD a, DWORD b) { return IsKeyJustUp(a) || IsKeyJustUp(b); }

	void DisableControls()
	{
		static const int controls[] = {
			19, 24, 27, 37, 140, 141, 142, 143, 257, 263, 264,     // char wheel, attack, phone, weapon wheel, melee (aim stays: menu attack order)
			172, 173, 174, 175, 176, 177, 178, 179, 180, 181,      // phone navigation
			199, 200,                                              // pause (Backspace/Esc handled by us)
			81, 82, 85, 99, 100,                                   // radio / vehicle weapon select
			107, 108, 109, 110, 111, 112, 117, 118,                // aircraft numpad (roll / pitch / yaw)
			201, 202,                                              // frontend accept / cancel
		};
		for (int ctrl : controls)
			CONTROLS::DISABLE_CONTROL_ACTION(0, ctrl, TRUE);
	}

	// Moves the selection by `dir`, skipping header rows.
	void Step(std::vector<MenuItem>& items, int& index, int dir)
	{
		int n = (int)items.size();
		for (int tries = 0; tries < n; tries++)
		{
			index = Wrap(index + dir, n);
			if (IsSelectable(items[index]))
				return;
		}
	}

	// Re-find the previously selected row by its text (rows come and go on unit pages).
	void Resync(std::vector<MenuItem>& items, Level& lvl)
	{
		int n = (int)items.size();
		if (!lvl.selectedText.empty() && (lvl.index >= n || items[lvl.index].text != lvl.selectedText))
		{
			for (int i = 0; i < n; i++)
			{
				if (items[i].text == lvl.selectedText)
				{
					lvl.index = i;
					break;
				}
			}
		}
		lvl.index = std::min(std::max(lvl.index, 0), n - 1);
		if (!IsSelectable(items[lvl.index]))
			Step(items, lvl.index, +1);
		lvl.selectedText = items[lvl.index].text;
	}

	void HandleInput(std::vector<MenuItem>& items)
	{
		Level& lvl = s_stack.back();
		Resync(items, lvl);

		if (Pressed(VK_NUMPAD8, VK_UP)) { Step(items, lvl.index, -1); Beep("NAV_UP_DOWN"); }
		if (Pressed(VK_NUMPAD2, VK_DOWN)) { Step(items, lvl.index, +1); Beep("NAV_UP_DOWN"); }
		lvl.selectedText = items[lvl.index].text;

		MenuItem& item = items[lvl.index];
		if (item.change)
		{
			if (Pressed(VK_NUMPAD4, VK_LEFT)) { item.change(-1); Beep("NAV_LEFT_RIGHT"); }
			if (Pressed(VK_NUMPAD6, VK_RIGHT)) { item.change(+1); Beep("NAV_LEFT_RIGHT"); }
		}
		if (item.select && Pressed(VK_NUMPAD5, VK_RETURN))
		{
			Beep("SELECT");
			s_pending = item.select; // run next frame, after this frame's menu draw
			return;
		}
		if (Pressed(VK_NUMPAD0, VK_BACK))
		{
			Beep("BACK");
			if (s_stack.size() > 1)
				s_stack.pop_back();
			else
				s_open = false;
		}
	}

	void Draw(const std::vector<MenuItem>& items, int selected, Page page)
	{
		float y = TOP;
		Rect(X, y, W, TITLE_H, 15, 30, 60, 235);
		Text("Bodyguards", X + W * 0.5f, y + 0.006f, 0.75f, 1, 255, 255, 255, Align::Center);
		y += TITLE_H;

		int n = (int)items.size();
		int selectable = 0, selectedPos = 0;
		for (int i = 0; i < n; i++)
		{
			if (!IsSelectable(items[i]))
				continue;
			selectable++;
			if (i <= selected)
				selectedPos = selectable;
		}
		Rect(X, y, W, SUB_H, 0, 0, 0, 220);
		Text(PageTitle(page), X + 0.006f, y + 0.004f, 0.32f, 0, 120, 180, 255);
		Text(std::to_string(selectedPos) + "/" + std::to_string(selectable), X + W - 0.006f, y + 0.004f, 0.32f, 0, 200, 200, 200, Align::Right);
		y += SUB_H;

		// Scroll window keeps the selection visible.
		int first = std::max(0, std::min(selected - MAX_ROWS / 2, n - MAX_ROWS));
		int last = std::min(n, first + MAX_ROWS);
		for (int i = first; i < last; i++)
		{
			const MenuItem& item = items[i];
			if (!IsSelectable(item))
			{
				Rect(X, y, W, ROW_H, 10, 10, 10, 210);
				Text(item.text, X + 0.006f, y + 0.006f, 0.32f, 0, 120, 180, 255);
				y += ROW_H;
				continue;
			}
			bool active = i == selected;
			Rect(X, y, W, ROW_H, active ? 235 : 20, active ? 235 : 20, active ? 235 : 20, active ? 240 : 190);
			int tc = active ? 10 : 245;
			Text(item.text, X + 0.006f, y + 0.005f, 0.35f, 0, tc, tc, tc);
			if (!item.value.empty())
				Text(item.value, X + W - 0.006f, y + 0.005f, 0.35f, 0, tc, tc, tc, Align::Right);
			y += ROW_H;
		}

		Rect(X, y, W, SUB_H, 0, 0, 0, 220);
		Text("Guards: " + std::to_string(Guards::GroupCount()) + "/" + std::to_string(Guards::MAX_GUARDS),
			X + 0.006f, y + 0.004f, 0.32f, 0, 200, 200, 200);
		Text(FollowModeName(g_cfg.followMode), X + W - 0.006f, y + 0.004f, 0.32f, 0, 200, 200, 200, Align::Right);
		y += SUB_H;

		// Help box for the selected row + key hints.
		std::vector<std::string> lines = WrapText(items[selected].help, HELP_CHARS_PER_LINE);
		lines.push_back("~c~Up/Down select  Left/Right change  Enter ok  Back return");
		float boxH = HELP_LINE_H * (float)lines.size() + 0.008f;
		Rect(X, y + 0.004f, W, boxH, 0, 0, 0, 200);
		float ty = y + 0.008f;
		for (auto& l : lines)
		{
			Text(l, X + 0.006f, ty, 0.30f, 0, 225, 225, 225);
			ty += HELP_LINE_H;
		}
	}
}

namespace Menu
{
	bool IsOpen() { return s_open; }

	void Toggle()
	{
		s_open = !s_open;
		Beep(s_open ? "SELECT" : "BACK");
	}

	void Close()
	{
		s_open = false;
		s_pending = nullptr;
	}

	void Update()
	{
		if (!s_open)
			return;

		DisableControls();

		// Run the action selected last frame; it may yield (spawning) without leaving the menu half-drawn.
		if (s_pending)
		{
			auto fn = s_pending;
			s_pending = nullptr;
			fn();
			if (!s_open)
				return;
		}

		Page page = s_stack.back().page;
		std::vector<MenuItem> items = BuildItems(page);
		HandleInput(items);
		if (!s_open)
			return;

		// Input may have changed page or values; rebuild before drawing.
		page = s_stack.back().page;
		items = BuildItems(page);
		Level& lvl = s_stack.back();
		Resync(items, lvl);
		Draw(items, lvl.index, page);
	}
}
