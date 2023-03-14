/*
 * Functions for general purpose USB device access in Microsoft Windows, by
 * dynamically loading & Linking WinUSB.
 *
 * Copyright 2020-2023 CompuPhase
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
#ifndef _USB_SUPPORT_H
#define _USB_SUPPORT_H

#include <windows.h>
#include <stdint.h>

#if defined __cplusplus
  extern "C" {
#endif

/*
 * General types (for both WinUSB and libusbK)
 */
typedef enum {
  PIPETYPE_CONTROL,
  PIPETYPE_ISOCHRONOUS,
  PIPETYPE_BULK,
  PIPETYPE_INTERRUPT
} USB_PIPE_TYPE;

typedef struct tagUSB_INTERFACE_DESCRIPTOR {
  uint8_t bLength;              /**< length of the descriptor in bytes */
  uint8_t bDescriptorType;
  uint8_t bInterfaceNumber;
  uint8_t bAlternateSetting;
  uint8_t bNumEndpoints;        /**< number of endpoints excluding the control endpoint */
  uint8_t bInterfaceClass;      /**< USB-IF class code for this interface */
  uint8_t bInterfaceSubClass;   /**< USB-IF subclass code for this interface*/
  uint8_t bInterfaceProtocol;   /**< USB-IF protocol code for this interface */
  uint8_t iInterface;           /**< index of the string descriptor for this interface */
} USB_INTERFACE_DESCRIPTOR;

typedef struct tagUSB_PIPE_INFORMATION {
  uint32_t PipeType;            /**< one of the values of the USB_PIPE_TYPE enumeration */
  uint8_t  PipeId;
  uint16_t MaximumPacketSize;   /**< in bytes */
  uint8_t  Interval;            /**< in milliseconds */
} USB_PIPE_INFORMATION;

typedef void* USB_INTERFACE_HANDLE;


/*
 * Part 1: WinUSB
 * minimal subset of WinUSB types & functions
 */

extern BOOL (__stdcall *_WinUsb_Initialize)(HANDLE DeviceHandle, USB_INTERFACE_HANDLE *InterfaceHandle);
extern BOOL (__stdcall *_WinUsb_Free)(USB_INTERFACE_HANDLE InterfaceHandle);
extern BOOL (__stdcall *_WinUsb_QueryInterfaceSettings)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t AlternateInterfaceNumber, USB_INTERFACE_DESCRIPTOR *UsbAltInterfaceDescriptor);
extern BOOL (__stdcall *_WinUsb_QueryPipe)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t AlternateInterfaceNumber, uint8_t PipeIndex, USB_PIPE_INFORMATION *PipeInformation);
extern BOOL (__stdcall *_WinUsb_ReadPipe)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t PipeID, uint8_t *Buffer, uint32_t BufferLength, uint32_t *LengthTransferred, LPOVERLAPPED Overlapped);

/* WinUSB loading & unloading functions */
BOOL WinUsb_Load(void);
BOOL WinUsb_Unload(void);
BOOL WinUsb_IsActive(void);


/*
 * Part 2: libusbK
 */

#define KLST_STRING_MAX_LEN 256

typedef struct tagKLST_DEVINFO {
  int Vid;
  int Pid;
  int MI;                               /**< interface number, set to -1 for non-composite devices */
  char InstanceID[KLST_STRING_MAX_LEN]; /**< uniquely identifies the USB device */
	int DriverID;
	char DeviceInterfaceGUID[KLST_STRING_MAX_LEN];
	char DeviceID[KLST_STRING_MAX_LEN];
	char ClassGUID[KLST_STRING_MAX_LEN];
	char Mfg[KLST_STRING_MAX_LEN];        /**< Manufacturer name */
	char DeviceDesc[KLST_STRING_MAX_LEN];
	char Service[KLST_STRING_MAX_LEN];    /**< driver/service name */
	char SymbolicLink[KLST_STRING_MAX_LEN];
	char DevicePath[KLST_STRING_MAX_LEN]; /**< Windows virtual path, as used in CreateFile() */
	int LUsb0FilterIndex;                 /**< libusb-win32 filter index id */
	BOOL Connected;
	long SyncFlags;                       /**< Synchronization flags (internal use only) */
	int BusNumber;
	INT DeviceAddress;
	char SerialNumber[KLST_STRING_MAX_LEN];
} KLST_DEVINFO;

typedef void* KUSB_HANDLE;
typedef void* KLST_HANDLE;

typedef BOOL __stdcall KLST_ENUM_DEVINFO_CB(KLST_HANDLE DeviceList, KLST_DEVINFO *DeviceInfo, PVOID Context);

extern BOOL (__stdcall *_UsbK_Init)(USB_INTERFACE_HANDLE* InterfaceHandle, const KLST_DEVINFO *DevInfo);
extern BOOL (__stdcall *_UsbK_Free)(USB_INTERFACE_HANDLE InterfaceHandle);
extern BOOL (__stdcall *_UsbK_QueryInterfaceSettings)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t AlternateInterfaceNumber, USB_INTERFACE_DESCRIPTOR *UsbAltInterfaceDescriptor);
extern BOOL (__stdcall *_UsbK_QueryPipe)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t AlternateInterfaceNumber, uint8_t PipeIndex, USB_PIPE_INFORMATION *PipeInformation);
extern BOOL (__stdcall *_UsbK_ReadPipe)(USB_INTERFACE_HANDLE InterfaceHandle, uint8_t PipeID, uint8_t *Buffer, uint32_t BufferLength, uint32_t *LengthTransferred, LPOVERLAPPED Overlapped);

extern BOOL (__stdcall *_LstK_Init)(KLST_DEVINFO **DeviceList, int Flags);
extern BOOL (__stdcall *_LstK_Free)(KLST_DEVINFO *DeviceList);
extern BOOL (__stdcall *_LstK_Count)(const KLST_DEVINFO *DeviceList, uint32_t *Count);
extern BOOL (__stdcall *_LstK_Enumerate)(const KLST_DEVINFO *DeviceList, KLST_ENUM_DEVINFO_CB *EnumDevListCB, void *Context);

/* WinUSB loading, unloading & compatibility functions */
BOOL UsbK_Load(void);
BOOL UsbK_Unload(void);
BOOL UsbK_IsActive(void);

#if defined __cplusplus
  }
#endif

#endif /* _USB_SUPPORT_H */

