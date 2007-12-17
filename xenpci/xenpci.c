/*
PV Drivers for Windows Xen HVM Domains
Copyright (C) 2007 James Harper

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "xenpci.h"
#include "hypercall.h"
#include <stdlib.h>

#define SHUTDOWN_PATH "control/shutdown"
#define BALLOON_PATH "memory/target"

DRIVER_INITIALIZE DriverEntry;
static NTSTATUS
XenPCI_AddDevice(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit);
static NTSTATUS
XenPCI_PrepareHardware(WDFDEVICE hDevice, WDFCMRESLIST Resources, WDFCMRESLIST ResourcesTranslated);
static NTSTATUS
XenPCI_ReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated);
static NTSTATUS
XenPCI_D0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState);
static NTSTATUS
XenPCI_D0EntryPostInterruptsEnabled(WDFDEVICE  Device, WDF_POWER_DEVICE_STATE PreviousState);
static NTSTATUS
XenPCI_D0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState);
static NTSTATUS
XenPCI_D0ExitPreInterruptsDisabled(WDFDEVICE  Device, WDF_POWER_DEVICE_STATE TargetState);
static VOID
XenPCI_IoDefault(WDFQUEUE Queue, WDFREQUEST Request);
static NTSTATUS
XenPCI_InterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice);
static NTSTATUS
XenPCI_InterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice);
static NTSTATUS
XenPCI_ChildListCreateDevice(WDFCHILDLIST ChildList, PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription, PWDFDEVICE_INIT ChildInit);
static NTSTATUS
XenPCI_DeviceResourceRequirementsQuery(WDFDEVICE Device, WDFIORESREQLIST IoResourceRequirementsList);
static NTSTATUS
XenPCI_FilterRemoveResourceRequirements(WDFDEVICE Device, WDFIORESREQLIST IoResourceRequirementsList);
static NTSTATUS
XenPCI_FilterAddResourceRequirements(WDFDEVICE Device, WDFIORESREQLIST RequirementsList);
static NTSTATUS
XenPCI_RemoveAddedResources(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw, WDFCMRESLIST ResourcesTranslated);

static VOID
XenBus_ShutdownHandler(char *Path, PVOID Data);
static VOID
XenBus_BalloonHandler(char *Path, PVOID Data);
static VOID
XenPCI_XenBusWatchHandler(char *Path, PVOID Data);

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, XenPCI_AddDevice)
#endif

/* Global (driver-wide) variables */
static BOOLEAN AutoEnumerate;
CM_PARTIAL_RESOURCE_DESCRIPTOR InterruptRaw;
CM_PARTIAL_RESOURCE_DESCRIPTOR InterruptTranslated;

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
  WDF_DRIVER_CONFIG config;
  NTSTATUS status;
  UNICODE_STRING RegKeyName;
  UNICODE_STRING RegValueName;
  HANDLE RegHandle;
  OBJECT_ATTRIBUTES RegObjectAttributes;
  char Buf[200];
  ULONG BufLen = 200;
  PKEY_VALUE_PARTIAL_INFORMATION KeyPartialValue;
  int State = 0;
  int StartPos = 0;
  WCHAR *SystemStartOptions;
  size_t SystemStartOptionsLen;
  size_t i;

  KdPrint((__DRIVER_NAME " --> DriverEntry\n"));

  RtlInitUnicodeString(&RegKeyName, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control");
  InitializeObjectAttributes(&RegObjectAttributes, &RegKeyName, OBJ_CASE_INSENSITIVE, NULL, NULL);
  status = ZwOpenKey(&RegHandle, KEY_READ, &RegObjectAttributes);
  if(!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME "     ZwOpenKey returned %08x\n", status));
  }

  RtlInitUnicodeString(&RegValueName, L"SystemStartOptions");
  status = ZwQueryValueKey(RegHandle, &RegValueName, KeyValuePartialInformation, Buf, BufLen, &BufLen);
  if(!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME "     ZwQueryKeyValue returned %08x\n", status));
  }
  //KdPrint((__DRIVER_NAME "     BufLen = %d\n", BufLen));
  KeyPartialValue = (PKEY_VALUE_PARTIAL_INFORMATION)Buf;
  KdPrint((__DRIVER_NAME "     Buf = %ws\n", KeyPartialValue->Data));
  SystemStartOptions = (WCHAR *)KeyPartialValue->Data;

  AutoEnumerate = FALSE;

  RtlStringCbLengthW(SystemStartOptions, KeyPartialValue->DataLength, &SystemStartOptionsLen);

  for (i = 0; i <= SystemStartOptionsLen/2; i++)
  {
    //KdPrint((__DRIVER_NAME "     pos = %d, state = %d, char = '%wc' (%d)\n", i, State, SystemStartOptions[i], SystemStartOptions[i]));
    
    switch (State)
    {
    case 0:
      if (SystemStartOptions[i] == L'G')
      {
        StartPos = i;
        State = 2;
      } else if (SystemStartOptions[i] != L' ')
      {
        State = 1;
      }
      break;
    case 1:
      if (SystemStartOptions[i] == L' ')
        State = 0;
      break;
    case 2:
      if (SystemStartOptions[i] == L'P')
        State = 3;
      else
        State = 0;
      break;
    case 3:
      if (SystemStartOptions[i] == L'L')
        State = 4;
      else
        State = 0;
      break;
    case 4:
      if (SystemStartOptions[i] == L'P')
        State = 5;
      else
        State = 0;
      break;
    case 5:
      if (SystemStartOptions[i] == L'V')
        State = 6;
      else
        State = 0;
      break;
    case 6:
      if (SystemStartOptions[i] == L' ' || SystemStartOptions[i] == 0)
        AutoEnumerate = TRUE;
      State = 0;
      break;
    }
  }

  KdPrint((__DRIVER_NAME "     AutoEnumerate = %d\n", AutoEnumerate));

  WDF_DRIVER_CONFIG_INIT(&config, XenPCI_AddDevice);
  status = WdfDriverCreate(
                      DriverObject,
                      RegistryPath,
                      WDF_NO_OBJECT_ATTRIBUTES,
                      &config,
                      WDF_NO_HANDLE);
  if(!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME " WdfDriverCreate failed with status 0x%08x\n", status));
  }

  KdPrint((__DRIVER_NAME " <-- DriverEntry\n"));

  return status;
}

