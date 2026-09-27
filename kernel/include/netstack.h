#ifndef NETSTACK_H
#define NETSTACK_H

#include "typesk.h"

/* Bring up the network stack: register the protocol handlers with netisr,
 * find the attached interface and give it an address.
 *
 * Called explicitly from flk.c, like every other bring-up step in this
 * kernel - see bus.h's "no magic" rule. Upstream this happens through
 * SYSINIT and the routing socket; there is neither here.
 *
 * The address is COMPILED IN, at QEMU user-mode networking's default:
 * 10.0.2.15/24 with the gateway at 10.0.2.2. There is no DHCP client, and
 * that is now the ONLY reason it is hardcoded - the SIOCAIFADDR ioctl is real
 * (netinet/in.c is vendored) and net_stack_init() issues it exactly as
 * ifconfig(8) would. What is missing is something to learn the address FROM,
 * not a way to set it. */
void net_stack_init(void);

/* One line per layer, for the boot log. */
void net_stack_report(uint8 color);

/* Send one ICMP echo REQUEST to `dst`, so the send path is exercised by
 * something other than a reply. Returns 0 if it was handed to the driver. */
int  net_ping(uint32 dst_be);

/* The interface everything is configured on, or NULL if this machine has no
 * NIC. The selftest checks it before doing anything. */
struct ifnet *net_interface(void);

/* This machine's IPv4 address, in network byte order. */
uint32 net_my_addr(void);

/* Two summary counters for the boot report. Both read the VENDORED
 * per-protocol statistics rather than anything netstack.c keeps, so they say
 * what FreeBSD's own accounting says happened. */
uint64 net_stat_rx_frames(void);
uint64 net_stat_icmp_echoes(void);

/* Send a ping to the gateway and wait for the answer. Returns the number of
 * failures. Must run AFTER sti - it waits for real received frames. */
int net_selftest(void);

/* Address configuration (network byte order throughout). net_configure
 * applies an address/mask/gateway to the NIC; net_configure_static applies
 * the compiled-in QEMU defaults. */
int    net_configure(uint32 addr, uint32 mask, uint32 gw);
int    net_configure_static(void);
uint32 net_gateway(void);
uint32 net_netmask(void);

/* DHCP (kernel/bsd/dhcp.c): acquire a lease - DISCOVER, OFFER, REQUEST,
 * ACK - and configure it, falling back to the static address if no server
 * answers. Needs interrupts and the scheduler (it sleeps). Starts a kernel
 * thread that renews the lease at T1. Returns 0 with a lease, 1 on the
 * static fallback. */
int  net_dhcp_start(void);
void net_dhcp_report(uint8 color);
/* What the last lease said, for tests and reports: 0 if none. */
uint32 net_dhcp_server(void);
uint32 net_dhcp_dns(void);
uint32 net_dhcp_lease_seconds(void);
uint64 net_dhcp_renewals(void);
int    net_dhcp_selftest(void);

#endif
