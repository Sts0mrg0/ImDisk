/*
    ImDisk Virtual Disk Driver for Windows NT/2000/XP.
    This driver emulates harddisk partitions, floppy drives and CD/DVD-ROM
    drives from disk image files, in virtual memory or by redirecting I/O
    requests somewhere else, possibly to another machine, through a
    co-operating user-mode service, ImDskSvc.

    Copyright (C) 2005-2007 Olof Lagerkvist.

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use,
    copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following
    conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
    OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
    OTHER DEALINGS IN THE SOFTWARE.

    This source file contains some GNU GPL licensed code:
    - Parts related to floppy emulation based on VFD by Ken Kato.
      http://chitchat.at.infoseek.co.jp/vmware/vfd.html
    Copyright (C) Free Software Foundation, Inc.
    Read gpl.txt for the full GNU GPL license.

    This source file may contain BSD licensed code:
    - Some code ported to NT from the FreeBSD md driver by Olof Lagerkvist.
      http://www.ltr-data.se
    Copyright (C) The FreeBSD Project.
    Copyright (C) The Regents of the University of California.
*/

#include <ntddk.h>
#include <ntdddisk.h>
#include <ntddcdrm.h>
#include <ntverp.h>
#include <mountmgr.h>
#include <stdio.h>

///
/// Definitions and imports are now in the "sources" file and managed by the
/// build utility.
///

#ifndef DEBUG_LEVEL
#define DEBUG_LEVEL 0
#endif

#if DEBUG_LEVEL >= 2
#define KdPrint2(x) DbgPrint x
#else
#define KdPrint2(x)
#endif

#if DEBUG_LEVEL >= 1
#undef KdPrint
#define KdPrint(x)  DbgPrint x
#endif

#include "..\inc\ntkmapi.h"
#include "..\inc\imdisk.h"
#include "..\inc\imdproxy.h"

#define IMDISK_DEFAULT_LOAD_DEVICES      0
#define IMDISK_DEFAULT_MAX_DEVICES       16

///
/// Constants for synthetical geometry of the virtual disks
///

// For hard drive partition-style devices
#define SECTOR_SIZE_HDD                  512

// For CD-ROM/DVD-style devices
#define SECTOR_SIZE_CD_ROM               2048
#define SECTORS_PER_TRACK_CD_ROM         32
#define TRACKS_PER_CYLINDER_CD_ROM       64

// For floppy devices. Based on Virtual Floppy Driver, VFD, by Ken Kato.
#define SECTOR_SIZE_FDD                  512
#define TRACKS_PER_CYLINDER_FDD          12
//
//	Sizes in bytes of different kinds of floppies
//
// 3.5" UHD
#define MEDIA_SIZE_240MB        (234752 << 10)
#define MEDIA_SIZE_120MB        (123264 << 10)
// 3.5"
#define MEDIA_SIZE_2800KB	(2880 << 10)
#define MEDIA_SIZE_1722KB       (1722 << 10)
#define MEDIA_SIZE_1680KB       (1680 << 10)
#define MEDIA_SIZE_1440KB	(1440 << 10)
#define	MEDIA_SIZE_820KB 	(820  << 10)
#define	MEDIA_SIZE_720KB 	(720  << 10)
// 5.25"
#define MEDIA_SIZE_1200KB	(1200 << 10)
#define MEDIA_SIZE_640KB        (640  << 10)
#define	MEDIA_SIZE_360KB	(360  << 10)
#define	MEDIA_SIZE_320KB 	(320  << 10)
#define MEDIA_SIZE_180KB	(180  << 10)
#define	MEDIA_SIZE_160KB 	(160  << 10)

//
//      Indexes for the following DISK_GEOMETRY table.
//
enum {
  // 3.5" UHD
  MEDIA_TYPE_240M,
  MEDIA_TYPE_120M,
  // 3.5"
  MEDIA_TYPE_2880K,
  MEDIA_TYPE_1722K,
  MEDIA_TYPE_1680K,
  MEDIA_TYPE_1440K,
  MEDIA_TYPE_820K,
  MEDIA_TYPE_720K,
  // 5.12"
  MEDIA_TYPE_1200K,
  MEDIA_TYPE_640K,
  MEDIA_TYPE_360K,
  MEDIA_TYPE_320K,
  MEDIA_TYPE_180K,
  MEDIA_TYPE_160K
};

DISK_GEOMETRY media_table[] = {
  // 3.5" UHD
  { { 963 }, F3_120M_512,  8, 32, 512 },
  { { 262 }, F3_120M_512, 32, 56, 512 },
  // 3.5"
  { {  80 }, F3_2Pt88_512, 2, 36, 512 },
  { {  82 }, F3_1Pt44_512, 2, 21, 512 },
  { {  80 }, F3_1Pt44_512, 2, 21, 512 },
  { {  80 }, F3_1Pt44_512, 2, 18, 512 },
  { {  82 }, F3_720_512,   2, 10, 512 },
  { {  80 }, F3_720_512,   2,  9, 512 },
  // 5.25"
  { {  80 }, F5_1Pt2_512,  2, 15, 512 },
  { {  40 }, F5_640_512,   2, 18, 512 },
  { {  40 }, F5_360_512,   2,  9, 512 },
  { {  40 }, F5_320_512,   2,  8, 512 },
  { {  40 }, F5_180_512,   1,  9, 512 },
  { {  40 }, F5_160_512,   1,  8, 512 }
};

//
//	TOC Data Track returned for virtual CD/DVD
//
#define TOC_DATA_TRACK                   0x04

//
//	Fill character for formatting virtual floppy media
//
#define MEDIA_FORMAT_FILL_DATA	0xf6

// This structure is used when a new device is about to be created. It is sent
// to the created device dispatch thread which also creates the device object.
typedef struct _DEVICE_THREAD_DATA
{
  PDRIVER_OBJECT driver_object;
  PIMDISK_CREATE_DATA create_data;
  PETHREAD client_thread;   // The client thread that device should impersonate
  KEVENT created_event;     // Set when device is created (or creation failed)
  BOOLEAN caller_waiting;   // If there is a caller waiting to free this data
  NTSTATUS status;          // Set after device creation attempt
} DEVICE_THREAD_DATA, *PDEVICE_THREAD_DATA;

