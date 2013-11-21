/* -*- indent-tabs-mode: nil; -*- */

#include "winutils.h"
#include "proxy.h"
#include "proxy_pollmgr.h"
#include "pxremap.h"

#ifndef RT_OS_WINDOWS
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>          /* XXX: inet_ntop */
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#else
#include <iprt/stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "winpoll.h"
#endif

#include "lwip/opt.h"

#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"

#if 1 /* XXX: force debug for now */
#undef  DPRINTF0
#undef  DPRINTF
#undef  DPRINTF1
#undef  DPRINTF2
#define DPRINTF0(args) do { printf args; } while (0)
#define DPRINTF(args)  do { printf args; } while (0)
#define DPRINTF1(args) do { printf args; } while (0)
#define DPRINTF2(args) do { printf args; } while (0)
#endif


/* forward */
struct ping_pcb;


/**
 * Global state for ping proxy collected in one entity to minimize
 * globals.  There's only one instance of this structure.
 *
 * Raw ICMP sockets are promiscuous, so it doesn't make sense to have
 * multiple.  If this code ever needs to support multiple netifs, the
 * netif member should be exiled into "pcb".
 */
struct pxping {
    SOCKET sock4;
    int ttl;
    int tos;

    SOCKET sock6;
    int hopl;

    struct pollmgr_handler pmhdl4;
    struct pollmgr_handler pmhdl6;

    struct netif *netif;

    /**
     * Protect lwIP and pmgr accesses to the list of pcbs.
     */
    sys_mutex_t lock;

    /*
     * We need to find pcbs both from the guest side and from the host
     * side.  If we need to support industrial grade ping throughput,
     * we will need two pcb hashes.  For now, a short linked list
     * should be enough.  Cf. pxping_pcb_for_request() and
     * pxping_pcb_for_reply().
     */
#define PXPING_MAX_PCBS 8
    size_t npcbs;
    struct ping_pcb *pcbs;

#define TIMEOUT 5
    int timer_active;
    size_t timeout_slot;
    struct ping_pcb *timeout_list[TIMEOUT];
};


/**
 * Quasi PCB for ping.
 */
struct ping_pcb {
    ipX_addr_t src;
    ipX_addr_t dst;

    u8_t is_ipv6;
    u8_t is_mapped;

    u16_t guest_id;
    u16_t host_id;

    /**
     * Desired slot in pxping::timeout_list.  See pxping_timer().
     */
    size_t timeout_slot;

    /**
     * Chaining for pxping::timeout_list
     */
    struct ping_pcb **pprev_timeout;
    struct ping_pcb *next_timeout;

    /**
     * Chaining for pxping::pcbs
     */
    struct ping_pcb *next;

    union {
        struct sockaddr_in sin;
        struct sockaddr_in6 sin6;
    } peer;
};


/**
 * lwIP thread callback message for IPv4 ping.
 *
 * We pass raw IP datagram for ip_output_if() so we only need pbuf and
 * netif (from pxping).
 */
struct ping_msg {
    struct tcpip_msg msg;
    struct pxping *pxping;
    struct pbuf *p;
};


/**
 * lwIP thread callback message for IPv6 ping.
 *
 * We cannot obtain raw IPv6 datagram from host without extra trouble,
 * so we pass ICMPv6 payload in pbuf and also other parameters to
 * ip6_output_if().
 */
struct ping6_msg {
    struct tcpip_msg msg;
    struct pxping *pxping;
    struct pbuf *p;
    ip6_addr_t src, dst;
    int hopl, tclass;
};


static void pxping_recv4(void *arg, struct pbuf *p);
static void pxping_recv6(void *arg, struct pbuf *p);

static void pxping_timer(void *arg);
static void pxping_timer_needed(struct pxping *pxping);

static struct ping_pcb *pxping_pcb_for_request(struct pxping *pxping,
                                               int is_ipv6,
                                               ipX_addr_t *src, ipX_addr_t *dst,
                                               u16_t guest_id);
static struct ping_pcb *pxping_pcb_for_reply(struct pxping *pxping, int is_ipv6,
                                             ipX_addr_t *dst, u16_t host_id);

static struct ping_pcb *pxping_pcb_allocate(struct pxping *pxping);
static void pxping_pcb_register(struct pxping *pxping, struct ping_pcb *pcb);
static void pxping_pcb_deregister(struct pxping *pxping, struct ping_pcb *pcb);
static void pxping_pcb_delete(struct pxping *pxping, struct ping_pcb *pcb);
static void pxping_timeout_add(struct pxping *pxping, struct ping_pcb *pcb);
static void pxping_timeout_del(struct pxping *pxping, struct ping_pcb *pcb);
static void pxping_pcb_debug_print(struct ping_pcb *pcb);

static int pxping_pmgr_pump(struct pollmgr_handler *handler, SOCKET fd, int revents);

static void pxping_pmgr_icmp4(struct pxping *pxping);
static void pxping_pmgr_icmp4_echo(struct pxping *pxping,
                                   u16_t iplen, struct sockaddr_in *peer);
static void pxping_pmgr_icmp4_error(struct pxping *pxping,
                                    u16_t iplen, struct sockaddr_in *peer);
static void pxping_pmgr_icmp6(struct pxping *pxping);
static void pxping_pmgr_icmp6_echo(struct pxping *pxping,
                                   u16_t iplen, struct sockaddr_in6 *peer);
static void pxping_pmgr_icmp6_error(struct pxping *pxping,
                                    u16_t iplen, struct sockaddr_in6 *peer);

static void pxping_pmgr_forward_inbound(struct pxping *pxping, u16_t iplen);
static void pxping_pcb_forward_inbound(void *arg);

static void pxping_pmgr_forward_inbound6(struct pxping *pxping,
                                         ip6_addr_t *src, ip6_addr_t *dst,
                                         u8_t hopl, u8_t tclass,
                                         u16_t icmplen);
static void pxping_pcb_forward_inbound6(void *arg);

