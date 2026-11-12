# Undercoop: NFSU2 6.13.30

Copyright (C) 2026 batuhan. GNU GPL version 3 only, with the additional
permission in LICENSE-7ZIP-EXCEPTION.txt.

## Launcher loader compatibility

The launcher previously rejected any `dinput8.dll` whose SHA-256 differed from
one preview build, even after its required virtual-file exports were found.
This produced "The ASI loader differs from the tested preview build."

The manager now accepts compatible Ultimate ASI Loader x86 builds by their
active location and API contract. It requires `IsUltimateASILoader`,
`AddVirtualPathForOverloadW` and `RemoveVirtualPathFromOverloadW`, with a positive
identity marker. The loaded module must be the game-root `dinput8.dll`; its
regular file is held open during activation and retained for the active setup.
Missing APIs, an unexpected location or unreadable loader now have specific
installation guidance. No loader is downloaded, replaced or executed by this
check. Other proxy filenames remain outside this integration.

This is a launcher-only patch. It does not change gameplay, career domains,
existing saves, installed mods, selection or the lobby's file comparison.
Teammates must still use the same loader and effective mod files. Automatic
first-run workspace preparation from 6.13.29 remains enabled.

## Validation

- 179 isolated x86 contract checks pass for compatible loaders with differing bytes,
  missing features, rejected identities, wrong/truncated module paths,
  invalid files and retained file locks. No game or third-party plugin is run.
- All 91 first-run bootstrap and installed-file comparison checks pass.
- Both the personal build and separately exported public-font source build
  must compile. The source manifest and private-asset exclusions are verified.
- Final in-game startup testing remains with the user, particularly on the
  previously rejected loader version.
