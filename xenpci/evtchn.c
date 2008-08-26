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

#if defined(__MINGW32__)
  #define xchg(p1, p2) InterlockedExchange((xen_long_t * volatile)p1, p2)
  /* rest implemented in mingw_extras.c */
#elif defined(_X86_)
  #define xchg(p1, p2) _InterlockedExchange(p1, p2)
  #define synch_clear_bit(p1, p2) _interlockedbittestandreset(p2, p1)
  #define synch_set_bit(p1, p2) _interlockedbittestandset(p2, p1)
  #define bit_scan_forward(p1, p2) _BitScanForward(p1, p2)
#else
  #define xchg(p1, p2) _InterlockedExchange64(p1, p2)
  #define synch_clear_bit(p1, p2) _interlockedbittestandreset64(p2, p1)
  #define synch_set_bit(p1, p2) _interlockedbittestandset64(p2, p1)
  #define bit_scan_forward(p1, p2) _BitScanForward64(p1, p2)
#endif

static DDKAPI VOID
EvtChn_DpcBounce(PRKDPC Dpc, PVOID Context, PVOID SystemArgument1, PVOID SystemArgument2)
{
  ev_action_t *action = Context;

  UNREFERENCED_PARAMETER(Dpc);
  UNREFERENCED_PARAMETER(SystemArgument1);
  UNREFERENCED_PARAMETER(SystemArgument2);

  //KdPrint((__DRIVER_NAME " --> " __FUNCTION__ "\n"));

  if (action->type == EVT_ACTION_TYPE_DPC)
  {
    action->ServiceRoutine(NULL, action->ServiceContext);
  }
  //KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));
}

BOOLEAN no_more_interrupts;

/* Called at DIRQL */
BOOLEAN
EvtChn_AckEvent(PVOID context, evtchn_port_t port)
{
  PXENPCI_DEVICE_DATA xpdd = context;
  ULONG evt_word;
  ULONG evt_bit;
  xen_ulong_t val;

  evt_bit = port & ((sizeof(xen_ulong_t) * 8) - 1);
  evt_word = port >> (5 + (sizeof(xen_ulong_t) >> 3));

  val = synch_clear_bit(evt_bit, (volatile xen_long_t *)&xpdd->evtchn_pending_pvt[evt_word]);
  
  //KdPrint((__DRIVER_NAME "     port %d = %d\n", port, !!val));

  return (BOOLEAN)!!val;
}