/*
 * NB: This is not documented except in RTFS.
 *
 * If ip_output_if() is passed dest == NULL then it treats p as
 * complete IP packet with payload pointing to the IP header.  It does
 * not build IP header, ignores all header-related arguments, fetches
 * real destination from the header in the pbuf and outputs pbuf to
 * the specified netif.
 */
#define ip_raw_output_if(p, netif)                      \
    (ip_output_if((p), NULL, NULL, 0, 0, 0, (netif)))



static struct pxping g_pxping;


err_t
pxping_init(struct netif *netif, SOCKET sock4, SOCKET sock6)
{
    const int on = 1;
    int status;

    if (sock4 == INVALID_SOCKET && sock6 == INVALID_SOCKET) {
        return ERR_VAL;
    }

    g_pxping.netif = netif;
    sys_mutex_new(&g_pxping.lock);

    g_pxping.sock4 = sock4;
    if (g_pxping.sock4 != INVALID_SOCKET) {
        g_pxping.ttl = -1;
        g_pxping.tos = 0;

        g_pxping.pmhdl4.callback = pxping_pmgr_pump;
        g_pxping.pmhdl4.data = (void *)&g_pxping;
        g_pxping.pmhdl4.slot = -1;
        pollmgr_add(&g_pxping.pmhdl4, g_pxping.sock4, POLLIN);

        ping_proxy_accept(pxping_recv4, &g_pxping);
    }

    g_pxping.sock6 = sock6;
    if (g_pxping.sock6 != INVALID_SOCKET) {
        g_pxping.hopl = -1;

#if !defined(IPV6_RECVPKTINFO)
#define IPV6_RECVPKTINFO (IPV6_PKTINFO)
#endif
        status = setsockopt(sock6, IPPROTO_IPV6, IPV6_RECVPKTINFO,
                            (const char *)&on, sizeof(on));
        if (status < 0) {
            perror("IPV6_RECVPKTINFO");
            /* XXX: this is fatal */
        }

#if !defined(IPV6_RECVHOPLIMIT)
#define IPV6_RECVHOPLIMIT (IPV6_HOPLIMIT)
#endif
        status = setsockopt(sock6, IPPROTO_IPV6, IPV6_RECVHOPLIMIT,
                            (const char *)&on, sizeof(on));
        if (status < 0) {
            perror("IPV6_RECVHOPLIMIT");
        }

#ifdef IPV6_RECVTCLASS
        /* TODO: IPV6_RECVTCLASS */
#endif

        g_pxping.pmhdl6.callback = pxping_pmgr_pump;
        g_pxping.pmhdl6.data = (void *)&g_pxping;
        g_pxping.pmhdl6.slot = -1;
        pollmgr_add(&g_pxping.pmhdl6, g_pxping.sock6, POLLIN);

        ping6_proxy_accept(pxping_recv6, &g_pxping);
    }

    return ERR_OK;
}


static u32_t
update16_with_chksum(u16_t *oldp, u16_t h)
{
    u32_t sum = (u16_t)~*oldp;
    sum += h;

    *oldp = h;
    return sum;
}


static u32_t
update32_with_chksum(u32_t *oldp, u32_t u)
{
    u32_t sum = ~*oldp;
    sum = FOLD_U32T(sum);
    sum += FOLD_U32T(u);

    *oldp = u;
    return sum;
}


static u32_t
updateip6_with_chksum(ip6_addr_t *oldp, const ip6_addr_t *ip6)
{
    u32_t sum;

    sum  = update32_with_chksum(&oldp->addr[0], ip6->addr[0]);
    sum += update32_with_chksum(&oldp->addr[1], ip6->addr[1]);
    sum += update32_with_chksum(&oldp->addr[2], ip6->addr[2]);
    sum += update32_with_chksum(&oldp->addr[3], ip6->addr[3]);

    return sum;
}


/**
 * ICMP Echo Request in pbuf "p" is to be proxied.
 */
static void
pxping_recv4(void *arg, struct pbuf *p)
{
    struct pxping *pxping = (struct pxping *)arg;
    struct ping_pcb *pcb;
    struct ip_hdr *iph;
    struct icmp_echo_hdr *icmph;
    int ttl, tos;
    u32_t sum;
    u16_t iphlen;
    u16_t id, seq;
    int status;

    iph = (/* UNCONST */ struct ip_hdr *)ip_current_header();
    iphlen = ip_current_header_tot_len();

    icmph = (struct icmp_echo_hdr *)p->payload;

    id  = icmph->id;
    seq = icmph->seqno;

    pcb = pxping_pcb_for_request(pxping, 0,
                                 ipX_current_src_addr(),
                                 ipX_current_dest_addr(),
                                 id);
    if (pcb == NULL) {
        pbuf_free(p);
        return;
    }

    pxping_pcb_debug_print(pcb); /* XXX */
    printf(" seq %d len %u ttl %d\n",
           ntohs(seq), (unsigned int)p->tot_len,
           IPH_TTL(iph));

    ttl = IPH_TTL(iph);
    if (!pcb->is_mapped) {
        if (ttl == 1) {
            pbuf_header(p, iphlen); /* back to IP header */
            icmp_time_exceeded(p, ICMP_TE_TTL);
            return;
        }
        --ttl;
    }

    /* rewrite ICMP echo header */
    sum = (u16_t)~icmph->chksum;
    sum += update16_with_chksum(&icmph->id, pcb->host_id);
    sum = FOLD_U32T(sum);
    icmph->chksum = ~sum;

    if (ttl != pxping->ttl) {
        status = setsockopt(pxping->sock4, IPPROTO_IP, IP_TTL,
                            (char *)&ttl, sizeof(ttl));
        if (status == 0) {
            pxping->ttl = ttl;
        }
        else {
            perror("IP_TTL");
        }
    }

    tos = IPH_TOS(iph);
    if (tos != pxping->tos) {
        status = setsockopt(pxping->sock4, IPPROTO_IP, IP_TOS,
                            (char *)&tos, sizeof(tos));
        if (status == 0) {
            pxping->tos = tos;
        }
        else {
            perror("IP_TOS");
        }
    }

    proxy_sendto(pxping->sock4, p,
                 &pcb->peer.sin, sizeof(pcb->peer.sin));

    pbuf_free(p);
}


