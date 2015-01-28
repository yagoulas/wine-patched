/*
 * MonitorConfig unit tests for Quartz
 *
 * Copyright (C) 2013 Sebastian Lackner
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

#define COBJMACROS

#include "wine/test.h"
#include "dshow.h"

static void test_monitorconfig_setmonitor(void)
{
    HRESULT hr;
    IUnknown *pVMR = NULL;
    IVMRMonitorConfig *pMonitorConfig = NULL;
    VMRGUID guid;

    hr = CoCreateInstance(&CLSID_VideoMixingRenderer, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUnknown, (LPVOID*)&pVMR);
    ok(hr == S_OK, "CoCreateInstance failed with %x.\n", hr);
    ok(pVMR != NULL, "pVMR is NULL.\n");
    if (!pVMR) goto out;

    hr = IUnknown_QueryInterface(pVMR, &IID_IVMRMonitorConfig, (LPVOID*)&pMonitorConfig);
    ok(hr == S_OK, "IUnknown_QueryInterface returned %x.\n", hr);
    ok(pMonitorConfig != NULL, "pMonitorConfig is NULL.\n");
    if (!pMonitorConfig) goto out;

    memset(&guid, 0, sizeof(guid));
    guid.pGUID = NULL; /* default DirectDraw device */
    hr = IVMRMonitorConfig_SetMonitor(pMonitorConfig, &guid);
    ok(hr == S_OK, "SetMonitor failed with %x.\n", hr);

    memset(&guid, 255, sizeof(guid));
    hr = IVMRMonitorConfig_GetMonitor(pMonitorConfig, &guid);
    ok(hr == S_OK, "GetMonitor failed with %x.\n", hr);
    ok(guid.pGUID == NULL, "GetMonitor returned guid.pGUID = %p, expected NULL.\n", guid.pGUID);

    memset(&guid, 0, sizeof(guid));
    guid.pGUID = NULL; /* default DirectDraw device */
    hr = IVMRMonitorConfig_SetDefaultMonitor(pMonitorConfig, &guid);
    ok(hr == S_OK, "SetDefaultMonitor failed with %x.\n", hr);

    memset(&guid, 255, sizeof(guid));
    hr = IVMRMonitorConfig_GetDefaultMonitor(pMonitorConfig, &guid);
    ok(hr == S_OK, "GetDefaultMonitor failed with %x.\n", hr);
    ok(guid.pGUID == NULL, "GetDefaultMonitor returned guid.pGUID = %p, expected NULL.\n", guid.pGUID);

out:
    if (pMonitorConfig) IVMRMonitorConfig_Release(pMonitorConfig);
    if (pVMR) IUnknown_Release(pVMR);
}

static void test_monitorconfig_getavailablemonitors(void)
{
    HRESULT hr;
    IUnknown *pVMR = NULL;
    IVMRMonitorConfig *pMonitorConfig = NULL;
    VMRMONITORINFO info[8];
    DWORD numdev_total, numdev;

    hr = CoCreateInstance(&CLSID_VideoMixingRenderer, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUnknown, (LPVOID*)&pVMR);
    ok(hr == S_OK, "CoCreateInstance failed with %x.\n", hr);
    ok(pVMR != NULL, "pVMR is NULL.\n");
    if (!pVMR) goto out;

    hr = IUnknown_QueryInterface(pVMR, &IID_IVMRMonitorConfig, (LPVOID*)&pMonitorConfig);
    ok(hr == S_OK, "IUnknown_QueryInterface returned %x.\n", hr);
    ok(pMonitorConfig != NULL, "pMonitorConfig is NULL.\n");
    if (!pMonitorConfig) goto out;

    /* call without any arguments */
    hr = IVMRMonitorConfig_GetAvailableMonitors(pMonitorConfig, NULL, 0, NULL);
    ok(hr == E_POINTER, "GetAvailableMonitors returned %x, expected E_POINTER.\n", hr);

    hr = IVMRMonitorConfig_GetAvailableMonitors(pMonitorConfig, info, 0, &numdev_total);
    ok(hr == E_INVALIDARG, "GetAvailableMonitors returned %x, expected E_INVALIDARG.\n", hr);

    numdev_total = 0;
    hr = IVMRMonitorConfig_GetAvailableMonitors(pMonitorConfig, NULL, 0, &numdev_total);
    ok(hr == S_OK, "GetAvailableMonitors failed with %x.\n", hr);
    ok(numdev_total > 0, "GetAvailableMonitors returned numdev_total = %d, expected > 0.\n", numdev_total);

    if (numdev_total > 1)
    {
        /* return just the first monitor */
        hr = IVMRMonitorConfig_GetAvailableMonitors(pMonitorConfig, info, 1, &numdev);
        ok(hr == S_OK, "GetAvailableMonitors failed with %x.\n", hr);
        ok(numdev == 1, "GetAvailableMonitors returned numdev = %d, expected 1.\n", numdev);
    }

    /* don't request information for more monitors than memory available */
    if (numdev_total > sizeof(info)/sizeof(VMRMONITORINFO))
        numdev_total = sizeof(info)/sizeof(VMRMONITORINFO);

    hr = IVMRMonitorConfig_GetAvailableMonitors(pMonitorConfig, info, numdev_total, &numdev);
    ok(hr == S_OK, "GetAvailableMonitors failed with %x.\n", hr);
    ok(numdev == numdev_total, "GetAvailableMonitors returned numdev = %d, expected %d.\n", numdev, numdev_total);

    /* TODO: Add test for content of info */

out:
    if (pMonitorConfig) IVMRMonitorConfig_Release(pMonitorConfig);
    if (pVMR) IUnknown_Release(pVMR);
}

START_TEST(monitorconfig)
{
    CoInitialize(NULL);

    test_monitorconfig_setmonitor();
    test_monitorconfig_getavailablemonitors();

    CoUninitialize();
}
