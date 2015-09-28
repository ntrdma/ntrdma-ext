# NTRDMA

## Name

NTRDMA - Non-Transparent Remote Direct Memory Access

## Synopsis

Load the NTRDMA kernel module:
```
SPA:~# modprobe ntrdma

SPB:~# modprobe ntrdma
```

Run RDMA applications.

## Description

NTRDMA is a device driver for the Linux and OFED RDMA software stack.  NTRDMA
uses a PCI-Express link, Non-Transparent Bridge (NTB), and general purpose DMA
hardware, as an efficient transport for moving data between the memory systems
of closely connected peers.  The name NTRDMA comes from the combination of NTB
and RDMA.

The NTRDMA driver implements RDMA data structures in software, which would
normally be implemented in hardware on an Infiniband or RoCE host bus adapter.
The four primary data structures that NTRDMA provides to RDMA applications are
Protection Domains, Memory Regions, Queue Pairs, and Completion Queues.
Protection Domains are a context for Memory Regions and Queue Pairs, with the
primary function of limiting access to memory to only that which has been
explicitly allowed by the peer application.  Memory Regions define ranges in the
computer memory that an application explicitly makes available to the peer.
Queue Pairs provide a way for an application to send asynchronous work requests
to the driver, to carry out operations on the peer memory, and to signal the
peer via the sending and receiving of messages.  Completion queues are used in
conjunction with Queue Pairs to indicate the completion of work requests.

## Examples

Use qperf to measure RDMA performance:
```
SPB:~$ numactl -N 1 -m 1 -- qperf

SPA:~$ numactl -N 1 -m 1 -- qperf peer rc_bw rc_rdma_write_bw rc_rdma_read_bw
rc_bw:
    bw  =  4.67 GB/sec
rc_rdma_write_bw:
    bw  =  4.67 GB/sec
rc_rdma_read_bw:
    bw  =  4.62 GB/sec
 
SPA:~$ numactl -N 1 -m 1 -- qperf peer rc_lat rc_rdma_write_lat rc_rdma_read_lat
rc_lat:
    latency  =  12.2 us
rc_rdma_write_lat:
    latency  =  12.8 us
rc_rdma_read_lat:
    latency  =  21.4 us
```

## Theory of Operation

The NTRDMA driver has the basic responsibility of moving data, efficiently,
between peers with direct write access to each other’s memory systems.  The
NTRDMA driver is implemented on top of hardware with limited capabilities, in
contrast with Infiniband which implements concepts like memory regions and
queue pairs in the hardware.  With Infiniband hardware, to send data and
consume a receive work request on the remote peer, the work request and data
are transmitted to the peer Infiniband device.  The Infiniband device
manipulates its internal queue pairs to consume a receive work request and
translate the incoming data to its destination address, and the and the
Infiniband device itself writes the data into the memory system.  With NTRDMA,
however, there is no Infiniband device.  Instead, the peers have direct write
access to each other’s memory systems.  Data can be moved from one peer to
another simply by copying between a source to destination address.

The challenge for moving data is determining the correct destination address on
the peer.  With Infiniband, again, the destination address is determined by the
receiving side Infiniband hardware.  Without Infiniband, the destination
address on the peer must be known by the sending side.  This is the main
problem solved by the NTRDMA driver, determining the destination address for
DMA copy operations, and assembling the chain of DMA requests to be offloaded
onto the general purpose DMA hardware.

### Remote Resources for RDMA

The two basic RDMA facilities available to a verbs application are Memory
Regions and Queue Pairs.  For the moment, disregard Protection Domains and
Completion Queues.  Memory Regions (MR) represent ranges of memory explicitly
made available from an application, to be accessed by the peer.  Queue Pairs
(QP) are a mechanism to perform operations between local and remote memory
regions.  The basic Work Request (WR) operations are read, write, send, and
recv.