typedef struct _DEVICE_EXTENSION
{
  LIST_ENTRY list_head;
  KSPIN_LOCK list_lock;
  KEVENT request_event;
  BOOLEAN terminate_thread;
  ULONG device_number;
  HANDLE file_handle;          // For file or proxy type
  PUCHAR image_buffer;         // For vm type
  PFILE_OBJECT proxy_device;   // Pointer to proxy communication object
  UNICODE_STRING file_name;    // Name of image file, if any
  WCHAR drive_letter;          // Drive letter if maintained by the driver
  DISK_GEOMETRY disk_geometry; // Virtual C/H/S geometry (Cylinders=Total size)
  LARGE_INTEGER image_offset;  // Offset in bytes in the image file
  ULONG media_change_count;
  BOOLEAN read_only;
  BOOLEAN vm_disk;             // TRUE if this device is a virtual memory disk
  BOOLEAN use_proxy;           // TRUE if this device uses proxy device for I/O
  PKTHREAD device_thread;      // Pointer to the dispatch thread object
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

// Prototypes for functions defined in this driver

NTSTATUS
DriverEntry(IN PDRIVER_OBJECT DriverObject,
	    IN PUNICODE_STRING RegistryPath);

VOID
ImDiskUnload(IN PDRIVER_OBJECT DriverObject);

NTSTATUS
ImDiskAddVirtualDisk(IN PDRIVER_OBJECT DriverObject,
		     IN OUT PIMDISK_CREATE_DATA CreateData,
		     IN PETHREAD ClientThread);

NTSTATUS
ImDiskAddVirtualDiskAfterInitialization(IN PDRIVER_OBJECT DriverObject,
					IN HANDLE ParameterKey,
					IN ULONG DeviceNumber);

VOID
ImDiskRemoveVirtualDisk(IN PDEVICE_OBJECT DeviceObject);

NTSTATUS
ImDiskCreateClose(IN PDEVICE_OBJECT DeviceObject,
		  IN PIRP Irp);

NTSTATUS
ImDiskReadWrite(IN PDEVICE_OBJECT DeviceObject,
		IN PIRP Irp);

NTSTATUS
ImDiskDeviceControl(IN PDEVICE_OBJECT DeviceObject,
		    IN PIRP Irp);

VOID
ImDiskDeviceThread(IN PVOID Context);

NTSTATUS
ImDiskConnectProxy(IN OUT PFILE_OBJECT *ProxyDevice,
		   OUT PIO_STATUS_BLOCK IoStatusBlock,
		   IN ULONG Flags,
		   IN PWSTR ConnectionString,
		   IN ULONG ConnectionStringLength);

NTSTATUS
ImDiskQueryInformationProxy(IN PFILE_OBJECT ProxyDevice,
			    OUT PIO_STATUS_BLOCK IoStatusBlock,
			    OUT PIMDPROXY_INFO_RESP ProxyInfoResponse,
			    IN ULONG ProxyInfoResponseLength);

NTSTATUS
ImDiskReadProxy(IN PFILE_OBJECT ProxyDevice,
		OUT PIO_STATUS_BLOCK IoStatusBlock,
		OUT PVOID Buffer,
		IN ULONG Length,
		IN PLARGE_INTEGER ByteOffset);

NTSTATUS
ImDiskWriteProxy(IN PFILE_OBJECT ProxyDevice,
		 OUT PIO_STATUS_BLOCK IoStatusBlock,
		 OUT PVOID Buffer,
		 IN ULONG Length,
		 IN PLARGE_INTEGER ByteOffset);

//
// Reads in a loop up to "Length" or until eof reached.
//
NTSTATUS
ImDiskSafeReadFile(IN HANDLE FileHandle,
		   OUT PIO_STATUS_BLOCK IoStatusBlock,
		   OUT PVOID Buffer,
		   IN ULONG Length,
		   IN PLARGE_INTEGER Offset);

NTSTATUS
ImDiskFloppyFormat(IN PDEVICE_EXTENSION Extension,
		   IN PIRP Irp);

//
// Pointer to the controller device object.
//
PDEVICE_OBJECT ImDiskCtlDevice;

//
// Allocation bitmap with currently cnfigured device numbers.
//
volatile ULONG DeviceList = 0;

//
// Max number of devices that can be dynamically created by IOCTL calls
// to the control device. Note that because the device number allocation is
// stored in a 32-bit bitfield, this number can be max 32.
//
ULONG MaxDevices;

#pragma code_seg("INIT")

//
// This is where it all starts...
//
NTSTATUS
DriverEntry(IN PDRIVER_OBJECT DriverObject,
	    IN PUNICODE_STRING RegistryPath)
{
  UNICODE_STRING parameter_path;
  UNICODE_STRING ctl_device_name;
  UNICODE_STRING sym_link;
  HANDLE key_handle;
  ULONG n_devices;
  NTSTATUS status;
  OBJECT_ATTRIBUTES object_attributes;
  ULONG n;

  // First open and read registry settings to find out if we should load and
  // mount anything automatically.
  parameter_path.Length = 0;

  parameter_path.MaximumLength = RegistryPath->Length +
    sizeof(IMDISK_CFG_PARAMETER_KEY);

  parameter_path.Buffer =
    (PWSTR) ExAllocatePool(PagedPool, parameter_path.MaximumLength);

  if (parameter_path.Buffer == NULL)
    return STATUS_INSUFFICIENT_RESOURCES;

  RtlCopyUnicodeString(&parameter_path, RegistryPath);

  RtlAppendUnicodeToString(&parameter_path, IMDISK_CFG_PARAMETER_KEY);

  InitializeObjectAttributes(&object_attributes, &parameter_path, 0, NULL,
			     NULL);

  status = ZwOpenKey(&key_handle, KEY_READ, &object_attributes);
  if (!NT_SUCCESS(status))
    key_handle = NULL;

  ExFreePool(parameter_path.Buffer);

  if (key_handle != NULL)
    {
      UNICODE_STRING number_of_devices_value;
      PKEY_VALUE_PARTIAL_INFORMATION value_info;
      ULONG required_size;

      value_info = ExAllocatePool(PagedPool,
				  sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
				  sizeof(ULONG));

      if (value_info == NULL)
	{
	  ZwClose(key_handle);
	  return STATUS_INSUFFICIENT_RESOURCES;
	}

      RtlInitUnicodeString(&number_of_devices_value,
			   IMDISK_CFG_LOAD_DEVICES_VALUE);
      
      status = ZwQueryValueKey(key_handle, &number_of_devices_value,
			       KeyValuePartialInformation, value_info,
			       sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
			       sizeof(ULONG), &required_size);

      if (!NT_SUCCESS(status))
	{
	  KdPrint(("ImDisk: Using default value for '%ws'.\n",
		   IMDISK_CFG_LOAD_DEVICES_VALUE));
	  n_devices = IMDISK_DEFAULT_LOAD_DEVICES;
	}
      else if (value_info->Type == REG_DWORD)
	n_devices = *(PULONG) value_info->Data;
      else
	{
	  ExFreePool(value_info);
	  ZwClose(key_handle);
	  return STATUS_INVALID_PARAMETER;
	}

      RtlInitUnicodeString(&number_of_devices_value,
			   IMDISK_CFG_MAX_DEVICES_VALUE);
      
      status = ZwQueryValueKey(key_handle, &number_of_devices_value,
			       KeyValuePartialInformation, value_info,
			       sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
			       sizeof(ULONG), &required_size);

      if (!NT_SUCCESS(status))
	{
	  KdPrint(("ImDisk: Using default value for '%ws'.\n",
		   IMDISK_CFG_MAX_DEVICES_VALUE));
	  MaxDevices = IMDISK_DEFAULT_MAX_DEVICES;
	}
      else if (value_info->Type == REG_DWORD)
	MaxDevices = *(PULONG) value_info->Data;
      else
	{
	  ExFreePool(value_info);
	  ZwClose(key_handle);
	  return STATUS_INVALID_PARAMETER;
	}

      ExFreePool(value_info);
    }
 else
   {
     KdPrint
       (("ImDisk: Cannot open registry key (%#x), using default values.\n",
	 status));

     n_devices = IMDISK_DEFAULT_LOAD_DEVICES;
     MaxDevices = IMDISK_DEFAULT_MAX_DEVICES;
   }

  // Create the control device.
  RtlInitUnicodeString(&ctl_device_name, IMDISK_CTL_DEVICE_NAME);

  status = IoCreateDevice(DriverObject,
			  sizeof(DEVICE_EXTENSION),
			  &ctl_device_name,
			  FILE_DEVICE_IMDISK,
			  0,
			  FALSE,
			  &ImDiskCtlDevice);

  if (!NT_SUCCESS(status))
    {
      KdPrint(("ImDisk: Cannot create control device (%#x).\n", status));
      if (key_handle != NULL)
	ZwClose(key_handle);

      return status;
    }

  // The control device gets a device_number of -1 to make it easily
  // distinguishable from the actual created devices.
  ((PDEVICE_EXTENSION) ImDiskCtlDevice->DeviceExtension)->device_number =
    (ULONG) -1;

  RtlInitUnicodeString(&sym_link, IMDISK_CTL_SYMLINK_NAME);
  IoCreateUnprotectedSymbolicLink(&sym_link, &ctl_device_name);

  // If the registry settings told us to create devices here in the start
  // procedure, do that now.
  for (n = 0; n < n_devices; n++)
    ImDiskAddVirtualDiskAfterInitialization(DriverObject, key_handle, n);

  if (key_handle != NULL)
    ZwClose(key_handle);

  DriverObject->MajorFunction[IRP_MJ_CREATE] = ImDiskCreateClose;
  DriverObject->MajorFunction[IRP_MJ_CLOSE] = ImDiskCreateClose;
  DriverObject->MajorFunction[IRP_MJ_READ] = ImDiskReadWrite;
  DriverObject->MajorFunction[IRP_MJ_WRITE] = ImDiskReadWrite;
  DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ImDiskDeviceControl;

  DriverObject->DriverUnload = ImDiskUnload;

  KdPrint(("ImDisk: Initialization done. Leaving DriverEntry().\n", status));

  return STATUS_SUCCESS;
}

NTSTATUS
ImDiskAddVirtualDiskAfterInitialization(IN PDRIVER_OBJECT DriverObject,
					IN HANDLE ParameterKey,
					IN ULONG DeviceNumber)
{
  NTSTATUS status;
  PDEVICE_THREAD_DATA device_thread_data;
  HANDLE thread_handle;
  LARGE_INTEGER wait_time;
  PKEY_VALUE_PARTIAL_INFORMATION value_info_image_file;
  PKEY_VALUE_PARTIAL_INFORMATION value_info_size;
  PKEY_VALUE_PARTIAL_INFORMATION value_info_flags;
  PKEY_VALUE_PARTIAL_INFORMATION value_info_drive_letter;
  ULONG required_size;
  PIMDISK_CREATE_DATA create_data;
  PWSTR value_name_buffer;
  UNICODE_STRING value_name;

  PAGED_CODE();

  ASSERT(DriverObject != NULL);
  ASSERT(ParameterKey != NULL);

  wait_time.QuadPart = -1;
  KeDelayExecutionThread(KernelMode, FALSE, &wait_time);

  value_info_image_file =
    ExAllocatePool(PagedPool,
		   sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
		   (MAXIMUM_FILENAME_LENGTH * sizeof(WCHAR)));

  if (value_info_image_file == NULL)
    {
      KdPrint(("ImDisk: Error creating device %u. (ExAllocatePool)\n",
	       DeviceNumber));

      return STATUS_INSUFFICIENT_RESOURCES;
    }

  value_info_size =
    ExAllocatePool(PagedPool,
		   sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
		   sizeof(LARGE_INTEGER));

  if (value_info_size == NULL)
    {
      KdPrint(("ImDisk: Error creating device %u. (ExAllocatePool)\n",
	       DeviceNumber));

      ExFreePool(value_info_image_file);
      return STATUS_INSUFFICIENT_RESOURCES;
    }

  value_info_flags =
    ExAllocatePool(PagedPool,
		   sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG));

  if (value_info_flags == NULL)
    {
      KdPrint(("ImDisk: Error creating device %u. (ExAllocatePool)\n",
	       DeviceNumber));

      ExFreePool(value_info_image_file);
      ExFreePool(value_info_size);
      return STATUS_INSUFFICIENT_RESOURCES;
    }

  value_info_drive_letter =
    ExAllocatePool(PagedPool,
		   sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(WCHAR));

  if (value_info_drive_letter == NULL)
    {
      KdPrint(("ImDisk: Error creating device %u. (ExAllocatePool)\n",
	       DeviceNumber));

      ExFreePool(value_info_image_file);
      ExFreePool(value_info_size);
      ExFreePool(value_info_flags);
      return STATUS_INSUFFICIENT_RESOURCES;
    }

  value_name_buffer = ExAllocatePool(PagedPool,
				     MAXIMUM_FILENAME_LENGTH * sizeof(WCHAR));

  if (value_name_buffer == NULL)
    {
      KdPrint(("ImDisk: Error creating device %u. (ExAllocatePool)\n",
	       DeviceNumber));
      
      ExFreePool(value_info_image_file);
      ExFreePool(value_info_size);
      ExFreePool(value_info_flags);
      ExFreePool(value_info_drive_letter);
      return STATUS_INSUFFICIENT_RESOURCES;
    }

  _snwprintf(value_name_buffer, MAXIMUM_FILENAME_LENGTH - 1,
	     IMDISK_CFG_IMAGE_FILE_PREFIX L"%u", DeviceNumber);
  value_name_buffer[MAXIMUM_FILENAME_LENGTH - 1] = 0;

  RtlInitUnicodeString(&value_name, value_name_buffer);

  status = ZwQueryValueKey(ParameterKey,
			   &value_name,
			   KeyValuePartialInformation,
			   value_info_image_file,
			   sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
			   (MAXIMUM_FILENAME_LENGTH * sizeof(WCHAR)),
			   &required_size);

  if ((!NT_SUCCESS(status)) |
      (value_info_image_file->Type != REG_SZ))
    {
      KdPrint(("ImDisk: Missing or bad '%ws' for device %i.\n",
	       value_name_buffer, DeviceNumber));

      *(PWCHAR) value_info_image_file->Data = 0;
      value_info_image_file->DataLength = sizeof(WCHAR);
    }

  _snwprintf(value_name_buffer, MAXIMUM_FILENAME_LENGTH - 1,
	     IMDISK_CFG_SIZE_PREFIX L"%u", DeviceNumber);
  value_name_buffer[MAXIMUM_FILENAME_LENGTH - 1] = 0;
  
  RtlInitUnicodeString(&value_name, value_name_buffer);

  status = ZwQueryValueKey(ParameterKey,
			   &value_name,
			   KeyValuePartialInformation,
			   value_info_size,
			   sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
			   sizeof(LARGE_INTEGER),
			   &required_size);

  if ((!NT_SUCCESS(status)) |
      (value_info_size->Type != REG_BINARY) |
      (value_info_size->DataLength != sizeof(LARGE_INTEGER)))
    {
      KdPrint(("ImDisk: Missing or bad '%ws' for device %i.\n",
	       value_name_buffer, DeviceNumber));

      ((PLARGE_INTEGER) value_info_size->Data)->QuadPart = 0;
    }

  _snwprintf(value_name_buffer, MAXIMUM_FILENAME_LENGTH - 1,
	     IMDISK_CFG_FLAGS_PREFIX L"%u", DeviceNumber);
  value_name_buffer[MAXIMUM_FILENAME_LENGTH - 1] = 0;

  RtlInitUnicodeString(&value_name, value_name_buffer);

  status = ZwQueryValueKey(ParameterKey,
			   &value_name,
			   KeyValuePartialInformation,
			   value_info_flags,
			   sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
			   sizeof(ULONG),
			   &required_size);

  if ((!NT_SUCCESS(status)) |
      (value_info_flags->Type != REG_DWORD))
    {
      KdPrint(("ImDisk: Missing or bad '%ws' for device %i.\n",
	       value_name_buffer, DeviceNumber));

      *(PULONG) value_info_flags->Data = 0;
    }

  _snwprintf(value_name_buffer, MAXIMUM_FILENAME_LENGTH - 1,
	     IMDISK_CFG_DRIVE_LETTER_PREFIX L"%u", DeviceNumber);
  value_name_buffer[MAXIMUM_FILENAME_LENGTH - 1] = 0;

  RtlInitUnicodeString(&value_name, value_name_buffer);

  status = ZwQueryValueKey(ParameterKey,
			   &value_name,
			   KeyValuePartialInformation,
			   value_info_drive_letter,
			   sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
			   sizeof(WCHAR),
			   &required_size);

  if (!NT_SUCCESS(status))
    {
      KdPrint(("ImDisk: Missing or bad '%ws' for device %i.\n",
	       value_name_buffer, DeviceNumber));

      *(PWCHAR) value_info_drive_letter->Data = 0;
    }

  ExFreePool(value_name_buffer);
  
  create_data =
    ExAllocatePool(PagedPool,
		   sizeof(IMDISK_CREATE_DATA) +
		   value_info_image_file->DataLength);

  if (create_data == NULL)
    {
      KdPrint(("ImDisk: Error creating device %u. (ExAllocatePool)\n",
	       DeviceNumber));

      ExFreePool(value_info_image_file);
      ExFreePool(value_info_size);
      ExFreePool(value_info_flags);
      ExFreePool(value_info_drive_letter);
      return STATUS_INSUFFICIENT_RESOURCES;
    }

  RtlZeroMemory(create_data, sizeof(IMDISK_CREATE_DATA));

  wcscpy(create_data->FileName, (PCWSTR) value_info_image_file->Data);

  create_data->FileNameLength = (USHORT)
    value_info_image_file->DataLength - sizeof(WCHAR);

  ExFreePool(value_info_image_file);

  create_data->DiskGeometry.Cylinders.QuadPart =
    ((PLARGE_INTEGER) value_info_size->Data)->QuadPart;

  ExFreePool(value_info_size);

  create_data->Flags = *(PULONG) value_info_flags->Data;

  ExFreePool(value_info_flags);

  create_data->DriveLetter = *(PWCHAR) value_info_drive_letter->Data;

  ExFreePool(value_info_drive_letter);

  create_data->DeviceNumber = DeviceNumber;

  device_thread_data = ExAllocatePool(PagedPool, sizeof(DEVICE_THREAD_DATA));
  if (device_thread_data == NULL)
    return STATUS_INSUFFICIENT_RESOURCES;

  device_thread_data->driver_object = DriverObject;
  device_thread_data->create_data = create_data;
  device_thread_data->client_thread = NULL;
  device_thread_data->caller_waiting = FALSE;

  status = PsCreateSystemThread(&thread_handle,
				(ACCESS_MASK) 0L,
				NULL,
				NULL,
				NULL,
				ImDiskDeviceThread,
				device_thread_data);

  if (!NT_SUCCESS(status))
    {
      KdPrint(("ImDisk: Cannot create device thread. (%#x)\n", status));

      ExFreePool(device_thread_data);
      ExFreePool(create_data);

      return status;
    }

  ZwClose(thread_handle);

  return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")

NTSTATUS
ImDiskAddVirtualDisk(IN PDRIVER_OBJECT DriverObject,
		     IN OUT PIMDISK_CREATE_DATA CreateData,
		     IN PETHREAD ClientThread)
{
  NTSTATUS status;
  DEVICE_THREAD_DATA device_thread_data;
  HANDLE thread_handle;

  PAGED_CODE();

  ASSERT(DriverObject != NULL);
  ASSERT(CreateData != NULL);

  device_thread_data.driver_object = DriverObject;
  device_thread_data.create_data = CreateData;
  device_thread_data.client_thread = ClientThread;
  device_thread_data.caller_waiting = TRUE;
  KeInitializeEvent(&device_thread_data.created_event,
		    NotificationEvent,
		    FALSE);

  status = PsCreateSystemThread(&thread_handle,
				(ACCESS_MASK) 0L,
				NULL,
				NULL,
				NULL,
				ImDiskDeviceThread,
				&device_thread_data);

  if (!NT_SUCCESS(status))
    {
      KdPrint(("ImDisk: Cannot create device thread. (%#x)\n", status));

      return status;
    }

  ZwClose(thread_handle);

  KeWaitForSingleObject(&device_thread_data.created_event,
			Executive,
			KernelMode,
			FALSE,
			NULL);

  if (!NT_SUCCESS(device_thread_data.status))
    {
      KdPrint(("ImDisk: Device thread failed to initialize. (%#x)\n",
	       device_thread_data.status));

      return device_thread_data.status;
    }

  return STATUS_SUCCESS;
}

NTSTATUS
ImDiskCreateDevice(IN PDRIVER_OBJECT DriverObject,
		   IN OUT PIMDISK_CREATE_DATA CreateData,
		   IN PETHREAD ClientThread,
		   OUT PDEVICE_OBJECT *DeviceObject)
{
  UNICODE_STRING file_name;
  PWCHAR device_name_buffer;
  UNICODE_STRING device_name;
  NTSTATUS status;
  PDEVICE_EXTENSION device_extension;
  DEVICE_TYPE device_type;
  ULONG device_characteristics;
  HANDLE file_handle = NULL;
  PUCHAR image_buffer = NULL;
  PFILE_OBJECT proxy_device = NULL;
  ULONG alignment_requirement;

  PAGED_CODE();

  ASSERT(CreateData != NULL);

  KdPrint
    (("ImDisk: Got request to create a virtual disk. Request data:\n"
      "DeviceNumber   = %#x\n"
      "DiskGeometry\n"
      "  .Cylinders   = %p%p\n"
      "  .MediaType   = %i\n"
      "  .T/C         = %u\n"
      "  .S/T         = %u\n"
      "  .B/S         = %u\n"
      "Flags          = %#x\n"
      "FileNameLength = %u\n"
      "FileName       = '%.*ws'\n"
      "DriveLetter    = %wc\n",
      CreateData->DeviceNumber,
      CreateData->DiskGeometry.Cylinders.HighPart,
      CreateData->DiskGeometry.Cylinders.LowPart,
      CreateData->DiskGeometry.MediaType,
      CreateData->DiskGeometry.TracksPerCylinder,
      CreateData->DiskGeometry.SectorsPerTrack,
      CreateData->DiskGeometry.BytesPerSector,
      CreateData->Flags,
      CreateData->FileNameLength,
      (int)(CreateData->FileNameLength / sizeof(*CreateData->FileName)),
      CreateData->FileName,
      CreateData->DriveLetter));

  // Auto-select type if not specified.
  if (IMDISK_TYPE(CreateData->Flags) == 0)
    if (CreateData->FileNameLength == 0)
      CreateData->Flags |= IMDISK_TYPE_VM;
    else
      CreateData->Flags |= IMDISK_TYPE_FILE;

  // Blank filenames only supported for non-zero VM disks.
  if (((CreateData->FileNameLength == 0) &
       (IMDISK_TYPE(CreateData->Flags) != IMDISK_TYPE_VM)) |
      ((CreateData->FileNameLength == 0) &
       (IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_VM) &
       (CreateData->DiskGeometry.Cylinders.QuadPart == 0)))
    {
      KdPrint(("ImDisk: Blank filenames only supported for non-zero length "
	       "vm type disks.\n"));

      return STATUS_INVALID_PARAMETER;
    }

  // Cannot create >= 2 GB VM disk in 32 bit version.
  if ((IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_VM) &
      ((CreateData->DiskGeometry.Cylinders.QuadPart & 0xFFFFFFFF80000000) !=
       0))
    {
      KdPrint(("ImDisk: Cannot create >= 2GB vm disks on 32-bit system.\n"));

      return STATUS_INVALID_PARAMETER;
    }

  // Auto-find first free device number
  if ((CreateData->DeviceNumber == IMDISK_AUTO_DEVICE_NUMBER) |
      (CreateData->DeviceNumber & 0xFFFFFFE0))
    {
      for (CreateData->DeviceNumber = 0;
	   CreateData->DeviceNumber < MaxDevices;
	   CreateData->DeviceNumber++)
	if ((~DeviceList) & (1 << CreateData->DeviceNumber))
	  break;

      if (CreateData->DeviceNumber >= MaxDevices)
	return STATUS_INVALID_PARAMETER;
    }

  file_name.Length = CreateData->FileNameLength;
  file_name.MaximumLength = CreateData->FileNameLength;
  file_name.Buffer = NULL;

  // If a file is to be opened or created, allocated name buffer and open that
  // file...
  if (CreateData->FileNameLength > 0)
    {
      IO_STATUS_BLOCK io_status;
      OBJECT_ATTRIBUTES object_attributes;
      UNICODE_STRING real_file_name;

      file_name.Buffer = ExAllocatePool(NonPagedPool,
					file_name.MaximumLength);

      if (file_name.Buffer == NULL)
	{
	  KdPrint(("ImDisk: Error allocating buffer for filename.\n"));
	  return STATUS_INSUFFICIENT_RESOURCES;
	}

      RtlCopyMemory(file_name.Buffer, CreateData->FileName,
		    CreateData->FileNameLength);
      // If no device-type specified, check if filename ends with .iso, .nrg or
      // .bin. In that case, set device-type automatically to FILE_DEVICE_CDROM
      if ((IMDISK_DEVICE_TYPE(CreateData->Flags) == 0) &
	  (CreateData->FileNameLength >= (4 * sizeof(*CreateData->FileName))))
	{
	  LPWSTR name = CreateData->FileName +
	    (CreateData->FileNameLength / sizeof(*CreateData->FileName)) - 4;
	  if ((_wcsnicmp(name, L".iso", 4) == 0) |
	      (_wcsnicmp(name, L".nrg", 4) == 0) |
	      (_wcsnicmp(name, L".bin", 4) == 0))
	    CreateData->Flags |= IMDISK_DEVICE_TYPE_CD | IMDISK_OPTION_RO;
	}

      if (IMDISK_DEVICE_TYPE(CreateData->Flags) == IMDISK_DEVICE_TYPE_CD)
	CreateData->Flags |= IMDISK_OPTION_RO;

      KdPrint(("ImDisk: Done with device type auto-selection by file ext.\n"));

      if (ClientThread != NULL)
	{
	  SECURITY_QUALITY_OF_SERVICE security_quality_of_service;
	  SECURITY_CLIENT_CONTEXT security_client_context;

	  RtlZeroMemory(&security_quality_of_service,
			sizeof(SECURITY_QUALITY_OF_SERVICE));

	  security_quality_of_service.Length =
	    sizeof(SECURITY_QUALITY_OF_SERVICE);
	  security_quality_of_service.ImpersonationLevel =
	    SecurityImpersonation;
	  security_quality_of_service.ContextTrackingMode =
	    SECURITY_STATIC_TRACKING;
	  security_quality_of_service.EffectiveOnly = FALSE;

	  SeCreateClientSecurity(ClientThread,
				 &security_quality_of_service,
				 FALSE,
				 &security_client_context);

	  SeImpersonateClient(&security_client_context, NULL);

	  SeDeleteClientSecurity(&security_client_context);
	}
      else
	KdPrint(("ImDisk: No impersonation information.\n"));

      if ((IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_PROXY) &
	  (IMDISK_PROXY_TYPE(CreateData->Flags) != IMDISK_PROXY_TYPE_DIRECT))
	RtlInitUnicodeString(&real_file_name, IMDPROXY_SVC_PIPE_NATIVE_NAME);
      else
	real_file_name = file_name;

      InitializeObjectAttributes(&object_attributes,
				 &real_file_name,
				 OBJ_CASE_INSENSITIVE, NULL, NULL);

      KdPrint(("ImDisk: Passing WriteMode=%#x and WriteShare=%#x\n",
	       (IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_PROXY) |
	       !((IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_VM) |
		 IMDISK_READONLY(CreateData->Flags)),
	       IMDISK_READONLY(CreateData->Flags) |
	       (IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_VM)));

      status =
	ZwCreateFile(&file_handle,
		     GENERIC_READ |
		     ((IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_PROXY) |
		      !((IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_VM) |
			IMDISK_READONLY(CreateData->Flags)) ?
		      GENERIC_WRITE : 0),
		     &object_attributes,
		     &io_status,
		     NULL,
		     FILE_ATTRIBUTE_NORMAL,
		     FILE_SHARE_READ |
		     FILE_SHARE_DELETE |
		     (IMDISK_READONLY(CreateData->Flags) |
		      (IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_VM) ?
		      FILE_SHARE_WRITE : 0),
		     FILE_OPEN,
		     IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_PROXY ?
		     FILE_NON_DIRECTORY_FILE |
		     FILE_SEQUENTIAL_ONLY |
		     FILE_NO_INTERMEDIATE_BUFFERING |
		     FILE_SYNCHRONOUS_IO_NONALERT :
		     FILE_NON_DIRECTORY_FILE |
		     FILE_RANDOM_ACCESS |
		     FILE_NO_INTERMEDIATE_BUFFERING |
		     FILE_SYNCHRONOUS_IO_NONALERT,
		     NULL,
		     0);

      // If not found we will create the file if a new non-zero size is
      // specified, read-only virtual disk is not specified and we are
      // creating a type 'file' virtual disk.
      if ((status == STATUS_OBJECT_NAME_NOT_FOUND) |
	  (status == STATUS_NO_SUCH_FILE))
	{
	  if ((CreateData->DiskGeometry.Cylinders.QuadPart == 0) |
	      IMDISK_READONLY(CreateData->Flags) |
	      (IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_VM))
	    {
	      ExFreePool(file_name.Buffer);

	      KdPrint(("ImDisk: File '%.*ws' not found. (%#x)\n",
		       (int) real_file_name.Length / sizeof(WCHAR),
		       real_file_name.Buffer,
		       status));

	      return status;
	    }

	  status =
	    ZwCreateFile(&file_handle,
			 GENERIC_READ |
			 GENERIC_WRITE,
			 &object_attributes,
			 &io_status,
			 &CreateData->DiskGeometry.Cylinders,
			 FILE_ATTRIBUTE_NORMAL,
			 FILE_SHARE_READ |
			 FILE_SHARE_DELETE,
			 FILE_OPEN_IF,
			 FILE_NON_DIRECTORY_FILE |
			 FILE_RANDOM_ACCESS |
			 FILE_NO_INTERMEDIATE_BUFFERING |
			 FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
      
	  if (!NT_SUCCESS(status))
	    {
	      ExFreePool(file_name.Buffer);

	      KdPrint(("ImDisk: Cannot create '%.*ws'. (%#x)\n",
		       (int)(CreateData->FileNameLength /
			     sizeof(*CreateData->FileName)),
		       CreateData->FileName,
		       status));
	      
	      return status;
	    }
	}
      else if (!NT_SUCCESS(status))
	{
	  ExFreePool(file_name.Buffer);
	  
	  KdPrint(("ImDisk: Cannot open file '%.*ws'. Status: %#x\n",
		   (int)(real_file_name.Length / sizeof(WCHAR)),
		   real_file_name.Buffer,
		   status));

	  return status;
	}

      KdPrint(("ImDisk: File '%.*ws' opened successfully.\n",
	       (int)(real_file_name.Length / sizeof(WCHAR)),
	       real_file_name.Buffer));

      // Adjust the file length to the requested virtual disk size.
      if ((CreateData->DiskGeometry.Cylinders.QuadPart != 0) &
	  (IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_FILE) &
	  (!IMDISK_READONLY(CreateData->Flags)) &
	  (CreateData->ImageOffset.QuadPart == 0))
	{
	  status = ZwSetInformationFile(file_handle,
					&io_status,
					&CreateData->DiskGeometry.Cylinders,
					sizeof
					(FILE_END_OF_FILE_INFORMATION),
					FileEndOfFileInformation);

	  if (!NT_SUCCESS(status))
	    {
	      ZwClose(file_handle);
	      ExFreePool(file_name.Buffer);
	      KdPrint(("ImDisk: Error setting eof (%#x).\n", status));
	      return status;
	    }
	}

      // Get the file size of the disk file.
      if (IMDISK_TYPE(CreateData->Flags) != IMDISK_TYPE_PROXY)
	{
	  FILE_STANDARD_INFORMATION file_standard;

	  status = ZwQueryInformationFile(file_handle,
					  &io_status,
					  &file_standard,
					  sizeof(FILE_STANDARD_INFORMATION),
					  FileStandardInformation);

	  if (!NT_SUCCESS(status))
	    {
	      ZwClose(file_handle);
	      ExFreePool(file_name.Buffer);

	      KdPrint
		(("ImDisk: Error getting FILE_STANDARD_INFORMATION (%#x).\n",
		  status));

	      return status;
	    }

	  // Allocate virtual memory for 'vm' type.
	  if (IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_VM)
	    {
	      ULONG max_size;

	      // Check that file size < 2 GB.
	      if (CreateData->DiskGeometry.Cylinders.QuadPart == 0)
		if ((file_standard.EndOfFile.QuadPart -
		     CreateData->ImageOffset.QuadPart) & 0xFFFFFFFF80000000)
		  {
		    ZwClose(file_handle);
		    ExFreePool(file_name.Buffer);

		    KdPrint(("ImDisk: VM disk >= 2GB not supported.\n"));

		    return STATUS_INSUFFICIENT_RESOURCES;
		  }
		else
		  CreateData->DiskGeometry.Cylinders.QuadPart =
		    file_standard.EndOfFile.QuadPart -
		    CreateData->ImageOffset.QuadPart;

	      max_size = CreateData->DiskGeometry.Cylinders.LowPart;
	      status =
		ZwAllocateVirtualMemory(NtCurrentProcess(),
					&image_buffer,
					0,
					&max_size,
					MEM_COMMIT,
					PAGE_READWRITE);
	      if (!NT_SUCCESS(status))
		{
		  ZwClose(file_handle);
		  ExFreePool(file_name.Buffer);

		  KdPrint(("ImDisk: Error allocating vm for image. (%#x)\n",
			   status));
		  
		  return STATUS_NO_MEMORY;
		}

	      alignment_requirement = FILE_BYTE_ALIGNMENT;

	      // Loading of image file has been moved to be done just before
	      // the service loop.
	    }
	  else
	    {
	      FILE_ALIGNMENT_INFORMATION file_alignment;

	      status = ZwQueryInformationFile(file_handle,
					      &io_status,
					      &file_alignment,
					      sizeof
					      (FILE_ALIGNMENT_INFORMATION),
					      FileAlignmentInformation);
	  
	      if (!NT_SUCCESS(status))
		{
		  ZwClose(file_handle);
		  ExFreePool(file_name.Buffer);

		  KdPrint(("ImDisk: Error querying file alignment (%#x).\n",
			   status));

		  return status;
		}

	      if (CreateData->DiskGeometry.Cylinders.QuadPart == 0)
		CreateData->DiskGeometry.Cylinders.QuadPart =
		  file_standard.EndOfFile.QuadPart -
		  CreateData->ImageOffset.QuadPart;

	      alignment_requirement = file_alignment.AlignmentRequirement;
	    }
	}
      else
	// If proxy is used, get the image file size from the proxy instead.
	{
	  IMDPROXY_INFO_RESP proxy_info;

	  status =
	    ObReferenceObjectByHandle(file_handle,
				      FILE_READ_ATTRIBUTES |
				      FILE_READ_DATA |
				      FILE_WRITE_DATA,
				      *IoFileObjectType,
				      KernelMode,
				      &proxy_device,
				      NULL);

	  if (!NT_SUCCESS(status))
	    {
	      ZwClose(file_handle);
	      ExFreePool(file_name.Buffer);

	      KdPrint(("ImDisk: Error referencing proxy device (%#x).\n",
		       status));

	      return status;
	    }

	  KdPrint(("ImDisk: Got reference to proxy object %#x.\n",
		   proxy_device));

	  if (IMDISK_PROXY_TYPE(CreateData->Flags) != IMDISK_PROXY_TYPE_DIRECT)
	    status = ImDiskConnectProxy(&proxy_device,
					&io_status,
					CreateData->Flags,
					CreateData->FileName,
					CreateData->FileNameLength);

	  if (!NT_SUCCESS(status))
	    {
	      ObDereferenceObject(proxy_device);
	      ZwClose(file_handle);
	      ExFreePool(file_name.Buffer);

	      KdPrint(("ImDisk: Error connecting proxy (%#x).\n", status));

	      return status;
	    }

	  status = ImDiskQueryInformationProxy(proxy_device,
					       &io_status,
					       &proxy_info,
					       sizeof(IMDPROXY_INFO_RESP));

	  if (!NT_SUCCESS(status))
	    {
	      ObDereferenceObject(proxy_device);
	      ZwClose(file_handle);
	      ExFreePool(file_name.Buffer);

	      KdPrint(("ImDisk: Error querying proxy (%#x).\n", status));

	      return status;
	    }

	  if (CreateData->DiskGeometry.Cylinders.QuadPart == 0)
	    CreateData->DiskGeometry.Cylinders.QuadPart = proxy_info.file_size;

	  if ((proxy_info.req_alignment - 1 > FILE_512_BYTE_ALIGNMENT) |
	      (CreateData->DiskGeometry.Cylinders.QuadPart == 0))
	    {
	      ObDereferenceObject(proxy_device);
	      ZwClose(file_handle);
	      ExFreePool(file_name.Buffer);

	      KdPrint(("ImDisk: Unsupported sizes. "
		       "Got %p%p size and %p%p alignment.\n",
		       proxy_info.file_size,
		       proxy_info.req_alignment));

	      return STATUS_INVALID_PARAMETER;
	    }

	  alignment_requirement = (ULONG) proxy_info.req_alignment - 1;

	  if (proxy_info.flags & IMDPROXY_FLAG_RO)
	    CreateData->Flags |= IMDISK_OPTION_RO;

	  KdPrint(("ImDisk: Got from proxy: Siz=%p%p Flg=%#x Alg=%#x.\n",
		   CreateData->DiskGeometry.Cylinders.HighPart,
		   CreateData->DiskGeometry.Cylinders.LowPart,
		   (ULONG) proxy_info.flags,
		   (ULONG) proxy_info.req_alignment));
	}

      if (CreateData->DiskGeometry.Cylinders.QuadPart == 0)
	{
	  ULONG free_size = 0;
      
	  KdPrint(("ImDisk: Fatal error: Number of cylinders equals zero.\n"));

	  if (proxy_device != NULL)
	    ObDereferenceObject(proxy_device);
	  if (file_handle != NULL)
	    ZwClose(file_handle);
	  if (file_name.Buffer != NULL)
	    ExFreePool(file_name.Buffer);
	  if (image_buffer != NULL)
	    ZwFreeVirtualMemory(NtCurrentProcess(),
				&image_buffer,
				&free_size, MEM_RELEASE);

	  return STATUS_INVALID_PARAMETER;
	}
    }
  // Blank vm-disk, just allocate...
  else
    {
      ULONG max_size;
      max_size = CreateData->DiskGeometry.Cylinders.LowPart;

      image_buffer = NULL;
      status =
	ZwAllocateVirtualMemory(NtCurrentProcess(),
				&image_buffer,
				0,
				&max_size,
				MEM_COMMIT,
				PAGE_READWRITE);
      if (!NT_SUCCESS(status))
	{
	  KdPrint
	    (("ImDisk: Error allocating virtual memory for vm disk (%#x).\n",
	      status));

	  return STATUS_NO_MEMORY;
	}

      alignment_requirement = FILE_BYTE_ALIGNMENT;
    }

  KdPrint(("ImDisk: Done with file/memory checks.\n"));

  // If no device-type specified and size matches common floppy sizes,
  // auto-select FILE_DEVICE_DISK with FILE_FLOPPY_DISKETTE and
  // FILE_REMOVABLE_MEDIA.
  // If still no device-type specified, specify FILE_DEVICE_DISK with no
  // particular characteristics. This will emulate a hard disk partition.
  if (IMDISK_DEVICE_TYPE(CreateData->Flags) == 0)
    switch (CreateData->DiskGeometry.Cylinders.QuadPart)
      {
      case MEDIA_SIZE_240MB:
      case MEDIA_SIZE_120MB:
      case MEDIA_SIZE_2800KB:
      case MEDIA_SIZE_1722KB:
      case MEDIA_SIZE_1680KB:
      case MEDIA_SIZE_1440KB:
      case MEDIA_SIZE_820KB:
      case MEDIA_SIZE_720KB:
      case MEDIA_SIZE_1200KB:
      case MEDIA_SIZE_640KB:
      case MEDIA_SIZE_360KB:
      case MEDIA_SIZE_320KB:
      case MEDIA_SIZE_180KB:
      case MEDIA_SIZE_160KB:
	CreateData->Flags |= IMDISK_DEVICE_TYPE_FD;
	break;

      default:
	CreateData->Flags |= IMDISK_DEVICE_TYPE_HD;
      }
	  
  KdPrint(("ImDisk: Done with device type selection for floppy sizes.\n"));

  // If some parts of the DISK_GEOMETRY structure are zero, auto-fill with
  // typical values for this type of disk.
  if (IMDISK_DEVICE_TYPE(CreateData->Flags) == IMDISK_DEVICE_TYPE_CD)
    {
      LONGLONG calccyl = CreateData->DiskGeometry.Cylinders.QuadPart;

      if (CreateData->DiskGeometry.BytesPerSector == 0)
	CreateData->DiskGeometry.BytesPerSector = SECTOR_SIZE_CD_ROM;

      calccyl /= CreateData->DiskGeometry.BytesPerSector;

      if (CreateData->DiskGeometry.SectorsPerTrack == 0)
	{
	  if (calccyl / SECTORS_PER_TRACK_CD_ROM * SECTORS_PER_TRACK_CD_ROM ==
	      calccyl)
	    CreateData->DiskGeometry.SectorsPerTrack =
	      SECTORS_PER_TRACK_CD_ROM;
	  else
	    CreateData->DiskGeometry.SectorsPerTrack = 1;
	}

      calccyl /= CreateData->DiskGeometry.SectorsPerTrack;

      if (CreateData->DiskGeometry.TracksPerCylinder == 0)
	{
	  if (calccyl /
	      TRACKS_PER_CYLINDER_CD_ROM * TRACKS_PER_CYLINDER_CD_ROM ==
	      calccyl)
	    CreateData->DiskGeometry.TracksPerCylinder =
	      TRACKS_PER_CYLINDER_CD_ROM;
	  else
	    CreateData->DiskGeometry.TracksPerCylinder = 1;
	}

      if (CreateData->DiskGeometry.MediaType == Unknown)
	CreateData->DiskGeometry.MediaType = RemovableMedia;
    }
  // Common floppy sizes geometries.
  else
    {
      LONGLONG calccyl = CreateData->DiskGeometry.Cylinders.QuadPart;

      if ((IMDISK_DEVICE_TYPE(CreateData->Flags) == IMDISK_DEVICE_TYPE_FD) &
	  (CreateData->DiskGeometry.BytesPerSector == 0) &
	  (CreateData->DiskGeometry.SectorsPerTrack == 0) &
	  (CreateData->DiskGeometry.TracksPerCylinder == 0) &
	  (CreateData->DiskGeometry.MediaType == Unknown))
	switch (calccyl)
	  {
	    // 3.5" formats
	  case MEDIA_SIZE_240MB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_240M];
	    break;

	  case MEDIA_SIZE_120MB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_120M];
	    break;

	  case MEDIA_SIZE_2800KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_2880K];
	    break;

	  case MEDIA_SIZE_1722KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_1722K];
	    break;

	  case MEDIA_SIZE_1680KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_1680K];
	    break;

	  case MEDIA_SIZE_1440KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_1440K];
	    break;

	  case MEDIA_SIZE_820KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_820K];
	    break;

	  case MEDIA_SIZE_720KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_720K];
	    break;

	    // 5.25" formats
	  case MEDIA_SIZE_1200KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_1200K];
	    break;

	  case MEDIA_SIZE_640KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_640K];
	    break;

	  case MEDIA_SIZE_360KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_360K];
	    break;

	  case MEDIA_SIZE_320KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_320K];
	    break;

	  case MEDIA_SIZE_180KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_180K];
	    break;

	  case MEDIA_SIZE_160KB:
	    CreateData->DiskGeometry = media_table[MEDIA_TYPE_160K];
	    break;
	  }

      // In this case the Cylinders member actually specifies the total size of
      // the virtual disk so restore that in case overwritten by the pre-
      // defined floppy geometries above.
      CreateData->DiskGeometry.Cylinders.QuadPart = calccyl;

      if (CreateData->DiskGeometry.BytesPerSector == 0)
	CreateData->DiskGeometry.BytesPerSector = SECTOR_SIZE_HDD;

      calccyl /= CreateData->DiskGeometry.BytesPerSector;

      if (CreateData->DiskGeometry.SectorsPerTrack == 0)
	{
	  CreateData->DiskGeometry.SectorsPerTrack = 1;

	  if ((calccyl / 7 * 7 == calccyl) &
	      (CreateData->DiskGeometry.SectorsPerTrack * 7 < 64))
	    {
	      CreateData->DiskGeometry.SectorsPerTrack *= 7;
	      calccyl /= 7;
	    }

	  if ((calccyl / 3 * 3 == calccyl) &
	      (CreateData->DiskGeometry.SectorsPerTrack * 3 < 64))
	    {
	      CreateData->DiskGeometry.SectorsPerTrack *= 3;
	      calccyl /= 3;
	    }

	  if ((calccyl / 3 * 3 == calccyl) &
	      (CreateData->DiskGeometry.SectorsPerTrack * 3 < 64))
	    {
	      CreateData->DiskGeometry.SectorsPerTrack *= 3;
	      calccyl /= 3;
	    }

	  while (((calccyl & 1) == 0) &
		 (CreateData->DiskGeometry.SectorsPerTrack <= 16))
	    {
	      CreateData->DiskGeometry.SectorsPerTrack <<= 1;
	      calccyl >>= 1;
	    }
	}
      else
	calccyl /= CreateData->DiskGeometry.SectorsPerTrack;

      if (CreateData->DiskGeometry.TracksPerCylinder == 0)
	{
	  CreateData->DiskGeometry.TracksPerCylinder = 1;

	  if (calccyl / 17 * 17 == calccyl)
	    {
	      CreateData->DiskGeometry.TracksPerCylinder *= 17;
	      calccyl /= 17;
	    }

	  if (calccyl / 5 * 5 == calccyl)
	    {
	      CreateData->DiskGeometry.TracksPerCylinder *= 5;
	      calccyl /= 5;
	    }

	  if (calccyl / 3 * 3 == calccyl)
	    {
	      CreateData->DiskGeometry.TracksPerCylinder *= 3;
	      calccyl /= 3;
	    }

	  while (((calccyl & 1) == 0) &
		 (CreateData->DiskGeometry.TracksPerCylinder <= 64))
	    {
	      CreateData->DiskGeometry.TracksPerCylinder <<= 1;
	      calccyl >>= 1;
	    }
	}

      if (CreateData->DiskGeometry.MediaType == Unknown)
	CreateData->DiskGeometry.MediaType = FixedMedia;
    }

  KdPrint(("ImDisk: Done with disk geometry setup.\n"));

  // Ensure upper-case driveletter.
  CreateData->DriveLetter &= ~0x20;

  // Now build real DeviceType and DeviceCharacteristics parameters.
  if (IMDISK_DEVICE_TYPE(CreateData->Flags) == IMDISK_DEVICE_TYPE_CD)
    {
      device_type = FILE_DEVICE_CD_ROM;
      device_characteristics = FILE_READ_ONLY_DEVICE | FILE_REMOVABLE_MEDIA;
    }
  else
    {
      device_type = FILE_DEVICE_DISK;

      if (IMDISK_DEVICE_TYPE(CreateData->Flags) == IMDISK_DEVICE_TYPE_FD)
	device_characteristics = FILE_FLOPPY_DISKETTE | FILE_REMOVABLE_MEDIA;
      else
	device_characteristics = 0;
    }

  if (IMDISK_REMOVABLE(CreateData->Flags))
    device_characteristics |= FILE_REMOVABLE_MEDIA;

  if (IMDISK_READONLY(CreateData->Flags))
    device_characteristics |= FILE_READ_ONLY_DEVICE;

  KdPrint
    (("ImDisk: After checks and translations we got this create data:\n"
      "DeviceNumber   = %#x\n"
      "DiskGeometry\n"
      "  .Cylinders   = %p%p\n"
      "  .MediaType   = %i\n"
      "  .T/C         = %u\n"
      "  .S/T         = %u\n"
      "  .B/S         = %u\n"
      "Flags          = %#x\n"
      "FileNameLength = %u\n"
      "FileName       = '%.*ws'\n"
      "DriveLetter    = %wc\n",
      CreateData->DeviceNumber,
      CreateData->DiskGeometry.Cylinders.HighPart,
      CreateData->DiskGeometry.Cylinders.LowPart,
      CreateData->DiskGeometry.MediaType,
      CreateData->DiskGeometry.TracksPerCylinder,
      CreateData->DiskGeometry.SectorsPerTrack,
      CreateData->DiskGeometry.BytesPerSector,
      CreateData->Flags,
      CreateData->FileNameLength,
      (int)(CreateData->FileNameLength / sizeof(*CreateData->FileName)),
      CreateData->FileName,
      CreateData->DriveLetter));

  // Buffer for device name
  device_name_buffer = ExAllocatePool(PagedPool,
				      MAXIMUM_FILENAME_LENGTH *
				      sizeof(*device_name_buffer));

  if (device_name_buffer == NULL)
    {
      ULONG free_size = 0;
      if (proxy_device != NULL)
	ObDereferenceObject(proxy_device);
      if (file_handle != NULL)
	ZwClose(file_handle);
      if (file_name.Buffer != NULL)
	ExFreePool(file_name.Buffer);
      if (image_buffer != NULL)
	ZwFreeVirtualMemory(NtCurrentProcess(),
			    &image_buffer,
			    &free_size, MEM_RELEASE);

      return STATUS_INSUFFICIENT_RESOURCES;
    }

  _snwprintf(device_name_buffer, MAXIMUM_FILENAME_LENGTH - 1,
	     IMDISK_DEVICE_BASE_NAME L"%u", CreateData->DeviceNumber);
  device_name_buffer[MAXIMUM_FILENAME_LENGTH - 1] = 0;

  KdPrint
    (("ImDisk: Creating device '%ws'. Device type %#x, characteristics %#x.\n",
      device_name_buffer, device_type, device_characteristics));

  RtlInitUnicodeString(&device_name, device_name_buffer);

  status = IoCreateDevice(DriverObject,
			  sizeof(DEVICE_EXTENSION),
			  &device_name,
			  device_type,
			  device_characteristics,
			  FALSE,
			  DeviceObject);

  if (!NT_SUCCESS(status))
    {
      ULONG free_size = 0;

      ExFreePool(device_name_buffer);
      if (proxy_device != NULL)
	ObDereferenceObject(proxy_device);
      if (file_handle != NULL)
	ZwClose(file_handle);
      if (file_name.Buffer != NULL)
	ExFreePool(file_name.Buffer);
      if (image_buffer != NULL)
	ZwFreeVirtualMemory(NtCurrentProcess(),
			    &image_buffer,
			    &free_size, MEM_RELEASE);

      KdPrint(("ImDisk: Cannot create device. (%#x)\n", status));

      return status;
    }

  KdPrint
    (("ImDisk: Setting the AlignmentRequirement field to %#x.",
      alignment_requirement));

  (*DeviceObject)->Flags |= DO_DIRECT_IO;
  (*DeviceObject)->AlignmentRequirement = alignment_requirement;

  device_extension = (PDEVICE_EXTENSION) (*DeviceObject)->DeviceExtension;

  // Auto-set our own read-only flag if the characteristics of the device
  // object is set to read-only.
  if ((*DeviceObject)->Characteristics & FILE_READ_ONLY_DEVICE)
    device_extension->read_only = TRUE;

  InitializeListHead(&device_extension->list_head);

  KeInitializeSpinLock(&device_extension->list_lock);

  KeInitializeEvent(&device_extension->request_event,
		    SynchronizationEvent, FALSE);

  device_extension->terminate_thread = FALSE;
  device_extension->device_number = CreateData->DeviceNumber;

  DeviceList |= 1 << CreateData->DeviceNumber;

  device_extension->file_name = file_name;

  device_extension->disk_geometry = CreateData->DiskGeometry;

  device_extension->image_offset = CreateData->ImageOffset;

  // VM disk.
  if (IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_VM)
    device_extension->vm_disk = TRUE;
  else
    device_extension->vm_disk = FALSE;

  device_extension->image_buffer = image_buffer;
  device_extension->file_handle = file_handle;

  // Use proxy service.
  if (IMDISK_TYPE(CreateData->Flags) == IMDISK_TYPE_PROXY)
    {
      device_extension->proxy_device = proxy_device;
      device_extension->use_proxy = TRUE;
    }
  else
    device_extension->use_proxy = FALSE;

  if (((*DeviceObject)->DeviceType == FILE_DEVICE_CD_ROM) |
      IMDISK_READONLY(CreateData->Flags))
    device_extension->read_only = TRUE;
  else
    device_extension->read_only = FALSE;
  
  if (device_extension->read_only)
    (*DeviceObject)->Characteristics |= FILE_READ_ONLY_DEVICE;
  else
    (*DeviceObject)->Characteristics &= ~FILE_READ_ONLY_DEVICE;

  device_extension->media_change_count++;

  if (CreateData->DriveLetter != 0)
    {
      WCHAR sym_link_global_wchar[] = L"\\DosDevices\\Global\\ :";
      UNICODE_STRING sym_link;

      sym_link_global_wchar[19] = CreateData->DriveLetter;

      KdPrint(("ImDisk: Creating symlink '%ws' -> '%ws'.\n",
	       sym_link_global_wchar, device_name_buffer));

      RtlInitUnicodeString(&sym_link, sym_link_global_wchar);
      status = IoCreateUnprotectedSymbolicLink(&sym_link, &device_name);

      if (!NT_SUCCESS(status))
	{
	  KdPrint(("ImDisk: Cannot symlink '%ws' to '%ws'. (%#x)\n",
		   sym_link_global_wchar, device_name_buffer, status));
	}

      device_extension->drive_letter = CreateData->DriveLetter;
    }

  device_extension->device_thread = KeGetCurrentThread();

  (*DeviceObject)->Flags &= ~DO_DEVICE_INITIALIZING;

  KdPrint(("ImDisk: Device '%ws' created.\n", device_name_buffer));

  ExFreePool(device_name_buffer);

  return STATUS_SUCCESS;
}