static NTSTATUS
get_hypercall_stubs(WDFDEVICE Device)
{
  PXENPCI_DEVICE_DATA xpdd = GetDeviceData(Device);
  DWORD32 cpuid_output[4];
  char xensig[13];
  ULONG i;
  ULONG pages;
  ULONG msr;  

  __cpuid(cpuid_output, 0x40000000);
  *(ULONG*)(xensig + 0) = cpuid_output[1];
  *(ULONG*)(xensig + 4) = cpuid_output[2];
  *(ULONG*)(xensig + 8) = cpuid_output[3];
  xensig[12] = '\0';
  KdPrint((__DRIVER_NAME " Xen Signature = %s, EAX = 0x%08x\n", xensig, cpuid_output[0]));

  __cpuid(cpuid_output, 0x40000002);
  pages = cpuid_output[0];
  msr = cpuid_output[1];
  //KdPrint((__DRIVER_NAME " Hypercall area is %u pages.\n", pages));

  xpdd->hypercall_stubs = ExAllocatePoolWithTag(NonPagedPool, pages * PAGE_SIZE, XENPCI_POOL_TAG);
  //KdPrint((__DRIVER_NAME " Hypercall area at %08x\n", hypercall_stubs));

  if (!xpdd->hypercall_stubs)
    return 1;
  for (i = 0; i < pages; i++) {
    ULONG pfn;
    //pfn = vmalloc_to_pfn((char *)hypercall_stubs + i * PAGE_SIZE);
    pfn = (ULONG)(MmGetPhysicalAddress(xpdd->hypercall_stubs + i * PAGE_SIZE).QuadPart >> PAGE_SHIFT);
    //KdPrint((__DRIVER_NAME " pfn = %08X\n", pfn));
    __writemsr(msr, ((ULONGLONG)pfn << PAGE_SHIFT) + i);
  }
  return STATUS_SUCCESS;
}

PHYSICAL_ADDRESS
XenPCI_AllocMMIO(WDFDEVICE Device, ULONG len)
{
  PXENPCI_DEVICE_DATA xpdd = GetDeviceData(Device);

  PHYSICAL_ADDRESS addr;

  addr = xpdd->platform_mmio_addr;
  addr.QuadPart += xpdd->platform_mmio_alloc;
  xpdd->platform_mmio_alloc += len;

  return addr;
}

static int
init_xen_info(WDFDEVICE Device)
{
  PXENPCI_DEVICE_DATA xpdd = GetDeviceData(Device);
  struct xen_add_to_physmap xatp;
  int ret;
  PHYSICAL_ADDRESS shared_info_area_unmapped;

  //setup_xen_features();
  //KdPrint((__DRIVER_NAME " init_xen_info Hypercall area at %08x\n", hypercall_stubs));

  shared_info_area_unmapped = XenPCI_AllocMMIO(Device, PAGE_SIZE);
  xatp.domid = DOMID_SELF;
  xatp.idx = 0;
  xatp.space = XENMAPSPACE_shared_info;
  xatp.gpfn = (xen_pfn_t)(shared_info_area_unmapped.QuadPart >> PAGE_SHIFT);
  ret = HYPERVISOR_memory_op(Device, XENMEM_add_to_physmap, &xatp);
  //KdPrint((__DRIVER_NAME " ret = %d\n", ret));

  xpdd->shared_info_area = MmMapIoSpace(shared_info_area_unmapped,
    PAGE_SIZE, MmNonCached);

  return 0;
}

static int
set_callback_irq(WDFDEVICE Device, ULONGLONG irq)
{
  struct xen_hvm_param a;
  int retval;

  //KdPrint((__DRIVER_NAME " --> set_callback_irq\n"));
  a.domid = DOMID_SELF;
  a.index = HVM_PARAM_CALLBACK_IRQ;
  a.value = irq;
  retval = HYPERVISOR_hvm_op(Device, HVMOP_set_param, &a);
  //KdPrint((__DRIVER_NAME " HYPERVISOR_hvm_op retval = %d\n", retval));
  //KdPrint((__DRIVER_NAME " <-- set_callback_irq\n"));
  return retval;
}