NTRDMA adds remote counterparts to these resources: Remote Memory Region (RMR)
and Remote Queue Pair (RQP).  A RMR is created on the peer, for an MR
registered locally; likewise, an RQP is created on the peer for a QP created
locally.  For write and send operations, where the requesting side sends the
data, the operation is carried out by the QP from a local MR to an RMR,
representing the destination on the peer.  For the read operation, where the
requesting side is the destination, the operation is carried out by the RQP on
the peer, still, from an MR on the peer to an RMR representing the destination
on the requesting side.  Data movement is always from a local MR, with
addresses in local memory, to an RMR, with addresses in peer memory.

Note that while a WR may also be described as either the source or destination
of a copy, such as from a send WR to a recv WR, really, the work requests only
indicate which MR and RMR, offset, and size are the source and destination.
MRs and RMRs are composed of a list of DMA addresses and sizes in memory,
whereas WRs are composed of a list of offsets and sizes in MRs.

A QP is a pair of send and recv work queue.  Work requests (WR) posted to the
send queue of the QP indicate work to be done, such as reading or writing to
specific MR, or sending to a posted recv WR.  WR posted to the recv queue
indicate the destination for send WR posted by the peer.  In NTRDMA, when WR
are posted to the QP, they are transformed into Work Queue Entries (WQE), which
are fixed size entries in a ring buffer.  The WQEs are intended to be suitable
for transmission over the wire, to the corresponding ring buffer in the RQP on
the peer.  Both the QP and RQP are involved in various stages of processing the
work requests.

In addition to ring buffers for the WQEs, the QPs also have corresponding
parallel ring buffers for Completion Queue Entries (CQE).  Each CQE in the QP,
is stored in the location corresponding to the work request that is completed.
The CQEs are not stored in a Completion Queue (CQ).  The earlier prototype
versions of NTRDMA did store CQEs in a CQ, and implemented a Remote CQ, however
the design was complicated due to the fact that multiple QPs, and multiple
request queues in the QPs, share the same CQ.  Some QP operations are completed
locally, while others are completed remotely, so coordinating different local
and remote access to the CQ required excessive locking, and coordination over
the wire.  Storing CQEs in the QP, corresponding to their WR, means no locking
or coordination is required, since an entry for a CQE is always available at a
location determined only by the corresponding location of the WQE.  In NTRDMA,
a CQ is available to verbs applications, but it is implemented internally by
round robin polling between the completion queues of associated queue pairs.

### Work Request Processing

The processing of work requests proceeds as follows.  The application specifies
a chain of WRs, and posts them to a queue pair.  The application can post
requests to the recv queue, or the send queue.  In both cases, the work
requests are transformed into WQEs, and stored in the next available locations
in the QP ring buffer.  In the case of requests to the send queue, control is
returned to the application immediately after storing the WQEs, so the
application can continue in parallel to the request processing.  In the case of
requests to the recv queue, the work requests are also copied to the
corresponding ring buffer of the RQP recv queue on the peer, before returning
to the application.  This is necessary to ensure that, after returning to the
application, if the application proceeds to post a send to the peer advertising
the posted recvs, the recvs are actually available to the peer; the send
advertising the recvs must not get ahead of the posting of the recvs.

Next, the send queue of the QP processes batches of WQE.  For each write or
send WQE, the QP copies data from local memory to the peer.  The source of the
copy is determined by the scatter list of the request.  The destination of a
send is a recv, consumed from the recv queue of the connected RQP, representing
the QP on the peer.  The destination of a write is a RMR, determined by the
RDMA key of the request, representing a MR on the peer.  After the data
portions of a batch of work requests has been assembled, the portion of the
ring containing the WQEs are also copied to the corresponding ring of the RQP
on the peer, and the peer is interrupted.