VOID
ImDiskUnload(IN PDRIVER_OBJECT DriverObject)
{
  PDEVICE_OBJECT device_object;

  PAGED_CODE();

  device_object = DriverObject->DeviceObject;

  KdPrint(("ImDisk: Entering ImDiskUnload for driver %#x. "
	   "Current device objects chain dump for this driver:\n",
	   DriverObject));

  while (device_object != NULL)
    {
      KdPrint(("%#x -> ", device_object));
      device_object = device_object->NextDevice;
    }

  KdPrint(("(null)\n"));

  device_object = DriverObject->DeviceObject;

  for (;;)
    {
      PDEVICE_OBJECT next_device;
      PDEVICE_EXTENSION device_extension;

      if (device_object == NULL)
	{
	  KdPrint
	    (("ImDisk: No more devices to delete. Leaving ImDiskUnload.\n"));
	  return;
	}
  
      next_device = device_object->NextDevice;
      device_extension = (PDEVICE_EXTENSION) device_object->DeviceExtension;

      KdPrint(("ImDisk: Now deleting device %i.\n",
	       device_extension->device_number));

      if (device_object == ImDiskCtlDevice)
	{
	  UNICODE_STRING sym_link;
	  LARGE_INTEGER time_out;
	  time_out.QuadPart = -1000000;

	  while (device_object->ReferenceCount != 0)
	    {
	      KdPrint(("ImDisk: Ctl device is busy. Waiting.\n"));

	      KeDelayExecutionThread(KernelMode, FALSE, &time_out);

	      time_out.LowPart <<= 2;
	    }

	  KdPrint(("ImDisk: Deleting ctl device.\n"));
	  RtlInitUnicodeString(&sym_link, IMDISK_CTL_SYMLINK_NAME);
	  IoDeleteSymbolicLink(&sym_link);
	  IoDeleteDevice(device_object);
	}
      else
	{
	  PKTHREAD device_thread;

	  KdPrint(("ImDisk: Shutting down device %i.\n",
		   device_extension->device_number));

	  device_thread = device_extension->device_thread;
	  ObReferenceObjectByPointer(device_thread, SYNCHRONIZE, NULL,
				     KernelMode);

	  ImDiskRemoveVirtualDisk(device_object);

	  KdPrint(("ImDisk: Waiting for device thread %i to terminate.\n",
		   device_extension->device_number));

	  KeWaitForSingleObject(device_thread,
				Executive,
				KernelMode,
				FALSE,
				NULL);

	  ObDereferenceObject(device_thread);
	}

      device_object = next_device;
    }
}