static NTSTATUS
XenPCI_AddDevice(
    IN WDFDRIVER Driver,
    IN PWDFDEVICE_INIT DeviceInit
    )
{
  NTSTATUS status;
  WDF_CHILD_LIST_CONFIG config;
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
  WDF_INTERRUPT_CONFIG interruptConfig;
  PNP_BUS_INFORMATION busInfo;
  BUS_INTERFACE_STANDARD BusInterface;
  WDFDEVICE Device;
  PXENPCI_DEVICE_DATA xpdd;

  UNREFERENCED_PARAMETER(Driver);

  KdPrint((__DRIVER_NAME " --> DeviceAdd\n"));

  // get PDO
  // get parent (should be PCI bus) (WdfPdoGetParent)
  // 

  WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
  WDF_CHILD_LIST_CONFIG_INIT(&config, sizeof(XENPCI_IDENTIFICATION_DESCRIPTION), XenPCI_ChildListCreateDevice);
  WdfFdoInitSetDefaultChildListConfig(DeviceInit, &config, WDF_NO_OBJECT_ATTRIBUTES);

//  WDF_FDO_EVENT_CALLBACKS_INIT(&FdoCallbacks);
//  FdoCallbacks.EvtDeviceFilterRemoveResourceRequirements = XenPCI_FilterRemoveResourceRequirements;
//  FdoCallbacks.EvtDeviceFilterAddResourceRequirements = XenPCI_FilterAddResourceRequirements;
//  FdoCallbacks.EvtDeviceRemoveAddedResources = XenPCI_RemoveAddedResources;
//  WdfFdoInitSetEventCallbacks(DeviceInit, &FdoCallbacks);

  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
  pnpPowerCallbacks.EvtDevicePrepareHardware = XenPCI_PrepareHardware;
  pnpPowerCallbacks.EvtDeviceReleaseHardware = XenPCI_ReleaseHardware;
  pnpPowerCallbacks.EvtDeviceD0Entry = XenPCI_D0Entry;
  pnpPowerCallbacks.EvtDeviceD0EntryPostInterruptsEnabled
    = XenPCI_D0EntryPostInterruptsEnabled;
  pnpPowerCallbacks.EvtDeviceD0ExitPreInterruptsDisabled
    = XenPCI_D0ExitPreInterruptsDisabled;
  pnpPowerCallbacks.EvtDeviceD0Exit = XenPCI_D0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

  WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

  /*initialize storage for the device context*/
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, XENPCI_DEVICE_DATA);

  /*create a device instance.*/
  status = WdfDeviceCreate(&DeviceInit, &attributes, &Device);  
  if(!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME " WdfDeviceCreate failed with status 0x%08x\n", status));
    return status;
  }
  xpdd = GetDeviceData(Device);
  xpdd->Device = Device;

  WdfDeviceSetSpecialFileSupport(Device, WdfSpecialFilePaging, TRUE);
  WdfDeviceSetSpecialFileSupport(Device, WdfSpecialFileHibernation, TRUE);
  WdfDeviceSetSpecialFileSupport(Device, WdfSpecialFileDump, TRUE);

  status = WdfFdoQueryForInterface(Device, &GUID_BUS_INTERFACE_STANDARD, (PINTERFACE) &BusInterface, sizeof(BUS_INTERFACE_STANDARD), 1, NULL);
  if(!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME " WdfFdoQueryForInterface (BusInterface) failed with status 0x%08x\n", status));
  }

  busInfo.BusTypeGuid = GUID_XENPCI_DEVCLASS;
  busInfo.LegacyBusType = Internal; //PNPBus;
  busInfo.BusNumber = 0;

  WdfDeviceSetBusInformationForChildren(Device, &busInfo);

  WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, EvtChn_Interrupt, NULL); //EvtChn_InterruptDpc);
  interruptConfig.EvtInterruptEnable = XenPCI_InterruptEnable;
  interruptConfig.EvtInterruptDisable = XenPCI_InterruptDisable;
  status = WdfInterruptCreate(Device, &interruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &xpdd->XenInterrupt);
  if (!NT_SUCCESS (status))
  {
    KdPrint((__DRIVER_NAME "WdfInterruptCreate failed 0x%08x\n", status));
    return status;
  }

  KdPrint((__DRIVER_NAME " <-- DeviceAdd\n"));
  return status;
}

static NTSTATUS
XenPCI_PrepareHardware(
  IN WDFDEVICE    Device,
  IN WDFCMRESLIST ResourceList,
  IN WDFCMRESLIST ResourceListTranslated)
{
  NTSTATUS status = STATUS_SUCCESS;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;
  ULONG i;
  PXENPCI_DEVICE_DATA xpdd = GetDeviceData(Device);

  KdPrint((__DRIVER_NAME " --> EvtDevicePrepareHardware\n"));

  for (i = 0; i < WdfCmResourceListGetCount(ResourceList); i++)
  {
    descriptor = WdfCmResourceListGetDescriptor(ResourceList, i);
    if(!descriptor)
      continue;
    switch (descriptor->Type)
    {
    case CmResourceTypeInterrupt:
      xpdd->irqNumber = descriptor->u.Interrupt.Vector;
      break;
    }
  }

  //KdPrint((__DRIVER_NAME " GSI = %d\n", irqNumber));

  //KdPrint((__DRIVER_NAME " ResourceListTranslated\n"));
  for (i = 0; i < WdfCmResourceListGetCount(ResourceListTranslated); i++)
  {
    descriptor = WdfCmResourceListGetDescriptor(ResourceListTranslated, i);
    if(!descriptor)
    {
      KdPrint((__DRIVER_NAME " --> EvtDevicePrepareHardware (No descriptor)\n"));
      return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    switch (descriptor->Type) {
    case CmResourceTypePort:
      //KdPrint((__DRIVER_NAME "     I/O mapped CSR: (%x) Length: (%d)\n", descriptor->u.Port.Start.LowPart, descriptor->u.Port.Length));
      break;
    case CmResourceTypeMemory:
      KdPrint((__DRIVER_NAME "     Memory mapped CSR:(%x:%x) Length:(%d)\n", descriptor->u.Memory.Start.LowPart, descriptor->u.Memory.Start.HighPart, descriptor->u.Memory.Length));
      xpdd->platform_mmio_addr = descriptor->u.Memory.Start; //(ULONG)MmMapIoSpace(descriptor->u.Memory.Start, descriptor->u.Memory.Length, MmNonCached);
      xpdd->platform_mmio_len = descriptor->u.Memory.Length;
      xpdd->platform_mmio_alloc = 0;
      break;
    case CmResourceTypeInterrupt:
      //KdPrint((__DRIVER_NAME "     Interrupt level: 0x%0x, Vector: 0x%0x\n", descriptor->u.Interrupt.Level, descriptor->u.Interrupt.Vector));
      memcpy(&InterruptRaw, WdfCmResourceListGetDescriptor(ResourceList, i), sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));
      memcpy(&InterruptTranslated, WdfCmResourceListGetDescriptor(ResourceListTranslated, i), sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));
      break;
    case CmResourceTypeDevicePrivate:
      //KdPrint((__DRIVER_NAME "     Private Data: 0x%02x 0x%02x 0x%02x\n", descriptor->u.DevicePrivate.Data[0], descriptor->u.DevicePrivate.Data[1], descriptor->u.DevicePrivate.Data[2] ));
      break;
    default:
      //KdPrint((__DRIVER_NAME "     Unhandled resource type (0x%x)\n", descriptor->Type));
      break;
    }
  }

  get_hypercall_stubs(Device);

  init_xen_info(Device);

  GntTbl_Init(Device);

  EvtChn_Init(Device);

  set_callback_irq(Device, xpdd->irqNumber);

  XenBus_Init(Device);

  //KdPrint((__DRIVER_NAME " upcall_pending = %d\n", shared_info_area->vcpu_info[0].evtchn_upcall_pending));

  xpdd->shared_info_area->vcpu_info[0].evtchn_upcall_mask = 0;

  //xen_reboot_init();

  KdPrint((__DRIVER_NAME " <-- EvtDevicePrepareHardware\n"));

  return status;
}

