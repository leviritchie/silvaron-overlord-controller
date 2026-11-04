# Controller: the native base layer

Part of `dinput8.dll` (`pip.cpp`, section "controller: the native base layer"). It makes the whole game usable with an XInput pad (Xbox-style), or a DualSense/DS4 read over HID, from launch: frontend, campaign, every menu, pre-battle, deployment and battle. It turns the pad into mouse and keyboard input. It also writes the pad's state to a file for the battle script's own controller layer (`CD_PAD`, `script/battle/mod/cinematic_director_pad.lua`), which handles unit selection, order mode and the drawn paths.

Technical notes for the controller base layer. Install steps for players are in README.md.

## Install
1. Build: run `build.bat` (it finds Visual Studio with vswhere and calls vcvars64), or take `dinput8.dll` from the latest release.
2. Copy `dinput8.dll` next to `Warhammer3.exe`. This is the same DLL as Silvaron PiP and Silvaron UI Scale.
3. Tick `silvaron_overlord.pack` in the launcher. The layer runs whenever that pack is ticked, whether or not `silvaron_pip.pack` is. It uses none of the PiP's hooks (no Present hook, no frame patches): it adds only its own thread and the DirectInput wrapper.
4. Turn **Steam Input off for TWW3**: Steam > Library > Total War: WARHAMMER III > Properties > Controller > "Disable Steam Input". With Steam Input on, Steam may turn the pad into its own mouse/keys (doubling ours) or hide it behind a virtual pad. The log says when Steam's environment variables suggest it is on. That check is a guess.

The layer stays off while the game builds a startpos (`process_campaign_startpos` in `user.script.txt`). It is also off when `silvaron_pad.cfg` says `enabled 0`.

## Default controls (every screen)
| Pad | Does |
|---|---|
| Left stick | Cursor. Speed goes with tilt squared, up to 2.5x after 0.8 s fully over. |
| A | Left mouse button; hold it to drag. |
| B | Right mouse button; hold it to drag (e.g. a battle formation). |
| Right stick | Camera pan: W/A/S/D held (each direction past half tilt). |
| LB / RB | Q / E (rotate). |
| LT / RT | Mouse wheel down / up (zoom out / in), repeated while held: every 250 ms lightly pressed, every 60 ms fully pressed. |
| D-pad | Arrow keys. |
| X | Enter (taken by the campaign layer on the map, where Enter ends the turn). |
| Y | Space. |
| Start | Escape. |
| Back | Tab. |
| L3 / R3 | Nothing. |

Input is sent only while a window of the game is in front. Everything held is let go when the game loses focus, when a binding is taken by the battle script, when the pad disconnects, and when the DLL unloads. Held keys don't auto-repeat.

