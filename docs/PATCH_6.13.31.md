# Undercoop: NFSU2 6.13.31

Copyright (C) 2026 batuhan. GNU GPL version 3 only, with the additional
permission in LICENSE-7ZIP-EXCEPTION.txt.

## Animation Controller compatibility

The reported crash was an access violation in AnimationController.asi at
module RVA 0x3714, called from its worker at RVA 0x3A27. The captured session
was still in the front end without an established CO-OP peer. The released
Animation Controller 1.2.1 file matches the supplied plugin exactly:

```
SHA-256 2ccbcaa1ced037c2f0ce2789b2e88b3bb758d9b29632e61e36e57096080a5650
Size    252416 bytes
```

The faulting command derives the list head by adding eight bytes to a missing
animation-scene pointer. Undercoop now adapts both the standalone query and
the command's inlined query. Bounded memory reads validate the current flow,
scene root, circular links and animation state. World queries also validate
the player's car chain. Missing, changing or malformed scenes return the
mod's busy state, which defers the command before native animation dispatch.
Unfocused instances likewise defer queries.

The adapter verifies the file hash, PE identity, loaded target ownership and
original instruction bytes before installing. Existing threads are briefly
paused for the two-site publication; installation is retried if a thread is
inside a replaced query. The module and file remain resident/pinned for the
adapter's lifetime. Unknown builds or altered target instructions are not
patched. The plugin is neither redistributed nor rewritten on disk.

The mod's hotkeys, INI, animation actions and visual options are retained.
This patch does not alter careers, balances, selected mods or network rules.
The independently authored adapter contains no upstream implementation source.

## Validation and manual check

- 115 isolated scene checks cover absent scenes, front-end/world contexts, empty
  and populated rings, state changes, invalid pointers, cycles and bounds.
- 33 native adapter checks exercise both x86 guard paths using test-owned code
  stubs, verify stack/calling conventions and verify only intended bytes change.
- 118 plugin identity checks reject altered artifacts and mismatched ownership,
  including verification and tamper rejection of the supplied plugin file.
- All 270 existing bootstrap and loader-compatibility checks pass.
- The personal build and exported public-font source are compiled separately.
- No game or third-party plugin is launched by the automated checks.

Final in-game validation remains with the players: use Animation Controller
1.2.1, exercise hood/trunk/door keys in the CO-OP browser and in a loaded car
scene, then test free-roam/menu transitions. The log should contain
`[ANIMATION_COMPAT] Verified Animation Controller 1.2.1` when the adapter installs.
If another build is used, the log explicitly reports that it was left unchanged.
