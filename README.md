# MHW DPS Meter (native)

An in-game damage meter for **Monster Hunter: World / Iceborne** written in C++ as a
**Stracker's Loader** plugin, so it runs anywhere Stracker's Loader does, including
**CrossOver on macOS** (D3DMetal) where SharpPluginLoader's .NET host does not.

It is a native port of the C# [mhw-dps-meter](https://github.com/happypurpleduck/mhw-dps-meter) SharpPluginLoader plugin
and writes the same fight-log format, so that project's web and GPUI viewers read these
logs unchanged.

During a quest it shows each hunter's **name**, **damage**, **DPS** and **% of party
damage** in the top-right corner, and **health bars for the large monsters** on the map
at the top centre. In the **training area** it counts your damage against
the pole, with a reset key and a fixed-length **time trial** with per-move results and
personal bests. Every quest is saved as a JSON fight log.

## Requirements

- Monster Hunter: World: Iceborne, Steam build **421810** (the current build). The
  build number is in the window title, `MONSTER HUNTER: WORLD(421810)`.
- [Stracker's Loader](https://www.nexusmods.com/monsterhunterworld/mods/1982)
  (`dinput8.dll` + `loader.dll` beside `MonsterHunterWorld.exe`).
- **DirectX 11** mode (the game's default; `DirectX12Enable=Off` in `graphics_option.ini`).

No .NET, no SharpPluginLoader, no CRCBypass.

## Install

No compiling needed:

1. Install [Stracker's Loader](https://www.nexusmods.com/monsterhunterworld/mods/1982) if you
   don't have it yet.
2. Download `MhwDpsMeter-native-<version>.zip` from
   [Releases](https://github.com/JimLau49/mhw-dps-native/releases).
3. Extract it into the folder that contains `MonsterHunterWorld.exe` (in Steam: right-click
   the game → Manage → Browse local files). You should end up with:

   ```text
   Monster Hunter World/
     dinput8.dll                       Stracker's Loader
     loader.dll
     nativePC/plugins/MhwDpsMeter.dll  this plugin
   ```

4. Start the game and press **F9** (fn+F9 on a Mac keyboard): the settings panel shows the
   plugin loaded.

It is a regular Windows DLL, so it should run wherever Stracker's Loader does: Windows,
Linux/Proton (launch option `WINEDLLOVERRIDES="dinput8=n,b" %command%`) and macOS/CrossOver.
So far it has only been tested on CrossOver. On first launch it creates
`nativePC/plugins/MhwDpsMeter/` with `settings.json`, `MhwDpsMeter.log` and `logs/`. To
uninstall, delete `nativePC/plugins/MhwDpsMeter.dll`.

To build and install from source on a Mac instead (finds a bottle named `Steam` in
`~/CXPBottles` or CrossOver's default `~/Library/Application Support/CrossOver/Bottles`):

```sh
make                      # needs: brew install mingw-w64
scripts/install.sh        # MHW_DIR=/path/to/game scripts/install.sh for another location
scripts/install.sh uninstall
```

## Keys

| Key | Action |
| --- | --- |
| **F9** | Settings / diagnostics panel (drag the meter by its title bar while open) |
| **F10** | Show / hide the overlay |
| **F7** | Training area: reset damage (also clears a finished trial) |
| **F8** | Training area: arm / cancel a time trial |
| **F6** | Write `logs/live-debug.json` diagnostics |

On a Mac keyboard the F-keys trigger media functions unless you hold **fn** (or enable
*Use F1, F2, etc. keys as standard function keys* in System Settings → Keyboard). Any
key can be rebound in `settings.json`, e.g. `"menu": "Ctrl+F9"`, `"overlay": "Insert"`,
`"reset": "0x77"`.

## What it shows

| Column | Meaning |
| --- | --- |
| Name | Party slot name (`*` = you), coloured like the in-game party HUD |
| Dmg | The quest-award total per hunter (the results-screen value). Solo or arena quests that never allocate that table fall back to your own hooked hits, then to large-monster HP lost |
| DPS | Damage / hunt time. The hunt clock is the in-game quest timer when it is readable (right for SOS joins), otherwise the plugin's own clock from the moment the quest starts |
| % | Share of the party total |

The overlay appears once you depart on a quest and stays on the results screen until
you leave the map. It stays hidden in the hub, expeditions and the Guiding Lands, which
have no quest-award table.

### Monster health

A second window lists every large monster alive on the current map with its HP bar
(green above 50 %, yellow above 25 %, then red) and `current / max` HP. It also works in
expeditions and the Guiding Lands, and disappears when the monster dies or is captured.
Small monsters, barrels and training targets are not shown. Turn it off with **Monster
health bars** in the F9 panel (`showMonsterHealth` in `settings.json`); F10 hides it
together with the damage meter. While F9 is open both windows can be dragged.

### Training area and time trials

In the training area the meter counts every hit you land (the pole and wagon are not
monsters). The DPS clock starts at your **first hit**; **F7** resets it.

**F8** arms a time trial (duration in the F9 panel: 5–600 s, presets 30/60/90/120/180).
The window starts on your first hit and ends exactly `duration` seconds later on the
hits' own timestamps. When it ends the result freezes with the top five moves and
whether it is a personal best for that weapon and duration. Trials are saved as fight
logs (`kind: "trial"`), and personal bests come from `logs/index.json`.

## Fight logs

```text
nativePC/plugins/MhwDpsMeter/logs/YYYY-MM-DD_HHmmss_<questId>_<result>.json
nativePC/plugins/MhwDpsMeter/logs/YYYY-MM-DD_HHmmss_trial<seconds>s_trial.json
nativePC/plugins/MhwDpsMeter/logs/index.json
```

Schema 2, identical to the C# plugin's: players, monsters, 2-second damage samples, every
one of your hits (crit, tenderized, attack id, monster part, move name), and events
(join/leave, weapon, cart, monster death). Point the C# project's `web/` or
`gpui-viewer/` at the `logs` folder to browse them. Logs are written on a background
thread, so the results screen does not hitch.

## Settings

`nativePC/plugins/MhwDpsMeter/settings.json` (written with defaults on first run):

| Key | Default | |
| --- | --- | --- |
| `overlayVisible`, `overlayOpacity` | `true`, `0.92` | also changed from F9 / F10 |
| `showMonsterHealth` | `true` | large-monster health bars |
| `trialDurationSeconds` | `60` | |
| `fontSize` | `0` | 0 = scale with the window height (~16 px at 1080p) |
| `keys.menu` … `keys.diagnostics` | F9, F10, F7, F8, F6 | `Ctrl+`, `Shift+`, `Alt+` prefixes, names like `Insert`, `Numpad5`, or `0x77` |
| `hooks.partContext` | `true` | monster part of each of your hits |
| `hooks.actions` | `true` | move name of each of your hits |
| `hooks.mainUpdate` | `true` | poll on the game thread (else the render thread) |
| `hooks.questNames` | `true` | read quest names from the game's text |
| `writeFightLogs` | `true` | |
| `fontPath`, `cjkFontPath` | `""` | override the font search (Windows paths) |
| `logLevel` | `"info"` | `"debug"` for more detail |

Fonts: the first of Segoe UI / Tahoma / Arial / Verdana found in the bottle, with Source
Han Sans / Meiryo / MS Gothic / YaHei (or macOS Hiragino through `Z:`) merged in so
Japanese, Chinese and Korean hunter names render.

## Troubleshooting

Everything the plugin does is logged to `nativePC/plugins/MhwDpsMeter/MhwDpsMeter.log`
(the previous run is kept as `MhwDpsMeter.log.prev`). Open the F9 panel and expand
**Diagnostics** for the live state, or press F6 to write it to `logs/live-debug.json`.

- **No log file at all**: Stracker's Loader did not load the plugin. Check `dinput8.dll`
  and `loader.dll` are beside the exe and the DLL is directly in `nativePC/plugins/`.
- **Log ends at "waiting for the game window"** or **no `overlay initialized` line**:
  the Present hook did not attach; the log says which route was tried (the game's own
  swap chain via `sMhRender`, or a probe device).
- **`DirectX 12 mode is not supported`**: switch the game to DirectX 11.
- **`Hit hook: off` / `no address map for build`**: the game build is not 421810. Drop a
  HunterPie-format `MonsterHunterWorld.<build>.map` into
  `nativePC/plugins/MhwDpsMeter/Addresses/` (see `addresses/`); keys missing from the
  file keep the built-in values.
- A hook that misbehaves can be switched off under `hooks` in `settings.json`.

To remove the plugin, delete `nativePC/plugins/MhwDpsMeter.dll` (or run
`scripts/install.sh uninstall`).

## How it works

- **Loading.** Stracker's `dinput8.dll` proxy loads `loader.dll`, which `LoadLibrary`s
  every DLL in `nativePC/plugins`. That happens while Steam's DRM stub resolves the game's
  imports: the exe is already decrypted, but WinMain has not run. `DllMain` only hooks
  `cSystem`'s constructor (to record the game's singletons as they are created) and
  starts a worker thread.
- **Game state** is read only through guarded reads: a VirtualQuery pre-check plus a
  copy routine whose access violations are absorbed by a vectored exception handler
  (verified under Wine + Rosetta by the harness). Nothing calls into game objects
  except the quest-name lookup, which runs on the game thread with a verified `sQuest`.
  Addresses come from the HunterPie-format map for the build (`addresses/`):
  quest state/timer/carts (`sQuest`), stage, local hunter and weapon, the party roster
  and quest-award damage table, and the large-monster list.
- **Hooks** (MinHook): `FUN_DEAL_DAMAGE` (your exact hits) and the caller that still
  holds the collision context (monster part), from the address map; and, found by
  SharpPluginLoader's byte signatures, `ActionController::DoAction` (move names),
  `sMhMain` update (game-thread polling) and `GetQuestName`. Every detour forwards all
  register arguments and the return value unchanged.
- **Overlay.** The worker takes the game's `IDXGISwapChain` from `sMhRender`
  (`+0x78 → +0x1488`, DX11), validates that its vtable lives in a DXGI/D3D module, and
  inline-hooks `Present` and `ResizeBuffers`. If `sMhRender` cannot be found it creates a
  throwaway device on a hidden window to locate them. Dear ImGui (DX11 + Win32 backends)
  draws on the game's own device; `D3DCompile` comes from the bottle's
  `d3dcompiler_47.dll` at runtime. Window messages are queued and replayed into ImGui on
  the render thread, and while F9 is open the clicks ImGui uses are kept from the game.
- **Meter.** A port of the C# plugin's state machine, polled at 10 Hz: quest start/end,
  results screen, training mode, time trials, the award-table / hooked-hit / monster-HP
  merge, carts, and fight-log recording.

Differences from the C# plugin: teammates get no estimated per-move rows (logs are
`hitCoverage: "local"`); monster events are limited to `death`, inferred from HP
(SharpPluginLoader's enrage/flinch callbacks have no equivalent here); carts are
attributed to you when your HP hit zero, otherwise left unattributed.

## Building and testing

```sh
brew install mingw-w64
make                         # build/MhwDpsMeter.dll and build/dps-harness.exe
make package                 # dist/MhwDpsMeter-native-<version>.zip (game-root layout)
scripts/run-harness.sh       # self-test inside the CrossOver bottle
```

The harness runs in the bottle and checks guarded reads (including a real fault on
freed memory), pattern scanning, the address maps, game-data tables, fight-log JSON and
`index.json`, and the full Present-hook + ImGui path on the bottle's D3D11
implementation. It saves the rendered frame as `build/harness-frame.bmp`.
`--no-render` skips the window.

`make data` regenerates `src/game_data_tables.inc` from the C# project's `data/` folder
(expects a checkout of it next to this one: `../mhw-dps-meter`, or `make data CS_DATA=<dir>`).

## Credits

Memory layouts and address maps: [HunterPie](https://github.com/HunterPie/HunterPie)
(Apache-2.0); signatures and struct offsets: [SharpPluginLoader](https://github.com/Fexty12573/SharpPluginLoader)
(MIT); hunt logic, log schema and verified part-damage context: the C#
[mhw-dps-meter](https://github.com/happypurpleduck/mhw-dps-meter). Libraries:
[Dear ImGui](https://github.com/ocornut/imgui) 1.92.9 (MIT),
[MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (BSD-2),
[nlohmann/json](https://github.com/nlohmann/json) 3.12.0 (MIT).
