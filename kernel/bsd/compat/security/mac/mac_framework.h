#ifndef _SECURITY_MAC_MAC_FRAMEWORK_H_
#define _SECURITY_MAC_MAC_FRAMEWORK_H_

/* MAC - FreeBSD's Mandatory Access Control framework, stubbed.
 *
 * MAC lets a policy module label every object in the kernel (a packet, an
 * interface, a socket) and be consulted on every operation between them.
 * Network code is instrumented with mac_ifnet_check_transmit,
 * mac_ifnet_create_mbuf and friends at every layer boundary.
 *
 * Genesis has no MAC policies, no labels to attach and nothing to enforce.
 * A kernel built without "options MAC" - which is what GENERIC ships as -
 * compiles these to nothing, and so does this.
 *
 * Every check returns 0, meaning ALLOWED. That is the correct answer when
 * there is no policy: it is what an unlabelled FreeBSD kernel does. It is
 * also the permissive direction, which is worth stating - if a policy
 * framework ever exists here, every one of these call sites is already in
 * the right place and this file is what becomes real.
 */

#define mac_ifnet_check_transmit(ifp, m)    (0)
#define mac_ifnet_create_mbuf(ifp, m)       do { } while (0)
#define mac_mbuf_copy(m_from, m_to)         do { } while (0)
#define mac_netinet_firewall_send(m)        do { } while (0)
#define mac_netinet_firewall_reply(mrecv, msend) do { } while (0)
#define mac_bpfdesc_check_receive(d, ifp)   (0)
#define mac_socket_check_deliver(so, m)     (0)
#define mac_socket_create_mbuf(so, m)       do { } while (0)
#define mac_syncache_create_mbuf(sc, m)     do { } while (0)
#define mac_ipq_match(m, q)                 (1)
#define mac_ipq_create(m, q)                do { } while (0)
#define mac_ipq_update(m, q)                do { } while (0)
#define mac_ipq_destroy(q)                  do { } while (0)
#define mac_ipq_reassemble(q, m)            do { } while (0)
#define mac_netinet_fragment(m, frag)       do { } while (0)

#endif