#pragma code_seg()

VOID
ImDiskRemoveVirtualDisk(IN PDEVICE_OBJECT DeviceObject)
{
  PDEVICE_EXTENSION device_extension;

  ASSERT(DeviceObject != NULL);

  device_extension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

  KdPrint(("ImDisk: Request to shutdown device %i.\n",
	   device_extension->device_number));

  device_extension->terminate_thread = TRUE;

  KeSetEvent(&device_extension->request_event, (KPRIORITY) 0, FALSE);
}

NTSTATUS
ImDiskCreateClose(IN PDEVICE_OBJECT DeviceObject,
		  IN PIRP Irp)
{
  PIO_STACK_LOCATION io_stack;
  PDEVICE_EXTENSION device_extension;

  ASSERT(DeviceObject != NULL);
  ASSERT(Irp != NULL);

  KdPrint(("ImDisk: Entering ImDiskCreateClose.\n"));

  io_stack = IoGetCurrentIrpStackLocation(Irp);
  device_extension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

  if (io_stack->FileObject->FileName.Length != 0)
    {
      KdPrint(("ImDisk: Attempt to open '%.*ws' on device %i.\n",
	       (int)(io_stack->FileObject->FileName.Length / sizeof(WCHAR)),
	       io_stack->FileObject->FileName.Buffer,
	       device_extension->device_number));

      Irp->IoStatus.Status = STATUS_OBJECT_NAME_NOT_FOUND;
      Irp->IoStatus.Information = 0;

      IoCompleteRequest(Irp, IO_NO_INCREMENT);

      return Irp->IoStatus.Status;
    }

  if ((io_stack->MajorFunction == IRP_MJ_CREATE) &
      (device_extension->terminate_thread == TRUE))
    {
      KdPrint(("ImDisk: Attempt to open device %i when shut down.\n",
	       device_extension->device_number));

      Irp->IoStatus.Status = STATUS_DELETE_PENDING;
      Irp->IoStatus.Information = 0;

      IoCompleteRequest(Irp, IO_NO_INCREMENT);

      return Irp->IoStatus.Status;
    }

  KdPrint(("ImDisk: Successfully created/closed a handle for device %i.\n",
	   device_extension->device_number));

  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = FILE_OPENED;

  IoCompleteRequest(Irp, IO_NO_INCREMENT);

  return STATUS_SUCCESS;
}

NTSTATUS
ImDiskReadWrite(IN PDEVICE_OBJECT DeviceObject,
		IN PIRP Irp)
{
  PDEVICE_EXTENSION device_extension;
  PIO_STACK_LOCATION io_stack;

  ASSERT(DeviceObject != NULL);
  ASSERT(Irp != NULL);

  device_extension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

  if (DeviceObject == ImDiskCtlDevice)
    {
      KdPrint(("ImDisk: Read/write attempt on ctl device.\n"));

      Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
      Irp->IoStatus.Information = 0;

      IoCompleteRequest(Irp, IO_NO_INCREMENT);

      return Irp->IoStatus.Status;
    }

  if (device_extension->terminate_thread)
    {
      KdPrint(("ImDisk: Read/write attempt on device %i with no media.\n",
	       device_extension->device_number));

      Irp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
      Irp->IoStatus.Information = 0;

      IoCompleteRequest(Irp, IO_NO_INCREMENT);

      return Irp->IoStatus.Status;
    }

  io_stack = IoGetCurrentIrpStackLocation(Irp);

  if ((io_stack->MajorFunction == IRP_MJ_WRITE) &&
      device_extension->read_only)
    {
      KdPrint(("ImDisk: Attempt to write to write-protected device %i.\n",
	       device_extension->device_number));

      Irp->IoStatus.Status = STATUS_MEDIA_WRITE_PROTECTED;
      Irp->IoStatus.Information = 0;

      IoCompleteRequest(Irp, IO_NO_INCREMENT);

      return STATUS_MEDIA_WRITE_PROTECTED;
    }

  if ((io_stack->Parameters.Read.ByteOffset.QuadPart +
       io_stack->Parameters.Read.Length) >
      (device_extension->disk_geometry.Cylinders.QuadPart))
    {
      KdPrint(("ImDisk: Read/write beyond eof on device %i.\n",
	       device_extension->device_number));

      Irp->IoStatus.Status = STATUS_SUCCESS;
      Irp->IoStatus.Information = 0;

      IoCompleteRequest(Irp, IO_NO_INCREMENT);

      return STATUS_SUCCESS;
    }

  if (io_stack->Parameters.Read.Length == 0)
    {
      KdPrint(("ImDisk: Read/write zero bytes on device %i.\n",
	       device_extension->device_number));

      Irp->IoStatus.Status = STATUS_SUCCESS;
      Irp->IoStatus.Information = 0;

      IoCompleteRequest(Irp, IO_NO_INCREMENT);

      return STATUS_SUCCESS;
    }

  KdPrint2(("ImDisk: Device %i got read/write request Offset=%p%p Len=%p.\n",
	    device_extension->device_number,
	    io_stack->Parameters.Read.ByteOffset.HighPart,
	    io_stack->Parameters.Read.ByteOffset.LowPart,
	    io_stack->Parameters.Read.Length));

  IoMarkIrpPending(Irp);

  ExInterlockedInsertTailList(&device_extension->list_head,
			      &Irp->Tail.Overlay.ListEntry,
			      &device_extension->list_lock);
  
  KeSetEvent(&device_extension->request_event, (KPRIORITY) 0, FALSE);

  return STATUS_PENDING;
}
  
