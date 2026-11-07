# Undercoop 6.13.27 — teammate parts and connection HUD

Copyright (C) 2026 batuhan. Original Undercoop documentation is licensed under
GNU GPL version 3 only, with LICENSE-7ZIP-EXCEPTION.txt.

## Teammate customization

The part identities could be resolved correctly while upgraded rims, spoilers
and other meshes remained hidden. The native RideInfo normalization step
rebuilt visibility after the human replica's finalized appearance had already
been copied. The human paths must preserve that finalized presentation after
normalization, consistent with the existing fleet construction path.

The correction covers initial teammate loading and live customization refresh.
It does not transfer native process pointers, change installed parts in a
career, or replace the player's own car configuration.

## Connection indicator

The game displays a small live connection indicator independently of console
visibility. It uses the actual connected gameplay transport rather than lobby
membership alone. Initial ENet default RTT values are not treated as measured
ping. Lost connectivity has explicit interrupted/disconnected presentation.

The text sits at the top right of the game client area, above the usual Bank
HUD. It has no background panel and does not take mouse or keyboard focus.
Measured ping is green at 100 ms or less, orange from101 through200 ms, and red
above200 ms. A sample older than2.5 seconds is interrupted rather than healthy.
If the session itself closes, the disconnect notice remains for15 seconds.
Snapshots are sampled at most four times per second and text is rerasterized
only when its contents, color or DPI changes.

Transport statistics are read as value snapshots; native car work and socket
ownership are unchanged. The indicator does not change packet delivery, game
time, race decisions or save handling.

## In-game acceptance test

Use the same build on both computers.

1. Load the imported career and inspect both remote cars. Compare installed
   rims, spoilers and body parts with each driver's own screen.
2. Change customization in a shop, return to Free Roam and inspect both cars
   again. Repeat a race entry/return to verify fresh loading as well as refresh.
3. Keep the console closed. Confirm ping/status remain readable and update.
4. Disconnect one player. The other screen must show the interruption and
   departure instead of keeping the previous healthy ping indefinitely.
5. Reconnect and verify that a new measurement is required before showing a
   healthy value for the new connection.

Automated regression results supplement this checklist. Final two-computer
gameplay validation remains necessary during Open Beta.

The public tests in tests/Run-Tests.cmd exercise native-normalizer clobbering,
preserved local caches/part pointers, threshold boundaries, stale/default RTT,
disconnect/reconnect, clock wrap, actual loopback ENet ACK measurements and
transparent colored rendering at96/144/192 DPI. Development regressions also
exercise the production human receiver and the native x86 ride-setup hook.