## silvaron_pad.cfg (game folder)
Written with the defaults when missing, and read again when it changes (checked every 2 s). Lines are `name = value` (or `name value`); `#` starts a comment.
- `enabled 1|0`: 0 turns the whole layer off (no mouse/keys and no `silvaron_pad.txt`).
- `inject both|di|sendinput|none`: the path or paths keys and buttons take (see below). `none` only writes the pad file.
- `cursor setpos|sendinput`: setpos puts the cursor at the new spot, kept inside the game window's client area; sendinput sends relative moves (subject to Windows pointer acceleration). setpos is used whenever SendInput isn't among the paths.
- `cursor_speed` (pixels a second at full tilt at 1080 high, scaled by the window's height; default 900), `deadzone` (radial, both sticks, 0-0.9; default 0.24).
- `glyphs auto|xbox|playstation` (default auto): the button pictures the scripts show (the pad file's family). auto picks PlayStation for a Sony pad, also one behind Steam Input, else Xbox. Any other value is not understood and auto kept.
- Bindings: `ls = cursor|none`. Then `a b x y lb rb lt rt start back l3 r3 dpad_up dpad_down dpad_left dpad_right rs_up rs_down rs_left rs_right`, each `= <action>`. An action is one of:
  - a key: a-z, 0-9, f1-f12, enter, space, escape, tab, backspace, up, down, left, right, shift, ctrl, alt, home, end, pageup, pagedown, insert, delete, grave, minus, equals, comma, period;
  - `mouse_left`, `mouse_right`, `mouse_middle`;
  - `wheel_up`, `wheel_down` (repeated while held);
  - `none`.

  Keys are sent by scancode of the current keyboard layout (`MapVirtualKey`), so a letter is the key that types it on that layout. A line not understood is logged and the default kept.

## The two injection paths
Which path the game reads is unknown. The exe imports only `DINPUT8!DirectInput8Create` and holds GUID_SysKeyboard/GUID_SysMouse, but it may also use window messages or raw input.
- **di**: `DirectInput8Create` (our proxy) returns a wrapped `IDirectInput8W`. Its `CreateDevice` wraps the system keyboard and mouse (`IDirectInputDevice8W`).
  - `GetDeviceState`: our held keys are ORed into the 256-byte keyboard state; our cursor motion, wheel and buttons are added to `DIMOUSESTATE(2)`.
  - `GetDeviceData`: our key, button, motion and wheel events are appended after the device's own, for a device whose buffer the game reads.
  - Only the W interface is wrapped (the exe holds IID_IDirectInput8W); an A request is logged and passed through.
- **sendinput**: `SendInput` with `KEYEVENTF_SCANCODE` (extended keys flagged) and mouse button/wheel/move events. This reaches the game whichever way it reads input, but only while the game is in front. It also reaches the DI devices (their buffers and deltas), so with `both` the DI side only ORs held keys and buttons into `GetDeviceState` (idempotent) and queues no `GetDeviceData` events, adds no wheel to `lZ`, and adds motion only when the cursor is moved by `SetCursorPos` (which makes no DI motion): each press, tick and move arrives once. With `di` alone the DI side does all of it. Events carry no `dwExtraInfo` (2026-11-02, in game the main menu took no clicks): the game's window procedure skips mouse messages whose `GetMessageExtraInfo()` isn't 0 (exe 0x1412772bd, `test rax, rax` / `jne` past the handler; meant for touch and pen), and the tag `0x5adda` they carried lost every click.

## silvaron_pad.txt (written, game folder)
Written atomically (`.tmp`, then `MoveFileExA` REPLACE_EXISTING), on change (at most every 16 ms) and at least every 250 ms. A replace that fails, because a reader holds the file open without delete sharing, is retried on the next write.
```
seq conn btns lx ly rx ry lt rt ix iy t fam src
p <16 press counts>
r <16 release counts>
end
```
- `seq`: counts up with each write.
- `conn`: 0 or 1.
- `btns`: XInput `wButtons`, decimal.
- Sticks: -1..1 after the radial deadzone, y up.
- Triggers: 0..1, raw (no threshold).
- `ix iy`: the right stick integrated over real seconds since start, so a reader diffs them and loses no motion between polls.
- `t`: ms since the pad thread started.
- `fam` (2026-11-02): the pad's family for button pictures. 0 none, 1 Xbox, 2 PlayStation. PlayStation when the pad is read over HID, or when XInput is in use and a Sony pad is present over HID (Steam Input makes a DualSense an XInput pad). `glyphs` in the cfg overrides it while a pad is connected. 0 without a pad (`pad_family`).
- `src` (2026-11-02): where the pad is read. 0 none, 1 XInput, 2 HID.
- The first 12 fields are as before; the battle script needs 12 and ignores the rest.
- Counts are latched since start, in `wButtons` bit order: up down left right start back l3 r3 lb rb (0x400) (0x800) a b x y. The 0x400/0x800 slots stay 0. Triggers have no counts; use their values.
- A disconnected pad, or no XInput at all, still writes `conn 0` every 250 ms.

## silvaron_pad_mode.txt (read, game folder)
Written by the battle script:
```
mode <battle|order> <t>
take <names>
remap <words>     (optional)
pulse <id> <wheel> <fwd_ms> <right_ms>     (optional)
end
```
Names are `dpad a b x y lb rb lt rt ls rs start back l3 r3` (`ls` = the cursor, `rs` = the pan keys). Taken inputs make no mouse/keys while the file's last write is under 1.5 s old (system time against its mtime). A file without `end` is ignored until complete. The file is checked every 20 ms. An input taken while held stays out of the base layer until it is released, even once the script gives it back (a B that closes order mode is still down when the mode file says `battle` again, and would otherwise also be a right-click).

`remap` (2026-11-03) changes what untaken inputs do while the file is fresh: `ls_keys` makes the left stick hold the right stick's bindings (W/A/S/D: pressed past 0.5 tilt, released below 0.35) and stops it moving the cursor (order mode on the tactical map: the stick pans it); `lbrb_wheel` makes LB the wheel down and RB the wheel up (not used by the scripts: LB/RB are the game's rotate). A stale file clears the remap.

`pulse` (2026-11-03) is a one-off camera nudge for zoom to fit on the tactical map: each new id is applied once, `wheel` notches (positive = wheel up) one every 40 ms, and the right stick's bindings (W/S for forward, D/A for right; negative = the other way) held for the given ms. It goes through the same paths as the base layer's wheel and keys.

## XInput
`xinput1_4.dll`, else `xinput9_1_0.dll`, loaded with LoadLibrary and GetProcAddress (no new link libraries). Empty slots are probed every 2 s (a disconnected slot can stall the call). The connected slot is polled every 8 ms (a high-resolution waitable timer; else Sleep). Only one pad is used, the first slot found.

## Sony pads over HID (2026-11-02)
A DualSense has no XInput unless Steam Input or DS4Windows makes one (in the user's test, with Steam Input on, it showed as XInput slot 0 through Steam's virtual pad). With Steam Input off, as advised above, it was invisible. `hid_thread` therefore reads DualSense (054c:0ce6), DualSense Edge (0df2) and DualShock 4 (05c4, 09cc, 0ba0) HID reports itself, over USB and Bluetooth.
- The device is found with SetupDi, opened shared for overlapped reads, and re-listed every 2 s while none is open. A pad silent for 2 s counts as gone. Links `hid.lib setupapi.lib`.
- While an XInput pad is in use (`g_xi_conn`, set by the pad thread) the Sony pad is never opened for reading. A pad being read is closed when XInput connects. Instead the list is checked every 2 s, opening each device with no access for its vendor and product ids only (`hid_open(..., read false)`). That sets `g_hid_seen`, so a DualSense behind Steam Input still reads as PlayStation. Logs `XInput in use - a Sony pad is there too` or `no Sony pad over HID`, and `an XInput pad is in use - the <pad> isn't read`.
- A Bluetooth pad gets its calibration feature report read (0x05 DualSense, 0x02 DS4), which switches it to full reports.
- `sony_parse` reads the DualSense full report (USB 0x01 of 64 bytes, Bluetooth 0x31), the DS4 report (USB 0x01, Bluetooth 0x11) and a Bluetooth pad's short 0x01 report.
- The result is an `XINPUT_GAMEPAD`: cross A, circle B, square X, triangle Y, L1/R1 the bumpers, options Start, create/share and the touchpad click Back, the PS button nothing.
- It is used only while no XInput pad is connected, so Steam's or DS4Windows' remapping comes first.
- Logs: `<pad> found over HID, reports of N bytes`, `full reports asked for`, `report 0x31 (first)`, `in use: the <pad> read over HID`, `gone`, and `couldn't be opened` (HidHide or DS4Windows holding it alone).
- Checked offline with a parser test and a live read of a DualSense over Bluetooth (report 0x31, sticks centred).

## Log (silvaron_pip_log.txt, `pad:` lines)
- The XInput dll, `connected, slot N`, `slot N disconnected`.
- The settings read or changed, and lines not understood.
- `DirectInput8Create(...)` and whether it was wrapped; each `CreateDevice`; each device's `SetDataFormat` size, `SetCooperativeLevel` flags and `SetProperty` (DIPROP number and value).
- The first call of each method, then each device's method counts over its first 10 s.
- The process's raw input registrations at 10 s and 60 s.
- Mode changes and the mode file going stale.
- The game coming in front or leaving it (20 lines).
- SendInput refusals.
- `buttons shown as PlayStation|Xbox|none`, when the family changes.

After an in-game run these lines tell which path works.

## Testing
Offline: a test program (not included in this repository) includes `pip.cpp` whole and runs 33 `CHECK` lines. They cover:
- the cfg parser (defaults, the default file text, overrides, bad lines, `glyphs`);
- the family rule (none, Xbox, PlayStation over HID or behind XInput, the cfg override);
- key names period, comma, h and 0-9;
- the mode parser (battle and order take sets);
- the radial deadzone;
- the pad file's format;
- the atomic write, including the refusal while a reader holds the file;
- the DI path's held keys and queued events;
- through the real system DirectInput, a wrapped keyboard whose `GetDeviceState` shows our W held and whose `GetDeviceData` returns our key-up.

## Unverified in game
- Whether the game reads keyboard/mouse through DI state, DI buffered data, raw input or window messages, so which path works, and whether `both` doubles wheel or motion.
- Whether `SetCursorPos` moves the game's cursor (it may draw its own from DI or raw deltas).
- Whether TWW3's camera keys default to W/A/S/D/Q/E on every layout.
- Whether scancode `SendInput` reaches the frontend's text boxes as wanted.
- Whether the Steam Input check fires.
- Whether a DualSense behind Steam Input still shows in the HID list (HidHide or similar would hide it, and the family would read Xbox; `glyphs playstation` fixes that). An Xbox pad in use with a Sony pad also plugged in reads as PlayStation.
- How the layer behaves with Steam's overlay open (the overlay is in-process, so it counts as the game in front).
