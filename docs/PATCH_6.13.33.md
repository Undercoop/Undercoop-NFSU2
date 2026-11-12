# Undercoop: NFSU2 6.13.33

In **Modpacks**, select **Deactivate** on the active instance to return to the mod selection you previously used outside modpacks. The launcher restores that setup and its career location. Activate the instance again to return to its own selection and careers.

With underground2.net 1.0.3, both players initially reported the same inventory hash. Player 2 then received a Windows sharing violation while reading `ReShade.ini`, and the watcher revoked compatibility. Both installed INIs still matched the original archive byte for byte.

The inventory scanner now briefly retries INI reads blocked by a file sharing or lock violation. Once the writer releases the file, the scanner compares its contents with the original hash. A persistent lock, a missing file or a real change still blocks verification.

The regression tests simulate the Windows writer lock, an unchanged write, a genuine edit and a missing INI. Launcher tests exercise switching out of an active instance and restoring the previous custom selection.
