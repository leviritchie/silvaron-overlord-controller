# Silvaron Overlord: controller support

Gamepad support for **Total War: WARHAMMER III**, for use with the [Silvaron Overlord](https://github.com/leviritchie/tww3-cinematic-director) mod.

The Steam Workshop can only deliver `.pack` files, so the controller's native part has to be installed by hand. It is one file, `dinput8.dll`, which goes in the game folder. With it you can play the whole game with a controller from launch: the main menu, the campaign map, every panel, pre-battle, deployment and battle. Silvaron Overlord then adds its own controller features on top: picking units, giving orders with the game paused, drawing paths on the ground, and button prompts.

Without this DLL, Silvaron Overlord works as normal; its controller features stay switched off.

## Requirements
- Total War: WARHAMMER III on Windows (Steam).
- The Silvaron Overlord mod, enabled in the launcher. The DLL does nothing unless `silvaron_overlord.pack` is ticked.
- An Xbox-style controller (XInput), or a PlayStation DualSense, DualSense Edge or DualShock 4 (USB or Bluetooth).

## Install
1. Download `dinput8.dll` from the [latest release](../../releases/latest).
2. Open the game folder: in Steam, right-click Total War: WARHAMMER III > Manage > Browse local files. This is the folder that holds `Warhammer3.exe`, usually `C:\Program Files (x86)\Steam\steamapps\common\Total War WARHAMMER III`.
3. Copy `dinput8.dll` into that folder, next to `Warhammer3.exe`.
4. **Turn Steam Input off for the game**: in Steam, right-click Total War: WARHAMMER III > Properties > Controller > set it to "Disable Steam Input". With Steam Input on, Steam turns the controller into its own mouse and keys, so every input arrives twice or not at all.
5. In the game launcher, enable Silvaron Overlord (`silvaron_overlord.pack`) and start the game.

Windows may warn about a downloaded DLL. If you'd rather not trust a prebuilt file, build it yourself from this source (see Building).

## Uninstall
Delete `dinput8.dll` from the game folder. You can also delete `silvaron_pad.cfg`, `silvaron_pad.txt`, `silvaron_pad_mode.txt` and `silvaron_pip_log.txt` there; the DLL and the mod create these.

## Controls
### Everywhere (menus, campaign, battle)
| Controller | Does |
|---|---|
| Left stick | Moves the mouse cursor (faster when held fully over) |
| A / Cross | Left click (hold to drag) |
| B / Circle | Right click (hold to drag, e.g. a battle formation) |
| Right stick | Moves the camera (W/A/S/D) |
| LB / RB | Rotate the camera (Q/E) |
| LT / RT | Zoom out / in (mouse wheel) |
| D-pad | Arrow keys |
| X / Square | Enter |
| Y / Triangle | Space |
| Start / Options | Escape |
| Back / Create, touchpad | Tab |

### Campaign map (with Silvaron Overlord)
| Controller | Does |
|---|---|
| D-pad left / right | Previous / next lord |
| D-pad up / down | Previous / next settlement |
| Y | The next required action; hold for 1.5 s to end the turn |
| X | Panel navigator: the D-pad moves between buttons, LB/RB change area, A presses, B closes |

### Battle (with Silvaron Overlord)
| Controller | Does |
|---|---|
| D-pad left / right | Previous / next unit, in unit-card order |
| D-pad up | Order mode for that unit: the game pauses and the tactical map opens |
| D-pad down | That unit's tactics card |
| Y | Turns the cinematic camera on or off |
| X | Clears the selection |

In order mode, the right stick draws the unit's path on the ground. Ending the path on an enemy makes it an attack (shooters shoot it). The list on the right sets how the unit moves and what it does when it arrives, and holds its abilities. **A** on Go sends the order and resumes the battle; **B** clears the line, and a second B leaves. The button prompts on the right of the screen show what each button does at that moment.

The battle and campaign features can be turned off in the mod's settings (Mod Configuration Tool): "Controller orders in battle", "Controller buttons on the campaign map", "Controller button prompts".

## Settings: silvaron_pad.cfg
The first launch writes `silvaron_pad.cfg` to the game folder, with the defaults and comments. Edit it to change any binding, the cursor speed, the deadzone or the button pictures (Xbox or PlayStation). Changes apply while the game runs. Delete the file to get the defaults back. Set `enabled = 0` to turn the controller off without removing the DLL. All options are described in [PAD.md](PAD.md).

## Troubleshooting
- **Nothing happens:** check that `dinput8.dll` is next to `Warhammer3.exe` (not in `data`), that Silvaron Overlord is ticked in the launcher, and that Steam Input is off. Then look at `silvaron_pip_log.txt` in the game folder: lines starting with `pad:` say which controller was found, which mod list was read, and what went wrong.
- **Inputs doubled, or the cursor jumps:** Steam Input is still on for the game.
- **A PlayStation controller isn't found:** connect it before starting the game, or replug it; it is looked for every 2 s.

## What the DLL does
Windows loads `dinput8.dll` from the game folder before the system's copy; this one passes every DirectInput call on to the real system DLL. It:
- reads the controller with XInput (or the HID reports of a Sony controller);
- turns the controller into mouse and keyboard input, through Windows (SendInput) and through the game's own DirectInput devices, only while the game's window is in front;
- writes the controller's state to `silvaron_pad.txt` in the game folder, where Silvaron Overlord's scripts read it, and reads `silvaron_pad_mode.txt`, where the scripts say which buttons they are using.

It makes no network connections and changes nothing outside the game folder.

The same DLL also carries code for two of the author's other mods, Silvaron PiP and Silvaron UI Scale. That code stays off unless their packs (`silvaron_pip.pack`, `silvaron_ui_scale.pack`) are ticked in the launcher.

## Building
Install Visual Studio 2019 or 2022 (Community is fine) or the Build Tools, with the "Desktop development with C++" workload. Then run `build.bat`, which writes `dinput8.dll` next to it. The "vswhere.exe is not recognized" line printed during the build comes from Visual Studio's own setup script and does no harm.

## License
MIT; see [LICENSE](LICENSE). Total War: WARHAMMER III is a trademark of The Creative Assembly and Sega; this project is not affiliated with them.
