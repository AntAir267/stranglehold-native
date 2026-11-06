# Contributing

Thanks for looking. This is an early, mostly untested port (see the README), so reports from
other computers and from later in the game are the most useful thing right now.

## Reports

Open an issue with your Linux distribution, graphics card and driver, how far you got, and the
end of `last-run.log` from `~/.local/share/stranglehold-native`. Never attach game files,
saves made from them, or anything extracted from the game.

## The rules

- **No game files, ever.** Nothing from the game goes into the repository: no code, data,
  pictures, sounds, shaders or text taken from it, and no C generated from it (`gen/` stays
  local). Facts about the program (addresses, offsets, file formats, hashes) are fine, and
  so is code you wrote that does what a game function does.
- **The original stays reachable.** The game plays as it shipped; anything new is an option,
  and with it off the game behaves as before. Gameplay, timing and physics never change.
- **Settings live in the game's own menus**, drawn in its own style. Files and environment
  variables can exist for testing, but never as the only way.
- **Every controller works**, plugged in before or after the game starts, in any order, with
  the keyboard and mouse still working alongside.
- **Evidence, not guesses.** In DESIGN.md and code comments, mark claims about the game's code
  [verified] with how you checked (disassembly, a runtime read, a test run) or [guess].
- **Checks must pass.** Build from clean, and run the tests in `tests/` that touch what you
  changed; say in the pull request what you ran and what it showed.
- Match the surrounding code: C for the runtime, plain Python for tools, the same comment
  style. Keep changes small and on one subject.

## Pull requests

Every outside pull request is read in full before anything in it is built or run, and only
the maintainer merges. Explain what the change does and why, and how you tested it.

By contributing you agree to release your work into the public domain under the Unlicense,
like the rest of the code here.
