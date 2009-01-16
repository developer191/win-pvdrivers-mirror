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

#if !defined(_XENPCI_H_)
#define _XENPCI_H_

#define __attribute__(arg) /* empty */
#define EISCONN 127

#include <ntifs.h>
#include <ntddk.h>

#ifdef __MINGW32__
#include "../mingw/mingw_extras.h"
#else
#define DDKAPI
#include <wdm.h>
//#include <wdf.h>
#include <initguid.h>
#include <wdmguid.h>
#include <errno.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
#endif

#define __DRIVER_NAME "XenPCI"

#include <xen_windows.h>
#include <memory.h>
#include <grant_table.h>
#include <event_channel.h>
#include <hvm/params.h>
#include <hvm/hvm_op.h>
#include <sched.h>
#include <io/xenbus.h>
#include "io/xs_wire.h"

#include <xen_public.h>

//{C828ABE9-14CA-4445-BAA6-82C2376C6518}
DEFINE_GUID( GUID_XENPCI_DEVCLASS, 0xC828ABE9, 0x14CA, 0x4445, 0xBA, 0xA6, 0x82, 0xC2, 0x37, 0x6C, 0x65, 0x18);

#define XENPCI_POOL_TAG (ULONG) 'XenP'

#define NR_RESERVED_ENTRIES 8
#define NR_GRANT_FRAMES 32
#define NR_GRANT_ENTRIES (NR_GRANT_FRAMES * PAGE_SIZE / sizeof(grant_entry_t))

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define EVT_ACTION_TYPE_EMPTY   0
#define EVT_ACTION_TYPE_NORMAL  1
#define EVT_ACTION_TYPE_DPC     2
#define EVT_ACTION_TYPE_IRQ     3
#define EVT_ACTION_TYPE_SUSPEND 4

#define XEN_PV_PRODUCT_NUMBER   0x0002
#define XEN_PV_PRODUCT_BUILD    0x00000001

extern ULONG qemu_filtered;
extern ULONG qemu_protocol_version;

typedef struct _ev_action_t {
  PXEN_EVTCHN_SERVICE_ROUTINE ServiceRoutine;
  PVOID ServiceContext;
  CHAR description[128];
  ULONG type; /* EVT_ACTION_TYPE_* */
  KDPC Dpc;
  ULONG vector;
  ULONG count;
  PVOID xpdd;
} ev_action_t;

typedef struct _XENBUS_WATCH_RING
{
  char Path[128];
  char Token[10];
} XENBUS_WATCH_RING;

typedef struct _XENBUS_WATCH_ENTRY {
  char Path[128];
  PXENBUS_WATCH_CALLBACK ServiceRoutine;
  PVOID ServiceContext;
  int Count;
  int Active;
  //int RemovePending;
  //int Running;
  //KEVENT CompleteEvent;
} XENBUS_WATCH_ENTRY, *PXENBUS_WATCH_ENTRY;

#define NR_EVENTS 1024
#define WATCH_RING_SIZE 128
#define NR_XB_REQS 32
#define MAX_WATCH_ENTRIES 128

#define CHILD_STATE_EMPTY 0
#define CHILD_STATE_DELETED 1
#define CHILD_STATE_ADDED 2

// TODO: tidy up & organize this struct

typedef enum {
    Unknown = 0,
    NotStarted,
    Started,
    StopPending,
    Stopped,
    RemovePending,
    SurpriseRemovePending,
    Removed
} DEVICE_PNP_STATE;

typedef struct
{
  PDEVICE_OBJECT fdo;
  PDEVICE_OBJECT pdo;
  PDEVICE_OBJECT lower_do;
  
  DEVICE_PNP_STATE current_pnp_state;
  DEVICE_PNP_STATE previous_pnp_state;
  DEVICE_POWER_STATE device_power_state;
  SYSTEM_POWER_STATE system_power_state;
  
  ULONG device_usage_paging;
  ULONG device_usage_dump;
  ULONG device_usage_hibernation;
} XENPCI_COMMON, *PXENPCI_COMMON;

