# NTRDMA

## Name

NTRDMA - Non-Transparent Remote Direct Memory Access

## Synopsis

Load the ntrdma kernel module:
```
SPA:~# modprobe ntrdma

SPB:~# modprobe ntrdma
```

Run RDMA applications.

## Description

NTRDMA is a device driver for the Linux and OFED RDMA software stack.  NTRDMA
uses a PCI-Express link, Non-Tranparent Bridge (NTB), and general purpose DMA
hardware, as an efficient transport for moving data between the memory systems
of closely connected peers.  The name NTRDMA comes from the combination of NTB
and RDMA.

The NTRDMA driver implements RDMA data structures in software, which would
normally be implemented in hardware on an Infiniband or RoCE host bus adapter.
The four primary data structures that NTRDMA provides to RDMA applications are
Protection Domains, Memory Regions, Queue Pairs, and Completion Queues.
Protection Domains are a context for Memory Regions and Queue Pairs, with the
primary function of limiting access to memory to only that which has been
explicily allowed by the peer application.  Memory Regions define ranges in the
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

TODO

### Remote Resources for RDMA

TODO

### Driver Initialization

TODO

### Virtual Doorbells

TODO

### Ping Pong Heartbeat

TODO

### Zero Copy Ethernet

TODO

### NTRDMA over TCP

TODO

## To Do

### Features To Do

- rdmacm support
- ntrdma fully-in-userspace driver
  - need a way to instantiate userspace ib device
    without an entry in /sys/class/infiniband
  - need to implement proto headers for userspace integration
  - should be easy to do ntrdma simulation over tcp in userspace
  - hw driver needs a way to poke ntb and dma from userspace

### Cleanup To Do

- module unloading
- quiesce and reset
- reindent for Linux coding style
- use coherent memory for ring index

### External To Do

- qperf send/recv flow control

## See Also

rxe - Software RoCE driver

## Author

Allen Hubbe \<Allen.Hubbe@emc.com\>

## License

This software has not been released.