/**
 * ICMPv6 Echo Request in pbuf "p" is to be proxied.
 */
static void
pxping_recv6(void *arg, struct pbuf *p)
{
    struct pxping *pxping = (struct pxping *)arg;
    struct ping_pcb *pcb;
    struct ip6_hdr *iph;
    struct icmp6_echo_hdr *icmph;
    int hopl;
    u32_t sum;
    u16_t iphlen;
    u16_t id, seq;
    int status;

    iph = (/* UNCONST */ struct ip6_hdr *)ip6_current_header();
    iphlen = ip_current_header_tot_len();

    icmph = (struct icmp6_echo_hdr *)p->payload;

    id  = icmph->id;
    seq = icmph->seqno;

    pcb = pxping_pcb_for_request(pxping, 1,
                                 ipX_current_src_addr(),
                                 ipX_current_dest_addr(),
                                 id);
    if (pcb == NULL) {
        pbuf_free(p);
        return;
    }

    pxping_pcb_debug_print(pcb); /* XXX */
    printf(" seq %d len %u hopl %d\n",
           ntohs(seq), (unsigned int)p->tot_len,
           IP6H_HOPLIM(iph));

    hopl = IP6H_HOPLIM(iph);
    if (!pcb->is_mapped) {
        if (hopl == 1) {
            pbuf_header(p, iphlen); /* back to IP header */
            icmp6_time_exceeded(p, ICMP6_TE_HL);
            return;
        }
        --hopl;
    }

    /*
     * Rewrite ICMPv6 echo header.  We don't need to recompute the
     * checksum since, unlike IPv4, checksum includes pseudo-header.
     * OS computes checksum for us on send() since it needs to select
     * source address.
     */
    icmph->id = pcb->host_id;

    /* TODO: use control messages to save a syscall? */
    if (hopl != pxping->hopl) {
        status = setsockopt(pxping->sock6, IPPROTO_IPV6, IPV6_UNICAST_HOPS,
                            (char *)&hopl, sizeof(hopl));
        if (status == 0) {
            pxping->hopl = hopl;
        }
        else {
            perror("IPV6_HOPLIMIT");
        }
    }

    proxy_sendto(pxping->sock6, p,
                 &pcb->peer.sin6, sizeof(pcb->peer.sin6));

    pbuf_free(p);
}


