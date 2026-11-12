# Undercoop: NFSU2 6.13.32

## Import underground2.net 1.0.3

Open **Modpacks → Import modpack**, select the original `underground2.net_1.0.3.zip`, review the files and choose **Import modpack**. The result appears as a separate instance in **Modpacks**. Select **Activate**, approve the selected native components and choose **Play**. Both players should use the same Undercoop build, the same archive and the same selected pack contents.

The importer uses the pack's `1 - Mod Files` configuration. It streams the ZIP one entry at a time and retains all ten ASI plugins and their data, including sound tables, Unlimiter profiles, ReShade presets and TexWizard textures. It preserves the current ASI loader and stores the imported resources below `mods/_undercoop/modpacks`. It copies the current careers into the new instance. The original archive, its optional variants, bundled save settings and old ASI loader remain separate.

The imported Extra Options INI disables its F5 Unlock All and F6 AutoDrive hotkeys so they do not conflict with CO-OP votes. Gameplay Unlock All starts off for the imported career. Use a supported 1.2 NTSC game installation with no duplicate loose copies of the pack's ASIs. Get the archive from the [pack author's release page](https://nfsmods.xyz/mod/2004).

## Mod comparison and consent

The lobby compares effective runtime files and gameplay settings. The local manager's recipe hash no longer makes identical effective files differ merely because they were installed in different layouts. Files and settings that actually differ still appear in `scripts/NFSU2Coop-Mods.txt`.

After both inventories have been verified, players may open the lobby console with **Z** and independently press **F5** to accept a mismatch or **F6** to decline. After mutual approval the lobby reports **MISMATCH ACCEPTED**. The approval belongs to that connection and exact inventory pair. Reconnecting or changing inventories requires a new decision. A broken scan, changed running files, an unsupported car catalog or native hooks, and an unavailable gameplay connection still block career start.

## Verification

The tests exercise the original 1.0.3 ZIP in an isolated directory, including archive extraction, instance creation, preparation of the ten native components without executing them and effective inventory comparison. Tests also exercise the launcher actions and the two-player consent protocol. The first import through the installed launcher and actual gameplay verification are player-run checks.