The RQP then processes the same WQEs.  For each write or send WQE, the RQP
DMA-syncs the data for the local CPU, which should be a no-op on most common
architectures.  For each read WQE, the RQP copies data from the local memory on
the remote side, to the remote memory of the requesting side, from the
perspective of the RQP.  The source of the read is the MR determined by the
RDMA key of the request.  The destination is determined by the scatter list of
the request, referring to RMRs representing the memory buffers on the
requesting side.  For each WQE, a corresponding CQE is stored in the RQP.
After the data portions of a batch of work requests has been assembled, for
read requests, the portion of the ring containing the CQEs is copied back to
the corresponding ring on the requesting side, and the requesting side is
interrupted.

When the requesting side is interrupted, it does not immediately process the
CQEs.  Instead, it notifies the verbs application that completions are
available, and postpones processing of CQEs until the application polls for
completions.  When the application polls for completions, the CQEs are
transformed into Work Completions to be returned to the application.  In the
case of a read work request, the destination buffer is also DMA-synced for the
CPU, which again is likely to be a no-op on common architectures.

### Interrupts, Virtual Doorbells, and Ring Indices

Fundamental to the operation of the NTRDMA driver is the ability to signal to
the peer driver that work items are available or completed.  NTB hardware
typically provides a doorbell register, and writing to the register will
generate an interrupt event on the peer.  Typically, an NTB driver would handle
the interrupt by reading which bits are set in the doorbell register, clear the
bits, and proceed to do any work related to the bits.  NTRDMA initially used
the doorbell register, but then was changed to work around a hardware errata:
any access to the memory mapped configuration BAR of the NTB device could
potentially hard lockup the system.  NTRDMA was changed to exchange an MSI
address and data pair with the peer, and write the interrupt message directly,
without using the doorbell register.  The doorbell bits of the register were
replaced with an array of memory locations, and the operation of ringing the
doorbell is now writing a value to a memory location, followed by writing data
to the MSI address.  This operation is referred to in NTRDMA by the name,
virtual doorbell.

Not using the doorbell register has some other interesting implications for
performance.  Firstly, the access to the virtual doorbell memory buffer is
faster than generating a PCI read of the doorbell register.  The virtual
doorbell values need not be simple bits, set or cleared; the NTRDMA driver
stores a sequence number in the virtual doorbell locations, so that multiple
observers can independently clear their own view of the doorbell, rather than
globally reset the state of the doorbell for all observers, as is the only
mechanism available to observers of the hardware doorbell.

Work queue entries, and completion queue entries, are copied between ring
buffers of peer NTRDMA driver, and the status of queue entries is determined by
various ring indices.  A ring buffer may typically have ring indices named
post, prod, cons, and cmpl, corresponding to the following states of work
processing.  Posted (post) means that entries, up to but not including that
index in the ring, have been initialized in the ring, but no other processing
has been done.  Produced (prod) means that entries have been processed, and
usually, copied to the peer ring buffer of the remote resource.  Consumed
(cons) means that entries have been processed by the remote resource, often
with a completion entry initialized in a parallel ring buffer, copied back to
the ring in the requesting peer.  Finally, completed (cmpl) means that the
application has polled the completion for the request.  Ring entries starting
from cmpl up to prod are available for new requests to be produced.

In particular, ring indices are used to indicate stages of work completion,
instead of flags per entry.  Flags have the advantage of allowing work to be
completed in parallel processes, each process updating the stage of completion
of the entries it affects.  With flags, however, there is no way to avoid the
issue of small updates per entry, to set the flag.  The entry must be fully
initialized before the flag is set, so unless the transport can guarantee order
of data an flags in the copy operation, the data must be written first,
followed by a dependent write to update the flags.  Ring indices have the
disadvantage with regards to parallelism, but the advantage for coalescing work
and indicating the stage of work completion for multiple entries with a single
update.  Minimizing the overhead of small updates is critical to the throughput
performance of the DMA engine.

### Ethernet Device

