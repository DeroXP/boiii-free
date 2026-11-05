/*
 * Xerces: MinHook's thread walk for Freeze(), included into the copy of
 * MinHook's hook.c that deps/premake/minhook.lua generates (MinHook is
 * BSD 2-clause, Copyright (C) 2009-2017 Tsuda Kageyu; this file is under the
 * same licence).
 *
 * Before it writes a hook, MinHook suspends every other thread of this
 * process. Upstream finds them with a Toolhelp snapshot of every thread on the
 * PC: on a PC with about 8,100 threads that took 40-77 ms per hook, 9.2 s for
 * the game's 212 hooks (DESIGN.md 12.4, measured 2026-11-05). NtGetNextThread
 * walks this process's own threads only. If it is missing or stops with any
 * status other than "no more entries", the original snapshot walk runs
 * instead, so a thread is never missed.
 */

#ifndef THREAD_QUERY_LIMITED_INFORMATION
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#endif

typedef LONG(NTAPI *XERCES_NT_GET_NEXT_THREAD)(HANDLE process, HANDLE thread,
                                               ACCESS_MASK access,
                                               ULONG attributes, ULONG flags,
                                               PHANDLE next);

#define XERCES_STATUS_NO_MORE_ENTRIES ((LONG)0x8000001AL)

static BOOL XercesAddThreadId(PFROZEN_THREADS pThreads, DWORD id)
{
    if (pThreads->pItems == NULL)
    {
        pThreads->capacity = INITIAL_THREAD_CAPACITY;
        pThreads->pItems
            = (LPDWORD)HeapAlloc(g_hHeap, 0, pThreads->capacity * sizeof(DWORD));
        if (pThreads->pItems == NULL)
            return FALSE;
    }
    else if (pThreads->size >= pThreads->capacity)
    {
        LPDWORD p;
        pThreads->capacity *= 2;
        p = (LPDWORD)HeapReAlloc(
            g_hHeap, 0, pThreads->pItems, pThreads->capacity * sizeof(DWORD));
        if (p == NULL)
            return FALSE;
        pThreads->pItems = p;
    }
    pThreads->pItems[pThreads->size++] = id;
    return TRUE;
}

static void XercesDropThreads(PFROZEN_THREADS pThreads)
{
    if (pThreads->pItems != NULL)
        HeapFree(g_hHeap, 0, pThreads->pItems);
    pThreads->pItems   = NULL;
    pThreads->capacity = 0;
    pThreads->size     = 0;
}

// Called with MinHook's lock held (Freeze), so the lookup needs no guard.
static BOOL EnumerateThreads(PFROZEN_THREADS pThreads)
{
    static XERCES_NT_GET_NEXT_THREAD next_thread = NULL;
    static BOOL looked_up = FALSE;
    if (!looked_up)
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        next_thread = ntdll != NULL
            ? (XERCES_NT_GET_NEXT_THREAD)GetProcAddress(ntdll, "NtGetNextThread")
            : NULL;
        looked_up = TRUE;
    }

    if (next_thread != NULL)
    {
        const DWORD self = GetCurrentThreadId();
        HANDLE current = NULL;
        BOOL ok = TRUE;
        LONG status;
        for (;;)
        {
            HANDLE found = NULL;
            status = next_thread(GetCurrentProcess(), current,
                                 THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &found);
            if (status < 0)
                break;
            if (current != NULL)
                CloseHandle(current);
            current = found;
            {
                const DWORD id = GetThreadId(current);
                if (id == 0)
                {
                    ok = FALSE; // a thread whose id cannot be read
                    break;
                }
                if (id != self && !XercesAddThreadId(pThreads, id))
                {
                    ok = FALSE;
                    break;
                }
            }
        }
        if (current != NULL)
            CloseHandle(current);
        if (ok && status == XERCES_STATUS_NO_MORE_ENTRIES)
            return TRUE;
        // Anything else (access denied, out of memory): walk it the old way.
        XercesDropThreads(pThreads);
    }

    return EnumerateThreadsSnapshot(pThreads);
}
