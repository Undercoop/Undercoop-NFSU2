# Undercoop 6.13.29

The ASI prepares its installation workspace automatically when the game starts.

- A clean game folder needs the ASI and its connection INI alongside the already installed compatible ASI loader.
- The first launch creates missing `mods`, `mods/_undercoop`, `mods/_undercoop/state`, and `global.ini`.
- A missing loader configuration uses the supported loader's deferred initialization default, so startup no longer silently skips the launcher.
- Existing configuration files, selections, modpacks and careers retain their contents. Additional archive and Binary support files are extracted from embedded resources when needed.
- A startup path that is unavailable, linked or occupied by a file produces a visible setup error.
- Generated loader startup metadata is excluded from the manager gameplay inventory, allowing fresh and existing setups to match. Changes to actual game resources and plugin settings still change the compatibility fingerprint.

The x86 regression test exercises clean discovery, an empty mod plan, selection persistence, partial-install repair, repeated startup, concurrent first launches, and file/junction conflicts. All 91 checks passed. Game startup remains subject to the user's normal two-copy test.

Run `scratch/TestCoopManagerBootstrap61329.bat` to repeat the startup checks.