static __inline void INIT_PNP_STATE(PXENPCI_COMMON common)
{
  common->current_pnp_state = NotStarted;
  common->previous_pnp_state = NotStarted;
}

static __inline void SET_PNP_STATE(PXENPCI_COMMON common, DEVICE_PNP_STATE state)
{
  common->previous_pnp_state = common->current_pnp_state;
  common->current_pnp_state = state;
}

static __inline void REVERT_PNP_STATE(PXENPCI_COMMON common)
{
  common->current_pnp_state = common->previous_pnp_state;
}

#define SHUTDOWN_RING_SIZE 128

#define SUSPEND_STATE_NONE      0 /* no suspend in progress */
#define SUSPEND_STATE_SCHEDULED 1 /* suspend scheduled */
#define SUSPEND_STATE_HIGH_IRQL 2 /* all processors are at high IRQL and spinning */
#define SUSPEND_STATE_RESUMING  3 /* we are the other side of the suspend and things are starting to get back to normal */

typedef struct {  
  XENPCI_COMMON common;
  
  BOOLEAN XenBus_ShuttingDown;

  PKINTERRUPT interrupt;
  ULONG irq_number;
  ULONG irq_vector;
  KIRQL irq_level;
  KINTERRUPT_MODE irq_mode;
  KAFFINITY irq_affinity;
  
  PHYSICAL_ADDRESS shared_info_area_unmapped;
  shared_info_t *shared_info_area;
  xen_ulong_t evtchn_pending_pvt[sizeof(xen_ulong_t) * 8];
  xen_ulong_t evtchn_pending_suspend[sizeof(xen_ulong_t) * 8];
  evtchn_port_t pdo_event_channel;
  BOOLEAN interrupts_masked;
  
  PHYSICAL_ADDRESS platform_mmio_addr;
  ULONG platform_mmio_orig_len;
  ULONG platform_mmio_len;
  ULONG platform_mmio_alloc;
  USHORT platform_mmio_flags;
  
  ULONG platform_ioport_addr;
  ULONG platform_ioport_len;

  char *hypercall_stubs;

  evtchn_port_t xen_store_evtchn;

  /* grant related */
  grant_entry_t *gnttab_table;
  PHYSICAL_ADDRESS gnttab_table_physical;
  grant_ref_t *gnttab_list;
  int gnttab_list_free;
  KSPIN_LOCK grant_lock;
  /* this is the maximum number of grant frames we have memory allocated for */
  /* after a resume it may not be the actual number of grant frames we have though */
  ULONG max_grant_frames;

  ev_action_t ev_actions[NR_EVENTS];
//  unsigned long bound_ports[NR_EVENTS/(8*sizeof(unsigned long))];

  PKTHREAD XenBus_ReadThread;
  KEVENT XenBus_ReadThreadEvent;
  PKTHREAD XenBus_WatchThread;
  KEVENT XenBus_WatchThreadEvent;

  XENBUS_WATCH_RING XenBus_WatchRing[WATCH_RING_SIZE];
  int XenBus_WatchRingReadIndex;
  int XenBus_WatchRingWriteIndex;

  struct xenstore_domain_interface *xen_store_interface;

  /* xenbus related */
  XENBUS_WATCH_ENTRY XenBus_WatchEntries[MAX_WATCH_ENTRIES];
  FAST_MUTEX xb_watch_mutex;
  FAST_MUTEX xb_request_mutex;
  KEVENT xb_request_complete_event;
  struct xsd_sockmsg *xb_reply;
  
  LIST_ENTRY child_list;
  
  int suspend_state;
  
  UNICODE_STRING legacy_interface_name;
  UNICODE_STRING interface_name;
  BOOLEAN interface_open;
  
  KSPIN_LOCK shutdown_ring_lock;
  CHAR shutdown_ring[SHUTDOWN_RING_SIZE];
  ULONG shutdown_prod;
  ULONG shutdown_cons;
  ULONG shutdown_start; /* the start of the most recent message on the ring */
  PIRP shutdown_irp;

  BOOLEAN log_interrupts;
} XENPCI_DEVICE_DATA, *PXENPCI_DEVICE_DATA;

