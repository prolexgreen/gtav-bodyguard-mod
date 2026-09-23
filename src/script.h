#pragma once

#include <windows.h>

void ScriptMain();
void SetModule(HMODULE module);
void DismissAll(bool force = false); // delete everything the mod spawned; force skips the "land first" safety check
