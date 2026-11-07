// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopCrashDialogWin.h"

namespace coop_crash {
using PrepareReport = void(*)(Report&,const EXCEPTION_POINTERS&);
using ShowReport = void(*)(const Report&) noexcept;

namespace reporter {
inline HANDLE signal=nullptr,presented=nullptr,worker=nullptr;
inline DWORD workerId=0;
inline volatile LONG installed=0,reported=0;
inline EXCEPTION_RECORD record{};
inline CONTEXT context{};
inline bool hasContext=false;
inline PrepareReport prepare=nullptr;
inline ShowReport show=nullptr;

inline DWORD WINAPI Run(void*) {
    const bool customWindow=show==static_cast<ShowReport>(&Show);
    if(customWindow)Prepare();
    if(WaitForSingleObject(signal,INFINITE)!=WAIT_OBJECT_0)return 1;
    Report report{};
    report.code=record.ExceptionCode;
    report.address=reinterpret_cast<uintptr_t>(record.ExceptionAddress);
    EXCEPTION_POINTERS fault{&record,hasContext?&context:nullptr};
    __try {
        if(prepare)prepare(report,fault);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // The raw exception is already available even if optional diagnostics fail.
    }
    __try {
        if(customWindow)Show(report,presented);
        else {
            SetEvent(presented);
            show(report);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

inline LONG WINAPI Filter(EXCEPTION_POINTERS* fault) {
    if(!worker || GetCurrentThreadId()==workerId || !fault || !fault->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedCompareExchange(&reported,1,0)==0) {
        record=*fault->ExceptionRecord;
        record.ExceptionRecord=nullptr;
        hasContext=fault->ContextRecord!=nullptr;
        if(hasContext)context=*fault->ContextRecord;
        if(!SetEvent(signal))return EXCEPTION_CONTINUE_SEARCH;
    }
    // Keep the faulting thread and its context alive until the crash screen
    // closes. Give Windows back control if a loader/resource lock prevents
    // the worker from reaching a visible window.
    const HANDLE waitFor[]{presented,worker};
    const DWORD first=WaitForMultipleObjects(2,waitFor,FALSE,5000);
    if(first!=WAIT_OBJECT_0 && first!=WAIT_OBJECT_0+1)return EXCEPTION_CONTINUE_SEARCH;
    DWORD status=1;
    if(WaitForSingleObject(worker,INFINITE)==WAIT_OBJECT_0 && GetExitCodeThread(worker,&status) && status==0)
        return EXCEPTION_EXECUTE_HANDLER;
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace reporter

inline void ReserveExceptionStack() noexcept {
    ULONG bytes=32*1024;
    SetThreadStackGuarantee(&bytes);
}

inline bool Install(PrepareReport prepare,ShowReport show=&Show) noexcept {
    using namespace reporter;
    if(!show)return false;
    if(InterlockedCompareExchange(&installed,1,0)!=0) {
        if(!worker)return false;
        SetUnhandledExceptionFilter(&Filter);
        return true;
    }
    ReserveExceptionStack();
    reporter::prepare=prepare;
    reporter::show=show;
    signal=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    presented=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(signal && presented)worker=CreateThread(nullptr,0,&Run,nullptr,0,&workerId);
    if(!worker) {
        if(signal)CloseHandle(signal);
        if(presented)CloseHandle(presented);
        signal=presented=nullptr;
        InterlockedExchange(&installed,0);
        return false;
    }
    SetUnhandledExceptionFilter(&Filter);
    return true;
}
} // namespace coop_crash