NTSTATUS
ImDiskDeviceControl(IN PDEVICE_OBJECT DeviceObject,
		    IN PIRP Irp)
{
  PDEVICE_EXTENSION device_extension;
  PIO_STACK_LOCATION io_stack;
  NTSTATUS status;

  ASSERT(DeviceObject != NULL);
  ASSERT(Irp != NULL);

  device_extension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

  io_stack = IoGetCurrentIrpStackLocation(Irp);

  KdPrint(("ImDisk: Device %i received IOCTL %#x IRP %#x.\n",
	   device_extension->device_number,
	   io_stack->Parameters.DeviceIoControl.IoControlCode,
	   Irp));

  if (device_extension->terminate_thread)
    {
      KdPrint(("ImDisk: IOCTL attempt on device %i with no media.\n",
	       device_extension->device_number));

      Irp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
      Irp->IoStatus.Information = 0;

      IoCompleteRequest(Irp, IO_NO_INCREMENT);

      return Irp->IoStatus.Status;
    }

  // The control device can only receive version queries, enumeration queries
  // or device create requests.
  if (DeviceObject == ImDiskCtlDevice)
    switch (io_stack->Parameters.DeviceIoControl.IoControlCode)
      {
      case IOCTL_IMDISK_QUERY_VERSION:
      case IOCTL_IMDISK_CREATE_DEVICE:
      case IOCTL_IMDISK_QUERY_DRIVER:
      case IOCTL_IMDISK_REFERENCE_HANDLE:
	break;

      default:
	KdPrint(("ImDisk: Invalid IOCTL %#x for control device.\n",
		 io_stack->Parameters.DeviceIoControl.IoControlCode));
	
	Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
	Irp->IoStatus.Information = 0;

	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return Irp->IoStatus.Status;
      }
  else
    switch (io_stack->Parameters.DeviceIoControl.IoControlCode)
      {
	// Invalid IOCTL codes for this driver's disk devices.
      case IOCTL_IMDISK_CREATE_DEVICE:
      case IOCTL_IMDISK_QUERY_DRIVER:
      case IOCTL_IMDISK_REFERENCE_HANDLE:
	KdPrint(("ImDisk: Invalid IOCTL %#x for disk device.\n",
		 io_stack->Parameters.DeviceIoControl.IoControlCode));
	
	Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
	Irp->IoStatus.Information = 0;

	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return Irp->IoStatus.Status;

	// The only IOCTL codes available without mounted image file.
      case IOCTL_IMDISK_QUERY_VERSION:
      case IOCTL_DISK_EJECT_MEDIA:
      case IOCTL_STORAGE_EJECT_MEDIA:
	break;

      default:
	if (device_extension->terminate_thread)
	  {
	    KdPrint(("ImDisk: Invalid IOCTL %#x for device %i. No media.\n",
		     io_stack->Parameters.DeviceIoControl.IoControlCode,
		     device_extension->device_number));

	    Irp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
	    Irp->IoStatus.Information = 0;
	    
	    IoCompleteRequest(Irp, IO_NO_INCREMENT);

	    return Irp->IoStatus.Status;
	  }
      }
  
  switch (io_stack->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_IMDISK_SET_DEVICE_FLAGS:
      KdPrint(("ImDisk: IOCTL_IMDISK_SET_DEVICE_FLAGS for device %i.\n",
	       device_extension->device_number));

      if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
	{
	  status = STATUS_ACCESS_DENIED;
	  Irp->IoStatus.Information = 0;
	  break;
	}

      if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
	  sizeof(IMDISK_SET_DEVICE_FLAGS))
	{
	  status = STATUS_INVALID_PARAMETER;
	  Irp->IoStatus.Information = 0;
	  break;
	}

      {
	PIMDISK_SET_DEVICE_FLAGS device_flags =
	  Irp->AssociatedIrp.SystemBuffer;

	if (IMDISK_READONLY(device_flags->FlagsToChange))
	  if (DeviceObject->DeviceType == FILE_DEVICE_DISK)
	    {
	      if (IMDISK_READONLY(device_flags->FlagValues))
		{
		  DeviceObject->Characteristics |= FILE_READ_ONLY_DEVICE;
		  device_extension->read_only = TRUE;

		  device_flags->FlagsToChange &= ~IMDISK_OPTION_RO;
		}
	      else
		// It is not possible to make a file- or proxy virtual disk
		// writable on the fly. (A physical image file or the proxy
		// comm channel might not be opened for writing.)
		if (device_extension->vm_disk) 
		  {
		    DeviceObject->Characteristics &= ~FILE_READ_ONLY_DEVICE;
		    device_extension->read_only = FALSE;
		    
		    device_flags->FlagsToChange &= ~IMDISK_OPTION_RO;
		  }
	    }

	if (IMDISK_REMOVABLE(device_flags->FlagsToChange))
	  if (DeviceObject->DeviceType == FILE_DEVICE_DISK)
	    {
	      if (IMDISK_REMOVABLE(device_flags->FlagValues))
		DeviceObject->Characteristics |= FILE_REMOVABLE_MEDIA;
	      else
		DeviceObject->Characteristics &= ~FILE_REMOVABLE_MEDIA;

	      device_flags->FlagsToChange &= ~IMDISK_OPTION_REMOVABLE;
	    }

	if (device_flags->FlagsToChange)
	  status = STATUS_INVALID_DEVICE_REQUEST;
	else
	  status = STATUS_SUCCESS;
      }

      if (io_stack->Parameters.DeviceIoControl.OutputBufferLength >=
	  sizeof(IMDISK_SET_DEVICE_FLAGS))
	Irp->IoStatus.Information = sizeof(IMDISK_SET_DEVICE_FLAGS);
      else
	Irp->IoStatus.Information = 0;

      break;

    case IOCTL_IMDISK_REFERENCE_HANDLE:
      KdPrint(("ImDisk: IOCTL_IMDISK_REFERENCE_HANDLE for device %i.\n",
	       device_extension->device_number));

      // This IOCTL requires work that must be done at IRQL < DISPATCH_LEVEL
      // but must be done in the thread context of the calling application and
      // not by the worker thread so therefore this check is done. Also, the
      // control device does not have a worker thread so that is another
      // reason.
      if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
	{
	  status = STATUS_ACCESS_DENIED;
	  Irp->IoStatus.Information = 0;
	  break;
	}

      if ((io_stack->Parameters.DeviceIoControl.InputBufferLength <
	   sizeof(HANDLE)) |
	  (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	   sizeof(PFILE_OBJECT)))
	{
	  status = STATUS_INVALID_PARAMETER;
	  Irp->IoStatus.Information = 0;
	  break;
	}

      KdPrint(("ImDisk: Referencing handle %#x.\n",
	       *(PHANDLE) Irp->AssociatedIrp.SystemBuffer));

      status =
	ObReferenceObjectByHandle(*(PHANDLE)
				  Irp->AssociatedIrp.SystemBuffer,
				  FILE_READ_ATTRIBUTES |
				  FILE_READ_DATA |
				  FILE_WRITE_DATA,
				  *IoFileObjectType,
				  KernelMode,
				  Irp->AssociatedIrp.SystemBuffer,
				  NULL);

      KdPrint(("ImDisk: Status=%#x, FILE_OBJECT %#x.\n",
	       status,
	       (PFILE_OBJECT) Irp->AssociatedIrp.SystemBuffer));

      if (!NT_SUCCESS(status))
	Irp->IoStatus.Information = 0;
      else
	Irp->IoStatus.Information = sizeof(PFILE_OBJECT);

      break;

    case IOCTL_IMDISK_CREATE_DEVICE:
      {
	PIMDISK_CREATE_DATA create_data;

	KdPrint(("ImDisk: IOCTL_IMDISK_CREATE_DEVICE for device %i.\n",
		 device_extension->device_number));

	// This IOCTL requires work that must be done at IRQL < DISPATCH_LEVEL
	// but the control device has no worker thread (does not handle any
	// other I/O) so therefore everything is done directly here. Therefore
	// this IRQL check is necessary.
	if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
	  {
	    status = STATUS_ACCESS_DENIED;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
	    sizeof(IMDISK_CREATE_DATA) - sizeof(*create_data->FileName))
	  {
	    KdPrint(("ImDisk: Invalid input buffer size (1). "
		     "Got: %u Expected at least: %u.\n",
		     io_stack->Parameters.DeviceIoControl.InputBufferLength,
		     sizeof(IMDISK_CREATE_DATA) -
		     sizeof(*create_data->FileName)));

	    status = STATUS_INVALID_PARAMETER;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	create_data = (PIMDISK_CREATE_DATA) Irp->AssociatedIrp.SystemBuffer;

	if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
	    sizeof(IMDISK_CREATE_DATA) +
	    create_data->FileNameLength -
	    sizeof(*create_data->FileName))
	  {
	    KdPrint(("ImDisk: Invalid input buffer size (2). "
		     "Got: %u Expected at least: %u.\n",
		     io_stack->Parameters.DeviceIoControl.InputBufferLength,
		     sizeof(IMDISK_CREATE_DATA) +
		     create_data->FileNameLength -
		     sizeof(*create_data->FileName)));

	    status = STATUS_INVALID_PARAMETER;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	status = ImDiskAddVirtualDisk(DeviceObject->DriverObject,
				      (PIMDISK_CREATE_DATA)
				      Irp->AssociatedIrp.SystemBuffer,
				      Irp->Tail.Overlay.Thread);

	if (NT_SUCCESS(status) &&
	    (io_stack->Parameters.DeviceIoControl.OutputBufferLength >=
	     io_stack->Parameters.DeviceIoControl.InputBufferLength))
	  Irp->IoStatus.Information =
	    io_stack->Parameters.DeviceIoControl.OutputBufferLength;
	else
	  Irp->IoStatus.Information = 0;
	break;
      }

    case IOCTL_DISK_EJECT_MEDIA:
    case IOCTL_STORAGE_EJECT_MEDIA:
      KdPrint(("ImDisk: IOCTL_DISK/STORAGE_EJECT_MEDIA for device %i.\n",
	       device_extension->device_number));

      ImDiskRemoveVirtualDisk(DeviceObject);

      Irp->IoStatus.Information = 0;
      status = STATUS_SUCCESS;
      break;

    case IOCTL_IMDISK_QUERY_DRIVER:
      {
	KdPrint(("ImDisk: IOCTL_IMDISK_QUERY_DRIVER for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(ULONG))
	  Irp->IoStatus.Information = 0;
	else
	  {
	    *(PULONG) Irp->AssociatedIrp.SystemBuffer = DeviceList;
	    Irp->IoStatus.Information = sizeof(ULONG);
	  }

	status = STATUS_SUCCESS;
	break;
      }

    case IOCTL_IMDISK_QUERY_DEVICE:
      {
	PIMDISK_CREATE_DATA create_data;

	KdPrint(("ImDisk: IOCTL_IMDISK_QUERY_DEVICE for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(IMDISK_CREATE_DATA) +
	    device_extension->file_name.Length +
	    sizeof(*create_data->FileName))
	  {
	    status = STATUS_BUFFER_TOO_SMALL;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	create_data = (PIMDISK_CREATE_DATA) Irp->AssociatedIrp.SystemBuffer;

	create_data->DeviceNumber = device_extension->device_number;
	create_data->DiskGeometry = device_extension->disk_geometry;

	create_data->Flags = 0;
	if (device_extension->read_only)
	  create_data->Flags |= IMDISK_OPTION_RO;

	if (DeviceObject->Characteristics & FILE_REMOVABLE_MEDIA)
	  create_data->Flags |= IMDISK_OPTION_REMOVABLE;

	if (DeviceObject->DeviceType == FILE_DEVICE_CD_ROM)
	  create_data->Flags |= IMDISK_DEVICE_TYPE_CD | IMDISK_OPTION_RO;
	else if (DeviceObject->Characteristics & FILE_FLOPPY_DISKETTE)
	  create_data->Flags |= IMDISK_DEVICE_TYPE_FD;
	else
	  create_data->Flags |= IMDISK_DEVICE_TYPE_HD;

	if (device_extension->vm_disk)
	  create_data->Flags |= IMDISK_TYPE_VM;
	else if (device_extension->use_proxy)
	  create_data->Flags |= IMDISK_TYPE_PROXY;
	else
	  create_data->Flags |= IMDISK_TYPE_FILE;

	create_data->DriveLetter = device_extension->drive_letter;

	create_data->FileNameLength = device_extension->file_name.Length;

	if (device_extension->file_name.Length > 0)
	  RtlCopyMemory(create_data->FileName,
			device_extension->file_name.Buffer,
			device_extension->file_name.Length);

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = sizeof(IMDISK_CREATE_DATA) +
	  create_data->FileNameLength -
	  sizeof(*create_data->FileName);

	break;
      }

    case IOCTL_DISK_CHECK_VERIFY:
    case IOCTL_CDROM_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY2:
      {
	KdPrint(("ImDisk: IOCTL_DISK/CDROM/STORAGE_CHECK_VERIFY/2 for "
		 "device %i.\n", device_extension->device_number));

	if (device_extension->vm_disk)
	  {
	    KdPrint(("ImDisk: Faked verify ok on vm device %i.\n",
		     device_extension->device_number));

	    if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
		sizeof(ULONG))
	      Irp->IoStatus.Information = 0;
	    else
	      {
		*(PULONG) Irp->AssociatedIrp.SystemBuffer =
		  device_extension->media_change_count;
		
		Irp->IoStatus.Information = sizeof(ULONG);
	      }
	    
	    status = STATUS_SUCCESS;
	  }
	else
	  status = STATUS_PENDING;

	break;
      }

    case IOCTL_IMDISK_QUERY_VERSION:
      {
	KdPrint(("ImDisk: IOCTL_IMDISK_QUERY_VERSION for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(ULONG))
	  status = STATUS_INVALID_PARAMETER;
	else
	  {
	    *(PULONG) Irp->AssociatedIrp.SystemBuffer = IMDISK_DRIVER_VERSION;
	    Irp->IoStatus.Information = sizeof(ULONG);
	    status = STATUS_SUCCESS;
	  }

	break;
      }

    case IOCTL_DISK_FORMAT_TRACKS:
    case IOCTL_DISK_FORMAT_TRACKS_EX:
      //	Only several checks are done here
      //	Actual operation is done by the device thread
      {
	PFORMAT_PARAMETERS param;
	PDISK_GEOMETRY geometry;

	KdPrint(("ImDisk: IOCTL_DISK_FORMAT_TRACKS for device %i.\n",
		 device_extension->device_number));

	/*
	if (~DeviceObject->Characteristics & FILE_FLOPPY_DISKETTE)
	  {
	    Irp->IoStatus.Information = 0;
	    status = STATUS_INVALID_DEVICE_REQUEST;
	    break;
	  }
	*/

	//	Media is writable?

	if (device_extension->read_only)
	  {
	    KdPrint(("ImDisk: Attempt to format write-protected image.\n"));

	    Irp->IoStatus.Information = 0;
	    status = STATUS_MEDIA_WRITE_PROTECTED;
	    break;
	  }

	//	Check input parameter size

	if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
	    sizeof(FORMAT_PARAMETERS))
	  {
	    Irp->IoStatus.Information = 0;
	    status = STATUS_INVALID_PARAMETER;
	    break;
	  }

	//	Input parameter sanity check

	param = (PFORMAT_PARAMETERS) Irp->AssociatedIrp.SystemBuffer;
	geometry = ExAllocatePool(NonPagedPool, sizeof(DISK_GEOMETRY));
	if (geometry == NULL)
	  {
	    Irp->IoStatus.Information = 0;
	    status = STATUS_INSUFFICIENT_RESOURCES;
	    break;
	  }

	RtlCopyMemory(geometry, &device_extension->disk_geometry,
		      sizeof(DISK_GEOMETRY));

	geometry->Cylinders.QuadPart /= geometry->TracksPerCylinder;
	geometry->Cylinders.QuadPart /= geometry->SectorsPerTrack;
	geometry->Cylinders.QuadPart /= geometry->BytesPerSector;

	if ((param->StartHeadNumber > geometry->TracksPerCylinder - 1) ||
	    (param->EndHeadNumber   > geometry->TracksPerCylinder - 1) ||
	    ((LONGLONG)param->StartCylinderNumber >
	     geometry->Cylinders.QuadPart) ||
	    ((LONGLONG)param->EndCylinderNumber >
	     geometry->Cylinders.QuadPart) ||
	    (param->EndCylinderNumber	< param->StartCylinderNumber))
	  {
	    ExFreePool(geometry);
	    Irp->IoStatus.Information = 0;
	    status = STATUS_INVALID_PARAMETER;
	    break;
	  }

	if ((param->StartCylinderNumber * geometry->TracksPerCylinder *
	     geometry->BytesPerSector * geometry->SectorsPerTrack +
	     param->StartHeadNumber * geometry->BytesPerSector *
	     geometry->SectorsPerTrack >=
	     device_extension->disk_geometry.Cylinders.QuadPart) |
	    (param->EndCylinderNumber * geometry->TracksPerCylinder *
	     geometry->BytesPerSector * geometry->SectorsPerTrack +
	     param->EndHeadNumber * geometry->BytesPerSector *
	     geometry->SectorsPerTrack >=
	     device_extension->disk_geometry.Cylinders.QuadPart))
	  {
	    ExFreePool(geometry);
	    Irp->IoStatus.Information = 0;
	    status = STATUS_INVALID_PARAMETER;
	    break;
	  }

	//	If this is an EX request then make a couple of extra checks

	if (io_stack->Parameters.DeviceIoControl.IoControlCode ==
	    IOCTL_DISK_FORMAT_TRACKS_EX)
	  {
	    PFORMAT_EX_PARAMETERS exparam;
	    ULONG paramsize;

	    KdPrint(("ImDisk: IOCTL_DISK_FORMAT_TRACKS_EX for device %i.\n",
		     device_extension->device_number));

	    if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
		sizeof(FORMAT_EX_PARAMETERS))
	      {
		ExFreePool(geometry);
		Irp->IoStatus.Information = 0;
		status = STATUS_INVALID_PARAMETER;
		break;
	      }

	    exparam = (PFORMAT_EX_PARAMETERS)Irp->AssociatedIrp.SystemBuffer;

	    paramsize = sizeof(FORMAT_EX_PARAMETERS)
	      + exparam->SectorsPerTrack * sizeof(USHORT)
	      - sizeof(USHORT);

	    if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
		paramsize ||
		exparam->FormatGapLength > geometry->SectorsPerTrack ||
		exparam->SectorsPerTrack != geometry->SectorsPerTrack)
	      {
		ExFreePool(geometry);
		Irp->IoStatus.Information = 0;
		status = STATUS_INVALID_PARAMETER;
		break;
	      }
	  }

	ExFreePool(geometry);
	status = STATUS_PENDING;
	break;
      }

    case IOCTL_DISK_GROW_PARTITION:
      {
	PDISK_GROW_PARTITION grow_partition;

	KdPrint(("ImDisk: IOCTL_DISK_GROW_PARTITION for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.InputBufferLength !=
	    sizeof(DISK_GROW_PARTITION))
	  {
	    status = STATUS_BUFFER_TOO_SMALL;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	if (device_extension->read_only)
	  {
	    status = STATUS_MEDIA_WRITE_PROTECTED;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	grow_partition = (PDISK_GROW_PARTITION)
	  Irp->AssociatedIrp.SystemBuffer;

	// Check so we don't get a smaller disk with these parameters
	if ((grow_partition->PartitionNumber != 1) |
	    (device_extension->disk_geometry.Cylinders.QuadPart +
	     grow_partition->BytesToGrow.QuadPart <
	     device_extension->disk_geometry.Cylinders.QuadPart))
	  {
	    status = STATUS_INVALID_PARAMETER;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	status = STATUS_PENDING;
	break;
      }

    case IOCTL_DISK_UPDATE_PROPERTIES:
      {
	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	break;
      }

    case IOCTL_DISK_GET_MEDIA_TYPES:
    case IOCTL_STORAGE_GET_MEDIA_TYPES:
    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
    case IOCTL_DISK_UPDATE_DRIVE_SIZE:
      {
	PDISK_GEOMETRY geometry;

	KdPrint(("ImDisk: IOCTL_DISK/STORAGE_GET_MEDIA_TYPES/DRIVE_GEOMETRY "
		 "for device %i.\n", device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(device_extension->disk_geometry))
	  {
	    status = STATUS_BUFFER_TOO_SMALL;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	geometry = (PDISK_GEOMETRY) Irp->AssociatedIrp.SystemBuffer;
	*geometry = device_extension->disk_geometry;
	geometry->Cylinders.QuadPart /= geometry->TracksPerCylinder;
	geometry->Cylinders.QuadPart /= geometry->SectorsPerTrack;
	geometry->Cylinders.QuadPart /= geometry->BytesPerSector;

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
	break;
      }

    case IOCTL_DISK_GET_LENGTH_INFO:
      {
	KdPrint(("ImDisk: IOCTL_DISK_GET_LENGTH_INFO for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(GET_LENGTH_INFORMATION))
	  {
	    status = STATUS_BUFFER_TOO_SMALL;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	((PGET_LENGTH_INFORMATION) Irp->AssociatedIrp.SystemBuffer)->
	  Length.QuadPart =
	  device_extension->disk_geometry.Cylinders.QuadPart;

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = sizeof(GET_LENGTH_INFORMATION);

	break;
      }

    case IOCTL_DISK_GET_PARTITION_INFO:
      {
	PPARTITION_INFORMATION partition_information;

	KdPrint(("ImDisk: IOCTL_DISK_GET_PARTITION_INFO for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(PARTITION_INFORMATION))
	  {
	    status = STATUS_BUFFER_TOO_SMALL;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	partition_information =
	  (PPARTITION_INFORMATION) Irp->AssociatedIrp.SystemBuffer;

	partition_information->StartingOffset.QuadPart =
	  (LONGLONG) device_extension->disk_geometry.BytesPerSector *
	  device_extension->disk_geometry.SectorsPerTrack;
	partition_information->PartitionLength =
	  device_extension->disk_geometry.Cylinders;
	partition_information->HiddenSectors = 1;
	partition_information->PartitionNumber = 1;
	partition_information->PartitionType = PARTITION_HUGE;
	partition_information->BootIndicator = FALSE;
	partition_information->RecognizedPartition = FALSE;
	partition_information->RewritePartition = FALSE;

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION);

	break;
      }

    case IOCTL_DISK_GET_PARTITION_INFO_EX:
      {
	PPARTITION_INFORMATION_EX partition_information_ex;

	KdPrint(("ImDisk: IOCTL_DISK_GET_PARTITION_INFO_EX for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(PARTITION_INFORMATION_EX))
	  {
	    status = STATUS_BUFFER_TOO_SMALL;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	partition_information_ex =
	  (PPARTITION_INFORMATION_EX) Irp->AssociatedIrp.SystemBuffer;

	partition_information_ex->PartitionStyle = PARTITION_STYLE_MBR;
	partition_information_ex->StartingOffset.QuadPart =
	  (LONGLONG) device_extension->disk_geometry.BytesPerSector *
	  device_extension->disk_geometry.SectorsPerTrack;
	partition_information_ex->PartitionLength =
	  device_extension->disk_geometry.Cylinders;
	partition_information_ex->PartitionNumber = 1;
	partition_information_ex->RewritePartition = FALSE;
	partition_information_ex->Mbr.PartitionType = PARTITION_HUGE;
	partition_information_ex->Mbr.BootIndicator = FALSE;
	partition_information_ex->Mbr.RecognizedPartition = FALSE;
	partition_information_ex->Mbr.HiddenSectors = 1;

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = sizeof(PARTITION_INFORMATION_EX);

	break;
      }

    case IOCTL_DISK_IS_WRITABLE:
      {
	KdPrint(("ImDisk: IOCTL_DISK_IS_WRITABLE for device %i.\n",
		 device_extension->device_number));

	if (!device_extension->read_only)
	  status = STATUS_SUCCESS;
	else
	  status = STATUS_MEDIA_WRITE_PROTECTED;

	Irp->IoStatus.Information = 0;

	break;
      }

    case IOCTL_DISK_MEDIA_REMOVAL:
    case IOCTL_STORAGE_MEDIA_REMOVAL:
      {
	KdPrint(("ImDisk: IOCTL_DISK/STORAGE_MEDIA_REMOVAL for device %i.\n",
		 device_extension->device_number));

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	break;
      }

    case IOCTL_CDROM_GET_LAST_SESSION:
      {
	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;

	break;
      }

    case IOCTL_CDROM_READ_TOC:
      {
	PCDROM_TOC cdrom_toc;

	KdPrint(("ImDisk: IOCTL_CDROM_READ_TOC for device %i.\n",
		 device_extension->device_number));

	if (DeviceObject->DeviceType != FILE_DEVICE_CD_ROM)
	  {
	    Irp->IoStatus.Information = 0;
	    status = STATUS_INVALID_DEVICE_REQUEST;
	    break;
	  }

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(CDROM_TOC))
	  {
	    status = STATUS_BUFFER_TOO_SMALL;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	cdrom_toc = (PCDROM_TOC) Irp->AssociatedIrp.SystemBuffer;

	RtlZeroMemory(cdrom_toc, sizeof(CDROM_TOC));

	cdrom_toc->FirstTrack = 1;
	cdrom_toc->LastTrack = 1;
	cdrom_toc->TrackData[0].Control = TOC_DATA_TRACK;

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = sizeof(CDROM_TOC);

	break;
      }

    case IOCTL_DISK_SET_PARTITION_INFO:
      {
	KdPrint(("ImDisk: IOCTL_DISK_SET_PARTITION_INFO for device %i.\n",
		 device_extension->device_number));

	if (device_extension->read_only)
	  {
	    KdPrint(("ImDisk: Attempt to partition read-only image.\n"));

	    status = STATUS_MEDIA_WRITE_PROTECTED;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
	    sizeof(SET_PARTITION_INFORMATION))
	  {
	    status = STATUS_INVALID_PARAMETER;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;

	break;
      }

    case IOCTL_DISK_SET_PARTITION_INFO_EX:
      {
	PSET_PARTITION_INFORMATION_EX partition_information_ex;

	KdPrint(("ImDisk: IOCTL_DISK_SET_PARTITION_INFO_EX for device %i.\n",
		 device_extension->device_number));

	if (device_extension->read_only)
	  {
	    KdPrint(("ImDisk: Attempt to partition read-only image.\n"));

	    status = STATUS_MEDIA_WRITE_PROTECTED;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
	    sizeof(SET_PARTITION_INFORMATION_EX))
	  {
	    status = STATUS_INVALID_PARAMETER;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	partition_information_ex = (PSET_PARTITION_INFORMATION_EX)
	  Irp->AssociatedIrp.SystemBuffer;

	if (partition_information_ex->PartitionStyle != PARTITION_STYLE_MBR)
	  {
	    status = STATUS_UNSUCCESSFUL;
	    Irp->IoStatus.Information = 0;
	  }
	else
	  {
	    status = STATUS_SUCCESS;
	    Irp->IoStatus.Information = 0;
	  }

	break;
      }

    case IOCTL_DISK_VERIFY:
      {
	PVERIFY_INFORMATION verify_information;

	KdPrint(("ImDisk: IOCTL_DISK_VERIFY for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.InputBufferLength <
	    sizeof(VERIFY_INFORMATION))
	  {
	    status = STATUS_INVALID_PARAMETER;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	verify_information = (PVERIFY_INFORMATION)
	  Irp->AssociatedIrp.SystemBuffer;

	if (device_extension->read_only)
	  {
	    KdPrint(("ImDisk: Attempt to verify read-only media.\n"));

	    status = STATUS_MEDIA_WRITE_PROTECTED;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	if (verify_information->StartingOffset.QuadPart +
	    verify_information->Length >
	    device_extension->disk_geometry.Cylinders.QuadPart)
	  {
	    KdPrint(("ImDisk: Attempt to verify beyond image size.\n"));

	    status = STATUS_NONEXISTENT_SECTOR;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;

	break;
      }

      // Ver 1.0.2 does no longer handle IOCTL_STORAGE_GET_DEVICE_NUMBER.
      // It was a very ugly attempt to workaround some problems that do not
      // seem to exist anylonger anyway. The data returned here made no sense
      // actually so in order to not risk breaking more things in the future I
      // have removed it completely.
      /*
    case IOCTL_STORAGE_GET_DEVICE_NUMBER:
      {
	PSTORAGE_DEVICE_NUMBER device_number;

	KdPrint(("ImDisk: IOCTL_STORAGE_GET_DEVICE_NUMBER for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(STORAGE_DEVICE_NUMBER))
	  {
	    status = STATUS_INVALID_PARAMETER;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	device_number = (PSTORAGE_DEVICE_NUMBER)
	  Irp->AssociatedIrp.SystemBuffer;

	device_number->DeviceType = DeviceObject->DeviceType;
	device_number->DeviceNumber = (ULONG) DeviceObject;
	device_number->PartitionNumber = (ULONG) -1;

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = sizeof(STORAGE_DEVICE_NUMBER);

	break;
      }
      */

    case IOCTL_STORAGE_GET_HOTPLUG_INFO:
      {
	PSTORAGE_HOTPLUG_INFO hotplug_info;

	KdPrint(("ImDisk: IOCTL_STORAGE_GET_HOTPLUG_INFO for device %i.\n",
		 device_extension->device_number));

	if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
	    sizeof(STORAGE_HOTPLUG_INFO))
	  {
	    status = STATUS_INVALID_PARAMETER;
	    Irp->IoStatus.Information = 0;
	    break;
	  }

	hotplug_info = (PSTORAGE_HOTPLUG_INFO)
	  Irp->AssociatedIrp.SystemBuffer;

	hotplug_info->Size = sizeof(STORAGE_HOTPLUG_INFO);
	if (DeviceObject->Characteristics & FILE_REMOVABLE_MEDIA)
	  {
	    hotplug_info->MediaRemovable = TRUE;
	    hotplug_info->MediaHotplug = TRUE;
	    hotplug_info->DeviceHotplug = TRUE;
	    hotplug_info->WriteCacheEnableOverride = FALSE;
	  }
	else
	  {
	    hotplug_info->MediaRemovable = FALSE;
	    hotplug_info->MediaHotplug = FALSE;
	    hotplug_info->DeviceHotplug = FALSE;
	    hotplug_info->WriteCacheEnableOverride = FALSE;
	  }

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information = sizeof(STORAGE_HOTPLUG_INFO);

	break;
      }

    case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
      {
	PMOUNTDEV_NAME mountdev_name = Irp->AssociatedIrp.SystemBuffer;
	int chars;

	KdPrint(("ImDisk: IOCTL_MOUNTDEV_QUERY_DEVICE_NAME for device %i.\n",
		 device_extension->device_number));

	if (device_extension->drive_letter == 0)
	  chars =
	    _snwprintf(mountdev_name->Name,
		       (io_stack->
			Parameters.DeviceIoControl.OutputBufferLength -
			FIELD_OFFSET(MOUNTDEV_NAME, Name)) >> 1,
		       IMDISK_DEVICE_BASE_NAME L"%u",
		       device_extension->device_number);
	else
	  chars =
	    _snwprintf(mountdev_name->Name,
		       (io_stack->
			Parameters.DeviceIoControl.OutputBufferLength -
			FIELD_OFFSET(MOUNTDEV_NAME, Name)) >> 1,
		       L"\\DosDevices\\%wc:",
		       device_extension->drive_letter);

	if (chars < 0)
	  {
	    if (io_stack->Parameters.DeviceIoControl.OutputBufferLength >=
		FIELD_OFFSET(MOUNTDEV_NAME, Name) +
		sizeof(mountdev_name->NameLength))
	      mountdev_name->NameLength = sizeof(IMDISK_DEVICE_BASE_NAME) +
		20;

	    KdPrint(("ImDisk: IOCTL_MOUNTDEV_QUERY_DEVICE_NAME overflow, "
		     "buffer length %u, returned %i.\n",
		     io_stack->Parameters.DeviceIoControl.OutputBufferLength,
		     chars));

	    status = STATUS_BUFFER_OVERFLOW;

	    if (io_stack->Parameters.DeviceIoControl.OutputBufferLength >=
		sizeof(MOUNTDEV_NAME))
	      Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
	    else
	      Irp->IoStatus.Information = 0;

	    break;
	  }

	mountdev_name->NameLength = (USHORT) chars << 1;

	status = STATUS_SUCCESS;
	Irp->IoStatus.Information =
	  FIELD_OFFSET(MOUNTDEV_NAME, Name) + mountdev_name->NameLength;

	KdPrint(("ImDisk: IOCTL_MOUNTDEV_QUERY_DEVICE_NAME returning %ws, "
		 "length %u total %u.\n",
		 mountdev_name->Name, mountdev_name->NameLength,
		 Irp->IoStatus.Information));

	break;
      }

    default:
      {
	KdPrint(("ImDisk: Unknown IOCTL %#x.\n",
		 io_stack->Parameters.DeviceIoControl.IoControlCode));

	status = STATUS_INVALID_DEVICE_REQUEST;
	Irp->IoStatus.Information = 0;
      }
    }

  if (status == STATUS_PENDING)
    {
      IoMarkIrpPending(Irp);

      ExInterlockedInsertTailList(&device_extension->list_head,
				  &Irp->Tail.Overlay.ListEntry,
				  &device_extension->list_lock);

      KeSetEvent(&device_extension->request_event, (KPRIORITY) 0, FALSE);
    }
  else
    {
      Irp->IoStatus.Status = status;

      IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

  return status;
}

#pragma code_seg("PAGE")

VOID
ImDiskDeviceThread(IN PVOID Context)
{
  PDEVICE_THREAD_DATA device_thread_data;
  PDEVICE_OBJECT device_object;
  PDEVICE_EXTENSION device_extension;
  PLIST_ENTRY request;
  PIRP irp;
  PIO_STACK_LOCATION io_stack;
  LARGE_INTEGER time_out;

  PAGED_CODE();

  ASSERT(Context != NULL);

  KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

  device_thread_data = (PDEVICE_THREAD_DATA) Context;

  // This is in case this thread is created by
  // ImDiskAddVirtualDiskAfterInitialization() when called from DriverEntry().
  // That indicates that no-one is waiting for us to return any status
  // in the device_thread_data members and that there is no-one freeing the
  // init structures after we are finished with them.
  // It also means that we need to wait for the control device to get ready for
  // I/O (in case a proxy or something need to call this driver during device
  // initialization).
  while (ImDiskCtlDevice->Flags & DO_DEVICE_INITIALIZING)
    {
      LARGE_INTEGER wait_time;

      KdPrint2(("ImDisk: Driver still initializing, waiting 100 ms...\n"));

      wait_time.QuadPart = -1000000;
      KeDelayExecutionThread(KernelMode, FALSE, &wait_time);
    }

  device_thread_data->status =
    ImDiskCreateDevice(device_thread_data->driver_object,
		       device_thread_data->create_data,
		       device_thread_data->client_thread,
		       &device_object);

  if (!NT_SUCCESS(device_thread_data->status))
    {
      if (device_thread_data->caller_waiting)
	KeSetEvent(&device_thread_data->created_event, (KPRIORITY) 0, FALSE);
      else
	{
	  ExFreePool(device_thread_data->create_data);
	  ExFreePool(device_thread_data);
	}

      PsTerminateSystemThread(STATUS_SUCCESS);
    }

  // Now we are done with initialization. Let the one that asks us to create
  // this device now that, or if no-one left there, clean up init structures
  // here.
  if (device_thread_data->caller_waiting)
    KeSetEvent(&device_thread_data->created_event, (KPRIORITY) 0, FALSE);
  else
    {
      ExFreePool(device_thread_data->create_data);
      ExFreePool(device_thread_data);
    }

  KdPrint(("ImDisk: Device thread initialized. (flags=%#x)\n",
	   device_object->Flags));

  device_extension = (PDEVICE_EXTENSION) device_object->DeviceExtension;

  time_out.QuadPart = -1000000;

  // If this is a VM backed disk that should be pre-loaded with an image file
  // we have to load the contents of that file now.
  if (device_extension->vm_disk && (device_extension->file_handle != NULL))
    {
      LARGE_INTEGER byte_offset = device_extension->image_offset;
      IO_STATUS_BLOCK io_status;
      NTSTATUS status;

      KdPrint(("ImDisk: Reading image file into vm disk buffer.\n"));

      status =
	ImDiskSafeReadFile(device_extension->file_handle,
			   &io_status,
			   device_extension->image_buffer,
			   device_extension->disk_geometry.Cylinders.LowPart,
			   &byte_offset);

      ZwClose(device_extension->file_handle);
      device_extension->file_handle = NULL;

      // Failure to read pre-load image is now considered a fatal error
      if (!NT_SUCCESS(status))
	{
	  KdPrint(("ImDisk: Failed to read image file (%#x).\n", status));

	  ImDiskRemoveVirtualDisk(device_object);
	}
      else
	KdPrint(("ImDisk: Image loaded successfully.\n"));
    }

  for (;;)
    {
      request =
	ExInterlockedRemoveHeadList(&device_extension->list_head,
				    &device_extension->list_lock);

      if (request == NULL)
	{
	  if (!device_extension->terminate_thread)
	    {
	      KeWaitForSingleObject(&device_extension->request_event,
				    Executive, KernelMode, FALSE, NULL);
	      continue;
	    }

	  KdPrint(("ImDisk: Device %i thread is shutting down.\n",
		   device_extension->device_number));

	  // If ReferenceCount is not zero, this device may have outstanding
	  // IRP-s or otherwise unfinished things to do. Let IRP-s be done by
	  // continuing this dispatch loop until ReferenceCount is zero.
	  if (device_object->ReferenceCount != 0)
	    {
	      KdPrint(("ImDisk: Device %i has %i references. Waiting.\n",
		       device_extension->device_number,
		       device_object->ReferenceCount));

	      KeDelayExecutionThread(KernelMode, FALSE, &time_out);

	      time_out.LowPart <<= 4;
	      continue;
	    }

	  if (device_extension->drive_letter != 0)
	    {
	      NTSTATUS status;
	      WCHAR sym_link_global_wchar[] = L"\\DosDevices\\Global\\ :";
	      WCHAR sym_link_wchar[] = L"\\DosDevices\\ :";
	      UNICODE_STRING sym_link;

	      sym_link_global_wchar[19] = device_extension->drive_letter;

	      KdPrint(("ImDisk: Removing symlink '%ws'.\n",
		       sym_link_global_wchar));

	      RtlInitUnicodeString(&sym_link, sym_link_global_wchar);
	      status = IoDeleteSymbolicLink(&sym_link);

	      if (!NT_SUCCESS(status))
		{
		  KdPrint
		    (("ImDisk: Cannot remove symlink '%ws'. (%#x)\n",
		      sym_link_global_wchar, status));
		}

	      sym_link_wchar[12] = device_extension->drive_letter;

	      KdPrint(("ImDisk: Removing symlink '%ws'.\n",
		       sym_link_wchar));

	      RtlInitUnicodeString(&sym_link, sym_link_wchar);
	      status = IoDeleteSymbolicLink(&sym_link);

	      if (!NT_SUCCESS(status))
		{
		  KdPrint
		    (("ImDisk: Cannot remove symlink '%ws'. (%#x)\n",
		      sym_link_wchar, status));
		}

	      device_extension->drive_letter = 0;
	    }

	  KdPrint(("ImDisk: Freeing resources for device %i.\n",
		   device_extension->device_number));

	  if (device_extension->proxy_device != NULL)
	    ObDereferenceObject(device_extension->proxy_device);

	  if (device_extension->vm_disk)
	    {
	      ULONG free_size = 0;
	      if (device_extension->image_buffer != NULL)
		ZwFreeVirtualMemory(NtCurrentProcess(),
				    &device_extension->image_buffer,
				    &free_size, MEM_RELEASE);

	      device_extension->image_buffer = NULL;
	    }
	  else
	    {
	      if (device_extension->file_handle != NULL)
		ZwClose(device_extension->file_handle);

	      device_extension->file_handle = NULL;
	    }

	  if (device_extension->file_name.Buffer != NULL)
	    {
	      ExFreePool(device_extension->file_name.Buffer);
	      device_extension->file_name.Buffer = NULL;
	      device_extension->file_name.Length = 0;
	      device_extension->file_name.MaximumLength = 0;
	    }

	  DeviceList &= ~(1 << device_extension->device_number);

	  KdPrint(("ImDisk: Deleting device object %i.\n",
		   device_extension->device_number));

	  IoDeleteDevice(device_object);

	  PsTerminateSystemThread(STATUS_SUCCESS);
	}

      irp = CONTAINING_RECORD(request, IRP, Tail.Overlay.ListEntry);

      io_stack = IoGetCurrentIrpStackLocation(irp);

      switch (io_stack->MajorFunction)
	{
	case IRP_MJ_READ:
	  {
	    PUCHAR buffer;
	    PUCHAR system_buffer =
	      (PUCHAR) MmGetSystemAddressForMdlSafe(irp->MdlAddress,
						    NormalPagePriority);
	    LARGE_INTEGER offset;

	    if (system_buffer == NULL)
	      {
		irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
		irp->IoStatus.Information = 0;
		break;
	      }

	    if (device_extension->vm_disk)
	      {
		RtlCopyMemory(system_buffer,
			      device_extension->image_buffer +
			      io_stack->Parameters.Read.ByteOffset.LowPart,
			      io_stack->Parameters.Read.Length);

		irp->IoStatus.Status = STATUS_SUCCESS;
		irp->IoStatus.Information = io_stack->Parameters.Read.Length;

		break;
	      }

	    offset.QuadPart = io_stack->Parameters.Read.ByteOffset.QuadPart +
	      device_extension->image_offset.QuadPart;

	    buffer = (PUCHAR)
	      ExAllocatePool(NonPagedPool, io_stack->Parameters.Read.Length);

	    if (buffer == NULL)
	      {
		irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
		irp->IoStatus.Information = 0;
		break;
	      }

	    if (device_extension->use_proxy)
	      {
		irp->IoStatus.Status =
		  ImDiskReadProxy(device_extension->proxy_device,
				  &irp->IoStatus,
				  buffer,
				  io_stack->Parameters.Read.Length,
				  &offset);

		if (!NT_SUCCESS(irp->IoStatus.Status))
		  {
		    KdPrint(("ImDisk: Read failed on device %i.\n",
			     device_extension->device_number));

		    // If indicating that proxy connection died we can do
		    // nothing else but remove this device.
		    if (irp->IoStatus.Status == STATUS_CONNECTION_RESET)
		      ImDiskRemoveVirtualDisk(device_object);

		    irp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
		    irp->IoStatus.Information = 0;
		  }
	      }
	    else				      
	      irp->IoStatus.Status =
		ZwReadFile(device_extension->file_handle,
			   NULL,
			   NULL,
			   NULL,
			   &irp->IoStatus,
			   buffer,
			   io_stack->Parameters.Read.Length,
			   &offset,
			   NULL);

	    RtlCopyMemory(system_buffer, buffer,
			  irp->IoStatus.Information);

	    ExFreePool(buffer);

	    break;
	  }

	case IRP_MJ_WRITE:
	  {
	    PUCHAR buffer;
	    PUCHAR system_buffer =
	      (PUCHAR) MmGetSystemAddressForMdlSafe(irp->MdlAddress,
						    NormalPagePriority);
	    LARGE_INTEGER offset;

	    if (system_buffer == NULL)
	      {
		irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
		irp->IoStatus.Information = 0;
		break;
	      }

	    if (device_extension->vm_disk)
	      {
		RtlCopyMemory(device_extension->image_buffer +
			      io_stack->Parameters.Write.ByteOffset.LowPart,
			      system_buffer,
			      io_stack->Parameters.Write.Length);

		irp->IoStatus.Status = STATUS_SUCCESS;
		irp->IoStatus.Information = io_stack->Parameters.Write.Length;

		break;
	      }

	    offset.QuadPart = io_stack->Parameters.Write.ByteOffset.QuadPart +
	      device_extension->image_offset.QuadPart;

	    buffer = (PUCHAR)
	      ExAllocatePool(NonPagedPool, io_stack->Parameters.Write.Length);

	    if (buffer == NULL)
	      {
		irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
		irp->IoStatus.Information = 0;
		break;
	      }

	    RtlCopyMemory(buffer, system_buffer,
			  io_stack->Parameters.Write.Length);

	    if (device_extension->use_proxy)
	      {
		irp->IoStatus.Status =
		  ImDiskWriteProxy(device_extension->proxy_device,
				   &irp->IoStatus,
				   buffer,
				   io_stack->Parameters.Write.Length,
				   &offset);

		if (!NT_SUCCESS(irp->IoStatus.Status))
		  {
		    KdPrint(("ImDisk: Write failed on device %i.\n",
			     device_extension->device_number));

		    // If indicating that proxy connection died we can do
		    // nothing else but remove this device.
		    if (irp->IoStatus.Status == STATUS_CONNECTION_RESET)
		      ImDiskRemoveVirtualDisk(device_object);

		    irp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
		    irp->IoStatus.Information = 0;
		  }
	      }
	    else				      
	      irp->IoStatus.Status =
		ZwWriteFile(device_extension->file_handle,
			    NULL,
			    NULL,
			    NULL,
			    &irp->IoStatus,
			    buffer,
			    io_stack->Parameters.Write.Length,
			    &offset,
			    NULL);

	    ExFreePool(buffer);

	    break;
	  }

	case IRP_MJ_DEVICE_CONTROL:
	  switch (io_stack->Parameters.DeviceIoControl.IoControlCode)
	    {
	    case IOCTL_DISK_CHECK_VERIFY:
	    case IOCTL_CDROM_CHECK_VERIFY:
	    case IOCTL_STORAGE_CHECK_VERIFY:
	    case IOCTL_STORAGE_CHECK_VERIFY2:
	      {
		PUCHAR buffer;
		
		buffer = (PUCHAR)
		  ExAllocatePool(NonPagedPool, + 1);

		if (buffer == NULL)
		  {
		    irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
		    irp->IoStatus.Information = 0;
		    break;
		  }

		if (device_extension->use_proxy)
		  irp->IoStatus.Status =
		    ImDiskReadProxy(device_extension->proxy_device,
				    &irp->IoStatus,
				    buffer,
				    0,
				    &device_extension->image_offset);
		else				      
		  irp->IoStatus.Status =
		    ZwReadFile(device_extension->file_handle,
			       NULL,
			       NULL,
			       NULL,
			       &irp->IoStatus,
			       buffer,
			       0,
			       &device_extension->image_offset,
			       NULL);

		ExFreePool(buffer);

		if (!NT_SUCCESS(irp->IoStatus.Status))
		  {
		    KdPrint(("ImDisk: Verify failed on device %i.\n",
			     device_extension->device_number));

		    // If indicating that proxy connection died we can do
		    // nothing else but remove this device.
		    if (irp->IoStatus.Status == STATUS_CONNECTION_RESET)
		      ImDiskRemoveVirtualDisk(device_object);

		    irp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
		    irp->IoStatus.Information = 0;
		    break;
		  }

		KdPrint(("ImDisk: Verify ok on device %i.\n",
			 device_extension->device_number));

		if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
		    sizeof(ULONG))
		  irp->IoStatus.Information = 0;
		else
		  {
		    *(PULONG) irp->AssociatedIrp.SystemBuffer =
		      device_extension->media_change_count;

		    irp->IoStatus.Information = sizeof(ULONG);
		  }

		irp->IoStatus.Status = STATUS_SUCCESS;
		break;
	      }

	    case IOCTL_DISK_FORMAT_TRACKS:
	    case IOCTL_DISK_FORMAT_TRACKS_EX:
	      {
		NTSTATUS status =
		  ImDiskFloppyFormat(device_extension, irp);

		if (!NT_SUCCESS(status))
		  {
		    // If indicating that proxy connection died we can do
		    // nothing else but remove this device.
		    if (status == STATUS_CONNECTION_RESET)
		      ImDiskRemoveVirtualDisk(device_object);

		    irp->IoStatus.Status = STATUS_NO_MEDIA_IN_DEVICE;
		    irp->IoStatus.Information = 0;
		    break;
		  }

		irp->IoStatus.Information = 0;
		irp->IoStatus.Status = status;
		break;
	      }

	    case IOCTL_DISK_GROW_PARTITION:
	      {
		NTSTATUS status;
		FILE_END_OF_FILE_INFORMATION new_size;

		new_size.EndOfFile.QuadPart =
		  device_extension->disk_geometry.Cylinders.QuadPart +
		  ((PDISK_GROW_PARTITION) irp->AssociatedIrp.SystemBuffer)->
		  BytesToGrow.QuadPart;

		if (device_extension->vm_disk)
		  {
		    ULONG max_size = new_size.EndOfFile.LowPart;
		    PVOID new_image_buffer = NULL;

		    // A vm type disk cannot be extened to a larger size than
		    // 2 GB.
		    if (new_size.EndOfFile.QuadPart & 0xFFFFFFFF80000000)
		      {
			irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
			irp->IoStatus.Information = 0;
			break;
		      }

		    status = ZwAllocateVirtualMemory(NtCurrentProcess(),
						     &new_image_buffer,
						     0,
						     &max_size,
						     MEM_COMMIT,
						     PAGE_READWRITE);

		    if (!NT_SUCCESS(status))
		      {
			irp->IoStatus.Status = status;
			irp->IoStatus.Information = 0;
			break;
		      }

		    RtlCopyMemory
		      (new_image_buffer,
		       device_extension->image_buffer,
		       device_extension->disk_geometry.Cylinders.LowPart);

		    max_size = 0;
		    ZwFreeVirtualMemory(NtCurrentProcess(),
					&device_extension->image_buffer,
					&max_size, MEM_RELEASE);

		    device_extension->image_buffer = new_image_buffer;
		    device_extension->disk_geometry.Cylinders =
		      new_size.EndOfFile;

		    irp->IoStatus.Information = 0;
		    irp->IoStatus.Status = STATUS_SUCCESS;
		    break;
		  }

		// For proxy-type disks and file-type disks with offset the
		// new size is just accepted and that's it.
		if ((device_extension->use_proxy) |
		    (device_extension->image_offset.QuadPart != 0))
		  {
		    device_extension->disk_geometry.Cylinders =
		      new_size.EndOfFile;
	    
		    irp->IoStatus.Information = 0;
		    irp->IoStatus.Status = STATUS_SUCCESS;
		    break;
		  }

		// For other, fixed file-backed disks we need to adjust the
		// physical filesize.

		status = ZwSetInformationFile(device_extension->file_handle,
					      &irp->IoStatus,
					      &new_size,
					      sizeof new_size,
					      FileEndOfFileInformation);

		if (NT_SUCCESS(status))
		  device_extension->disk_geometry.Cylinders =
		    new_size.EndOfFile;

		irp->IoStatus.Information = 0;
		irp->IoStatus.Status = status;
		break;
	      }

	    default:
	      irp->IoStatus.Status = STATUS_DRIVER_INTERNAL_ERROR;
	    }
	  break;

	default:
	  irp->IoStatus.Status = STATUS_DRIVER_INTERNAL_ERROR;
	}

      IoCompleteRequest(irp,
			NT_SUCCESS(irp->IoStatus.Status) ?
			IO_DISK_INCREMENT : IO_NO_INCREMENT);
    }
}

NTSTATUS
ImDiskSafeReadFile(IN HANDLE FileHandle,
		   OUT PIO_STATUS_BLOCK IoStatusBlock,
		   OUT PVOID Buffer,
		   IN ULONG Length,
		   IN PLARGE_INTEGER Offset)
{
  NTSTATUS status;
  ULONG LengthDone = 0;

  PAGED_CODE();

  ASSERT(FileHandle != NULL);
  ASSERT(IoStatusBlock != NULL);
  ASSERT(Buffer != NULL);

  while (LengthDone < Length)
    {
      ULONG RequestLength = Length - LengthDone;

      for (;;)
	{
	  LARGE_INTEGER RequestOffset;
	  PUCHAR InterBuffer = ExAllocatePool(PagedPool, RequestLength);

	  if (InterBuffer == NULL)
	    {
	      KdPrint(("ImDisk: Insufficient paged pool to allocate "
		       "intermediate buffer for ImDiskSafeReadFile() "
		       "(%u bytes).\n", RequestLength));

	      RequestLength >>= 2;
	      continue;
	    }

	  RequestOffset.QuadPart = Offset->QuadPart + LengthDone;

	  status = ZwReadFile(FileHandle,
			      NULL,
			      NULL,
			      NULL,
			      IoStatusBlock,
			      InterBuffer,
			      RequestLength,
			      &RequestOffset,
			      NULL);

	  if ((status == STATUS_INSUFFICIENT_RESOURCES) |
	      (status == STATUS_INVALID_BUFFER_SIZE) |
	      (status == STATUS_INVALID_PARAMETER))
	    {
	      ExFreePool(InterBuffer);

	      RequestLength >>= 2;
	      continue;
	    }

	  if (!NT_SUCCESS(status))
	    {
	      ExFreePool(InterBuffer);
	      break;
	    }

	  RtlCopyMemory((PUCHAR) Buffer + LengthDone, InterBuffer,
			IoStatusBlock->Information);

	  ExFreePool(InterBuffer);
	  break;
	}

      if (!NT_SUCCESS(status))
	{
	  IoStatusBlock->Status = status;
	  IoStatusBlock->Information = LengthDone;
	  return IoStatusBlock->Status;
	}

      if (IoStatusBlock->Information == 0)
	{
	  IoStatusBlock->Status = STATUS_CONNECTION_RESET;
	  IoStatusBlock->Information = LengthDone;
	  return IoStatusBlock->Status;
	}

      LengthDone += IoStatusBlock->Information;
    }

  IoStatusBlock->Status = STATUS_SUCCESS;
  IoStatusBlock->Information = LengthDone;
  return IoStatusBlock->Status;
}

NTSTATUS
ImDiskSafeIOStream(IN PFILE_OBJECT FileObject,
		   IN UCHAR MajorFunction,
		   OUT PIO_STATUS_BLOCK IoStatusBlock,
		   OUT PVOID Buffer,
		   IN ULONG Length)
{
  NTSTATUS status;
  ULONG length_done = 0;
  KEVENT event;
  PIO_STACK_LOCATION io_stack;
  LARGE_INTEGER offset = { 0 };

  PAGED_CODE();

  KdPrint2(("ImDiskSafeIOStream: FileObject=%#x, MajorFunction=%#x, IoStatusBlock=%#x, Buffer=%#x, Length=%#x.\n", FileObject, MajorFunction, IoStatusBlock, Buffer, Length));

  ASSERT(FileObject != NULL);
  ASSERT(IoStatusBlock != NULL);
  ASSERT(Buffer != NULL);

  KeInitializeEvent(&event,
		    NotificationEvent,
		    FALSE);

  while (length_done < Length)
    {
      ULONG RequestLength = Length - length_done;

      do
	{
	  PIRP irp;

	  KdPrint2(("ImDiskSafeIOStream: Building IRP...\n"));

	  irp = IoBuildSynchronousFsdRequest(MajorFunction,
					     FileObject->DeviceObject,
					     (PUCHAR) Buffer + length_done,
					     RequestLength,
					     &offset,
					     &event,
					     IoStatusBlock);

	  if (irp == NULL)
	    {
	      KdPrint(("ImDiskSafeIOStream: Error building IRP.\n"));

	      IoStatusBlock->Status = STATUS_INSUFFICIENT_RESOURCES;
	      IoStatusBlock->Information = length_done;
	      return IoStatusBlock->Status;
	    }

	  KdPrint2(("ImDiskSafeIOStream: Built IRP=%#x.\n", irp));

	  io_stack = IoGetNextIrpStackLocation(irp);
	  io_stack->FileObject = FileObject;
	  io_stack->DeviceObject = FileObject->DeviceObject;

	  KdPrint2(("ImDiskSafeIOStream: MajorFunction= %#x, Length=%#x\n",
		    io_stack->MajorFunction,
		    io_stack->Parameters.Read.Length));

	  KeResetEvent(&event);

	  status = IoCallDriver(io_stack->FileObject->DeviceObject, irp);

	  if (status == STATUS_PENDING)
	    KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
	  else if (!NT_SUCCESS(status))
	    break;

	  status = IoStatusBlock->Status;

	  KdPrint2(("ImDiskSafeIOStream: IRP %#x completed. Status=%#x.\n",
		    irp, IoStatusBlock->Status));

	  RequestLength >>= 1;
	}
      while ((status == STATUS_INVALID_BUFFER_SIZE) |
	     (status == STATUS_INVALID_PARAMETER));

      if (!NT_SUCCESS(status))
	{
	  KdPrint2(("ImDiskSafeIOStream: I/O failed. Status=%#x.\n", status));

	  IoStatusBlock->Status = status;
	  IoStatusBlock->Information = 0;
	  return IoStatusBlock->Status;
	}

      KdPrint2(("ImDiskSafeIOStream: I/O done. Status=%#x. Length=%#x\n",
		status, IoStatusBlock->Information));

      if (IoStatusBlock->Information == 0)
	{
	  IoStatusBlock->Status = STATUS_CONNECTION_RESET;
	  IoStatusBlock->Information = 0;
	  return IoStatusBlock->Status;
	}

      length_done += IoStatusBlock->Information;
    }

  KdPrint2(("ImDiskSafeIOStream: I/O complete.\n"));

  IoStatusBlock->Status = STATUS_SUCCESS;
  IoStatusBlock->Information = length_done;
  return IoStatusBlock->Status;
}

///
/// Note that this function when successful replaces the ProxyDevice pointer
/// to point to the connected device object instead of the proxy service pipe.
/// This means that the only reference to the proxy service pipe after calling
/// this function is the original handle to the pipe.
///
NTSTATUS
ImDiskConnectProxy(IN OUT PFILE_OBJECT *ProxyDevice,
		   OUT PIO_STATUS_BLOCK IoStatusBlock,
		   IN ULONG Flags,
		   IN PWSTR ConnectionString,
		   IN ULONG ConnectionStringLength)
{
  IMDPROXY_CONNECT_REQ connect_req;
  IMDPROXY_CONNECT_RESP connect_resp;
  NTSTATUS status;

  PAGED_CODE();

  ASSERT(ProxyDevice != NULL);
  ASSERT(IoStatusBlock != NULL);
  ASSERT(ConnectionString != NULL);

  connect_req.request_code = IMDPROXY_REQ_CONNECT;
  connect_req.flags = Flags;
  connect_req.length = ConnectionStringLength;

  KdPrint(("ImDisk Proxy Client: Sending IMDPROXY_CONNECT_REQ.\n"));

  status = ImDiskSafeIOStream(*ProxyDevice,
			      IRP_MJ_WRITE,
			      IoStatusBlock,
			      &connect_req,
			      sizeof(connect_req));

  if (!NT_SUCCESS(status))
    {
      IoStatusBlock->Status = status;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  KdPrint(("ImDisk Proxy Client: Sending connection string.\n"));

  status = ImDiskSafeIOStream(*ProxyDevice,
			      IRP_MJ_WRITE,
			      IoStatusBlock,
			      ConnectionString,
			      ConnectionStringLength);

  if (!NT_SUCCESS(status))
    {
      IoStatusBlock->Status = status;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  KdPrint(("ImDisk Proxy Client: Sent all connect data.\n"));

  status = ImDiskSafeIOStream(*ProxyDevice,
			      IRP_MJ_READ,
			      IoStatusBlock,
			      &connect_resp,
			      sizeof(IMDPROXY_CONNECT_RESP));

  if (!NT_SUCCESS(status))
    {
      IoStatusBlock->Status = status;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  if (connect_resp.error_code != 0)
    {
      IoStatusBlock->Status = STATUS_CONNECTION_REFUSED;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  // If the proxy gave us a reference to an object to use for direct connection
  // to the server we have to change the active reference to use here.
  if (connect_resp.object_ptr != 0)
    {
      ObDereferenceObject(*ProxyDevice);
      *ProxyDevice = (PFILE_OBJECT)(ULONG) connect_resp.object_ptr;
    }

  KdPrint(("ImDisk Proxy Client: Got ok response IMDPROXY_CONNECT_RESP.\n"));

  IoStatusBlock->Status = STATUS_SUCCESS;
  IoStatusBlock->Information = 0;
  return IoStatusBlock->Status;
}

NTSTATUS
ImDiskQueryInformationProxy(IN PFILE_OBJECT ProxyDevice,
			    OUT PIO_STATUS_BLOCK IoStatusBlock,
			    OUT PIMDPROXY_INFO_RESP ProxyInfoResponse,
			    IN ULONG ProxyInfoResponseLength)
{
  ULONGLONG proxy_req = IMDPROXY_REQ_INFO;
  NTSTATUS status;

  PAGED_CODE();

  ASSERT(ProxyDevice != NULL);
  ASSERT(IoStatusBlock != NULL);

  if ((ProxyInfoResponse == NULL) |
      (ProxyInfoResponseLength < sizeof(IMDPROXY_INFO_RESP)))
    {
      IoStatusBlock->Status = STATUS_BUFFER_OVERFLOW;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  KdPrint(("ImDisk Proxy Client: Sending IMDPROXY_REQ_INFO.\n"));

  status = ImDiskSafeIOStream(ProxyDevice,
			      IRP_MJ_WRITE,
			      IoStatusBlock,
			      &proxy_req,
			      sizeof(proxy_req));

  if (!NT_SUCCESS(status))
    {
      IoStatusBlock->Status = status;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  KdPrint(("ImDisk Proxy Client: Sent IMDPROXY_REQ_INFO.\n"));

  status = ImDiskSafeIOStream(ProxyDevice,
			      IRP_MJ_READ,
			      IoStatusBlock,
			      ProxyInfoResponse,
			      sizeof(IMDPROXY_INFO_RESP));

  if (!NT_SUCCESS(status))
    {
      IoStatusBlock->Status = status;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  KdPrint(("ImDisk Proxy Client: Got ok response IMDPROXY_INFO_RESP.\n"));

  if (ProxyInfoResponse->req_alignment - 1 > FILE_512_BYTE_ALIGNMENT)
    {
      KdPrint(("ImDisk IMDPROXY_INFO_RESP: Unsupported sizes. "
	       "Got %p-%p size and %p-%p alignment.\n",
	       ProxyInfoResponse->file_size,
	       ProxyInfoResponse->req_alignment));

      IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  IoStatusBlock->Status = STATUS_SUCCESS;
  IoStatusBlock->Information = 0;
  return IoStatusBlock->Status;
}

NTSTATUS
ImDiskReadProxy(IN PFILE_OBJECT ProxyDevice,
		OUT PIO_STATUS_BLOCK IoStatusBlock,
		OUT PVOID Buffer,
		IN ULONG Length,
		IN PLARGE_INTEGER ByteOffset)
{
  IMDPROXY_READ_REQ read_req;
  IMDPROXY_READ_RESP read_resp;
  NTSTATUS status;

  PAGED_CODE();

  ASSERT(ProxyDevice != NULL);
  ASSERT(IoStatusBlock != NULL);
  ASSERT(Buffer != NULL);
  ASSERT(ByteOffset != NULL);

  read_req.request_code = IMDPROXY_REQ_READ;
  read_req.offset = ByteOffset->QuadPart;
  read_req.length = Length;

  KdPrint2(("ImDisk Proxy Client: IMDPROXY_REQ_READ %u bytes at %u.\n",
	    (ULONG) read_req.length, (ULONG) read_req.offset));

  status = ImDiskSafeIOStream(ProxyDevice,
			      IRP_MJ_WRITE,
			      IoStatusBlock,
			      &read_req,
			      sizeof(read_req));

  if (!NT_SUCCESS(status))
    {
      IoStatusBlock->Status = STATUS_IO_DEVICE_ERROR;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  KdPrint2(("ImDisk Proxy Client: IMDPROXY_REQ_READ sent. Waiting IMDPROXY_REQ_RESP.\n"));

  status = ImDiskSafeIOStream(ProxyDevice,
			      IRP_MJ_READ,
			      IoStatusBlock,
			      &read_resp,
			      sizeof(read_resp));

  if (!NT_SUCCESS(status))
    {
      IoStatusBlock->Status = STATUS_IO_DEVICE_ERROR;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  if (read_resp.errorno != 0)
    {
      KdPrint(("ImDisk Proxy Client: Server returned error %p-%p.\n",
	       read_resp.errorno));
      IoStatusBlock->Status = STATUS_IO_DEVICE_ERROR;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  if (read_resp.length != Length)
    {
      KdPrint(("ImDisk Proxy Client: IMDPROXY_REQ_READ %u bytes, "
	       "IMDPROXY_RESP_READ %u bytes.\n",
	       Length, (ULONG) read_resp.length));
 
      /* No longer a fatal error...
      IoStatusBlock->Status = STATUS_IO_DEVICE_ERROR;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
      */
    }
  
  KdPrint2
    (("ImDisk Proxy Client: Got ok response. Waiting for data stream.\n"));

  status = ImDiskSafeIOStream(ProxyDevice,
			      IRP_MJ_READ,
			      IoStatusBlock,
			      Buffer,
			      (ULONG) read_resp.length);

  if (!NT_SUCCESS(status))
    KdPrint(("ImDisk Proxy Client: Data stream of %u bytes received with I/O "
	     "status %#x. Status returned by stream reader is %#x.\n",
	     IoStatusBlock->Information, IoStatusBlock->Status, status));

  KdPrint2
    (("ImDisk Proxy Client: Received %u byte data stream.\n",
      IoStatusBlock->Information));

  return status;
}

NTSTATUS
ImDiskWriteProxy(IN PFILE_OBJECT ProxyDevice,
		 OUT PIO_STATUS_BLOCK IoStatusBlock,
		 IN PVOID Buffer,
		 IN ULONG Length,
		 IN PLARGE_INTEGER ByteOffset)
{
  IMDPROXY_READ_REQ write_req;
  IMDPROXY_READ_RESP write_resp;
  NTSTATUS status;

  PAGED_CODE();

  ASSERT(ProxyDevice != NULL);
  ASSERT(IoStatusBlock != NULL);
  ASSERT(Buffer != NULL);
  ASSERT(ByteOffset != NULL);

  write_req.request_code = IMDPROXY_REQ_WRITE;
  write_req.offset = ByteOffset->QuadPart;
  write_req.length = Length;

  KdPrint2(("ImDisk Proxy Client: IMDPROXY_REQ_WRITE %u bytes at %u.\n",
	    (ULONG) write_req.length, (ULONG) write_req.offset));

  status = ImDiskSafeIOStream(ProxyDevice,
			      IRP_MJ_WRITE,
			      IoStatusBlock,
			      &write_req,
			      sizeof(write_req));

  if (!NT_SUCCESS(status))
    {
      IoStatusBlock->Status = STATUS_IO_DEVICE_ERROR;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  KdPrint2
    (("ImDisk Proxy Client: IMDPROXY_REQ_WRITE sent. Sending data stream.\n"));

  status = ImDiskSafeIOStream(ProxyDevice,
			      IRP_MJ_WRITE,
			      IoStatusBlock,
			      Buffer,
			      (ULONG) write_req.length);

  if (!NT_SUCCESS(status))
    {
      KdPrint(("ImDisk Proxy Client: Data stream send failed. "
	       "Sent %u bytes with I/O status %#x.\n",
	       IoStatusBlock->Information, IoStatusBlock->Status));

      IoStatusBlock->Status = STATUS_IO_DEVICE_ERROR;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  KdPrint2
    (("ImDisk Proxy Client: Data stream of %u bytes sent with I/O status %#x. "
      "Status returned by stream writer is %#x. "
      "Waiting for IMDPROXY_RESP_WRITE.\n",
      IoStatusBlock->Information, IoStatusBlock->Status, status));

  status = ImDiskSafeIOStream(ProxyDevice,
			      IRP_MJ_READ,
			      IoStatusBlock,
			      &write_resp,
			      sizeof(write_resp));

  if (!NT_SUCCESS(status))
    {
      IoStatusBlock->Status = STATUS_IO_DEVICE_ERROR;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  if (write_resp.errorno != 0)
    {
      KdPrint(("ImDisk Proxy Client: Server returned error %p-%p.\n",
	       write_resp.errorno));
      IoStatusBlock->Status = STATUS_IO_DEVICE_ERROR;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }

  if (write_resp.length != Length)
    {
      KdPrint(("ImDisk Proxy Client: IMDPROXY_REQ_WRITE %u bytes, "
	       "IMDPROXY_RESP_WRITE %u bytes.\n",
	       Length, (ULONG) write_resp.length));
      IoStatusBlock->Status = STATUS_IO_DEVICE_ERROR;
      IoStatusBlock->Information = 0;
      return IoStatusBlock->Status;
    }
  
  KdPrint2(("ImDisk Proxy Client: Got ok response. "
	    "Resetting IoStatusBlock fields.\n"));

  IoStatusBlock->Status = STATUS_SUCCESS;
  IoStatusBlock->Information = Length;
  return IoStatusBlock->Status;
}

//
//	Format tracks
//	Actually, just fills specified range of tracks with fill characters
//
NTSTATUS
ImDiskFloppyFormat(IN PDEVICE_EXTENSION Extension,
		   IN PIRP Irp)
{
  PFORMAT_PARAMETERS	param;
  ULONG			track_length;
  PUCHAR		format_buffer;
  LARGE_INTEGER		start_offset;
  LARGE_INTEGER		end_offset;
  NTSTATUS		status;

  PAGED_CODE();

  ASSERT(Extension != NULL);
  ASSERT(Irp != NULL);

  param = (PFORMAT_PARAMETERS) Irp->AssociatedIrp.SystemBuffer;

  track_length =
    Extension->disk_geometry.BytesPerSector *
    Extension->disk_geometry.SectorsPerTrack;

  start_offset.QuadPart =
    param->StartCylinderNumber * Extension->disk_geometry.TracksPerCylinder *
    track_length + param->StartHeadNumber * track_length;

  end_offset.QuadPart =
    param->EndCylinderNumber * Extension->disk_geometry.TracksPerCylinder *
    track_length + param->EndHeadNumber * track_length;

  if (Extension->vm_disk)
    {
      LARGE_INTEGER wait_time;

      RtlFillMemory(((PUCHAR) Extension->image_buffer) + start_offset.LowPart,
		    end_offset.LowPart - start_offset.LowPart + track_length,
		    MEDIA_FORMAT_FILL_DATA);

      wait_time.QuadPart = -1;
      KeDelayExecutionThread(KernelMode, FALSE, &wait_time);

      Irp->IoStatus.Information = 0;
      return STATUS_SUCCESS;
    }

  start_offset.QuadPart += Extension->image_offset.QuadPart;
  end_offset.QuadPart += Extension->image_offset.QuadPart;

  format_buffer = ExAllocatePool(PagedPool, track_length);

  if (format_buffer == NULL)
    {
      Irp->IoStatus.Information = 0;
      return STATUS_INSUFFICIENT_RESOURCES;
    }

  RtlFillMemory(format_buffer, track_length, MEDIA_FORMAT_FILL_DATA);

  do
    {
      if (Extension->use_proxy)
	status = ImDiskWriteProxy(Extension->proxy_device,
				  &Irp->IoStatus,
				  format_buffer,
				  track_length,
				  &start_offset);
      else
	status = ZwWriteFile(Extension->file_handle,
			     NULL,
			     NULL,
			     NULL,
			     &Irp->IoStatus,
			     format_buffer,
			     track_length,
			     &start_offset,
			     NULL);

      if (!NT_SUCCESS(status))
	{
	  KdPrint(("ImDisk Format failed: Write failed with status %#x.\n",
		   status));

	  break;
	}

      start_offset.QuadPart += track_length;
    }
  while (start_offset.QuadPart <= end_offset.QuadPart);

  ExFreePool(format_buffer);

  Irp->IoStatus.Information = 0;

  return status;
}
