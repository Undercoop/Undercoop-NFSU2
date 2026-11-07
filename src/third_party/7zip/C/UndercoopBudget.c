/* Undercoop decoder-local allocation budget.
 * Copyright (c)2026 Undercoop contributors.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * No CRT/process allocator is patched. All prefixed blocks are owned and freed
 * exclusively by this DLL's C helpers or its local C++new/delete operators.
 */
#include "Precomp.h"
#include "7zWindows.h"
#include "UndercoopBudget.h"
#include <stdint.h>
#include <string.h>

#if !defined(_WIN32) || !defined(UNDERCOOP_ARCHIVE_BUDGET)
#error UndercoopBudget is for the explicitly budgeted Windows decoder only.
#endif

#define UCB_SINGLE ((size_t)128 * 1024 * 1024)
#define UCB_TOTAL ((size_t)256 * 1024 * 1024)
#define UCB_COOKIE 0x55434152u
typedef struct UcbHeader {
  void *allocation;
  size_t bytes;
  size_t charge;
  uintptr_t cookie;
} UcbHeader;
static volatile LONG g_ucb_live = 0;
static volatile LONG g_ucb_peak = 0;
static volatile LONG g_ucb_rejected = 0;
static volatile LONG g_ucb_blocks = 0;

static void UcbReject(void) { InterlockedIncrement(&g_ucb_rejected); }
static int UcbReserve(size_t charge)
{
  LONG previous = InterlockedCompareExchange(&g_ucb_live, 0, 0);
  for (;;)
  {
    LONG observed;
    LONG next;
    if (charge > UCB_TOTAL - (size_t)previous)
    {
      UcbReject();
      return 0;
    }
    next = previous + (LONG)charge;
    observed = InterlockedCompareExchange(&g_ucb_live, next, previous);
    if (observed == previous)
    {
      LONG peak = InterlockedCompareExchange(&g_ucb_peak, 0, 0);
      while (peak < next)
      {
        observed = InterlockedCompareExchange(&g_ucb_peak, next, peak);
        if (observed == peak) break;
        peak = observed;
      }
      return 1;
    }
    previous = observed;
  }
}

void *__cdecl UndercoopArchiveBudgetAligned(size_t size, size_t alignment)
{
  void *allocation;
  uintptr_t payload;
  UcbHeader *header;
  size_t charge;
  if (!size) return NULL;
  if (alignment < 16) alignment = 16;
  if (size > UCB_SINGLE || alignment > 65536 || (alignment & (alignment - 1)) ||
      size > SIZE_MAX - sizeof(UcbHeader) - (alignment - 1))
  {
    UcbReject();
    return NULL;
  }
  charge = size + sizeof(UcbHeader) + alignment - 1;
  if (!UcbReserve(charge)) return NULL;
  allocation = HeapAlloc(GetProcessHeap(), 0, charge);
  if (!allocation)
  {
    InterlockedExchangeAdd(&g_ucb_live, -(LONG)charge);
    UcbReject();
    return NULL;
  }
  payload = ((uintptr_t)allocation + sizeof(UcbHeader) + alignment - 1) &
      ~((uintptr_t)alignment - 1);
  header = (UcbHeader *)(payload - sizeof(UcbHeader));
  header->allocation = allocation;
  header->bytes = size;
  header->charge = charge;
  header->cookie = UCB_COOKIE ^ payload;
  InterlockedIncrement(&g_ucb_blocks);
  return (void *)payload;
}
void *__cdecl UndercoopArchiveBudgetAlloc(size_t size)
{
  return UndercoopArchiveBudgetAligned(size, 16);
}
void __cdecl UndercoopArchiveBudgetFree(void *address)
{
  UcbHeader *header;
  void *allocation;
  size_t charge;
  if (!address) return;
  header = (UcbHeader *)((char *)address - sizeof(UcbHeader));
  /* Never guess a CRT fallback for a foreign pointer. */
  if (header->cookie != (UCB_COOKIE ^ (uintptr_t)address)) return;
  allocation = header->allocation;
  charge = header->charge;
  header->cookie = 0;
  if (HeapFree(GetProcessHeap(), 0, allocation))
  {
    InterlockedExchangeAdd(&g_ucb_live, -(LONG)charge);
    InterlockedDecrement(&g_ucb_blocks);
  }
}
void *__cdecl UndercoopArchiveBudgetRealloc(void *address, size_t size)
{
  UcbHeader *header;
  void *result;
  size_t copy;
  if (!address) return UndercoopArchiveBudgetAlloc(size);
  if (!size) { UndercoopArchiveBudgetFree(address); return NULL; }
  header = (UcbHeader *)((char *)address - sizeof(UcbHeader));
  if (header->cookie != (UCB_COOKIE ^ (uintptr_t)address)) return NULL;
  result = UndercoopArchiveBudgetAlloc(size);
  if (!result) return NULL; /* Original allocation stays valid on failure. */
  copy = header->bytes < size ? header->bytes : size;
  memcpy(result, address, copy);
  UndercoopArchiveBudgetFree(address);
  return result;
}
unsigned long long __cdecl UndercoopArchiveBudgetLiveBytes(void)
{ return (unsigned long long)InterlockedCompareExchange(&g_ucb_live, 0, 0); }
unsigned long long __cdecl UndercoopArchiveBudgetPeakBytes(void)
{ return (unsigned long long)InterlockedCompareExchange(&g_ucb_peak, 0, 0); }
unsigned long long __cdecl UndercoopArchiveBudgetRejected(void)
{ return (unsigned long long)(unsigned long)InterlockedCompareExchange(&g_ucb_rejected, 0, 0); }
unsigned long long __cdecl UndercoopArchiveBudgetBlocks(void)
{ return (unsigned long long)InterlockedCompareExchange(&g_ucb_blocks, 0, 0); }
unsigned long long __cdecl UndercoopArchiveBudgetSingleLimit(void) { return UCB_SINGLE; }
unsigned long long __cdecl UndercoopArchiveBudgetTotalLimit(void) { return UCB_TOTAL; }