static NTSTATUS
XenPCI_ReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
  UNREFERENCED_PARAMETER(Device);
  UNREFERENCED_PARAMETER(ResourcesTranslated);

  return STATUS_SUCCESS;
}

static NTSTATUS
XenPCI_D0Entry(
    IN WDFDEVICE  Device,
    IN WDF_POWER_DEVICE_STATE PreviousState
    )
{
  NTSTATUS status = STATUS_SUCCESS;

  UNREFERENCED_PARAMETER(Device);
  UNREFERENCED_PARAMETER(PreviousState);

  KdPrint((__DRIVER_NAME " --> EvtDeviceD0Entry\n"));

  KdPrint((__DRIVER_NAME " <-- EvtDeviceD0Entry\n"));

  return status;
}

static NTSTATUS
XenPCI_D0EntryPostInterruptsEnabled(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
  NTSTATUS status = STATUS_SUCCESS;
  //OBJECT_ATTRIBUTES oa;
  char *response;
  char *msgTypes;
  char **Types;
  int i;
  char buffer[128];

  UNREFERENCED_PARAMETER(PreviousState);

  KdPrint((__DRIVER_NAME " --> EvtDeviceD0EntryPostInterruptsEnabled\n"));

  XenBus_Start(Device);

  KdPrint((__DRIVER_NAME "     A\n"));
  
  response = XenBus_AddWatch(Device, XBT_NIL, SHUTDOWN_PATH, XenBus_ShutdownHandler, Device);
  KdPrint((__DRIVER_NAME "     shutdown watch response = '%s'\n", response)); 

  response = XenBus_AddWatch(Device, XBT_NIL, BALLOON_PATH, XenBus_BalloonHandler, Device);
  KdPrint((__DRIVER_NAME "     shutdown watch response = '%s'\n", response)); 

  response = XenBus_AddWatch(Device, XBT_NIL, "device", XenPCI_XenBusWatchHandler, Device);
  KdPrint((__DRIVER_NAME "     device watch response = '%s'\n", response)); 

  msgTypes = XenBus_List(Device, XBT_NIL, "device", &Types);
  if (!msgTypes) {
    for (i = 0; Types[i]; i++)
    {
      RtlStringCbPrintfA(buffer, ARRAY_SIZE(buffer), "device/%s", Types[i]);
      //KdPrint((__DRIVER_NAME "     ls device[%d] -> %s\n", i, Types[i]));
      XenPCI_XenBusWatchHandler(buffer, Device);
      ExFreePoolWithTag(Types[i], XENPCI_POOL_TAG);
    }
  }
  KdPrint((__DRIVER_NAME " <-- EvtDeviceD0EntryPostInterruptsEnabled\n"));

  return status;
}

static NTSTATUS
XenPCI_D0ExitPreInterruptsDisabled(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
  NTSTATUS status = STATUS_SUCCESS;

  UNREFERENCED_PARAMETER(TargetState);

  KdPrint((__DRIVER_NAME " --> D0ExitPreInterruptsDisabled\n"));

  switch (KeGetCurrentIrql())
  {
  case PASSIVE_LEVEL:
    KdPrint((__DRIVER_NAME "     PASSIVE_LEVEL\n"));
    break;
  case APC_LEVEL:
    KdPrint((__DRIVER_NAME "     APC_LEVEL\n"));
    break;
  case DISPATCH_LEVEL:
    KdPrint((__DRIVER_NAME "     DISPATCH_LEVEL\n"));
    break;
  default:
    KdPrint((__DRIVER_NAME "     %d\n", KeGetCurrentIrql()));
    break;
  }

  XenBus_Stop(Device);

  KdPrint((__DRIVER_NAME " <-- D0ExitPreInterruptsDisabled\n"));

  return status;
}

static NTSTATUS
XenPCI_D0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
  NTSTATUS status = STATUS_SUCCESS;

  UNREFERENCED_PARAMETER(Device);
  UNREFERENCED_PARAMETER(TargetState);

  KdPrint((__DRIVER_NAME " --> DeviceD0Exit\n"));

  switch (KeGetCurrentIrql())
  {
  case PASSIVE_LEVEL:
    KdPrint((__DRIVER_NAME "     PASSIVE_LEVEL\n"));
    break;
  case APC_LEVEL:
    KdPrint((__DRIVER_NAME "     APC_LEVEL\n"));
    break;
  case DISPATCH_LEVEL:
    KdPrint((__DRIVER_NAME "     DISPATCH_LEVEL\n"));
    break;
  default:
    KdPrint((__DRIVER_NAME "     %d\n", KeGetCurrentIrql()));
    break;
  }

  XenBus_Close(Device);

  KdPrint((__DRIVER_NAME " <-- DeviceD0Exit\n"));

  return status;
}

static VOID 
XenPCI_IoDefault(
    IN WDFQUEUE  Queue,
    IN WDFREQUEST  Request
    )
{
  UNREFERENCED_PARAMETER(Queue);

  KdPrint((__DRIVER_NAME " --> EvtDeviceIoDefault\n"));

  WdfRequestComplete(Request, STATUS_NOT_IMPLEMENTED);

  KdPrint((__DRIVER_NAME " <-- EvtDeviceIoDefault\n"));
}

static NTSTATUS
XenPCI_InterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
  PXENPCI_DEVICE_DATA xpdd = GetDeviceData(AssociatedDevice);
  UNREFERENCED_PARAMETER(Interrupt);

  KdPrint((__DRIVER_NAME " --> EvtInterruptEnable\n"));

  xpdd->shared_info_area->vcpu_info[0].evtchn_upcall_mask = 0;

  KdPrint((__DRIVER_NAME " <-- EvtInterruptEnable\n"));

  return STATUS_SUCCESS;
}

