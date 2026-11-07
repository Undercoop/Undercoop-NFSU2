// Undercoop decoder-local allocation budget.
// Copyright (c)2026 Undercoop contributors.
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "StdAfx.h"
#include <new>
#include "../../C/UndercoopBudget.h"

#ifndef UNDERCOOP_ARCHIVE_BUDGET
#error This allocator must never be linked into the game/ASI or an unguarded DLL.
#endif

void *__cdecl operator new(size_t size)
{
  void *p = UndercoopArchiveBudgetAlloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void *__cdecl operator new[](size_t size) { return ::operator new(size); }
void __cdecl operator delete(void *p) noexcept { UndercoopArchiveBudgetFree(p); }
void __cdecl operator delete[](void *p) noexcept { UndercoopArchiveBudgetFree(p); }
void __cdecl operator delete(void *p, size_t) noexcept { UndercoopArchiveBudgetFree(p); }
void __cdecl operator delete[](void *p, size_t) noexcept { UndercoopArchiveBudgetFree(p); }
void *__cdecl operator new(size_t size, const std::nothrow_t&) noexcept
{ return UndercoopArchiveBudgetAlloc(size ? size : 1); }
void *__cdecl operator new[](size_t size, const std::nothrow_t&) noexcept
{ return UndercoopArchiveBudgetAlloc(size ? size : 1); }
void __cdecl operator delete(void *p, const std::nothrow_t&) noexcept { UndercoopArchiveBudgetFree(p); }
void __cdecl operator delete[](void *p, const std::nothrow_t&) noexcept { UndercoopArchiveBudgetFree(p); }

#if defined(__cpp_aligned_new)
void *__cdecl operator new(size_t size, std::align_val_t alignment)
{
  void *p = UndercoopArchiveBudgetAligned(size ? size : 1, static_cast<size_t>(alignment));
  if (!p) throw std::bad_alloc();
  return p;
}
void *__cdecl operator new[](size_t size, std::align_val_t alignment)
{ return ::operator new(size, alignment); }
void __cdecl operator delete(void *p, std::align_val_t) noexcept { UndercoopArchiveBudgetFree(p); }
void __cdecl operator delete[](void *p, std::align_val_t) noexcept { UndercoopArchiveBudgetFree(p); }
void __cdecl operator delete(void *p, size_t, std::align_val_t) noexcept { UndercoopArchiveBudgetFree(p); }
void __cdecl operator delete[](void *p, size_t, std::align_val_t) noexcept { UndercoopArchiveBudgetFree(p); }
void *__cdecl operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{ return UndercoopArchiveBudgetAligned(size ? size : 1, static_cast<size_t>(alignment)); }
void *__cdecl operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{ return UndercoopArchiveBudgetAligned(size ? size : 1, static_cast<size_t>(alignment)); }
void __cdecl operator delete(void *p, std::align_val_t, const std::nothrow_t&) noexcept { UndercoopArchiveBudgetFree(p); }
void __cdecl operator delete[](void *p, std::align_val_t, const std::nothrow_t&) noexcept { UndercoopArchiveBudgetFree(p); }
#endif

// Private diagnostic exports exercise the exact DLL-local C++operators. They
// are not archive callbacks and no caller-owned allocation crosses allocators.
extern "C" void *__cdecl UndercoopArchiveBudgetCppAlloc(size_t size, int kind)
{
  try
  {
    if (kind == 1) return ::operator new[](size);
    if (kind == 2) return ::operator new(size, std::nothrow);
    if (kind == 3) return ::operator new[](size, std::nothrow);
    return ::operator new(size);
  }
  catch (...) { return NULL; }
}
extern "C" void __cdecl UndercoopArchiveBudgetCppFree(void *p, int kind)
{
  if (kind == 1 || kind == 3) ::operator delete[](p);
  else ::operator delete(p);
}
