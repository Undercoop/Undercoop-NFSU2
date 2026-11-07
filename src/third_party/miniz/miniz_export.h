#pragma once
// Undercoop embeds the read-only tinfl implementation in its one-TU ASI build.
// Inline declarations retain header-only linkage without exporting DLL symbols.
#define MINIZ_EXPORT inline