static void
pxping_pcb_debug_print(struct ping_pcb *pcb)
{
    char addrbuf[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"];
    const char *addrstr;
    int sdom = pcb->is_ipv6 ? AF_INET6 : AF_INET;

    DPRINTF(("ping %p:", (void *)pcb));

    addrstr = inet_ntop(sdom, (void *)&pcb->src, addrbuf, sizeof(addrbuf));
    DPRINTF((" %s", addrstr));

    DPRINTF((" ->"));

    addrstr = inet_ntop(sdom, (void *)&pcb->dst, addrbuf, sizeof(addrbuf));
    DPRINTF((" %s", addrstr));

    DPRINTF((" id %04x->%04x", ntohs(pcb->guest_id), ntohs(pcb->host_id)));
}


static struct ping_pcb *
pxping_pcb_allocate(struct pxping *pxping)
{
    struct ping_pcb *pcb;

    if (pxping->npcbs >= PXPING_MAX_PCBS) {
        return NULL;
    }

    pcb = (struct ping_pcb *)malloc(sizeof(*pcb));
    if (pcb == NULL) {
        return NULL;
    }

    ++pxping->npcbs;
    return pcb;
}


static void
pxping_pcb_delete(struct pxping *pxping, struct ping_pcb *pcb)
{
    LWIP_ASSERT1(pxping->npcbs > 0);
    LWIP_ASSERT1(pxping->next == NULL);
    LWIP_ASSERT1(pxping->pprev_timeout == NULL);

    printf("%s: ping %p\n", __func__, (void *)pcb);

    --pxping->npcbs;
    free(pcb);
}


static void
pxping_timeout_add(struct pxping *pxping, struct ping_pcb *pcb)
{
    struct ping_pcb **chain;

    LWIP_ASSERT1(pcb->pprev_timeout == NULL);

    chain = &pxping->timeout_list[pcb->timeout_slot];
    if ((pcb->next_timeout = *chain) != NULL) {
        (*chain)->pprev_timeout = &pcb->next_timeout;
    }
    *chain = pcb;
    pcb->pprev_timeout = chain;
}


static void
pxping_timeout_del(struct pxping *pxping, struct ping_pcb *pcb)
{
    LWIP_UNUSED_ARG(pxping);

    LWIP_ASSERT1(pcb->pprev_timeout != NULL);
    if (pcb->next_timeout != NULL) {
        pcb->next_timeout->pprev_timeout = pcb->pprev_timeout;
    }
    *pcb->pprev_timeout = pcb->next_timeout;
    pcb->pprev_timeout = NULL;
    pcb->next_timeout = NULL;
}


static void
pxping_pcb_register(struct pxping *pxping, struct ping_pcb *pcb)
{
    pcb->next = pxping->pcbs;
    pxping->pcbs = pcb;

    pxping_timeout_add(pxping, pcb);
}


static void
pxping_pcb_deregister(struct pxping *pxping, struct ping_pcb *pcb)
{
    struct ping_pcb **p;

    for (p = &pxping->pcbs; *p != NULL; p = &(*p)->next) {
        if (*p == pcb) {
            *p = pcb->next;
            break;
        }
    }

    pxping_timeout_del(pxping, pcb);
}


static struct ping_pcb *
pxping_pcb_for_request(struct pxping *pxping,
                       int is_ipv6, ipX_addr_t *src, ipX_addr_t *dst,
                       u16_t guest_id)
{
    struct ping_pcb *pcb;

    /* on lwip thread, so no concurrent updates */
    for (pcb = pxping->pcbs; pcb != NULL; pcb = pcb->next) {
        if (pcb->guest_id == guest_id
            && pcb->is_ipv6 == is_ipv6
            && ipX_addr_cmp(is_ipv6, &pcb->dst, dst)
            && ipX_addr_cmp(is_ipv6, &pcb->src, src))
        {
            break;
        }
    }

    if (pcb == NULL) {
        int mapped;

        pcb = pxping_pcb_allocate(pxping);
        if (pcb == NULL) {
            return NULL;
        }

        pcb->is_ipv6 = is_ipv6;
        ipX_addr_copy(is_ipv6, pcb->src, *src);
        ipX_addr_copy(is_ipv6, pcb->dst, *dst);

        pcb->guest_id = guest_id;
#ifdef RT_OS_WINDOWS
# define random() (rand())
#endif
        pcb->host_id = random() & 0xffffUL;

        pcb->pprev_timeout = NULL;
        pcb->next_timeout = NULL;

        if (is_ipv6) {
            pcb->peer.sin6.sin6_family = AF_INET6;
#if HAVE_SA_LEN
            pcb->peer.sin6.sin6_len = sizeof(pcb->peer.sin6);
#endif
            pcb->peer.sin.sin_port = htons(IPPROTO_ICMPV6);
            mapped = pxremap_outbound_ip6((ip6_addr_t *)&pcb->peer.sin6.sin6_addr,
                                          ipX_2_ip6(&pcb->dst));
        }
        else {
            pcb->peer.sin.sin_family = AF_INET;
#if HAVE_SA_LEN
            pcb->peer.sin.sin_len = sizeof(pcb->peer.sin);
#endif
            pcb->peer.sin.sin_port = htons(IPPROTO_ICMP);
            mapped = pxremap_outbound_ip4((ip_addr_t *)&pcb->peer.sin.sin_addr,
                                          ipX_2_ip(&pcb->dst));
        }

        if (mapped == PXREMAP_FAILED) {
            free(pcb);
            return NULL;
        }
        else {
            pcb->is_mapped = (mapped == PXREMAP_MAPPED);
        }

        pcb->timeout_slot = pxping->timeout_slot;

        sys_mutex_lock(&pxping->lock);
        pxping_pcb_register(pxping, pcb);
        sys_mutex_unlock(&pxping->lock);

        pxping_pcb_debug_print(pcb); /* XXX */
        printf(" - created\n");

        pxping_timer_needed(pxping);
    }
    else {
        /* just bump up expiration timeout lazily */
        pxping_pcb_debug_print(pcb); /* XXX */
        printf(" - slot %d -> %d\n",
               (unsigned int)pcb->timeout_slot,
               (unsigned int)pxping->timeout_slot);
        pcb->timeout_slot = pxping->timeout_slot;
    }

    return pcb;
}


/**
 * Called on pollmgr thread.  Caller must do the locking since caller
 * is going to use the returned pcb, which needs to be protected from
 * being expired by pxping_timer() on lwip thread.
 */
static struct ping_pcb *
pxping_pcb_for_reply(struct pxping *pxping,
                     int is_ipv6, ipX_addr_t *dst, u16_t host_id)
{
    struct ping_pcb *pcb;

    for (pcb = pxping->pcbs; pcb != NULL; pcb = pcb->next) {
        if (pcb->host_id == host_id
            && pcb->is_ipv6 == is_ipv6
            /* XXX: allow broadcast pings? */
            && ipX_addr_cmp(is_ipv6, &pcb->dst, dst))
        {
            return pcb;
        }
    }

    return NULL;
}


static void
pxping_timer(void *arg)
{
    struct pxping *pxping = (struct pxping *)arg;
    struct ping_pcb **chain, *pcb;

    pxping->timer_active = 0;

    /*
     * New slot points to the list of pcbs to check for expiration.
     */
    LWIP_ASSERT1(pxping->timeout_slot < TIMEOUT);
    if (++pxping->timeout_slot == TIMEOUT) {
        pxping->timeout_slot = 0;
    }

    chain = &pxping->timeout_list[pxping->timeout_slot];
    pcb = *chain;

    /* protect from pollmgr concurrent reads */
    sys_mutex_lock(&pxping->lock);

    while (pcb != NULL) {
        struct ping_pcb *xpcb = pcb;
        pcb = pcb->next_timeout;

        if (xpcb->timeout_slot == pxping->timeout_slot) {
            /* expired */
            printf("... ");
            pxping_pcb_debug_print(xpcb);
            printf(" - expired\n");

            pxping_pcb_deregister(pxping, xpcb);
            pxping_pcb_delete(pxping, xpcb);
        }
        else {
            /*
             * If there was another request, we updated timeout_slot
             * but delayed actually moving the pcb until now.
             */
            printf("... ");
            pxping_pcb_debug_print(xpcb);
            printf(" - alive slot %d -> %d\n",
                   (unsigned int)pxping->timeout_slot,
                   (unsigned int)xpcb->timeout_slot);

            pxping_timeout_del(pxping, xpcb); /* from current slot */
            pxping_timeout_add(pxping, xpcb); /* to new slot */
        }
    }

    sys_mutex_unlock(&pxping->lock);
    pxping_timer_needed(pxping);
}


static void
pxping_timer_needed(struct pxping *pxping)
{
    if (!pxping->timer_active && pxping->pcbs != NULL) {
        pxping->timer_active = 1;
        sys_timeout(1 * 1000, pxping_timer, pxping);
    }
}


static int
pxping_pmgr_pump(struct pollmgr_handler *handler, SOCKET fd, int revents)
{
    struct pxping *pxping;

    pxping = (struct pxping *)handler->data;
    LWIP_ASSERT1(fd == pxping->sock4 || fd == pxping->sock6);

    if (revents & ~(POLLIN|POLLERR)) {
        DPRINTF0(("%s: unexpected revents 0x%x\n", __func__, revents));
        return POLLIN;
    }

    if (revents & POLLERR) {
        int sockerr = -1;
        socklen_t optlen = (socklen_t)sizeof(sockerr);
        int status;

        status = getsockopt(fd, SOL_SOCKET,
                            SO_ERROR, (char *)&sockerr, &optlen);
        if (status < 0) {
            DPRINTF(("%s: sock %d: SO_ERROR failed with errno %d\n",
                     __func__, fd, errno));
        }
        else {
            DPRINTF(("%s: sock %d: errno %d\n",
                     __func__, fd, sockerr));
        }
    }

    if ((revents & POLLIN) == 0) {
        return POLLIN;
    }

    if (fd == pxping->sock4) {
        pxping_pmgr_icmp4(pxping);
    }
    else /* fd == pxping->sock6 */ {
        pxping_pmgr_icmp6(pxping);
    }

    return POLLIN;
}


/**
 * Process incoming ICMP message for the host.
 * NB: we will get a lot of spam here and have to sift through it.
 */
static void
pxping_pmgr_icmp4(struct pxping *pxping)
{
    struct sockaddr_in sin;
    socklen_t salen = sizeof(sin);
    ssize_t nread;
    struct ip_hdr *iph;
    struct icmp_echo_hdr *icmph;
    u16_t iplen;

    memset(&sin, 0, sizeof(sin));

    /*
     * Reads from raw IPv4 sockets deliver complete IP datagrams with
     * IP header included.
     */
    nread = recvfrom(pxping->sock4, pollmgr_udpbuf, sizeof(pollmgr_udpbuf), 0,
                     (struct sockaddr *)&sin, &salen);
    if (nread < 0) {
        perror(__func__);
        return;
    }

    if (nread < IP_HLEN) {
        DPRINTF2(("%s: read %d bytes, IP header truncated\n",
                  __func__, (unsigned int)nread));
        return;
    }

    iph = (struct ip_hdr *)pollmgr_udpbuf;

    /* match version */
    if (IPH_V(iph) != 4) {
        DPRINTF2(("%s: unexpected IP version %d\n", __func__, IPH_V(iph)));
        return;
    }

    /* no fragmentation */
    if ((IPH_OFFSET(iph) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0) {
        DPRINTF2(("%s: dropping fragmented datagram\n", __func__));
        return;
    }

    /* no options */
    if (IPH_HL(iph) * 4 != IP_HLEN) {
        DPRINTF2(("%s: dropping datagram with options (IP header length %d)\n",
                  __func__, IPH_HL(iph) * 4));
        return;
    }

    if (IPH_PROTO(iph) != IP_PROTO_ICMP) {
        DPRINTF2(("%s: unexpected protocol %d\n", __func__, IPH_PROTO(iph)));
        return;
    }

    /* XXX: TODO: not for loopback */
    if (IPH_TTL(iph) == 1) {
        DPRINTF2(("%s: dropping packet with ttl 1\n", __func__));
        return;
    }

    iplen = IPH_LEN(iph);
#if !defined(RT_OS_DARWIN)
    /* darwin reports IPH_LEN in host byte order */
    iplen = ntohs(iplen);
#endif
#if defined(RT_OS_DARWIN) || defined(RT_OS_SOLARIS)
    /* darwin and solaris change IPH_LEN to payload length only */
    iplen += IP_HLEN;           /* we verified there are no options */
    IPH_LEN(iph) = htons(iplen);
#endif
    if (nread < iplen) {
        DPRINTF2(("%s: read %d bytes but total length is %d bytes\n",
                  __func__, (unsigned int)nread, (unsigned int)iplen));
        return;
    }

    if (iplen < IP_HLEN + ICMP_HLEN) {
        DPRINTF2(("%s: IP length %d bytes, ICMP header truncated\n",
                  __func__, iplen));
        return;
    }

    icmph = (struct icmp_echo_hdr *)(pollmgr_udpbuf + IP_HLEN);
    if (ICMPH_TYPE(icmph) == ICMP_ER) {
        pxping_pmgr_icmp4_echo(pxping, iplen, &sin);
    }
    else if (ICMPH_TYPE(icmph) == ICMP_DUR || ICMPH_TYPE(icmph) == ICMP_TE) {
        pxping_pmgr_icmp4_error(pxping, iplen, &sin);
    }
#if 1
    else {
        DPRINTF2(("%s: ignoring ICMP type %d\n", __func__, ICMPH_TYPE(icmph)));
    }
#endif
}


/**
 * Check if this incoming ICMP echo reply is for one of our pings and
 * forward it to the guest.
 */
static void
pxping_pmgr_icmp4_echo(struct pxping *pxping,
                       u16_t iplen, struct sockaddr_in *peer)
{
    struct ip_hdr *iph;
    struct icmp_echo_hdr *icmph;
    u16_t id, seq;
    int mapped;
    struct ping_pcb *pcb;
    ip_addr_t guest_ip, target_ip, unmapped_target_ip;
    u16_t guest_id;
    u32_t sum;

    iph = (struct ip_hdr *)pollmgr_udpbuf;
    icmph = (struct icmp_echo_hdr *)(pollmgr_udpbuf + IP_HLEN);

    id  = icmph->id;
    seq = icmph->seqno;

    {
        char addrbuf[sizeof "255.255.255.255"];
        const char *addrstr;

        addrstr = inet_ntop(AF_INET, &peer->sin_addr, addrbuf, sizeof(addrbuf));
        DPRINTF(("<--- PING %s id 0x%x seq %d\n",
                 addrstr, ntohs(id), ntohs(seq)));
    }

    ip_addr_copy(target_ip, iph->src);
    mapped = pxremap_inbound_ip4(&unmapped_target_ip, &target_ip);
    if (mapped == PXREMAP_FAILED) {
        return;
    }

    sys_mutex_lock(&pxping->lock);
    pcb = pxping_pcb_for_reply(pxping, 0, ip_2_ipX(&unmapped_target_ip), id);
    if (pcb == NULL) {
        sys_mutex_unlock(&pxping->lock);
        DPRINTF2(("%s: no match\n", __func__));
        return;
    }

    DPRINTF2(("%s: pcb %p\n", __func__, (void *)pcb));

    /* save info before unlocking since pcb may expire */
    ip_addr_copy(guest_ip, *ipX_2_ip(&pcb->src));
    guest_id = pcb->guest_id;

    sys_mutex_unlock(&pxping->lock);

    /* rewrite ICMP echo header */
    sum = (u16_t)~icmph->chksum;
    sum += update16_with_chksum(&icmph->id, guest_id);
    sum = FOLD_U32T(sum);
    icmph->chksum = ~sum;

    /* rewrite IP header */
    sum = (u16_t)~IPH_CHKSUM(iph);
    sum += update32_with_chksum((u32_t *)&iph->dest,
                                ip4_addr_get_u32(&guest_ip));
    if (mapped == PXREMAP_MAPPED) {
        sum += update32_with_chksum((u32_t *)&iph->src,
                                    ip4_addr_get_u32(&unmapped_target_ip));
    }
    else {
        IPH_TTL_SET(iph, IPH_TTL(iph) - 1);
        sum += PP_NTOHS(~0x0100);
    }
    sum = FOLD_U32T(sum);
    IPH_CHKSUM_SET(iph, ~sum);

    pxping_pmgr_forward_inbound(pxping, iplen);
}


/**
 * Check if this incoming ICMP error (destination unreachable or time
 * exceeded) is about one of our pings and forward it to the guest.
 */
static void
pxping_pmgr_icmp4_error(struct pxping *pxping,
                        u16_t iplen, struct sockaddr_in *peer)
{
    struct ip_hdr *iph, *oiph;
    struct icmp_echo_hdr *icmph, *oicmph;
    u16_t oipoff, oiphlen, oiplen;
    u16_t id, seq;
    struct ping_pcb *pcb;
    ip_addr_t pcb_src, pcb_dst;
    u16_t guest_id;
    int mapped;
    u32_t sum;

    iph = (struct ip_hdr *)pollmgr_udpbuf;
    icmph = (struct icmp_echo_hdr *)(pollmgr_udpbuf + IP_HLEN);

    oipoff = IP_HLEN + ICMP_HLEN;
    oiplen = iplen - oipoff; /* NB: truncated length, not IPH_LEN(oiph) */
    if (oiplen < IP_HLEN) {
        DPRINTF2(("%s: original datagram truncated to %d bytes\n",
                  __func__, oiplen));
    }

    /* IP header of the original message */
    oiph = (struct ip_hdr *)(pollmgr_udpbuf + oipoff);

    /* match version */
    if (IPH_V(oiph) != 4) {
        DPRINTF2(("%s: unexpected IP version %d\n", __func__, IPH_V(oiph)));
        return;
    }

    /* can't match fragments except the first one */
    if ((IPH_OFFSET(oiph) & PP_HTONS(IP_OFFMASK)) != 0) {
        DPRINTF2(("%s: ignoring fragment with offset %d\n",
                  __func__, ntohs(IPH_OFFSET(oiph) & PP_HTONS(IP_OFFMASK))));
        return;
    }

    if (IPH_PROTO(oiph) != IP_PROTO_ICMP) {
#if 0
        /* don't spam with every "destination unreachable" in the system */
        DPRINTF2(("%s: ignoring protocol %d\n", __func__, IPH_PROTO(oiph)));
#endif
        return;
    }

    oiphlen = IPH_HL(oiph) * 4;
    if (oiplen < oiphlen + ICMP_HLEN) {
        DPRINTF2(("%s: original datagram truncated to %d bytes\n",
                  __func__, oiplen));
        return;
    }

    oicmph = (struct icmp_echo_hdr *)(pollmgr_udpbuf + oipoff + oiphlen);
    if (ICMPH_TYPE(oicmph) != ICMP_ECHO) {
        DPRINTF2(("%s: ignoring ICMP error for original ICMP type %d\n",
                  __func__, ICMPH_TYPE(oicmph)));
        return;
    }

    id  = oicmph->id;
    seq = oicmph->seqno;

    {
        char addrbuf[sizeof "255.255.255.255"];
        const char *addrstr;

        addrstr = inet_ntop(AF_INET, &oiph->dest, addrbuf, sizeof(addrbuf));
        DPRINTF2(("%s: ping %s id 0x%x seq %d",
                  __func__, addrstr, ntohs(id), ntohs(seq)));
    }

    if (ICMPH_TYPE(icmph) == ICMP_DUR) {
        DPRINTF2((" unreachable (code %d)\n", ICMPH_CODE(icmph)));
    }
    else {
        DPRINTF2((" time exceeded\n"));
    }

    sys_mutex_lock(&pxping->lock);
    pcb = pxping_pcb_for_reply(pxping, 0, ip_2_ipX(&oiph->dest), id);
    if (pcb == NULL) {
        sys_mutex_unlock(&pxping->lock);
        DPRINTF2(("%s: no match\n", __func__));
        return;
    }

    DPRINTF2(("%s: pcb %p\n", __func__, (void *)pcb));

    /* save info before unlocking since pcb may expire */
    mapped = pcb->is_mapped;
    ip_addr_copy(pcb_src, *ipX_2_ip(&pcb->src));
    ip_addr_copy(pcb_dst, *ipX_2_ip(&pcb->dst));
    guest_id = pcb->guest_id;

    sys_mutex_unlock(&pxping->lock);

    /*
     * NB: Checksum in the outer ICMP error header is not affected by
     * changes to inner headers.
     */

    /* rewrite inner ICMP echo header */
    sum = (u16_t)~oicmph->chksum;
    sum += update16_with_chksum(&oicmph->id, guest_id);
    sum = FOLD_U32T(sum);
    oicmph->chksum = ~sum;

    /* rewrite inner IP header */
    sum = (u16_t)~IPH_CHKSUM(oiph);
    sum += update32_with_chksum((u32_t *)&oiph->src, ip4_addr_get_u32(&pcb_src));
    /* XXX: FIXME: rewrite dst if mapped */
    sum = FOLD_U32T(sum);
    IPH_CHKSUM_SET(oiph, ~sum);

    /* rewrite outer IP header */
    sum = (u16_t)~IPH_CHKSUM(iph);
    sum += update32_with_chksum((u32_t *)&iph->dest, ip4_addr_get_u32(&pcb_src));
    if (!mapped) { /* XXX: FIXME: error may be from elsewhere */
        IPH_TTL_SET(iph, IPH_TTL(iph) - 1);
        sum += PP_NTOHS(~0x0100);
    }
    sum = FOLD_U32T(sum);
    IPH_CHKSUM_SET(iph, ~sum);

    pxping_pmgr_forward_inbound(pxping, iplen);
}


static void
pxping_pmgr_icmp6(struct pxping *pxping)
{
    struct msghdr mh;
    struct iovec iov[1];
    static u8_t cmsgbuf[128];
    struct cmsghdr *cmh;
    struct sockaddr_in6 sin6;
    socklen_t salen = sizeof(sin6);
    ssize_t nread;
    struct icmp6_echo_hdr *icmph;
#if defined(RT_OS_LINUX) && !defined(__USE_GNU)
    /* XXX: https://sourceware.org/bugzilla/show_bug.cgi?id=6775 */
    struct in6_pktinfo {
        struct in6_addr ipi6_addr;
        unsigned int ipi6_ifindex;
    };
#endif
    struct in6_pktinfo *pktinfo;
    int hopl, tclass;

    int mapped;
    struct ping_pcb *pcb;
    ip6_addr_t guest_ip, target_ip, unmapped_target_ip;
    u16_t id, guest_id;
    u32_t sum;

    char addrbuf[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"];
    const char *addrstr;

    /*
     * Reads from raw IPv6 sockets deliver only the payload.  Full
     * headers are available via recvmsg(2)/cmsg(3).
     */
    memset(&mh, 0, sizeof(mh));
    mh.msg_name = &sin6;
    mh.msg_namelen = sizeof(sin6);
    iov[0].iov_base = pollmgr_udpbuf;
    iov[0].iov_len = sizeof(pollmgr_udpbuf);
    mh.msg_iov = iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cmsgbuf;
    mh.msg_controllen = sizeof(cmsgbuf);
    mh.msg_flags = 0;

    nread = recvmsg(pxping->sock6, &mh, 0);
    if (nread < 0) {
        perror(__func__);
        return;
    }
    addrstr = inet_ntop(AF_INET6, (void *)&sin6.sin6_addr, addrbuf, sizeof(addrbuf));

    icmph = (struct icmp6_echo_hdr *)pollmgr_udpbuf;
    DPRINTF2(("%s: %s ICMPv6: ", __func__, addrstr));

    id = 0;
    if (icmph->type == ICMP6_TYPE_EREP) {
        id = icmph->id;
        DPRINTF2(("echo reply %04x %u\n",
                  (unsigned int)icmph->id, (unsigned int)icmph->seqno));
    }
    else { /* XXX */
        if (icmph->type == ICMP6_TYPE_EREQ) {
            DPRINTF2(("echo request %04x %u\n",
                      (unsigned int)icmph->id, (unsigned int)icmph->seqno));
        }
        else if (icmph->type == ICMP6_TYPE_DUR) {
            DPRINTF2(("destination unreachable\n"));
        }
        else if (icmph->type == ICMP6_TYPE_PTB) {
            DPRINTF2(("packet too big\n"));
        }
        else if (icmph->type == ICMP6_TYPE_TE) {
            DPRINTF2(("time exceeded\n"));
        }
        else if (icmph->type == ICMP6_TYPE_PP) {
            DPRINTF2(("parameter problem\n"));
        }
        else {
            DPRINTF2(("type %d len %u\n", icmph->type, (unsigned int)nread));
        }
        return;
    }

    /* XXX: refactor into pxping_pmgr_icmp6_echo(), pxping_pmgr_icmp6_error() */

    pktinfo = NULL;
    hopl = -1;
    tclass = -1;
    for (cmh = CMSG_FIRSTHDR(&mh); cmh != NULL; cmh = CMSG_NXTHDR(&mh, cmh)) {
        if (cmh->cmsg_len == 0)
            break;

        if (cmh->cmsg_level == IPPROTO_IPV6
            && cmh->cmsg_type == IPV6_HOPLIMIT
            && cmh->cmsg_len == CMSG_LEN(sizeof(int)))
        {
            hopl = *(int *)CMSG_DATA(cmh);
            DPRINTF2(("hoplimit = %d\n", hopl));
        }

        if (cmh->cmsg_level == IPPROTO_IPV6
            && cmh->cmsg_type == IPV6_PKTINFO
            && cmh->cmsg_len == CMSG_LEN(sizeof(struct in6_pktinfo)))
        {
            pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmh);
            DPRINTF2(("pktinfo found\n"));
        }
    }

    if (pktinfo == NULL) {
        /*
         * ip6_output_if() doesn't do checksum for us so we need to
         * manually recompute it - for this we must know the
         * destination address of the pseudo-header that we will
         * rewrite with guest's address.
         */
        DPRINTF2(("%s: unable to get pktinfo\n", __func__));
        return;
    }

    ip6_addr_copy(target_ip, *(ip6_addr_t *)&sin6.sin6_addr);
    mapped = pxremap_inbound_ip6(&unmapped_target_ip, &target_ip);
    if (mapped == PXREMAP_FAILED) {
        return;
    }

    sys_mutex_lock(&pxping->lock);
    pcb = pxping_pcb_for_reply(pxping, 1, ip6_2_ipX(&unmapped_target_ip), id);
    if (pcb == NULL) {
        sys_mutex_unlock(&pxping->lock);
        DPRINTF2(("%s: no match\n", __func__));
        return;
    }

    DPRINTF2(("%s: pcb %p\n", __func__, (void *)pcb));

    /* save info before unlocking since pcb may expire */
    ip6_addr_copy(guest_ip, *ipX_2_ip6(&pcb->src));
    guest_id = pcb->guest_id;

    sys_mutex_unlock(&pxping->lock);

    /* rewrite ICMPv6 echo header */
    sum = (u16_t)~icmph->chksum;
    sum += update16_with_chksum(&icmph->id, guest_id);

    /* dst address in pseudo header (clobbers pktinfo) */
    sum += updateip6_with_chksum((ip6_addr_t *)&pktinfo->ipi6_addr, &guest_ip);

    /* src address in pseudo header (clobbers target_ip) */
    if (mapped) {
        sum += updateip6_with_chksum(&target_ip, &unmapped_target_ip);
    }

    sum = FOLD_U32T(sum);
    icmph->chksum = ~sum;

    if (hopl < 0) {
        hopl = LWIP_ICMP6_HL;
    }
    else if (!mapped) {
        if (hopl == 1) {
            return;
        }
        --hopl;
    }

    if (tclass < 0) {
        tclass = 0;
    }

    pxping_pmgr_forward_inbound6(pxping,
                                 &unmapped_target_ip, /* echo reply src */
                                 &guest_ip, /* echo reply dst */
                                 hopl, tclass, (u16_t)nread);
}


static void
pxping_pmgr_icmp6_echo(struct pxping *pxping,
                       u16_t iplen, struct sockaddr_in6 *peer)
{
}

static void
pxping_pmgr_icmp6_error(struct pxping *pxping,
                        u16_t iplen, struct sockaddr_in6 *peer)
{
}


/**
 * Hand off ICMP datagram to the lwip thread where it will be
 * forwarded to the guest.
 *
 * We no longer need ping_pcb.  The pcb may get expired on the lwip
 * thread, but we have already patched necessary information into the
 * datagram.
 */
static void
pxping_pmgr_forward_inbound(struct pxping *pxping, u16_t iplen)
{
    struct pbuf *p;
    struct ping_msg *msg;
    err_t error;

    p = pbuf_alloc(PBUF_LINK, iplen, PBUF_RAM);
    if (p == NULL) {
        DPRINTF(("%s: pbuf_alloc(%d) failed\n",
                 __func__, (unsigned int)iplen));
        return;
    }

    error = pbuf_take(p, pollmgr_udpbuf, iplen);
    if (error != ERR_OK) {
        DPRINTF(("%s: pbuf_take(%d) failed\n",
                 __func__, (unsigned int)iplen));
        pbuf_free(p);
        return;
    }

    msg = (struct ping_msg *)malloc(sizeof(*msg));
    if (msg == NULL) {
        pbuf_free(p);
        return;
    }

    msg->msg.type = TCPIP_MSG_CALLBACK_STATIC;
    msg->msg.sem = NULL;
    msg->msg.msg.cb.function = pxping_pcb_forward_inbound;
    msg->msg.msg.cb.ctx = (void *)msg;

    msg->pxping = pxping;
    msg->p = p;

    proxy_lwip_post(&msg->msg);
}


static void
pxping_pcb_forward_inbound(void *arg)
{
    struct ping_msg *msg = (struct ping_msg *)arg;
    err_t error;

    LWIP_ASSERT1(msg != NULL);
    LWIP_ASSERT1(msg->pxping != NULL);
    LWIP_ASSERT1(msg->p != NULL);

    error = ip_raw_output_if(msg->p, msg->pxping->netif);
    if (error != ERR_OK) {
        DPRINTF(("%s: ip_output_if: %s\n",
                 __func__, proxy_lwip_strerr(error)));
        pbuf_free(msg->p);
    }

    free(msg);
}


static void
pxping_pmgr_forward_inbound6(struct pxping *pxping,
                             ip6_addr_t *src, ip6_addr_t *dst,
                             u8_t hopl, u8_t tclass,
                             u16_t icmplen)
{
    struct pbuf *p;
    struct ping6_msg *msg;

    err_t error;

    p = pbuf_alloc(PBUF_IP, icmplen, PBUF_RAM);
    if (p == NULL) {
        DPRINTF(("%s: pbuf_alloc(%d) failed\n",
                 __func__, (unsigned int)icmplen));
        return;
    }

    error = pbuf_take(p, pollmgr_udpbuf, icmplen);
    if (error != ERR_OK) {
        DPRINTF(("%s: pbuf_take(%d) failed\n",
                 __func__, (unsigned int)icmplen));
        pbuf_free(p);
        return;
    }

    msg = (struct ping6_msg *)malloc(sizeof(*msg));
    if (msg == NULL) {
        pbuf_free(p);
        return;
    }

    msg->msg.type = TCPIP_MSG_CALLBACK_STATIC;
    msg->msg.sem = NULL;
    msg->msg.msg.cb.function = pxping_pcb_forward_inbound6;
    msg->msg.msg.cb.ctx = (void *)msg;

    msg->pxping = pxping;
    msg->p = p;
    ip6_addr_copy(msg->src, *src);
    ip6_addr_copy(msg->dst, *dst);
    msg->hopl = hopl;
    msg->tclass = tclass;

    proxy_lwip_post(&msg->msg);
}


static void
pxping_pcb_forward_inbound6(void *arg)
{
    struct ping6_msg *msg = (struct ping6_msg *)arg;
    err_t error;

    LWIP_ASSERT1(msg != NULL);
    LWIP_ASSERT1(msg->pxping != NULL);
    LWIP_ASSERT1(msg->p != NULL);

    error = ip6_output_if(msg->p,
                          &msg->src, &msg->dst, msg->hopl, msg->tclass,
                          IP6_NEXTH_ICMP6, msg->pxping->netif);
    if (error != ERR_OK) {
        DPRINTF(("%s: ip6_output_if: %s\n",
                 __func__, proxy_lwip_strerr(error)));
        pbuf_free(msg->p);
    }

    free(msg);
}