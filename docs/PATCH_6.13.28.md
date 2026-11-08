# Undercoop 6.13.28

Copyright (C) 2026 batuhan. GNU GPL version 3 only, with the additional
permission in LICENSE-7ZIP-EXCEPTION.txt.

## Geometry streaming

The 6.13.27 installed/visibility-byte correction is retained. This patch also
addresses native geometry residency and visual construction, rather than
forcing all parts visible or moving a car vertically.

Static inspection of the supported executable establishes these separate facts:

- `61CA10` checks native resource completion flags, not actual drawable meshes.
- Native submission through `494820` only finds meshes in open file indexes.
  An absent index can silently skip a request and still complete its callback.
- `634800` discards an unresolved LOD-A model. A later geometry load does not
  necessarily repair the visual that was already constructed.
- Missing higher LODs are normal: the native renderer substitutes earlier LODs.
  They must not become compulsory assets or a permanent loading barrier.

The game-thread adapter holds bounded, explicitly owned references to native
`CARS/.../GEOMETRY*.BIN` indexes during the co-op world. Native car, shared-part
and wheel streaming use the same indexes. The normal close function still runs;
non-null close callbacks first retire our own reference to preserve the native
completion barrier. References are released in the frontend or on leaving co-op.
Unknown patched native entries are preserved instead of overwritten.

A staged teammate appearance is committed only after indexed meshes are actually
drawable. An incomplete staged request can be resubmitted through its own native
descriptor and original callbacks while the queue is idle; the old visual is
retained. Optional absent LODs keep the engine's fallback behavior.

A bounded check can rebind an incomplete full-car visual after a missing LOD-A
mesh becomes resident. It validates the native car/Ride/visual ownership and
does not change installed parts, physics, position, wheel pose or career data.
Both pieces of a part are checked. Automatic reconstruction is limited to an
initial incomplete-visual window; once a visual is healthy it is left alone so
later crash damage is not treated as a missing-load repair.
Traffic's different simplified body representation is excluded from visual
reconstruction; its geometry loads still benefit from the shared index fix.

The saved live capture predates the extra eModel/solid fields. It confirms the
resource/header state and consistent origin/COM arithmetic, but cannot prove
which exact missing screenshot mesh took which native failure path. Gameplay
verification of the reported missing parts and low-looking/shadow symptom
remains necessary; an offline build/test is not that verification.

## Teammate activity

The existing connection HUD includes a second line for the teammate's activity.
Small independent snapshots work without a spawned car, so menus and loading
do not depend on position packets. Current session, membership, generation,
sender and sequence are checked; activity becomes unavailable when stale.
The observer uses active UI packages and verified native context. Ambiguous
contexts receive a generic description rather than an invented location.
Shop classification uses the entered-shop field at career `+8734`, with the
native switch's garage, paint, body, performance, car-lot, specialties and
unique-reward values. A nearby shop trigger alone does not establish location.

## Manual mod folders

The launcher can inspect and register game-ready folders and generates its own
package metadata. Registered manual packages use the same enable/disable,
native-code approval, settings, dependency and modpack systems as other mods.
An unregistered folder does not prevent valid packages from being discovered.

Folder import reviews file mappings and native-code requirements, checks for
source changes before commit, and keeps installation within the managed mod
tree. Folder import does not execute external installers or reinterpret an
unknown tool-specific installation script as a ready-to-use resource mod.

## Verification and remaining gameplay checks

Source tests cover owned header lifetimes, native-hook boundaries, optional LOD
handling, activity freshness/session changes, folder inspection/commit and the
existing connection HUD. A complete private build and a fresh public-source
build are required for this release.

In game, test both players with upgraded rims/spoiler/exhaust, then change cars
and revisit shops. Check ambient/race AI, near/far views, stance and shadows,
race/Free Roam transitions, leaving/rejoining and the activity indicator.
Test importing a folder yourself, toggling it and switching modpacks. Existing
careers and connection settings must remain intact throughout deployment.