static DDKAPI BOOLEAN
EvtChn_Interrupt(PKINTERRUPT Interrupt, PVOID Context)
{
/*
For HVM domains, Xen always triggers the event on CPU0. Because the
interrupt is delivered via the virtual PCI device it might get delivered
to CPU != 0, but we should always use vcpu_info[0]
*/
  int cpu = 0;
  vcpu_info_t *vcpu_info;
  PXENPCI_DEVICE_DATA xpdd = (PXENPCI_DEVICE_DATA)Context;
  shared_info_t *shared_info_area = xpdd->shared_info_area;
  xen_ulong_t evt_words;
  unsigned long evt_word;
  unsigned long evt_bit;
  unsigned int port;
  ev_action_t *ev_action;
  BOOLEAN handled = FALSE;
  BOOLEAN deferred = FALSE;
  int i;

  //KdPrint((__DRIVER_NAME " --> " __FUNCTION__ " (cpu = %d)\n", cpu));

  if (xpdd->interrupts_masked)
  {
    KdPrint((__DRIVER_NAME "     unhandled interrupt\n"));
    return TRUE;
  }
    
  //ASSERT(!no_more_interrupts);
  
  /* we should check the evtchn_pending_pvt here and report if != 0 */
  
  UNREFERENCED_PARAMETER(Interrupt);

  vcpu_info = &shared_info_area->vcpu_info[cpu];

  vcpu_info->evtchn_upcall_pending = 0;

  evt_words = (xen_ulong_t)xchg((volatile xen_long_t *)&vcpu_info->evtchn_pending_sel, 0);
  
  while (bit_scan_forward(&evt_word, evt_words))
  {
    evt_words &= ~(1 << evt_word);
    while (bit_scan_forward(&evt_bit, shared_info_area->evtchn_pending[evt_word] & ~shared_info_area->evtchn_mask[evt_word]))
    {
      handled = TRUE;
      port = (evt_word << 5) + evt_bit;
      ev_action = &xpdd->ev_actions[port];
      ev_action->count++;
      switch (ev_action->type)
      {
      case EVT_ACTION_TYPE_NORMAL:
        synch_clear_bit(evt_bit, (volatile xen_long_t *)&shared_info_area->evtchn_pending[evt_word]);
        ev_action->ServiceRoutine(NULL, ev_action->ServiceContext);
        break;
      case EVT_ACTION_TYPE_IRQ:
        synch_clear_bit(evt_bit, (volatile xen_long_t *)&shared_info_area->evtchn_pending[evt_word]);
        synch_set_bit(evt_bit, (volatile xen_long_t *)&xpdd->evtchn_pending_pvt[evt_word]);
        deferred = TRUE;
        break;
      case EVT_ACTION_TYPE_DPC:
        synch_clear_bit(evt_bit, (volatile xen_long_t *)&shared_info_area->evtchn_pending[evt_word]);
        KeInsertQueueDpc(&ev_action->Dpc, NULL, NULL);
        break;
      case EVT_ACTION_TYPE_SUSPEND:
        synch_clear_bit(evt_bit, (volatile xen_long_t *)&shared_info_area->evtchn_pending[evt_word]);
        KdPrint((__DRIVER_NAME "     EVT_ACTION_TYPE_SUSPEND\n"));
        for (i = 0; i < ARRAY_SIZE(xpdd->evtchn_pending_pvt); i++)
        {
          if (xpdd->ev_actions[i].type == EVT_ACTION_TYPE_IRQ)
          {
            int suspend_bit = i & ((sizeof(xen_ulong_t) * 8) - 1);
            int suspend_word = i >> (5 + (sizeof(xen_ulong_t) >> 3));
            synch_set_bit(suspend_bit, (volatile xen_long_t *)&xpdd->evtchn_pending_pvt[suspend_word]);
          }
        }
        deferred = TRUE;
        break;
      default:
        synch_clear_bit(evt_bit, (volatile xen_long_t *)&shared_info_area->evtchn_pending[evt_word]);
        KdPrint((__DRIVER_NAME "     Unhandled Event!!!\n"));
        break;
      }
    }
  }

  //KdPrint((__DRIVER_NAME " <-- " __FUNCTION__ "\n"));

  return handled && !deferred;
}

