/*
 * Unit test suite for vcomp work-sharing implementation
 *
 * Copyright 2012 Dan Kegel
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "wine/test.h"

static void WINAPIV (*p_vcomp_barrier)(void);
static void WINAPIV (*p_vcomp_fork)(DWORD parallel, int nargs, void *helper, ...);
static void CDECL (*p_vcomp_for_dynamic_init)(int flags, int first, int last, int mystep, int chunksize);
static int CDECL (*p_vcomp_for_dynamic_next)(int *pcounter, int *pchunklimit);
static void CDECL (*p_vcomp_for_static_end)(void);
static void CDECL (*p_vcomp_for_static_init)(int first, int last, int mystep, int chunksize, int *pnloops, int *pfirst, int *plast, int *pchunksize, int *pfinalchunkstart);
static void CDECL (*p_vcomp_for_static_simple_init)(int first, int last, int mystep, int step, int *pfirst, int *plast);
static void CDECL (*p_vcomp_sections_init)(int n);
static int CDECL (*p_vcomp_sections_next)(void);

#define GETFUNC(x) do { p##x = (void*)GetProcAddress(vcomp, #x); ok(p##x != NULL, "Export '%s' not found\n", #x); } while(0)

/* Matches definitions in ../vcomp_private.h */
#define VCOMP_DYNAMIC_FOR_FLAGS_DOWN 0x0
#define VCOMP_DYNAMIC_FOR_FLAGS_UP 0x40

static BOOL init(void)
{
    HMODULE vcomp = LoadLibraryA("vcomp.dll");
    if (!vcomp)
    {
        win_skip("vcomp.dll not installed\n");
        return FALSE;
    }

    GETFUNC(_vcomp_barrier);
    GETFUNC(_vcomp_fork);
    GETFUNC(_vcomp_for_dynamic_init);
    GETFUNC(_vcomp_for_dynamic_next);
    GETFUNC(_vcomp_for_static_end);
    GETFUNC(_vcomp_for_static_init);
    GETFUNC(_vcomp_for_static_simple_init);
    GETFUNC(_vcomp_sections_init);
    GETFUNC(_vcomp_sections_next);

    return TRUE;
}

static LONG volatile ncalls;
static LONG volatile nsum;

static void CDECL _test_vcomp_for_dynamic_worker_up(void)
{
    int i, limit;

    InterlockedIncrement(&ncalls);

    /* pragma omp schedule(dynamic,16) */
    /* for (i=0; i<=17; i++) */
    p_vcomp_for_dynamic_init(VCOMP_DYNAMIC_FOR_FLAGS_UP, 0, 17, 1, 16);
    while (p_vcomp_for_dynamic_next(&i, &limit))
    {
        for (; i<=limit; i++)
        {
            int j;
            for (j=0; j<i; j++)
                InterlockedIncrement(&nsum);
        }
    }
}

static void CDECL _test_vcomp_for_dynamic_worker_down(void)
{
    int i, limit;

    InterlockedIncrement(&ncalls);

    /* pragma omp schedule(dynamic,16) */
    /* for (i=17; i>=0; i--) */
    p_vcomp_for_dynamic_init(VCOMP_DYNAMIC_FOR_FLAGS_DOWN, 17, 0, 1, 16);
    while (p_vcomp_for_dynamic_next(&i, &limit))
    {
        for (; i>=limit; i--)
        {
            int j;
            for (j=0; j<i; j++)
                InterlockedIncrement(&nsum);
        }
    }
}

static void test_vcomp_for_dynamic(void)
{
    /* for (i=0; i<=17; i++) nsum += i; */
    ncalls = 0;
    nsum = 0;
    p_vcomp_fork(1, 0, _test_vcomp_for_dynamic_worker_up);
    ok(ncalls >= 1, "expected >= 1 call, got %d\n", ncalls);
    ok(nsum == 9*17, "expected sum 9*17, got %d\n", nsum);

    /* for (i=17; i>=0; i--) nsum += i; */
    ncalls = 0;
    nsum = 0;
    p_vcomp_fork(1, 0, _test_vcomp_for_dynamic_worker_down);
    ok(ncalls >= 1, "expected >= 1 call, got %d\n", ncalls);
    ok(nsum == 9*17, "expected sum 9*17, got %d\n", nsum);
}

static void CDECL _test_vcomp_for_static_init_worker(void)
{
    const int my_start = 0;
    const int my_end = 12;
    const int my_incr = 1;
    const int my_chunksize = 1;
    int nloops, chunkstart, chunkend, chunksize, finalchunkstart;

    InterlockedIncrement(&ncalls);

    /* for (i=0; i<=12; i++) */
    p_vcomp_for_static_init(my_start, my_end, my_incr, my_chunksize,
        &nloops, &chunkstart, &chunkend, &chunksize, &finalchunkstart);

    do
    {
        int i;
        if (chunkstart == finalchunkstart) chunkend = my_end;

        for (i=chunkstart; i <= chunkend; i += my_incr)
        {
            int j;
            for (j=0; j<i; j++)
                InterlockedIncrement(&nsum);
        }
        chunkstart += chunksize;
        chunkend += chunksize;
    }
    while (--nloops > 0);

    p_vcomp_for_static_end();
}

static void test_vcomp_for_static_init(void)
{
    ncalls = 0;
    nsum = 0;
    p_vcomp_fork(1, 0, _test_vcomp_for_static_init_worker);
    ok(ncalls >= 1, "expected >= 1 call, got %d\n", ncalls);
    ok(nsum == 6*13, "expected sum 6*13, got %d\n", nsum);
}

static void CDECL _test_vcomp_for_static_simple_init_worker(void)
{
    int i, my_limit;

    InterlockedIncrement(&ncalls);

    /* for (i=0; i<=12; i++) */
    p_vcomp_for_static_simple_init(0, 12, 1, 1, &i, &my_limit);

    while (i <= my_limit)
    {
        int j;
        for (j=0; j<i; j++)
            InterlockedIncrement(&nsum);
        i++;
    }

    p_vcomp_for_static_end();
}

static void test_vcomp_for_static_simple_init(void)
{
    ncalls = 0;
    nsum = 0;
    p_vcomp_fork(1, 0, _test_vcomp_for_static_simple_init_worker);
    ok(ncalls >= 1, "expected >= 1 call, got %d\n", ncalls);
    ok(nsum == 6*13, "expected sum 6*13, got %d\n", nsum);
}

int section_calls[3];

static void CDECL _test_vcomp_sections_worker(void)
{
    p_vcomp_sections_init(3);

    for (;;)
    {
        int i = p_vcomp_sections_next();
        if (i < 0 || i >= 3) break;
        section_calls[i]++;
    }

    p_vcomp_barrier();
}

static void test_vcomp_sections(void)
{
    section_calls[0] = 0;
    section_calls[1] = 0;
    section_calls[2] = 0;
    p_vcomp_fork(1, 0, _test_vcomp_sections_worker);
    ok(section_calls[0] == 1, "section 0 not called once\n");
    ok(section_calls[1] == 1, "section 1 not called once\n");
    ok(section_calls[2] == 1, "section 2 not called once\n");
}

START_TEST(work)
{
    if (!init())
        return;

    test_vcomp_for_dynamic();
    test_vcomp_for_static_init();
    test_vcomp_for_static_simple_init();
    test_vcomp_sections();
}
