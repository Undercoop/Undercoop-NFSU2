# miniz inflater provenance

The unchanged upstream files in this directory were copied from the already
present `research/ultimate-asi-loader-01eb152/external/miniz` checkout. Its Git
commit is `77d0dce8627735138c51770d1799a1ef48f2117d`; its changelog identifies
version 3.1.2. Upstream is https://github.com/richgel999/miniz.

Only `miniz_tinfl.c` is compiled, through `CoopZipImport.h`, with archive APIs,
compression APIs, stdio, time and allocating helper APIs disabled. ZIP parsing,
Windows-safe ownership paths, budgets and CRC checks belong to Undercoop.

`miniz_export.h` is the local header-only integration shim. The original MIT
license and copyright notices are retained in `LICENSE` and source headers.
No downloaded binaries, extraction commands or native installers participate.