NTSTATUS
EvtChn_Bind(PVOID Context, evtchn_port_t Port, PKSERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  FUNCTION_ENTER();
  
  if (xpdd->ev_actions[Port].type != EVT_ACTION_TYPE_EMPTY)
  {
    xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_EMPTY;
    KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
    KdPrint((__DRIVER_NAME " Handler for port %d already registered, replacing\n", Port));
  }

  xpdd->ev_actions[Port].ServiceRoutine = ServiceRoutine;
  xpdd->ev_actions[Port].ServiceContext = ServiceContext;
  xpdd->ev_actions[Port].xpdd = xpdd;
  KeMemoryBarrier();
  xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_NORMAL;

  EvtChn_Unmask(Context, Port);

  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_BindDpc(PVOID Context, evtchn_port_t Port, PKSERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  FUNCTION_ENTER();

  if (xpdd->ev_actions[Port].type != EVT_ACTION_TYPE_EMPTY)
  {
    xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_EMPTY;
    KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
    KdPrint((__DRIVER_NAME " Handler for port %d already registered, replacing\n", Port));
  }

  xpdd->ev_actions[Port].ServiceRoutine = ServiceRoutine;
  xpdd->ev_actions[Port].ServiceContext = ServiceContext;
  xpdd->ev_actions[Port].xpdd = xpdd;
  KeInitializeDpc(&xpdd->ev_actions[Port].Dpc, EvtChn_DpcBounce, &xpdd->ev_actions[Port]);
  KeMemoryBarrier(); // make sure that the new service routine is only called once the context is set up
  xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_DPC;

  EvtChn_Unmask(Context, Port);

  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_BindIrq(PVOID Context, evtchn_port_t Port, ULONG vector, PCHAR description)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  FUNCTION_ENTER();

  if (xpdd->ev_actions[Port].type != EVT_ACTION_TYPE_EMPTY)
  {
    xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_EMPTY;
    KeMemoryBarrier(); // make sure we don't call the old Service Routine with the new data...
    KdPrint((__DRIVER_NAME " Handler for port %d already registered, replacing\n", Port));
  }

  KeInitializeDpc(&xpdd->ev_actions[Port].Dpc, EvtChn_DpcBounce, &xpdd->ev_actions[Port]);
  xpdd->ev_actions[Port].vector = vector;
  xpdd->ev_actions[Port].xpdd = xpdd;
  KeMemoryBarrier();
  xpdd->ev_actions[Port].type = EVT_ACTION_TYPE_IRQ;
  strncpy(xpdd->ev_actions[Port].description, description, 128);

  EvtChn_Unmask(Context, Port);

  FUNCTION_EXIT();

  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Unbind(PVOID Context, evtchn_port_t Port)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  
  EvtChn_Mask(Context, Port);
  xpdd->ev_actions[Port].ServiceRoutine = NULL;
  KeMemoryBarrier();
  xpdd->ev_actions[Port].ServiceContext = NULL;

  if (xpdd->ev_actions[Port].type == EVT_ACTION_TYPE_DPC)
    KeRemoveQueueDpc(&xpdd->ev_actions[Port].Dpc);
  
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Mask(PVOID Context, evtchn_port_t port)
{
  PXENPCI_DEVICE_DATA xpdd = Context;

  synch_set_bit(port & 31,
    (volatile xen_long_t *)&xpdd->shared_info_area->evtchn_mask[port >> 5]);
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Unmask(PVOID context, evtchn_port_t port)
{
  PXENPCI_DEVICE_DATA xpdd = context;

  synch_clear_bit(port & 31,
    (volatile xen_long_t *)&xpdd->shared_info_area->evtchn_mask[port >> 5]);
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_Notify(PVOID Context, evtchn_port_t Port)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  struct evtchn_send send;

  send.port = Port;
  (void)HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_send, &send);
  return STATUS_SUCCESS;
}

evtchn_port_t
EvtChn_BindIpi(PVOID context, ULONG vcpu)
{
  PXENPCI_DEVICE_DATA xpdd = context;
  evtchn_bind_ipi_t op;
  
  FUNCTION_ENTER();
  op.vcpu = vcpu;
  op.port = 0;
  HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_bind_ipi, &op);
  FUNCTION_EXIT();
  return op.port;
}

evtchn_port_t
EvtChn_AllocUnbound(PVOID Context, domid_t Domain)
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  evtchn_alloc_unbound_t op;
  op.dom = DOMID_SELF;
  op.remote_dom = Domain;
  HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_alloc_unbound, &op);
  return op.port;
}

VOID
EvtChn_Close(PVOID Context, evtchn_port_t port )
{
  PXENPCI_DEVICE_DATA xpdd = Context;
  evtchn_close_t op;
  op.port = port;
  HYPERVISOR_event_channel_op(xpdd, EVTCHNOP_close, &op);
  return;
}

