# ScriptHookV SDK

The ScriptHookV SDK is not included in this repository because Alexander Blade's licence does not allow redistribution.

1. Download the SDK from <http://www.dev-c.com/gtav/scripthookv/> ("ScriptHookV SDK").
2. Extract it so that this folder contains:

```
sdk/
  inc/
    enums.h
    main.h
    nativeCaller.h
    natives.h
    types.h
  lib/
    ScriptHookV.lib
```

3. Build with CMake (see the main README).

The project is written against the SDK's native names as shipped in the download (`AI::`, `UI::`, `CONTROLS::`, `GAMEPLAY::` namespaces).