In addition to RDMA facilities, NTRDMA also provides an Ethernet device for the
Linux network stack.  The Ethernet device is built out of the same parts as
QPs, except it is simplified and targeted to delivering low overhead Ethernet
functionality, without data placement as in RDMA, but with data copy avoidance.
The network device implements something like a receive queue of a queue pair,
where empty network buffers are posted, to be consumed by incoming sends.  The
buffer addresses are stored in a ring, with the size of the buffer, and copied
to the corresponding ring on the peer.  When the network layer transmits
packets, they either immediately consume a receive from the receive queue, or
the transmit packets are dropped.  Dynamic queue length, and stopping the
transmit queue, are used to prevent the network layer from attempting to
transmit packets that would be dropped.  When receive buffers are consumed,
data is transmitted to the specified address on the peer, and the offset and
length of the data are stored in the send queue.  The send queue is then copied
to the peer, and the peer is interrupted.  When the peer is interrupted, it
notifies the network stack that it should resume polling for incoming packets.
The Linux network stack is responsible for scheduling of all transmissions and
receive processing.

### Driver Initialization

TODO: stages of the NTB port state machine

* How state changes are indicated
* Negotiation of version, exchanging an address
* The driver command ring, virtual doorbells
* Enabling Ethernet and RDMA remote resources
* avoidance of scratchpads after initialization

### NTRDMA over TCP

The NTRDMA driver is extended to support a TCP back end instead of NTB
hardware.  The TCP back end allows NTRDMA to be deployed for system and
integration testing in environments without NTB hardware, and in environments
other than the EMC software simulation environment.

Instead of directly writing into the peer memory, NTRDMA can be configured to
copy data into and out of a TCP-connected socket.  Data is encapsulated in the
TCP stream with very simple messaging, to simulate the placement of data at
specified addresses, and deliver simulated interrupts.  The most fundamental
difference in the driver using the TCP back end is with respect to the DMA
mapping of memory.  With hardware, memory buffers are DMA mapped so that
hardware can access it by a DMA address.  With TCP, the kernel itself will be
reading and writing the buffers using kernel virtual addresses, not DMA
addresses.  Buffers allocated by the driver, such as with kmalloc, may be
simply referred to by their address, with no additional mapping.  Memory
regions allocated by user space applications are pinned, and then instead of
DMA mapping the pages for hardware access, the pages are virtually mapped into
the kernel memory space.  The NTRDMA TCP back end is aware that DMA addresses
in this configuration are actually kernel virtual addresses, as it copies data
buffers into and out of the TCP-connected sockets.

### NTRDMA Operating System Abstraction

The NTRDMA driver was developed in three major phases.  There was an initial
prototype phase with limited scope and functionality, followed a more ambitious
second prototype, and a final third prototype which has been released as NTRDMA
v1.0.  The second phase of the prototype introduced an operating system
abstraction layer in the RDMA driver.

After the success of the first limited prototype, a team was convened to review
the design, and build a more ambitious version of the driver.  One immediate
limitation of the driver was the dependence on the specific hardware
configuration and manual intervention, so one of the first features included
the new design was a hardware abstraction layer for NTB.  The team also decided
to build the driver in a software simulation environment, to support a rapid
development cycle, and to take advantage of an extensive suite of existing
tests covering the normal operating conditions and failure cases.  In addition
to the new hardware abstractions, the NTRDMA driver was extended with an
operating system abstraction, so it could be easily ported between Linux and
EMC operating environments like the simulation environment.

Once NTRDMA was running in the simulation environment, an effort was undertaken
to port the driver back to Linux.  Linux at the time had an NTB driver, but it
exposed a higher level transport API that did not support RDMA, and it did not
expose any of the NTB hardware capabilities.  The NTB API developed in the
simulation environment was ported to Linux, and the NTB hardware drivers were
modified to use the new API.  The NTB API now exposes lower level functionality
of NTB hardware, and allows higher level transports, like the existing NTB
transport driver and network device, to interchanged with different
implementations, such as the NTRDMA driver.  The new NTB API and changes to the
NTB hardware drivers were contributed to Linux, and are now included in Linux
version 4.2.