NTSTATUS
EvtChn_Init(PXENPCI_DEVICE_DATA xpdd)
{
  int i;

  FUNCTION_ENTER();

  for (i = 0; i < NR_EVENTS; i++)
  {
    EvtChn_Mask(xpdd, i);
    xpdd->ev_actions[i].type = EVT_ACTION_TYPE_EMPTY;
    xpdd->ev_actions[i].count = 0;
  }

  for (i = 0; i < 8; i++)
  {
    xpdd->shared_info_area->evtchn_pending[i] = 0;
  }

  for (i = 0; i < MAX_VIRT_CPUS; i++)
  {
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_pending = 0;
    xpdd->shared_info_area->vcpu_info[i].evtchn_pending_sel = 0;
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 1; /* apparantly this doesn't do anything */
  }

  KeMemoryBarrier();

  hvm_set_parameter(xpdd, HVM_PARAM_CALLBACK_IRQ, xpdd->irq_number);

  for (i = 0; i < MAX_VIRT_CPUS; i++)
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 0;  
  xpdd->interrupts_masked = FALSE;
  KeMemoryBarrier();

  xpdd->suspend_evtchn = EvtChn_BindIpi(xpdd, 0);
  xpdd->ev_actions[xpdd->suspend_evtchn].type = EVT_ACTION_TYPE_SUSPEND;
  EvtChn_Unmask(xpdd, xpdd->suspend_evtchn);
  
  KdPrint((__DRIVER_NAME "     suspend_evtchn = %d\n", xpdd->suspend_evtchn));

  FUNCTION_EXIT();
  
  return STATUS_SUCCESS;
}

NTSTATUS
EvtChn_ConnectInterrupt(PXENPCI_DEVICE_DATA xpdd)
{
  NTSTATUS status = STATUS_SUCCESS;
  
  ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

  status = IoConnectInterrupt(
    &xpdd->interrupt,
  	EvtChn_Interrupt,
  	xpdd,
  	NULL,
  	xpdd->irq_vector,
  	xpdd->irq_level,
  	xpdd->irq_level,
  	LevelSensitive,
  	TRUE,
  	xpdd->irq_affinity,
  	FALSE);
  
  if (!NT_SUCCESS(status))
  {
    KdPrint((__DRIVER_NAME "     IoConnectInterrupt failed 0x%08x\n", status));
    return status;
  }

  return status;
}

NTSTATUS
EvtChn_Shutdown(PXENPCI_DEVICE_DATA xpdd)
{
  int i;
//  LARGE_INTEGER wait_time;

  xpdd->interrupts_masked = TRUE;
  for (i = 0; i < MAX_VIRT_CPUS; i++)
    xpdd->shared_info_area->vcpu_info[i].evtchn_upcall_mask = 1;
  KeMemoryBarrier();
  hvm_set_parameter(xpdd, HVM_PARAM_CALLBACK_IRQ, 0);
  //KeMemoryBarrier();
  //no_more_interrupts = TRUE;
  //KeMemoryBarrier();

#if (NTDDI_VERSION >= NTDDI_WINXP)
  KeFlushQueuedDpcs();
#else
  for (i = 0; i < NR_EVENTS; i++)
  {
    if (xpdd->ev_actions[i].type == EVT_ACTION_TYPE_DPC || xpdd->ev_actions[i].type == EVT_ACTION_TYPE_IRQ)
    {
      KeRemoveQueueDpc(&xpdd->ev_actions[i].Dpc);
    }
  }
#endif

/*
KdPrint((__DRIVER_NAME "     Starting fake delay (IRQL = %d)\n", KeGetCurrentIrql()));
wait_time.QuadPart = 1000 * (-1 * 10 * 1000);
KeDelayExecutionThread(KernelMode, FALSE, &wait_time);
KdPrint((__DRIVER_NAME "     Done with fake delay\n"));
*/
  //IoDisconnectInterrupt(xpdd->interrupt);

  return STATUS_SUCCESS;
}