static NTSTATUS
XenPCI_InterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
  PXENPCI_DEVICE_DATA xpdd = GetDeviceData(AssociatedDevice);
  UNREFERENCED_PARAMETER(Interrupt);

  //KdPrint((__DRIVER_NAME " --> EvtInterruptDisable\n"));

  xpdd->shared_info_area->vcpu_info[0].evtchn_upcall_mask = 1;
  // should we kick off any pending interrupts here?

  //KdPrint((__DRIVER_NAME " <-- EvtInterruptDisable\n"));

  return STATUS_SUCCESS;
}

static NTSTATUS
XenPCI_ChildListCreateDevice(
  WDFCHILDLIST ChildList,
  PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
  PWDFDEVICE_INIT ChildInit)
{
  NTSTATUS status;
  WDFDEVICE ChildDevice = NULL;
  PXENPCI_IDENTIFICATION_DESCRIPTION XenIdentificationDesc;
  DECLARE_UNICODE_STRING_SIZE(buffer, 20);
  WDF_OBJECT_ATTRIBUTES PdoAttributes;
  DECLARE_CONST_UNICODE_STRING(DeviceLocation, L"Xen Bus");
  WDF_QUERY_INTERFACE_CONFIG  qiConfig;
  PXENPCI_XEN_DEVICE_DATA ChildDeviceData = NULL;

  UNREFERENCED_PARAMETER(ChildList);

  //KdPrint((__DRIVER_NAME " --> ChildListCreateDevice\n"));

  XenIdentificationDesc = CONTAINING_RECORD(IdentificationDescription, XENPCI_IDENTIFICATION_DESCRIPTION, Header);

  //KdPrint((__DRIVER_NAME "     Type = %wZ\n", &XenIdentificationDesc->DeviceType));

  //DeviceInit = WdfPdoInitAllocate(Device);
  WdfDeviceInitSetDeviceType(ChildInit, FILE_DEVICE_CONTROLLER);

  status = RtlUnicodeStringPrintf(&buffer, L"Xen\\%wZ\0", &XenIdentificationDesc->DeviceType);
  status = WdfPdoInitAssignDeviceID(ChildInit, &buffer);
  status = WdfPdoInitAddHardwareID(ChildInit, &buffer);
  status = WdfPdoInitAddCompatibleID(ChildInit, &buffer);

  status = RtlUnicodeStringPrintf(&buffer, L"%02d", 0);
  status = WdfPdoInitAssignInstanceID(ChildInit, &buffer);

  status = RtlUnicodeStringPrintf( &buffer, L"%wZ", &XenIdentificationDesc->DeviceType);
  status = WdfPdoInitAddDeviceText(ChildInit, &buffer, &DeviceLocation, 0x409);

  WdfPdoInitSetDefaultLocale(ChildInit, 0x409);
  
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&PdoAttributes, XENPCI_XEN_DEVICE_DATA);

//  WDF_PDO_EVENT_CALLBACKS_INIT(&PdoCallbacks);
//  PdoCallbacks.EvtDeviceResourceRequirementsQuery = XenPCI_DeviceResourceRequirementsQuery;
//  WdfPdoInitSetEventCallbacks(ChildInit, &PdoCallbacks);

  status = WdfDeviceCreate(&ChildInit, &PdoAttributes, &ChildDevice);
  if (!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME "     WdfDeviceCreate status = %08X\n", status));
  }

  WdfDeviceSetSpecialFileSupport(ChildDevice, WdfSpecialFilePaging, TRUE);
  WdfDeviceSetSpecialFileSupport(ChildDevice, WdfSpecialFileHibernation, TRUE);
  WdfDeviceSetSpecialFileSupport(ChildDevice, WdfSpecialFileDump, TRUE);

  ChildDeviceData = GetXenDeviceData(ChildDevice);
  ChildDeviceData->Magic = XEN_DATA_MAGIC;
  ChildDeviceData->AutoEnumerate = AutoEnumerate;
  ChildDeviceData->WatchHandler = NULL;
  strncpy(ChildDeviceData->BasePath, XenIdentificationDesc->Path, 128);
  memcpy(&ChildDeviceData->InterruptRaw, &InterruptRaw, sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));
  memcpy(&ChildDeviceData->InterruptTranslated, &InterruptTranslated, sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));
  
  ChildDeviceData->EvtChnInterface.InterfaceHeader.Size = sizeof(ChildDeviceData->EvtChnInterface);
  ChildDeviceData->EvtChnInterface.InterfaceHeader.Version = 1;
  ChildDeviceData->EvtChnInterface.InterfaceHeader.Context = WdfPdoGetParent(ChildDevice);
  ChildDeviceData->EvtChnInterface.InterfaceHeader.InterfaceReference = WdfDeviceInterfaceReferenceNoOp;
  ChildDeviceData->EvtChnInterface.InterfaceHeader.InterfaceDereference = WdfDeviceInterfaceDereferenceNoOp;
  ChildDeviceData->EvtChnInterface.Bind = EvtChn_Bind;
  ChildDeviceData->EvtChnInterface.Unbind = EvtChn_Unbind;
  ChildDeviceData->EvtChnInterface.Mask = EvtChn_Mask;
  ChildDeviceData->EvtChnInterface.Unmask = EvtChn_Unmask;
  ChildDeviceData->EvtChnInterface.Notify = EvtChn_Notify;
  ChildDeviceData->EvtChnInterface.AllocUnbound = EvtChn_AllocUnbound;
  WDF_QUERY_INTERFACE_CONFIG_INIT(&qiConfig, (PINTERFACE)&ChildDeviceData->EvtChnInterface, &GUID_XEN_IFACE_EVTCHN, NULL);
  status = WdfDeviceAddQueryInterface(ChildDevice, &qiConfig);
  if (!NT_SUCCESS(status))
  {
    return status;
  }