The resulting NTRDMA code now has the following abstractions, which are
extraneous when considering NTRDMA simply with respect to its existence in the
Linux kernel.  The operating system abstraction includes things like allocating
and freeing memory, and mapping addresses between different spaces like IO and
DMA.  Different implementations of the abstraction are used in different
configurations of NTRDMA, whether it is configured to use NTB and DMA hardware
back end, or a TCP socket as a back end for testing without hardware.  The
operating system abstraction may be removed from NTRDMA, however, considering
that doing so means we would be abandoning the possibility to test same driver
code in the EMC simulation environment, or easily port NTRDMA to other
operating environments, such as a standalone driver in POSIX user space, or to
BSD or other operating systems.  Removing the operating system abstraction may
also mean we would have to abandon the TCP back end as a configuration option.
It is a trade off, and the decision may still come down in favor of removing
the abstraction from the Linux driver.

## TODO

The current development goal for the NTRDMA driver is to bring it to a state of
readiness for inclusion in the Linux kernel, perhaps as a driver in staging.

### Missing Functionality

- Support for RDMA Connection Manager (rdmacm)
  - may need to expose more of the Ethernet device to the rest of the driver
- Support for ibverbs async events
  - port up/down, qp failed, etc
- Support for ibverbs memory registration work requests

### Broken Functionality

- Support module unloading and reloading
  - fix the hang unloading the network device
  - figure out how to kill the tcp recv kthread (tcp backend)
  - check accuracy of resource reference counting

### Unstable Functionality

- Use coherent memory for ring index
  - Store the ring index in a separate, coherent buffer, instead of the
    dma-mapped ring buffer.  As is, a ring index *may* sneak ahead of ring data
    in non-coherent memory systems.  The ring data should be synced *after*
    obtaining the current ring index from the coherent buffer, to ensure that
    the data in the ring is at least as recent as the ring index.  Please refer
    to the Ethernet support code for the proper implementation.
- Quiesce and reset port state transitions
  - ensure that the port can reinitialize after a peer module reload or reboot.
  - ensure no outstanding dma exiting the quiesce state
  - ensure proper resource cleanup in the reset state
  - auto-reset after timeout if the peer fails to return

### Code Cleanup / Unacceptable Implementation

- Throw away the NTB abstraction
  - use the new Linux NTB interface directly
- Throw away the DMA abstraction
  - use the Linux dmaengine interface directly
  - Maybe: minimal abstraction to allow swapping the dma or tcp back end
- Throw away the operating system abstraction
  - use Linux synchronization, printing, and debug primitives directly
- The MSI message workaround might not be accepted upstream
  - what can we do... because a hardware lockup is bad news
  - NTRDMA only supports MSI, not MSI-X yet, and it's a hack

### Research Topics

- qperf flow control for send/recv requests
- NTRDMA as a fully-in-user-space driver
  - instantiate a user space IB device without /sys/class/infiniband entry
  - user space integration with non-kernel APIs
  - simulation over tcp in user space, using sockets
  - user space NTB and DMA hardware drivers

## See Also

## Author

Allen Hubbe \<Allen.Hubbe@emc.com\>

## License

Copyright (c) 2014, 2015 EMC Corporation. All rights reserved.

This driver is released under the terms of a dual BSD/GPLv2 license.  The
COPYING file in this repository is the same text of the GPLv2 license as it
appears in the Linux kernel.  Each code file in this repository also contains
BSD/GPLv2 license summary text at the top of the file, as it appears in RDMA
drivers in Linux and OFED.  This contribution and the software it contains, and
any patches properly signed-off-by and accepted into this repository, may be
used and included in Linux and/or OFED (at the discretion of the upstream
maintainers), and without exception, any other project with compatible license
terms.

