/*
 * Functions for general purpose USB device access in Microsoft Windows, by
 * dynamically loading & Linking WinUSB or libusbK. For libusbK, the native
 * API is used, but note that libusbK also provides an implementation of the
 * WinUSB API.
 *
 * Copyright 2020 CompuPhase
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <windows.h>
#include <assert.h>
#include "usb-support.h"

/*
 * Part 1: WinUSB
 */

BOOL (__stdcall *_WinUsb_Initialize)(HANDLE DeviceHandle, USB_INTERFACE_HANDLE *InterfaceHandle) = NULL;
BOOL (__stdcall *_WinUsb_Free)(USB_INTERFACE_HANDLE InterfaceHandle) = NULL;
BOOL (__stdcall *_WinUsb_QueryInterfaceSettings)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t AlternateInterfaceNumber, USB_INTERFACE_DESCRIPTOR *UsbAltInterfaceDescriptor) = NULL;
BOOL (__stdcall *_WinUsb_QueryPipe)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t AlternateInterfaceNumber, uint8_t PipeIndex, USB_PIPE_INFORMATION *PipeInformation) = NULL;
BOOL (__stdcall *_WinUsb_ReadPipe)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t PipeID, uint8_t *Buffer, uint32_t BufferLength, uint32_t *LengthTransferred, LPOVERLAPPED Overlapped) = NULL;

static HMODULE hinstWinUSB = NULL;

BOOL WinUsb_Load(void)
{
  if (hinstWinUSB != NULL)
    return TRUE;  /* double initialization */

  hinstWinUSB = LoadLibrary("winusb.dll");
  if (hinstWinUSB == NULL)
    return FALSE;

  _WinUsb_Initialize = (void*)GetProcAddress(hinstWinUSB, "WinUsb_Initialize");
  _WinUsb_Free = (void*)GetProcAddress(hinstWinUSB, "WinUsb_Free");
  _WinUsb_QueryInterfaceSettings = (void*)GetProcAddress(hinstWinUSB, "WinUsb_QueryInterfaceSettings");
  _WinUsb_QueryPipe = (void*)GetProcAddress(hinstWinUSB, "WinUsb_QueryPipe");
  _WinUsb_ReadPipe = (void*)GetProcAddress(hinstWinUSB, "WinUsb_ReadPipe");
  assert(_WinUsb_Initialize != NULL && _WinUsb_Free != NULL && _WinUsb_QueryInterfaceSettings != NULL
         && _WinUsb_QueryPipe != NULL && _WinUsb_ReadPipe != NULL);
  return TRUE;
}

BOOL WinUsb_Unload(void)
{
  if (hinstWinUSB == NULL)
    return FALSE;
  FreeLibrary(hinstWinUSB);
  hinstWinUSB = NULL;
  _WinUsb_Initialize = NULL;
  _WinUsb_Free = NULL;
  _WinUsb_QueryInterfaceSettings = NULL;
  _WinUsb_QueryPipe = NULL;
  _WinUsb_ReadPipe = NULL;

  return TRUE;
}

BOOL WinUsb_IsActive(void)
{
  return hinstWinUSB != NULL;
}


/*
 * Part 2: libusbK
 */

BOOL (__stdcall *_UsbK_Init)(USB_INTERFACE_HANDLE* InterfaceHandle, const KLST_DEVINFO *DevInfo) = NULL;
BOOL (__stdcall *_UsbK_Free)(USB_INTERFACE_HANDLE InterfaceHandle) = NULL;
BOOL (__stdcall *_UsbK_QueryInterfaceSettings)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t AlternateInterfaceNumber, USB_INTERFACE_DESCRIPTOR *UsbAltInterfaceDescriptor) = NULL;
BOOL (__stdcall *_UsbK_QueryPipe)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t AlternateInterfaceNumber, uint8_t PipeIndex, USB_PIPE_INFORMATION *PipeInformation) = NULL;
BOOL (__stdcall *_UsbK_ReadPipe)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t PipeID, uint8_t *Buffer, uint32_t BufferLength, uint32_t *LengthTransferred, LPOVERLAPPED Overlapped) = NULL;

BOOL (__stdcall *_LstK_Init)(KLST_DEVINFO **DeviceList, int Flags) = NULL;
BOOL (__stdcall *_LstK_Free)(KLST_DEVINFO *DeviceList) = NULL;
BOOL (__stdcall *_LstK_Count)(const KLST_DEVINFO *DeviceList, uint32_t *Count) = NULL;
BOOL (__stdcall *_LstK_Enumerate)(const KLST_DEVINFO *DeviceList, KLST_ENUM_DEVINFO_CB *EnumDevListCB, void *Context) = NULL;

static HMODULE hinstUsbK = NULL;

BOOL UsbK_Load(void)
{
  if (hinstUsbK != NULL)
    return TRUE;  /* double initialization */

  hinstUsbK = LoadLibrary("libusbK.dll");
  if (hinstUsbK == NULL)
    return FALSE;

  _UsbK_Init = (void*)GetProcAddress(hinstUsbK, "UsbK_Init");
  _UsbK_Free = (void*)GetProcAddress(hinstUsbK, "UsbK_Free");
  _UsbK_QueryInterfaceSettings = (void*)GetProcAddress(hinstUsbK, "UsbK_QueryInterfaceSettings");
  _UsbK_QueryPipe = (void*)GetProcAddress(hinstUsbK, "UsbK_QueryPipe");
  _UsbK_ReadPipe = (void*)GetProcAddress(hinstUsbK, "UsbK_ReadPipe");
  _LstK_Init = (void*)GetProcAddress(hinstUsbK, "LstK_Init");
  _LstK_Free = (void*)GetProcAddress(hinstUsbK, "LstK_Free");
  _LstK_Count = (void*)GetProcAddress(hinstUsbK, "LstK_Count");
  _LstK_Enumerate = (void*)GetProcAddress(hinstUsbK, "LstK_Enumerate");
  assert(_UsbK_Init != NULL && _UsbK_Free != NULL && _UsbK_QueryInterfaceSettings != NULL
         && _UsbK_QueryPipe != NULL && _UsbK_ReadPipe != NULL && _LstK_Init != NULL
         && _LstK_Free != NULL  && _LstK_Count != NULL  && _LstK_Enumerate != NULL);
  return TRUE;
}

BOOL UsbK_Unload(void)
{
  if (hinstUsbK == NULL)
    return FALSE;
  FreeLibrary(hinstUsbK);
  hinstUsbK = NULL;
  _UsbK_Init = NULL;
  _UsbK_Free = NULL;
  _UsbK_QueryInterfaceSettings = NULL;
  _UsbK_QueryPipe = NULL;
  _UsbK_ReadPipe = NULL;
  _LstK_Init = NULL;
  _LstK_Free = NULL;
  _LstK_Count = NULL;
  _LstK_Enumerate = NULL;

  return TRUE;
}

BOOL UsbK_IsActive(void)
{
  return hinstUsbK != NULL;
}