/*
  ChildDeviceData->XenInterface.InterfaceHeader.Size = sizeof(ChildDeviceData->XenInterface);
  ChildDeviceData->XenInterface.InterfaceHeader.Version = 1;
  ChildDeviceData->XenInterface.InterfaceHeader.Context = NULL;
  ChildDeviceData->XenInterface.InterfaceHeader.InterfaceReference = WdfDeviceInterfaceReferenceNoOp;
  ChildDeviceData->XenInterface.InterfaceHeader.InterfaceDereference = WdfDeviceInterfaceDereferenceNoOp;
  ChildDeviceData->XenInterface.AllocMMIO = XenPCI_AllocMMIO;
  WDF_QUERY_INTERFACE_CONFIG_INIT(&qiConfig, (PINTERFACE)&ChildDeviceData->XenInterface, &GUID_XEN_IFACE_XEN, NULL);
  status = WdfDeviceAddQueryInterface(ChildDevice, &qiConfig);
  if (!NT_SUCCESS(status)) {
    return status;
  }
*/

  ChildDeviceData->GntTblInterface.InterfaceHeader.Size = sizeof(ChildDeviceData->GntTblInterface);
  ChildDeviceData->GntTblInterface.InterfaceHeader.Version = 1;
  ChildDeviceData->GntTblInterface.InterfaceHeader.Context = WdfPdoGetParent(ChildDevice);
  ChildDeviceData->GntTblInterface.InterfaceHeader.InterfaceReference = WdfDeviceInterfaceReferenceNoOp;
  ChildDeviceData->GntTblInterface.InterfaceHeader.InterfaceDereference = WdfDeviceInterfaceDereferenceNoOp;
  ChildDeviceData->GntTblInterface.GrantAccess = GntTbl_GrantAccess;
  ChildDeviceData->GntTblInterface.EndAccess = GntTbl_EndAccess;
  WDF_QUERY_INTERFACE_CONFIG_INIT(&qiConfig, (PINTERFACE)&ChildDeviceData->GntTblInterface, &GUID_XEN_IFACE_GNTTBL, NULL);
  status = WdfDeviceAddQueryInterface(ChildDevice, &qiConfig);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  ChildDeviceData->XenBusInterface.InterfaceHeader.Size = sizeof(ChildDeviceData->XenBusInterface);
  ChildDeviceData->XenBusInterface.InterfaceHeader.Version = 1;
  ChildDeviceData->XenBusInterface.InterfaceHeader.Context = WdfPdoGetParent(ChildDevice);
  //XenBusInterface.InterfaceHeader.Context = ExAllocatePoolWithTag(NonPagedPool, (strlen(XenIdentificationDesc->Path) + 1), XENPCI_POOL_TAG);
  //strcpy(XenBusInterface.InterfaceHeader.Context, XenIdentificationDesc->Path);
  ChildDeviceData->XenBusInterface.Read = XenBus_Read;
  ChildDeviceData->XenBusInterface.Write = XenBus_Write;
  ChildDeviceData->XenBusInterface.Printf = XenBus_Printf;
  ChildDeviceData->XenBusInterface.StartTransaction = XenBus_StartTransaction;
  ChildDeviceData->XenBusInterface.EndTransaction = XenBus_EndTransaction;
  ChildDeviceData->XenBusInterface.List = XenBus_List;
  ChildDeviceData->XenBusInterface.AddWatch = XenBus_AddWatch;
  ChildDeviceData->XenBusInterface.RemWatch = XenBus_RemWatch;
  WDF_QUERY_INTERFACE_CONFIG_INIT(&qiConfig, (PINTERFACE)&ChildDeviceData->XenBusInterface, &GUID_XEN_IFACE_XENBUS, NULL);
  status = WdfDeviceAddQueryInterface(ChildDevice, &qiConfig);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  //KdPrint((__DRIVER_NAME " <-- ChildListCreateDevice\n"));

  return status;
}

VOID
XenPCI_XenBusWatchHandler(char *Path, PVOID Data)
{
  XENPCI_IDENTIFICATION_DESCRIPTION description;
  NTSTATUS status;
  char **Bits;
  int Count;
  WDFDEVICE Device = Data;
  WDFCHILDLIST ChildList;
  WDF_CHILD_LIST_ITERATOR ChildIterator;
  WDFDEVICE ChildDevice;
  PXENPCI_XEN_DEVICE_DATA ChildDeviceData;
  
  ANSI_STRING AnsiBuf;

  UNREFERENCED_PARAMETER(Data);

  KdPrint((__DRIVER_NAME " --> XenBusWatchHandle\n"));

  //KdPrint((__DRIVER_NAME "     %s\n", Path));

  ChildList = WdfFdoGetDefaultChildList(Device);

  Bits = SplitString(Path, '/', 3, &Count);
  switch (Count)
  {
    case 0:
    case 1:
      break;
    case 2:
      // add or update the device node
      WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&description.Header, sizeof(description));
      strncpy(description.Path, Path, 128);
      RtlInitAnsiString(&AnsiBuf, Bits[1]);
      //KdPrint((__DRIVER_NAME "     Name = %s\n", Bits[1]));  
      RtlAnsiStringToUnicodeString(&description.DeviceType, &AnsiBuf, TRUE);
      status = WdfChildListAddOrUpdateChildDescriptionAsPresent(ChildList, &description.Header, NULL);
      break;
    case 3:
      WDF_CHILD_LIST_ITERATOR_INIT(&ChildIterator, WdfRetrievePresentChildren);
      WdfChildListBeginIteration(ChildList, &ChildIterator);
      while (NT_SUCCESS(WdfChildListRetrieveNextDevice(ChildList, &ChildIterator, &ChildDevice, NULL)))
      {
        ChildDeviceData = GetXenDeviceData(ChildDevice);
        if (!ChildDeviceData)
        {
          KdPrint((__FUNCTION__ " No child device data, should never happen\n"));
          continue;
        }
        if (strncmp(ChildDeviceData->BasePath, Path, strlen(ChildDeviceData->BasePath)) == 0 && Path[strlen(ChildDeviceData->BasePath)] == '/')
        {
          //KdPrint((__DRIVER_NAME "     Child Path = %s (Match - WatchHandler = %08x)\n", ChildDeviceData->BasePath, ChildDeviceData->WatchHandler));
          if (ChildDeviceData->WatchHandler != NULL)
            ChildDeviceData->WatchHandler(Path, ChildDeviceData->WatchContext);
        }
        else
        {
          //KdPrint((__DRIVER_NAME "     Child Path = %s (No Match)\n", ChildDeviceData->BasePath));
        }
      }
      WdfChildListEndIteration(ChildList, &ChildIterator);
      break;
    default:
      KdPrint((__FUNCTION__ ": Unknown case %d\n", Count));
      break;
  }

  FreeSplitString(Bits, Count);
  
  KdPrint((__DRIVER_NAME " <-- XenBusWatchHandle\n"));  
}

