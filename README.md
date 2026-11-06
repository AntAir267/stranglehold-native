# Stranglehold native

An unofficial native **Linux** version of **John Woo Presents Stranglehold** (Midway, 2007),
made by static recompilation. The game's own program (Retail-Stranglehold.exe from the GOG
release) and the DLLs it ships with are translated into C on your computer, then built against
a new stand-in for the parts of Windows it uses, on SDL3. Its Direct3D 9 drawing goes to Vulkan
through DXVK.

> **Generally untested.** So far only the menus and the first level have been played, with a
> controller, on one computer (a GPD Win Max 2, Radeon 890M). Nothing past that has been
> played. Expect crashes and missing pieces, and please report what you find in Issues. There
> are no downloads: you build it yourself from your own copy of the game.

**Bring your own game.** This repository holds only the translator, the runtime and tools,
never any of the game's code or data. You need the **GOG release** of Stranglehold (version
1.1 v2); the build checks each file it translates against the GOG files' SHA-256 before
anything is built. The C made from the game stays on your computer (`gen/`, ignored by git).

## What works

- Menus, movies, music and sound, keyboard and mouse.
- Controllers with the Xbox 360 version's controls: its four controller layouts, its
  Control Config screen (picked automatically while a controller is in use), Xbox button
  pictures in the menus and button prompts, rumble, and Auto Aim starting at level 2 as on the
  360 (Pause > Options > Aim Assist changes it). With keyboard and mouse the PC menus come back.
- Every resolution your screen offers (the PC version only listed 15); a picture of a different
  shape from the screen gets bars instead of being stretched.

What hasn't been checked: anything past the first level, saving and loading over a whole game,
other graphics cards, multiplayer.

## Building

You need 64-bit Linux, about 6 GB of free memory for the translation step and some more for
compiling, and:

- gcc, make, Python 3 with [Capstone](https://www.capstone-engine.org/) 5.0 (a `.venv` in this
  folder is used if present: `python3 -m venv .venv && .venv/bin/pip install capstone==5.0.9`),
  and liblzo2 (the game's packages are LZO-compressed).
- SDL3 3.4.18, built as a shared library into `third_party/sdl3/`.
- DXVK 3.1.1, built natively with SDL3 (only d3d9), installed into `third_party/dxvk-native/`.
  See [third_party/README.md](third_party/README.md).
- The game: unpack the GOG installer (for example with innoextract) so that `Binaries/` and
  `StrangleholdGame/` are in `~/.local/share/stranglehold-native/game`, or pass `GAME=<folder>`.

Then `make` translates the program (a few minutes, about 74,000 functions), compiles the result
(the slow part: half an hour or more), and writes the controller data into your user folder
(`~/.local/share/stranglehold-native`) from your own game files. `make install` puts the game in
`~/.local/opt/stranglehold-native` with an applications-menu entry.

Run `build/stranglehold` (add `--window` for a window). Saves and settings go to the user
folder; `options.txt` there holds the port's few extra settings (display, rumble,
controller_layout, frame_limit).

## How it's made

[DESIGN.md](DESIGN.md) is the full working record: what is translated, every hook into the game
and why, the controller work, the decoded Midway menu format (`tools/screenset.py`), and what
was tried and dropped. Code facts are marked [verified] with their evidence or [guess].

- `tools/`: the translator (x86 to C), package and menu tools for the game's files.
- `runtime/`: the Windows, Direct3D, DirectSound and DirectInput stand-ins, and the game hooks.
- `builds/gog.py`: everything specific to this one build of the game, as addresses only.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Never attach or commit game files.

## Licence

The code here is released into the public domain under the [Unlicense](LICENSE).
`third_party/dr_libs/dr_flac.h` is David Reid's (public domain or MIT-0). SDL3 and DXVK (zlib
licence) are not included; you build them yourself.

Stranglehold is a trademark of its owners. This project is not affiliated with or endorsed by
Midway, Warner Bros. Games, Tiger Hill Entertainment or GOG.
