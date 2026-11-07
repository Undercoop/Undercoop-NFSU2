/* Undercoop decoder-local allocation budget.
 * Copyright (c)2026 Undercoop contributors.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * This guard is compiled only into Undercoop's source-built archive decoder.
 */
#ifndef UNDERCOOP_ARCHIVE_BUDGET_H
#define UNDERCOOP_ARCHIVE_BUDGET_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *__cdecl UndercoopArchiveBudgetAlloc(size_t size);
void *__cdecl UndercoopArchiveBudgetAligned(size_t size, size_t alignment);
void *__cdecl UndercoopArchiveBudgetRealloc(void *address, size_t size);
void __cdecl UndercoopArchiveBudgetFree(void *address);
unsigned long long __cdecl UndercoopArchiveBudgetLiveBytes(void);
unsigned long long __cdecl UndercoopArchiveBudgetPeakBytes(void);
unsigned long long __cdecl UndercoopArchiveBudgetRejected(void);
unsigned long long __cdecl UndercoopArchiveBudgetBlocks(void);
unsigned long long __cdecl UndercoopArchiveBudgetSingleLimit(void);
unsigned long long __cdecl UndercoopArchiveBudgetTotalLimit(void);
#ifdef __cplusplus
}
#endif
#endif