static void
XenBus_ShutdownHandler(char *Path, PVOID Data)
{
  WDFDEVICE Device = Data;
  char *value;
  xenbus_transaction_t xbt;
  int retry;

  UNREFERENCED_PARAMETER(Path);

  KdPrint((__DRIVER_NAME " --> XenBus_ShutdownHandler\n"));

  XenBus_StartTransaction(Device, &xbt);

  XenBus_Read(Device, XBT_NIL, SHUTDOWN_PATH, &value);

  KdPrint((__DRIVER_NAME "     Shutdown Value = %s\n", value));

  // should check for error here... but why have we been called at all???
  if (value != NULL && strlen(value) != 0)
    XenBus_Write(Device, XBT_NIL, SHUTDOWN_PATH, "");

  XenBus_EndTransaction(Device, xbt, 0, &retry);
  
  KdPrint((__DRIVER_NAME " <-- XenBus_ShutdownHandler\n"));
}

static VOID
XenBus_BalloonHandler(char *Path, PVOID Data)
{
  WDFDEVICE Device = Data;
  char *value;
  xenbus_transaction_t xbt;
  int retry;

  UNREFERENCED_PARAMETER(Path);

  KdPrint((__DRIVER_NAME " --> XenBus_BalloonHandler\n"));

  XenBus_StartTransaction(Device, &xbt);

  XenBus_Read(Device, XBT_NIL, BALLOON_PATH, &value);

  KdPrint((__DRIVER_NAME "     Balloon Value = %s\n", value));

  // use the memory_op(unsigned int op, void *arg) hypercall to adjust this
  // use XENMEM_increase_reservation and XENMEM_decrease_reservation

  XenBus_EndTransaction(Device, xbt, 0, &retry);
  
  KdPrint((__DRIVER_NAME " <-- XenBus_BalloonHandler\n"));
}

/*
IO_RESOURCE_DESCRIPTOR MemoryDescriptor;

static NTSTATUS
XenPCI_FilterRemoveResourceRequirements(WDFDEVICE Device, WDFIORESREQLIST RequirementsList)
{
  NTSTATUS status;
  WDFIORESLIST ResourceList;
  PIO_RESOURCE_DESCRIPTOR Descriptor;

  int i, j;
  int offset;

  //KdPrint((__DRIVER_NAME " --> FilterRemoveResourceRequirements\n"));

  for (i = 0; i < WdfIoResourceRequirementsListGetCount(RequirementsList); i++)
  {
    ResourceList = WdfIoResourceRequirementsListGetIoResList(RequirementsList, i);
    //KdPrint((__DRIVER_NAME "     Resource List %d\n", i));
    //KdPrint((__DRIVER_NAME "     %d resources in list\n", WdfIoResourceListGetCount(ResourceList)));
    offset = 0;
    for (j = 0; j < WdfIoResourceListGetCount(ResourceList); j++)
    {
      //KdPrint((__DRIVER_NAME "       Resource %d\n", j));
      Descriptor = WdfIoResourceListGetDescriptor(ResourceList, j - offset);

      switch (Descriptor->Type) {
      case CmResourceTypePort:
        //KdPrint((__DRIVER_NAME "         Port\n"));
        break;
      case CmResourceTypeMemory:
        //KdPrint((__DRIVER_NAME "         Memory %08X%08X - %08X%08X\n", Descriptor->u.Memory.MinimumAddress.HighPart, Descriptor->u.Memory.MinimumAddress.LowPart, Descriptor->u.Memory.MaximumAddress.HighPart, Descriptor->u.Memory.MaximumAddress.LowPart));
        //KdPrint((__DRIVER_NAME "         Length %08X\n", Descriptor->u.Memory.Length));
        //KdPrint((__DRIVER_NAME "         ShareDisposition %02X\n", Descriptor->ShareDisposition));
        //KdPrint((__DRIVER_NAME "         Option %02X\n", Descriptor->Option));
        if (!Descriptor->Option || Descriptor->Option == IO_RESOURCE_PREFERRED) {
          memcpy(&MemoryDescriptor, Descriptor, sizeof(IO_RESOURCE_DESCRIPTOR));
          //platform_mmio_orig_len = MemoryDescriptor.u.Memory.Length;
          //MemoryDescriptor.u.Memory.Length = PAGE_SIZE;
          MemoryDescriptor.ShareDisposition = CmResourceShareShared;
        }
        WdfIoResourceListRemove(ResourceList, j - offset);
        offset++;
        break;
      case CmResourceTypeInterrupt:
        //KdPrint((__DRIVER_NAME "         Interrupt\n"));
        break;
      case CmResourceTypeDevicePrivate:
        //KdPrint((__DRIVER_NAME "         Private\n"));
        break;
      default:
        //KdPrint((__DRIVER_NAME "         Unknown Type (0x%x)\n", Descriptor->Type));
        break;
      }
    }
  }
  status = STATUS_SUCCESS;

  KdPrint((__DRIVER_NAME " <-- FilterRemoveResourceRequirements\n"));

  return status;
}


static NTSTATUS
XenPCI_FilterAddResourceRequirements(WDFDEVICE Device, WDFIORESREQLIST RequirementsList)
{
  NTSTATUS status;
  WDFIORESLIST ResourceList;
  PIO_RESOURCE_DESCRIPTOR Descriptor;

  int i, j;

  KdPrint((__DRIVER_NAME " --> FilterAddResourceRequirements\n"));


  for (i = 0; i < WdfIoResourceRequirementsListGetCount(RequirementsList); i++)
  {
    ResourceList = WdfIoResourceRequirementsListGetIoResList(RequirementsList, i);
    //KdPrint((__DRIVER_NAME "     Resource List %d\n", i));
    //KdPrint((__DRIVER_NAME "     %d resources in list\n", WdfIoResourceListGetCount(ResourceList)));
    WdfIoResourceListAppendDescriptor(ResourceList, &MemoryDescriptor);
    //KdPrint((__DRIVER_NAME "         Memory %08X%08X - %08X%08X\n", MemoryDescriptor.u.Memory.MinimumAddress.HighPart, MemoryDescriptor.u.Memory.MinimumAddress.LowPart, MemoryDescriptor.u.Memory.MaximumAddress.HighPart, MemoryDescriptor.u.Memory.MaximumAddress.LowPart));
    //KdPrint((__DRIVER_NAME "         Length %08X\n", MemoryDescriptor.u.Memory.Length));
    for (j = 0; j < WdfIoResourceListGetCount(ResourceList); j++)
    {
      //KdPrint((__DRIVER_NAME "       Resource %d\n", j));
      Descriptor = WdfIoResourceListGetDescriptor(ResourceList, j);

      switch (Descriptor->Type) {
      case CmResourceTypePort:
        //KdPrint((__DRIVER_NAME "         Port\n"));
        break;
      case CmResourceTypeMemory:
        //KdPrint((__DRIVER_NAME "         Memory %08X%08X - %08X%08X\n", Descriptor->u.Memory.MinimumAddress.HighPart, Descriptor->u.Memory.MinimumAddress.LowPart, Descriptor->u.Memory.MaximumAddress.HighPart, Descriptor->u.Memory.MaximumAddress.LowPart));
        //KdPrint((__DRIVER_NAME "         Length %08X\n", Descriptor->u.Memory.Length));
        //KdPrint((__DRIVER_NAME "         ShareDisposition %02X\n", Descriptor->ShareDisposition));
        //Descriptor->ShareDisposition = CmResourceShareShared;
        //memcpy(&MemoryDescriptor, Descriptor, sizeof(IO_RESOURCE_DESCRIPTOR));
        //platform_mmio_orig_len = MemoryDescriptor.u.Memory.Length;
        //MemoryDescriptor.u.Memory.Length = PAGE_SIZE;
        //WdfIoResourceListRemove(ResourceList, j);
        break;
      case CmResourceTypeInterrupt:
        //KdPrint((__DRIVER_NAME "         Interrupt\n"));
        break;
      case CmResourceTypeDevicePrivate:
        //KdPrint((__DRIVER_NAME "         Private\n"));
        break;
      default:
        //KdPrint((__DRIVER_NAME "         Unknown Type (0x%x)\n", Descriptor->Type));
        break;
      }
    }
  }
  status = STATUS_SUCCESS;

  //KdPrint((__DRIVER_NAME " <-- FilterAddResourceRequirements\n"));

  return status;
}

static NTSTATUS
XenPCI_RemoveAddedResources(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw, WDFCMRESLIST ResourcesTranslated)
{
  //KdPrint((__DRIVER_NAME " --> RemoveAddedResources\n"));
  //KdPrint((__DRIVER_NAME " <-- RemoveAddedResources\n"));

  return STATUS_SUCCESS;
}

*/

