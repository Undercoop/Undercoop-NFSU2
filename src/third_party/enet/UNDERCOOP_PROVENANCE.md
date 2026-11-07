ENet 1.3.18, unmodified upstream C sources and public headers.

Source: https://github.com/lsalzman/enet/tree/v1.3.18
Archive: https://codeload.github.com/lsalzman/enet/tar.gz/refs/tags/v1.3.18
Archive SHA-256: 28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd
License: MIT; see LICENSE.

Win32 static build sources: callbacks.c, compress.c, host.c, list.c,
packet.c, peer.c, protocol.c, win32.c. Add include/ to the include search
path and link ws2_32.lib and winmm.lib. No transport DLL is required.

CoopTransport uses ENet only for connected gameplay. Original UDP
membership/discovery retains the canonical endpoint and port. ENet binds
canonical port + 1000, has a 1200-byte MTU, and separates reliable control,
unreliable state and reliable bulk channels. Session/nonce/endpoint binding
is not cryptographic identity or encryption.
