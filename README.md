# Bodyguard Mod for GTA V

A single-player bodyguard and security-detail mod for Grand Theft Auto V, built as a native ScriptHookV plugin. Spawn a suited protection team that follows you on foot and in cars, hire a personal driver who takes you to your map waypoint, roll with a full motorcade, and call in guard helicopters that rappel your men down to you.

Tested on **GTA V Enhanced**. It should also work on the Legacy edition with a matching ScriptHookV build.

## Features

**Bodyguards**
- Up to seven guards in suits who join your group, follow you, ride with you and defend you.
- Pick their weapon from the menu; drivers and pilots keep a sidearm.
- Auto-defend: anyone who attacks you or your men gets engaged without an order.
- Optional invincibility, infinite ammo and map blips.

**Orders**
- Aim at a person and press a key: everyone attacks.
- Aim at a vehicle instead: guards shoot the occupants, escort cars chase it.
- One key to regroup and cease fire.

**Escort cars**
- Guards who don't fit in your car follow in an SUV (spawned, commandeered from the street, or automatic), including a catch-up when they fall far behind.

**Personal driver**
- A chauffeur brings the car of your choice (armoured Schafter, Cognoscenti, Baller LE, Stretch, and more), pulls up at the curb, honks, puts the hazards on and opens the rear door.
- Sit in the back, set a waypoint, and he drives you there and pulls over at the side of the road.
- **Hurry** works like telling a taxi to hurry: faster, more aggressive driving, toggled from the menu or with a key mid-trip.
- When you get out he can stay at the wheel, guard the car, or walk with you.

**Motorcade**
- Lead and rear cars full of guards, fully performance-modded and driven by expert AI drivers.
- With your driver: the lead car drives the route to your waypoint, your car follows it, rear cars follow you. Nobody waits in the road for anybody.
- Driving yourself: choose **Lead to waypoint** (a lead car heads for your waypoint and you follow it) or **Follow behind** (every car chains behind you).
- On arrival everyone parks in formation at the curb. Walk away from your parked car and two cars stay to guard it while the other crews walk with you.
- Guards stay in their cars unless there are hostiles, or you turn on **Get out when you stop**.
- Speed matches your driver's Hurry setting.

**Helicopters**
- One or two guard helicopters (Maverick, Police Maverick, Annihilator) circle overhead and attack your targets.
- Press a key and their crews rappel down and join your team.
- A separate luxury transport helicopter (Swift Deluxe, Volatus, SuperVolito) lands to pick you up and flies you to your waypoint, with the guard helicopters in formation on either side.

**Quality of life**
- In-game menu with a description for every option.
- Hotkeys are ignored while the pause menu, phone, cutscenes or loading screens are up.
- Leftover guards and vehicles from a previous session are cleaned up automatically when the script starts.

## Installation

1. Install [ScriptHookV](http://www.dev-c.com/gtav/scripthookv/) (including its ASI loader, `dinput8.dll`).
2. Download the latest release from the [Releases](https://github.com/prolexgreen/gtav-bodyguard-mod/releases) page.
3. Copy `BodyguardMod.asi` and `BodyguardMod.ini` into the game folder, next to `GTA5_Enhanced.exe` (or `GTA5.exe` on Legacy).
4. Remove other bodyguard mods to avoid them fighting over your guard group.

## Controls

All keys can be changed in `BodyguardMod.ini`.

| Key | Action |
|---|---|
| `F7` | Open / close the menu |
| Arrow keys or Numpad `8` `2` `4` `6` | Navigate / change values |
| `Enter` or Numpad `5` | Select |
| `Backspace` or Numpad `0` | Back |
| `J` | Attack the person or vehicle you are aiming at |
| `K` | Regroup / cease fire |
| `L` | Call your driver / start the trip / toggle Hurry while he is driving |
| `U` | Helicopter crews rappel down to you |
| `F` | Get into the back of your driver's car or the landed transport helicopter |

See [Controls.txt](Controls.txt) for a walkthrough of each feature.

## Configuration

`BodyguardMod.ini` holds every default: keys, guard model and weapon, health and armour, follow mode and escort car, driver car and behaviour, motorcade size, formation and hurry mode, helicopter models, and combat options. Each key is commented in the file. Menu changes apply immediately for the session; edit the ini to change the defaults.

## Building from source

Requirements: Visual Studio 2022 with the C++ workload (Build Tools are enough), CMake 3.20+, and the ScriptHookV SDK.

1. Download the ScriptHookV SDK from [dev-c.com](http://www.dev-c.com/gtav/scripthookv/) and extract its `inc/` and `lib/` folders into `sdk/` (see [sdk/README.md](sdk/README.md)). The SDK is not redistributable, so it is not part of this repository.
2. Configure and build:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The plugin is written in C++17, links statically against the CRT, and depends only on `ScriptHookV.dll`. Output: `build/Release/BodyguardMod.asi` together with a copy of `BodyguardMod.ini`.

### Project layout

| Path | Contents |
|---|---|
| `src/script.cpp` | Main loop, hotkeys, input gating, session restart handling |
| `src/guards.*` | Guard roster: spawning, group membership, weapons, blips, cleanup |
| `src/escort.*` | Overflow escort cars |
| `src/chauffeur.*` | Personal driver: pickup, waypoint driving, curbside stops, hurry |
| `src/motorcade.*` | Convoy modes, speed governor, formation parking, guard detail |
| `src/heli.*` | Guard helicopters, rappelling, transport helicopter |
| `src/combat.*` | Attack orders, auto-defend, target tracking |
| `src/menu.*` | Menu rendering and input |
| `src/vehicles.*` | Shared vehicle helpers: spawning, road-side positions, tuning |
| `src/registry.*` | Tracks spawned entities across script restarts for cleanup |
| `src/config.*` | INI parsing and model / weapon lists |

## Known limitations

- Landing spots for the transport helicopter are chosen near you without checking for trees or overhangs. Call it in an open area.
- Road-side positions are derived from the game's road network data and can be slightly off on unusual roads.
- The 7-guard cap is the game's own ped-group limit. Motorcade and helicopter crews are managed separately and do not count towards it until they join you on foot.

## Credits

- Alexander Blade for ScriptHookV and its SDK.
- The community-maintained native reference (nativedb) for native documentation.

## Licence

Released under the [MIT Licence](LICENSE).
