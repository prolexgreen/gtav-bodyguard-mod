#pragma once

#include "common.h"

#include <functional>

// One menu row. Rows with neither `select` nor `change` are drawn as dim section headers.
struct MenuItem
{
	std::string text;
	std::string value;                // shown on the right; empty for plain actions
	std::function<void()> select;     // Enter / Numpad5 (runs on the next frame, so it may spawn / yield)
	std::function<void(int)> change;  // Left / Right (-1 / +1)
	std::string help;                 // description shown under the menu while the row is selected
};

namespace Menu
{
	bool IsOpen();
	void Toggle();
	void Close();
	void Update(); // handle input and draw; call every frame

	// Helpers for modules building their pages
	inline int Wrap(int v, int n) { return ((v % n) + n) % n; }
	inline std::string OnOff(bool b) { return b ? "ON" : "OFF"; }
	inline std::string Choice(const std::string& s) { return "< " + s + " >"; }

	inline MenuItem Header(const std::string& text) { return MenuItem{ text, "", nullptr, nullptr, "" }; }
	inline MenuItem Action(const std::string& text, std::function<void()> fn, const std::string& help = "")
	{
		return MenuItem{ text, "", fn, nullptr, help };
	}
	inline MenuItem Toggle(const std::string& text, bool& flag, const std::string& help = "")
	{
		bool* f = &flag;
		return MenuItem{ text, OnOff(flag), [f] { *f = !*f; }, [f](int) { *f = !*f; }, help };
	}
	// Left/right cycles `index` through `count` entries; `label` is the current entry's display name.
	inline MenuItem List(const std::string& text, int& index, int count, const std::string& label, const std::string& help = "")
	{
		int* i = &index;
		return MenuItem{ text, Choice(label), nullptr, [i, count](int d) { *i = Wrap(*i + d, count); }, help };
	}
}
