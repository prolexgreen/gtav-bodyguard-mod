#pragma once

#include "common.h"

namespace Escort
{
	void Update();
	void Reset();                 // delete/release all escort vehicles (used by Dismiss all)
	void RetaskAll();             // force drivers to re-issue their escort (or chase) task
	void Forget();                // drop all state, no natives (script restart; handles may be stale)
}