typedef struct {  
  XENPCI_COMMON common;
  PDEVICE_OBJECT bus_pdo;
  PDEVICE_OBJECT bus_fdo;
  BOOLEAN reported_missing;
  char path[128];
  char device[128];
  ULONG index;
  ULONG irq_number;
  ULONG irq_vector;
  KIRQL irq_level;
  char backend_path[128];
  //PVOID xenbus_request;
  KEVENT backend_state_event;
  ULONG backend_state;
  PHYSICAL_ADDRESS config_page_phys;
  ULONG config_page_length;
  PUCHAR requested_resources_start;
  PUCHAR requested_resources_ptr;
  PUCHAR assigned_resources_start;
  PUCHAR assigned_resources_ptr;
  XENPCI_DEVICE_STATE device_state;
  BOOLEAN restart_on_resume;
} XENPCI_PDO_DEVICE_DATA, *PXENPCI_PDO_DEVICE_DATA;

typedef struct
{
  LIST_ENTRY entry;
  int state;
  PXENPCI_PDO_DEVICE_DATA context;
} XEN_CHILD, *PXEN_CHILD;

#define XEN_INTERFACE_VERSION 1

#define DEVICE_INTERFACE_TYPE_LEGACY 0
#define DEVICE_INTERFACE_TYPE_XENBUS 1

typedef struct {
  ULONG type; /* must be the first member */
  KSPIN_LOCK lock;
  ULONG len;
  union {
    struct xsd_sockmsg msg;
    UCHAR buffer[PAGE_SIZE];
  } u;
  LIST_ENTRY read_list_head;
  PIRP pending_read_irp;
} device_interface_xenbus_context_t;


#include "hypercall.h"

#define XBT_NIL ((xenbus_transaction_t)0)

#if 0
static __inline VOID
XenPci_FreeMem(PVOID Ptr)
{
  ExFreePoolWithTag(Ptr, XENPCI_POOL_TAG);
}
#endif

NTSTATUS
hvm_get_stubs(PXENPCI_DEVICE_DATA xpdd);
NTSTATUS
hvm_free_stubs(PXENPCI_DEVICE_DATA xpdd);

NTSTATUS
XenPci_Power_Fdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Dummy_Fdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Pnp_Fdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Create_Fdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Close_Fdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Read_Fdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Write_Fdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Cleanup_Fdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_SystemControl_Fdo(PDEVICE_OBJECT device_object, PIRP irp);