static NTSTATUS
XenPCI_DeviceResourceRequirementsQuery(WDFDEVICE Device, WDFIORESREQLIST IoResourceRequirementsList)
{
  NTSTATUS  status;
  WDFIORESLIST resourceList;
  IO_RESOURCE_DESCRIPTOR descriptor;
  PXENPCI_DEVICE_DATA xpdd = GetDeviceData(Device);

  //KdPrint((__DRIVER_NAME " --> DeviceResourceRequirementsQuery\n"));

  status = WdfIoResourceListCreate(IoResourceRequirementsList, WDF_NO_OBJECT_ATTRIBUTES, &resourceList);
  if (!NT_SUCCESS(status))
    return status;

/*
  RtlZeroMemory(&descriptor, sizeof(descriptor));

  descriptor.Option = 0;
  descriptor.Type = CmResourceTypeInterrupt;
  descriptor.ShareDisposition = CmResourceShareDeviceExclusive;
  descriptor.Flags = CM_RESOURCE_MEMORY_READ_WRITE;
  descriptor.u.Interrupt.MinimumVector = 1024;
  descriptor.u.Interrupt.MaximumVector = 1024+255;

  //KdPrint((__DRIVER_NAME "     MinimumVector = %d, MaximumVector = %d\n", descriptor.u.Interrupt.MinimumVector, descriptor.u.Interrupt.MaximumVector));

  status = WdfIoResourceListAppendDescriptor(resourceList, &descriptor);
  if (!NT_SUCCESS(status))
    return status;
*/

  RtlZeroMemory(&descriptor, sizeof(descriptor));

  descriptor.Option = 0;
  descriptor.Type = CmResourceTypeMemory;
  descriptor.ShareDisposition = CmResourceShareShared; //CmResourceShareDeviceExclusive;
  descriptor.Flags = CM_RESOURCE_MEMORY_READ_WRITE;
  descriptor.u.Memory.Length = PAGE_SIZE;
  descriptor.u.Memory.Alignment = PAGE_SIZE;
  descriptor.u.Memory.MinimumAddress.QuadPart
    = xpdd->platform_mmio_addr.QuadPart + PAGE_SIZE;
  descriptor.u.Memory.MaximumAddress.QuadPart
    = xpdd->platform_mmio_addr.QuadPart + xpdd->platform_mmio_len - 1;

  //KdPrint((__DRIVER_NAME "     MinimumAddress = %08x, MaximumAddress = %08X\n", descriptor.u.Memory.MinimumAddress.LowPart, descriptor.u.Memory.MaximumAddress.LowPart));

  status = WdfIoResourceListAppendDescriptor(resourceList, &descriptor);
  if (!NT_SUCCESS(status))
    return status;

  status = WdfIoResourceRequirementsListAppendIoResList(IoResourceRequirementsList, resourceList);
  if (!NT_SUCCESS(status))
    return status;

  //KdPrint((__DRIVER_NAME " <-- DeviceResourceRequirementsQuery\n"));

  return status;
}