NTSTATUS
XenPci_Irp_Create_XenBus(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Close_XenBus(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Read_XenBus(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Write_XenBus(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Cleanup_XenBus(PDEVICE_OBJECT device_object, PIRP irp);

NTSTATUS
XenPci_Power_Pdo(PDEVICE_OBJECT device_object, PIRP irp);
//NTSTATUS
//XenPci_Dummy_Pdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Pnp_Pdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Create_Pdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Close_Pdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Read_Pdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Write_Pdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_Irp_Cleanup_Pdo(PDEVICE_OBJECT device_object, PIRP irp);
NTSTATUS
XenPci_SystemControl_Pdo(PDEVICE_OBJECT device_object, PIRP irp);

NTSTATUS
XenPci_Pdo_Suspend(PDEVICE_OBJECT device_object);
NTSTATUS
XenPci_Pdo_Resume(PDEVICE_OBJECT device_object);

VOID
XenPci_DumpPdoConfig(PDEVICE_OBJECT device_object);

typedef VOID
(*PXENPCI_HIGHSYNC_FUNCTION)(PVOID context);

VOID
XenPci_HighSync(PXENPCI_HIGHSYNC_FUNCTION function0, PXENPCI_HIGHSYNC_FUNCTION functionN, PVOID context);

VOID
XenPci_PatchKernel(PXENPCI_DEVICE_DATA xpdd, PVOID base, ULONG length);

struct xsd_sockmsg *
XenBus_Raw(PXENPCI_DEVICE_DATA xpdd, struct xsd_sockmsg *msg);
char *
XenBus_Read(PVOID Context, xenbus_transaction_t xbt, char *path, char **value);
char *
XenBus_Write(PVOID Context, xenbus_transaction_t xbt, char *path, char *value);
char *
XenBus_Printf(PVOID Context, xenbus_transaction_t xbt, char *path, char *fmt, ...);
char *
XenBus_StartTransaction(PVOID Context, xenbus_transaction_t *xbt);
char *
XenBus_EndTransaction(PVOID Context, xenbus_transaction_t t, int abort, int *retry);
char *
XenBus_List(PVOID Context, xenbus_transaction_t xbt, char *prefix, char ***contents);
char *
XenBus_AddWatch(PVOID Context, xenbus_transaction_t xbt, char *Path, PXENBUS_WATCH_CALLBACK ServiceRoutine, PVOID ServiceContext);
char *
XenBus_RemWatch(PVOID Context, xenbus_transaction_t xbt, char *Path, PXENBUS_WATCH_CALLBACK ServiceRoutine, PVOID ServiceContext);
//VOID
//XenBus_ThreadProc(PVOID StartContext);
NTSTATUS
XenBus_Init(PXENPCI_DEVICE_DATA xpdd);
NTSTATUS
XenBus_Close(PXENPCI_DEVICE_DATA xpdd);
NTSTATUS
XenBus_Start(PXENPCI_DEVICE_DATA xpdd);
NTSTATUS
XenBus_Stop(PXENPCI_DEVICE_DATA xpdd);
NTSTATUS
XenBus_Suspend(PXENPCI_DEVICE_DATA xpdd);
NTSTATUS
XenBus_Resume(PXENPCI_DEVICE_DATA xpdd);
NTSTATUS
XenBus_StopThreads(PXENPCI_DEVICE_DATA xpdd);

PHYSICAL_ADDRESS
XenPci_AllocMMIO(PXENPCI_DEVICE_DATA xpdd, ULONG len);

NTSTATUS
EvtChn_Init(PXENPCI_DEVICE_DATA xpdd);
NTSTATUS
EvtChn_ConnectInterrupt(PXENPCI_DEVICE_DATA xpdd);
NTSTATUS
EvtChn_Shutdown(PXENPCI_DEVICE_DATA xpdd);

NTSTATUS
EvtChn_Mask(PVOID Context, evtchn_port_t Port);
NTSTATUS
EvtChn_Unmask(PVOID Context, evtchn_port_t Port);
NTSTATUS
EvtChn_Bind(PVOID Context, evtchn_port_t Port, PXEN_EVTCHN_SERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext);
NTSTATUS
EvtChn_BindDpc(PVOID Context, evtchn_port_t Port, PXEN_EVTCHN_SERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext);
NTSTATUS
EvtChn_BindIrq(PVOID Context, evtchn_port_t Port, ULONG vector, PCHAR description);
evtchn_port_t
EvtChn_AllocIpi(PVOID context, ULONG vcpu);
NTSTATUS
EvtChn_Unbind(PVOID Context, evtchn_port_t Port);
NTSTATUS
EvtChn_Notify(PVOID Context, evtchn_port_t Port);
VOID
EvtChn_Close(PVOID Context, evtchn_port_t Port);
evtchn_port_t
EvtChn_AllocUnbound(PVOID Context, domid_t Domain);
BOOLEAN
EvtChn_AckEvent(PVOID context, evtchn_port_t port);

VOID
GntTbl_Init(PXENPCI_DEVICE_DATA xpdd);
VOID
GntTbl_InitMap(PXENPCI_DEVICE_DATA xpdd);
grant_ref_t
GntTbl_GrantAccess(PVOID Context, domid_t domid, uint32_t, int readonly, grant_ref_t ref);
BOOLEAN
GntTbl_EndAccess(PVOID Context, grant_ref_t ref, BOOLEAN keepref);
VOID
GntTbl_PutRef(PVOID Context, grant_ref_t ref);
grant_ref_t
GntTbl_GetRef(PVOID Context);
int 
GntTbl_Map(PVOID Context, unsigned int start_idx, unsigned int end_idx);
#endif
