/*
 *
 * Copyright (c) 2004-2006 Kip Macy
 * All rights reserved.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/sys/dev/xen/netfront/netfront.c,v 1.24.2.1.2.1 2009/10/25 01:10:29 kensmith Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/sx.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <net/if_media.h>

#include <net/bpf.h>

#include <net/if_types.h>
#include <net/if.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/if_ether.h>
#if __FreeBSD_version >= 700000
#include <netinet/tcp.h>
#include <netinet/tcp_lro.h>
#endif

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/clock.h>      /* for DELAY */
#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/frame.h>
#include <machine/vmparam.h>

#include <sys/bus.h>
#include <sys/rman.h>

#include <machine/intr_machdep.h>

#include <machine/xen/xen-os.h>
#include <machine/xen/xenfunc.h>
#include <xen/hypervisor.h>
#include <xen/xen_intr.h>
#include <xen/evtchn.h>
#include <xen/gnttab.h>
#include <xen/interface/memory.h>
#include <xen/interface/io/netif.h>
#include <xen/xenbus/xenbusvar.h>

#include <dev/xen/netfront/mbufq.h>

#include "xenbus_if.h"

#define XN_CSUM_FEATURES	(CSUM_TCP | CSUM_UDP | CSUM_TSO)

#define GRANT_INVALID_REF	0

#define NET_TX_RING_SIZE __RING_SIZE((netif_tx_sring_t *)0, PAGE_SIZE)
#define NET_RX_RING_SIZE __RING_SIZE((netif_rx_sring_t *)0, PAGE_SIZE)

#if __FreeBSD_version >= 700000
/*
 * Should the driver do LRO on the RX end
 *  this can be toggled on the fly, but the
 *  interface must be reset (down/up) for it
 *  to take effect.
 */
static int xn_enable_lro = 1;
TUNABLE_INT("hw.xn.enable_lro", &xn_enable_lro);
#else

#define IFCAP_TSO4	0
#define CSUM_TSO	0

#endif

#ifdef CONFIG_XEN
static int MODPARM_rx_copy = 0;
module_param_named(rx_copy, MODPARM_rx_copy, bool, 0);
MODULE_PARM_DESC(rx_copy, "Copy packets from network card (rather than flip)");
static int MODPARM_rx_flip = 0;
module_param_named(rx_flip, MODPARM_rx_flip, bool, 0);
MODULE_PARM_DESC(rx_flip, "Flip packets from network card (rather than copy)");
#else
static const int MODPARM_rx_copy = 1;
static const int MODPARM_rx_flip = 0;
#endif

#define MAX_SKB_FRAGS	(65536/PAGE_SIZE + 2)
#define RX_COPY_THRESHOLD 256

#define net_ratelimit() 0

struct netfront_info;
struct netfront_rx_info;

static void xn_txeof(struct netfront_info *);
static void xn_rxeof(struct netfront_info *);
static void network_alloc_rx_buffers(struct netfront_info *);

static void xn_tick_locked(struct netfront_info *);
static void xn_tick(void *);

static void xn_intr(void *);
static void xn_start_locked(struct ifnet *);
static void xn_start(struct ifnet *);
static int  xn_ioctl(struct ifnet *, u_long, caddr_t);
static void xn_ifinit_locked(struct netfront_info *);
static void xn_ifinit(void *);
static void xn_stop(struct netfront_info *);
#ifdef notyet
static void xn_watchdog(struct ifnet *);
#endif

static void show_device(struct netfront_info *sc);
#ifdef notyet
static void netfront_closing(device_t dev);
#endif
static void netif_free(struct netfront_info *info);
static int netfront_detach(device_t dev);

static int talk_to_backend(device_t dev, struct netfront_info *info);
static int create_netdev(device_t dev);
static void netif_disconnect_backend(struct netfront_info *info);
static int setup_device(device_t dev, struct netfront_info *info);
static void end_access(int ref, void *page);

/* Xenolinux helper functions */
int network_connect(struct netfront_info *);

static void xn_free_rx_ring(struct netfront_info *);

static void xn_free_tx_ring(struct netfront_info *);

static int xennet_get_responses(struct netfront_info *np,
	struct netfront_rx_info *rinfo, RING_IDX rp, struct mbuf **list,
	int *pages_flipped_p);

#define virt_to_mfn(x) (vtomach(x) >> PAGE_SHIFT)

#define INVALID_P2M_ENTRY (~0UL)

/*
 * Mbuf pointers. We need these to keep track of the virtual addresses
 * of our mbuf chains since we can only convert from virtual to physical,
 * not the other way around.  The size must track the free index arrays.
 */
struct xn_chain_data {
		struct mbuf		*xn_tx_chain[NET_TX_RING_SIZE+1];
		int			xn_tx_chain_cnt;
		struct mbuf		*xn_rx_chain[NET_RX_RING_SIZE+1];
};


struct net_device_stats
{
	u_long	rx_packets;		/* total packets received	*/
	u_long	tx_packets;		/* total packets transmitted	*/
	u_long	rx_bytes;		/* total bytes received 	*/
	u_long	tx_bytes;		/* total bytes transmitted	*/
	u_long	rx_errors;		/* bad packets received		*/
	u_long	tx_errors;		/* packet transmit problems	*/
	u_long	rx_dropped;		/* no space in linux buffers	*/
	u_long	tx_dropped;		/* no space available in linux	*/
	u_long	multicast;		/* multicast packets received	*/
	u_long	collisions;

	/* detailed rx_errors: */
	u_long	rx_length_errors;
	u_long	rx_over_errors;		/* receiver ring buff overflow	*/
	u_long	rx_crc_errors;		/* recved pkt with crc error	*/
	u_long	rx_frame_errors;	/* recv'd frame alignment error */
	u_long	rx_fifo_errors;		/* recv'r fifo overrun		*/
	u_long	rx_missed_errors;	/* receiver missed packet	*/

	/* detailed tx_errors */
	u_long	tx_aborted_errors;
	u_long	tx_carrier_errors;
	u_long	tx_fifo_errors;
	u_long	tx_heartbeat_errors;
	u_long	tx_window_errors;
	
	/* for cslip etc */
	u_long	rx_compressed;
	u_long	tx_compressed;
};

struct netfront_info {
		
	struct ifnet *xn_ifp;
#if __FreeBSD_version >= 700000
	struct lro_ctrl xn_lro;
#endif

	struct net_device_stats stats;
	u_int tx_full;

	netif_tx_front_ring_t tx;
	netif_rx_front_ring_t rx;

	struct mtx   tx_lock;
	struct mtx   rx_lock;
	struct sx    sc_lock;

	u_int handle;
	u_int irq;
	u_int copying_receiver;
	u_int carrier;
		
	/* Receive-ring batched refills. */
#define RX_MIN_TARGET 32
#define RX_MAX_TARGET NET_RX_RING_SIZE
	int rx_min_target, rx_max_target, rx_target;

	/*
	 * {tx,rx}_skbs store outstanding skbuffs. The first entry in each
	 * array is an index into a chain of free entries.
	 */

	grant_ref_t gref_tx_head;
	grant_ref_t grant_tx_ref[NET_TX_RING_SIZE + 1]; 
	grant_ref_t gref_rx_head;
	grant_ref_t grant_rx_ref[NET_TX_RING_SIZE + 1]; 

#define TX_MAX_TARGET min(NET_RX_RING_SIZE, 256)
	device_t xbdev;
	int tx_ring_ref;
	int rx_ring_ref;
	uint8_t mac[ETHER_ADDR_LEN];
	struct xn_chain_data	xn_cdata;	/* mbufs */
	struct mbuf_head xn_rx_batch;	/* head of the batch queue */

	int			xn_if_flags;
	struct callout	        xn_stat_ch;

	u_long rx_pfn_array[NET_RX_RING_SIZE];
	multicall_entry_t rx_mcl[NET_RX_RING_SIZE+1];
	mmu_update_t rx_mmu[NET_RX_RING_SIZE];
};

#define rx_mbufs xn_cdata.xn_rx_chain
#define tx_mbufs xn_cdata.xn_tx_chain

#define XN_LOCK_INIT(_sc, _name) \
        mtx_init(&(_sc)->tx_lock, #_name"_tx", "network transmit lock", MTX_DEF); \
        mtx_init(&(_sc)->rx_lock, #_name"_rx", "network receive lock", MTX_DEF);  \
        sx_init(&(_sc)->sc_lock, #_name"_rx")

#define XN_RX_LOCK(_sc)           mtx_lock(&(_sc)->rx_lock)
#define XN_RX_UNLOCK(_sc)         mtx_unlock(&(_sc)->rx_lock)

#define XN_TX_LOCK(_sc)           mtx_lock(&(_sc)->tx_lock)
#define XN_TX_UNLOCK(_sc)         mtx_unlock(&(_sc)->tx_lock)

#define XN_LOCK(_sc)           sx_xlock(&(_sc)->sc_lock); 
#define XN_UNLOCK(_sc)         sx_xunlock(&(_sc)->sc_lock); 

#define XN_LOCK_ASSERT(_sc)    sx_assert(&(_sc)->sc_lock, SX_LOCKED); 
#define XN_RX_LOCK_ASSERT(_sc)    mtx_assert(&(_sc)->rx_lock, MA_OWNED); 
#define XN_TX_LOCK_ASSERT(_sc)    mtx_assert(&(_sc)->tx_lock, MA_OWNED); 
#define XN_LOCK_DESTROY(_sc)   mtx_destroy(&(_sc)->rx_lock); \
                               mtx_destroy(&(_sc)->tx_lock); \
                               sx_destroy(&(_sc)->sc_lock);

struct netfront_rx_info {
	struct netif_rx_response rx;
	struct netif_extra_info extras[XEN_NETIF_EXTRA_TYPE_MAX - 1];
};

#define netfront_carrier_on(netif)	((netif)->carrier = 1)
#define netfront_carrier_off(netif)	((netif)->carrier = 0)
#define netfront_carrier_ok(netif)	((netif)->carrier)

/* Access macros for acquiring freeing slots in xn_free_{tx,rx}_idxs[]. */



/*
 * Access macros for acquiring freeing slots in tx_skbs[].
 */

static inline void
add_id_to_freelist(struct mbuf **list, unsigned short id)
{
	KASSERT(id != 0, ("add_id_to_freelist: the head item (0) must always be free."));
	list[id] = list[0];
	list[0]  = (void *)(u_long)id;
}

static inline unsigned short
get_id_from_freelist(struct mbuf **list)
{
	u_int id = (u_int)(u_long)list[0];
	KASSERT(id != 0, ("get_id_from_freelist: the head item (0) must always remain free."));
	list[0] = list[id];
	return (id);
}

static inline int
xennet_rxidx(RING_IDX idx)
{
	return idx & (NET_RX_RING_SIZE - 1);
}

static inline struct mbuf *
xennet_get_rx_mbuf(struct netfront_info *np,
						RING_IDX ri)
{
	int i = xennet_rxidx(ri);
	struct mbuf *m;

	m = np->rx_mbufs[i];
	np->rx_mbufs[i] = NULL;
	return (m);
}

static inline grant_ref_t
xennet_get_rx_ref(struct netfront_info *np, RING_IDX ri)
{
	int i = xennet_rxidx(ri);
	grant_ref_t ref = np->grant_rx_ref[i];
	np->grant_rx_ref[i] = GRANT_INVALID_REF;
	return ref;
}

#define IPRINTK(fmt, args...) \
    printf("[XEN] " fmt, ##args)
#define WPRINTK(fmt, args...) \
    printf("[XEN] " fmt, ##args)
#if 0
#define DPRINTK(fmt, args...) \
    printf("[XEN] %s: " fmt, __func__, ##args)
#else
#define DPRINTK(fmt, args...)
#endif

/**
 * Read the 'mac' node at the given device's node in the store, and parse that
 * as colon-separated octets, placing result the given mac array.  mac must be
 * a preallocated array of length ETH_ALEN (as declared in linux/if_ether.h).
 * Return 0 on success, or errno on error.
 */
static int 
xen_net_read_mac(device_t dev, uint8_t mac[])
{
	int error, i;
	char *s, *e, *macstr;

	error = xenbus_read(XBT_NIL, xenbus_get_node(dev), "mac", NULL,
	    (void **) &macstr);
	if (error)
		return (error);

	s = macstr;
	for (i = 0; i < ETHER_ADDR_LEN; i++) {
		mac[i] = strtoul(s, &e, 16);
		if (s == e || (e[0] != ':' && e[0] != 0)) {
			free(macstr, M_DEVBUF);
			return (ENOENT);
		}
		s = &e[1];
	}
	free(macstr, M_DEVBUF);
	return (0);
}

/**
 * Entry point to this code when a new device is created.  Allocate the basic
 * structures and the ring buffers for communication with the backend, and
 * inform the backend of the appropriate details for those.  Switch to
 * Connected state.
 */
static int 
netfront_probe(device_t dev)
{

	if (!strcmp(xenbus_get_type(dev), "vif")) {
		device_set_desc(dev, "Virtual Network Interface");
		return (0);
	}

	return (ENXIO);
}

static int
netfront_attach(device_t dev)
{	
	int err;

	err = create_netdev(dev);
	if (err) {
		xenbus_dev_fatal(dev, err, "creating netdev");
		return err;
	}

#if __FreeBSD_version >= 700000
	SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
	    OID_AUTO, "enable_lro", CTLTYPE_INT|CTLFLAG_RW,
	    &xn_enable_lro, 0, "Large Receive Offload");
#endif

	return 0;
}


/**
 * We are reconnecting to the backend, due to a suspend/resume, or a backend
 * driver restart.  We tear down our netif structure and recreate it, but
 * leave the device-layer structures intact so that this is transparent to the
 * rest of the kernel.
 */
static int
netfront_resume(device_t dev)
{
	struct netfront_info *info = device_get_softc(dev);

	netif_disconnect_backend(info);
	return (0);
}


/* Common code used when first setting up, and when resuming. */
static int 
talk_to_backend(device_t dev, struct netfront_info *info)
{
	const char *message;
	struct xenbus_transaction xbt;
	const char *node = xenbus_get_node(dev);
	int err;

	err = xen_net_read_mac(dev, info->mac);
	if (err) {
		xenbus_dev_fatal(dev, err, "parsing %s/mac", node);
		goto out;
	}

	/* Create shared ring, alloc event channel. */
	err = setup_device(dev, info);
	if (err)
		goto out;
	
 again:
	err = xenbus_transaction_start(&xbt);
	if (err) {
		xenbus_dev_fatal(dev, err, "starting transaction");
		goto destroy_ring;
	}
	err = xenbus_printf(xbt, node, "tx-ring-ref","%u",
			    info->tx_ring_ref);
	if (err) {
		message = "writing tx ring-ref";
		goto abort_transaction;
	}
	err = xenbus_printf(xbt, node, "rx-ring-ref","%u",
			    info->rx_ring_ref);
	if (err) {
		message = "writing rx ring-ref";
		goto abort_transaction;
	}
	err = xenbus_printf(xbt, node,
		"event-channel", "%u", irq_to_evtchn_port(info->irq));
	if (err) {
		message = "writing event-channel";
		goto abort_transaction;
	}
	err = xenbus_printf(xbt, node, "request-rx-copy", "%u",
			    info->copying_receiver);
	if (err) {
		message = "writing request-rx-copy";
		goto abort_transaction;
	}
	err = xenbus_printf(xbt, node, "feature-rx-notify", "%d", 1);
	if (err) {
		message = "writing feature-rx-notify";
		goto abort_transaction;
	}
	err = xenbus_printf(xbt, node, "feature-sg", "%d", 1);
	if (err) {
		message = "writing feature-sg";
		goto abort_transaction;
	}
#if __FreeBSD_version >= 700000
	err = xenbus_printf(xbt, node, "feature-gso-tcpv4", "%d", 1);
	if (err) {
		message = "writing feature-gso-tcpv4";
		goto abort_transaction;
	}
#endif

	err = xenbus_transaction_end(xbt, 0);
	if (err) {
		if (err == EAGAIN)
			goto again;
		xenbus_dev_fatal(dev, err, "completing transaction");
		goto destroy_ring;
	}
	
	return 0;
	
 abort_transaction:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, err, "%s", message);
 destroy_ring:
	netif_free(info);
 out:
	return err;
}


static int 
setup_device(device_t dev, struct netfront_info *info)
{
	netif_tx_sring_t *txs;
	netif_rx_sring_t *rxs;
	int error;
	struct ifnet *ifp;
	
	ifp = info->xn_ifp;

	info->tx_ring_ref = GRANT_INVALID_REF;
	info->rx_ring_ref = GRANT_INVALID_REF;
	info->rx.sring = NULL;
	info->tx.sring = NULL;
	info->irq = 0;

	txs = (netif_tx_sring_t *)malloc(PAGE_SIZE, M_DEVBUF, M_NOWAIT|M_ZERO);
	if (!txs) {
		error = ENOMEM;
		xenbus_dev_fatal(dev, error, "allocating tx ring page");
		goto fail;
	}
	SHARED_RING_INIT(txs);
	FRONT_RING_INIT(&info->tx, txs, PAGE_SIZE);
	error = xenbus_grant_ring(dev, virt_to_mfn(txs), &info->tx_ring_ref);
	if (error)
		goto fail;

	rxs = (netif_rx_sring_t *)malloc(PAGE_SIZE, M_DEVBUF, M_NOWAIT|M_ZERO);
	if (!rxs) {
		error = ENOMEM;
		xenbus_dev_fatal(dev, error, "allocating rx ring page");
		goto fail;
	}
	SHARED_RING_INIT(rxs);
	FRONT_RING_INIT(&info->rx, rxs, PAGE_SIZE);

	error = xenbus_grant_ring(dev, virt_to_mfn(rxs), &info->rx_ring_ref);
	if (error)
		goto fail;

	error = bind_listening_port_to_irqhandler(xenbus_get_otherend_id(dev),
	    "xn", xn_intr, info, INTR_TYPE_NET | INTR_MPSAFE, &info->irq);

	if (error) {
		xenbus_dev_fatal(dev, error,
				 "bind_evtchn_to_irqhandler failed");
		goto fail;
	}

	show_device(info);
	
	return (0);
	
 fail:
	netif_free(info);
	return (error);
}

/**
 * If this interface has an ipv4 address, send an arp for it. This
 * helps to get the network going again after migrating hosts.
 */
static void
netfront_send_fake_arp(device_t dev, struct netfront_info *info)
{
	struct ifnet *ifp;
	struct ifaddr *ifa;
	
	ifp = info->xn_ifp;
	TAILQ_FOREACH(ifa, &ifp->if_addrhead, ifa_link) {
		if (ifa->ifa_addr->sa_family == AF_INET) {
			arp_ifinit(ifp, ifa);
		}
	}
}

/**
 * Callback received when the backend's state changes.
 */
static int
netfront_backend_changed(device_t dev, XenbusState newstate)
{
	struct netfront_info *sc = device_get_softc(dev);
		
	DPRINTK("newstate=%d\n", newstate);

	switch (newstate) {
	case XenbusStateInitialising:
	case XenbusStateInitialised:
	case XenbusStateConnected:
	case XenbusStateUnknown:
	case XenbusStateClosed:
	case XenbusStateReconfigured:
	case XenbusStateReconfiguring:
		break;
	case XenbusStateInitWait:
		if (xenbus_get_state(dev) != XenbusStateInitialising)
			break;
		if (network_connect(sc) != 0)
			break;
		xenbus_set_state(dev, XenbusStateConnected);
		netfront_send_fake_arp(dev, sc);
		break;
	case XenbusStateClosing:
		xenbus_set_state(dev, XenbusStateClosed);
		break;
	}
	return (0);
}

static void
xn_free_rx_ring(struct netfront_info *sc)
{
#if 0
	int i;
	
	for (i = 0; i < NET_RX_RING_SIZE; i++) {
		if (sc->xn_cdata.xn_rx_chain[i] != NULL) {
			m_freem(sc->xn_cdata.xn_rx_chain[i]);
			sc->xn_cdata.xn_rx_chain[i] = NULL;
		}
	}
	
	sc->rx.rsp_cons = 0;
	sc->xn_rx_if->req_prod = 0;
	sc->xn_rx_if->event = sc->rx.rsp_cons ;
#endif
}

static void
xn_free_tx_ring(struct netfront_info *sc)
{
#if 0
	int i;
	
	for (i = 0; i < NET_TX_RING_SIZE; i++) {
		if (sc->xn_cdata.xn_tx_chain[i] != NULL) {
			m_freem(sc->xn_cdata.xn_tx_chain[i]);
			sc->xn_cdata.xn_tx_chain[i] = NULL;
		}
	}
	
	return;
#endif
}

/*
 * Do some brief math on the number of descriptors available to
 * determine how many slots are available.
 *
 * Firstly - wouldn't something with RING_FREE_REQUESTS() be more applicable?
 * Secondly - MAX_SKB_FRAGS is a Linux construct which may not apply here.
 * Thirdly - it isn't used here anyway; the magic constant '24' is possibly
 *   wrong?
 * The "2" is presumably to ensure there are also enough slots available for
 * the ring entries used for "options" (eg, the TSO entry before a packet
 * is queued); I'm not sure why its 2 and not 1. Perhaps to make sure there's
 * a "free" node in the tx mbuf list (node 0) to represent the freelist?
 *
 * This only figures out whether any xenbus ring descriptors are available;
 * it doesn't at all reflect how many tx mbuf ring descriptors are also
 * available.
 */
static inline int
netfront_tx_slot_available(struct netfront_info *np)
{
	return ((np->tx.req_prod_pvt - np->tx.rsp_cons) <
		(TX_MAX_TARGET - /* MAX_SKB_FRAGS */ 24 - 2));
}
static void
netif_release_tx_bufs(struct netfront_info *np)
{
	struct mbuf *m;
	int i;

	for (i = 1; i <= NET_TX_RING_SIZE; i++) {
		m = np->xn_cdata.xn_tx_chain[i];

		if (((u_long)m) < KERNBASE)
			continue;
		gnttab_grant_foreign_access_ref(np->grant_tx_ref[i],
		    xenbus_get_otherend_id(np->xbdev),
		    virt_to_mfn(mtod(m, vm_offset_t)),
		    GNTMAP_readonly);
		gnttab_release_grant_reference(&np->gref_tx_head,
		    np->grant_tx_ref[i]);
		np->grant_tx_ref[i] = GRANT_INVALID_REF;
		add_id_to_freelist(np->tx_mbufs, i);
		np->xn_cdata.xn_tx_chain_cnt--;
		if (np->xn_cdata.xn_tx_chain_cnt < 0) {
			panic("netif_release_tx_bufs: tx_chain_cnt must be >= 0");
		}
		m_freem(m);
	}
}

static void
network_alloc_rx_buffers(struct netfront_info *sc)
{
	int otherend_id = xenbus_get_otherend_id(sc->xbdev);
	unsigned short id;
	struct mbuf *m_new;
	int i, batch_target, notify;
	RING_IDX req_prod;
	struct xen_memory_reservation reservation;
	grant_ref_t ref;
	int nr_flips;
	netif_rx_request_t *req;
	vm_offset_t vaddr;
	u_long pfn;
	
	req_prod = sc->rx.req_prod_pvt;

	if (unlikely(sc->carrier == 0))
		return;
	
	/*
	 * Allocate skbuffs greedily, even though we batch updates to the
	 * receive ring. This creates a less bursty demand on the memory
	 * allocator, so should reduce the chance of failed allocation
	 * requests both for ourself and for other kernel subsystems.
	 */
	batch_target = sc->rx_target - (req_prod - sc->rx.rsp_cons);
	for (i = mbufq_len(&sc->xn_rx_batch); i < batch_target; i++) {
		MGETHDR(m_new, M_DONTWAIT, MT_DATA);
		if (m_new == NULL) 
			goto no_mbuf;

		m_cljget(m_new, M_DONTWAIT, MJUMPAGESIZE);
		if ((m_new->m_flags & M_EXT) == 0) {
			m_freem(m_new);

no_mbuf:
			if (i != 0)
				goto refill;
			/*
			 * XXX set timer
			 */
			break;
		}
		m_new->m_len = m_new->m_pkthdr.len = MJUMPAGESIZE;
		
		/* queue the mbufs allocated */
		mbufq_tail(&sc->xn_rx_batch, m_new);
	}
	
	/* Is the batch large enough to be worthwhile? */
	if (i < (sc->rx_target/2)) {
		if (req_prod >sc->rx.sring->req_prod)
			goto push;
		return;
	}
	/* Adjust floating fill target if we risked running out of buffers. */
	if ( ((req_prod - sc->rx.sring->rsp_prod) < (sc->rx_target / 4)) &&
	     ((sc->rx_target *= 2) > sc->rx_max_target) )
		sc->rx_target = sc->rx_max_target;

refill:
	for (nr_flips = i = 0; ; i++) {
		if ((m_new = mbufq_dequeue(&sc->xn_rx_batch)) == NULL)
			break;

		m_new->m_ext.ext_arg1 = (vm_paddr_t *)(uintptr_t)(
				vtophys(m_new->m_ext.ext_buf) >> PAGE_SHIFT);

		id = xennet_rxidx(req_prod + i);

		KASSERT(sc->xn_cdata.xn_rx_chain[id] == NULL,
		    ("non-NULL xm_rx_chain"));
		sc->xn_cdata.xn_rx_chain[id] = m_new;

		ref = gnttab_claim_grant_reference(&sc->gref_rx_head);
		KASSERT((short)ref >= 0, ("negative ref"));
		sc->grant_rx_ref[id] = ref;

		vaddr = mtod(m_new, vm_offset_t);
		pfn = vtophys(vaddr) >> PAGE_SHIFT;
		req = RING_GET_REQUEST(&sc->rx, req_prod + i);

		if (sc->copying_receiver == 0) {
			gnttab_grant_foreign_transfer_ref(ref,
			    otherend_id, pfn);
			sc->rx_pfn_array[nr_flips] = PFNTOMFN(pfn);
			if (!xen_feature(XENFEAT_auto_translated_physmap)) {
				/* Remove this page before passing
				 * back to Xen.
				 */
				set_phys_to_machine(pfn, INVALID_P2M_ENTRY);
				MULTI_update_va_mapping(&sc->rx_mcl[i],
				    vaddr, 0, 0);
			}
			nr_flips++;
		} else {
			gnttab_grant_foreign_access_ref(ref,
			    otherend_id,
			    PFNTOMFN(pfn), 0);
		}
		req->id = id;
		req->gref = ref;
		
		sc->rx_pfn_array[i] =
		    vtomach(mtod(m_new,vm_offset_t)) >> PAGE_SHIFT;
	} 
	
	KASSERT(i, ("no mbufs processed")); /* should have returned earlier */
	KASSERT(mbufq_len(&sc->xn_rx_batch) == 0, ("not all mbufs processed"));
	/*
	 * We may have allocated buffers which have entries outstanding
	 * in the page * update queue -- make sure we flush those first!
	 */
	PT_UPDATES_FLUSH();
	if (nr_flips != 0) {
#ifdef notyet
		/* Tell the ballon driver what is going on. */
		balloon_update_driver_allowance(i);
#endif
		set_xen_guest_handle(reservation.extent_start, sc->rx_pfn_array);
		reservation.nr_extents   = i;
		reservation.extent_order = 0;
		reservation.address_bits = 0;
		reservation.domid        = DOMID_SELF;

		if (!xen_feature(XENFEAT_auto_translated_physmap)) {

			/* After all PTEs have been zapped, flush the TLB. */
			sc->rx_mcl[i-1].args[MULTI_UVMFLAGS_INDEX] =
			    UVMF_TLB_FLUSH|UVMF_ALL;
	
			/* Give away a batch of pages. */
			sc->rx_mcl[i].op = __HYPERVISOR_memory_op;
			sc->rx_mcl[i].args[0] = XENMEM_decrease_reservation;
			sc->rx_mcl[i].args[1] =  (u_long)&reservation;
			/* Zap PTEs and give away pages in one big multicall. */
			(void)HYPERVISOR_multicall(sc->rx_mcl, i+1);

			/* Check return status of HYPERVISOR_dom_mem_op(). */
			if (unlikely(sc->rx_mcl[i].result != i))
				panic("Unable to reduce memory reservation\n");
			} else {
				if (HYPERVISOR_memory_op(
				    XENMEM_decrease_reservation, &reservation)
				    != i)
					panic("Unable to reduce memory "
					    "reservation\n");
		}
	} else {
		wmb();
	}
			
	/* Above is a suitable barrier to ensure backend will see requests. */
	sc->rx.req_prod_pvt = req_prod + i;
push:
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&sc->rx, notify);
	if (notify)
		notify_remote_via_irq(sc->irq);
}

static void
xn_rxeof(struct netfront_info *np)
{
	struct ifnet *ifp;
#if __FreeBSD_version >= 700000
	struct lro_ctrl *lro = &np->xn_lro;
	struct lro_entry *queued;
#endif
	struct netfront_rx_info rinfo;
	struct netif_rx_response *rx = &rinfo.rx;
	struct netif_extra_info *extras = rinfo.extras;
	RING_IDX i, rp;
	multicall_entry_t *mcl;
	struct mbuf *m;
	struct mbuf_head rxq, errq;
	int err, pages_flipped = 0, work_to_do;

	do {
		XN_RX_LOCK_ASSERT(np);
		if (!netfront_carrier_ok(np))
			return;

		mbufq_init(&errq);
		mbufq_init(&rxq);

		ifp = np->xn_ifp;
	
		rp = np->rx.sring->rsp_prod;
		rmb();	/* Ensure we see queued responses up to 'rp'. */

		i = np->rx.rsp_cons;
		while ((i != rp)) {
			memcpy(rx, RING_GET_RESPONSE(&np->rx, i), sizeof(*rx));
			memset(extras, 0, sizeof(rinfo.extras));

			m = NULL;
			err = xennet_get_responses(np, &rinfo, rp, &m,
			    &pages_flipped);

			if (unlikely(err)) {
				if (m)
					mbufq_tail(&errq, m);
				np->stats.rx_errors++;
				i = np->rx.rsp_cons;
				continue;
			}

			m->m_pkthdr.rcvif = ifp;
			if ( rx->flags & NETRXF_data_validated ) {
				/* Tell the stack the checksums are okay */
				/*
				 * XXX this isn't necessarily the case - need to add
				 * check
				 */
				
				m->m_pkthdr.csum_flags |=
					(CSUM_IP_CHECKED | CSUM_IP_VALID | CSUM_DATA_VALID
					    | CSUM_PSEUDO_HDR);
				m->m_pkthdr.csum_data = 0xffff;
			}

			np->stats.rx_packets++;
			np->stats.rx_bytes += m->m_pkthdr.len;

			mbufq_tail(&rxq, m);
			np->rx.rsp_cons = ++i;
		}

		if (pages_flipped) {
			/* Some pages are no longer absent... */
#ifdef notyet
			balloon_update_driver_allowance(-pages_flipped);
#endif
			/* Do all the remapping work, and M->P updates, in one big
			 * hypercall.
			 */
			if (!!xen_feature(XENFEAT_auto_translated_physmap)) {
				mcl = np->rx_mcl + pages_flipped;
				mcl->op = __HYPERVISOR_mmu_update;
				mcl->args[0] = (u_long)np->rx_mmu;
				mcl->args[1] = pages_flipped;
				mcl->args[2] = 0;
				mcl->args[3] = DOMID_SELF;
				(void)HYPERVISOR_multicall(np->rx_mcl,
				    pages_flipped + 1);
			}
		}
	
		while ((m = mbufq_dequeue(&errq)))
			m_freem(m);

		/* 
		 * Process all the mbufs after the remapping is complete.
		 * Break the mbuf chain first though.
		 */
		while ((m = mbufq_dequeue(&rxq)) != NULL) {
			ifp->if_ipackets++;
			
			/*
			 * Do we really need to drop the rx lock?
			 */
			XN_RX_UNLOCK(np);
#if __FreeBSD_version >= 700000
			/* Use LRO if possible */
			if ((ifp->if_capenable & IFCAP_LRO) == 0 ||
			    lro->lro_cnt == 0 || tcp_lro_rx(lro, m, 0)) {
				/*
				 * If LRO fails, pass up to the stack
				 * directly.
				 */
				(*ifp->if_input)(ifp, m);
			}
#else
			(*ifp->if_input)(ifp, m);
#endif
			XN_RX_LOCK(np);
		}
	
		np->rx.rsp_cons = i;

#if __FreeBSD_version >= 700000
		/*
		 * Flush any outstanding LRO work
		 */
		while (!SLIST_EMPTY(&lro->lro_active)) {
			queued = SLIST_FIRST(&lro->lro_active);
			SLIST_REMOVE_HEAD(&lro->lro_active, next);
			tcp_lro_flush(lro, queued);
		}
#endif

#if 0
		/* If we get a callback with very few responses, reduce fill target. */
		/* NB. Note exponential increase, linear decrease. */
		if (((np->rx.req_prod_pvt - np->rx.sring->rsp_prod) > 
			((3*np->rx_target) / 4)) && (--np->rx_target < np->rx_min_target))
			np->rx_target = np->rx_min_target;
#endif
	
		network_alloc_rx_buffers(np);

		RING_FINAL_CHECK_FOR_RESPONSES(&np->rx, work_to_do);
	} while (work_to_do);
}

static void 
xn_txeof(struct netfront_info *np)
{
	RING_IDX i, prod;
	unsigned short id;
	struct ifnet *ifp;
	netif_tx_response_t *txr;
	struct mbuf *m;
	
	XN_TX_LOCK_ASSERT(np);
	
	if (!netfront_carrier_ok(np))
		return;
	
	ifp = np->xn_ifp;
	ifp->if_timer = 0;
	
	do {
		prod = np->tx.sring->rsp_prod;
		rmb(); /* Ensure we see responses up to 'rp'. */
		
		for (i = np->tx.rsp_cons; i != prod; i++) {
			txr = RING_GET_RESPONSE(&np->tx, i);
			if (txr->status == NETIF_RSP_NULL)
				continue;

			id = txr->id;
			m = np->xn_cdata.xn_tx_chain[id]; 
			KASSERT(m != NULL, ("mbuf not found in xn_tx_chain"));
			M_ASSERTVALID(m);
			
			/*
			 * Increment packet count if this is the last
			 * mbuf of the chain.
			 */
			if (!m->m_next)
				ifp->if_opackets++;
			if (unlikely(gnttab_query_foreign_access(
			    np->grant_tx_ref[id]) != 0)) {
				printf("network_tx_buf_gc: warning "
				    "-- grant still in use by backend "
				    "domain.\n");
				goto out; 
			}
			gnttab_end_foreign_access_ref(
				np->grant_tx_ref[id]);
			gnttab_release_grant_reference(
				&np->gref_tx_head, np->grant_tx_ref[id]);
			np->grant_tx_ref[id] = GRANT_INVALID_REF;
			
			np->xn_cdata.xn_tx_chain[id] = NULL;
			add_id_to_freelist(np->xn_cdata.xn_tx_chain, id);
			np->xn_cdata.xn_tx_chain_cnt--;
			if (np->xn_cdata.xn_tx_chain_cnt < 0) {
				panic("netif_release_tx_bufs: tx_chain_cnt must be >= 0");
			}
			m_free(m);
			/* Only mark the queue active if we've freed up at least one slot to try */
			ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
		}
		np->tx.rsp_cons = prod;
		
		/*
		 * Set a new event, then check for race with update of
		 * tx_cons. Note that it is essential to schedule a
		 * callback, no matter how few buffers are pending. Even if
		 * there is space in the transmit ring, higher layers may
		 * be blocked because too much data is outstanding: in such
		 * cases notification from Xen is likely to be the only kick
		 * that we'll get.
		 */
		np->tx.sring->rsp_event =
		    prod + ((np->tx.sring->req_prod - prod) >> 1) + 1;

		mb();
	} while (prod != np->tx.sring->rsp_prod);
	
 out: 
	if (np->tx_full &&
	    ((np->tx.sring->req_prod - prod) < NET_TX_RING_SIZE)) {
		np->tx_full = 0;
#if 0
		if (np->user_state == UST_OPEN)
			netif_wake_queue(dev);
#endif
	}

}

static void
xn_intr(void *xsc)
{
	struct netfront_info *np = xsc;
	struct ifnet *ifp = np->xn_ifp;

#if 0
	if (!(np->rx.rsp_cons != np->rx.sring->rsp_prod &&
	    likely(netfront_carrier_ok(np)) &&
	    ifp->if_drv_flags & IFF_DRV_RUNNING))
		return;
#endif
	if (np->tx.rsp_cons != np->tx.sring->rsp_prod) {
		XN_TX_LOCK(np);
		xn_txeof(np);
		XN_TX_UNLOCK(np);			
	}	

	XN_RX_LOCK(np);
	xn_rxeof(np);
	XN_RX_UNLOCK(np);

	if (ifp->if_drv_flags & IFF_DRV_RUNNING &&
	    !IFQ_DRV_IS_EMPTY(&ifp->if_snd))
		xn_start(ifp);
}


static void
xennet_move_rx_slot(struct netfront_info *np, struct mbuf *m,
	grant_ref_t ref)
{
	int new = xennet_rxidx(np->rx.req_prod_pvt);

	KASSERT(np->rx_mbufs[new] == NULL, ("rx_mbufs != NULL"));
	np->rx_mbufs[new] = m;
	np->grant_rx_ref[new] = ref;
	RING_GET_REQUEST(&np->rx, np->rx.req_prod_pvt)->id = new;
	RING_GET_REQUEST(&np->rx, np->rx.req_prod_pvt)->gref = ref;
	np->rx.req_prod_pvt++;
}

static int
xennet_get_extras(struct netfront_info *np,
    struct netif_extra_info *extras, RING_IDX rp)
{
	struct netif_extra_info *extra;
	RING_IDX cons = np->rx.rsp_cons;

	int err = 0;

	do {
		struct mbuf *m;
		grant_ref_t ref;

		if (unlikely(cons + 1 == rp)) {
#if 0			
			if (net_ratelimit())
				WPRINTK("Missing extra info\n");
#endif			
			err = -EINVAL;
			break;
		}

		extra = (struct netif_extra_info *)
		RING_GET_RESPONSE(&np->rx, ++cons);

		if (unlikely(!extra->type ||
			extra->type >= XEN_NETIF_EXTRA_TYPE_MAX)) {
#if 0				
			if (net_ratelimit())
				WPRINTK("Invalid extra type: %d\n",
					extra->type);
#endif			
			err = -EINVAL;
		} else {
			memcpy(&extras[extra->type - 1], extra, sizeof(*extra));
		}

		m = xennet_get_rx_mbuf(np, cons);
		ref = xennet_get_rx_ref(np, cons);
		xennet_move_rx_slot(np, m, ref);
	} while (extra->flags & XEN_NETIF_EXTRA_FLAG_MORE);

	np->rx.rsp_cons = cons;
	return err;
}

static int
xennet_get_responses(struct netfront_info *np,
	struct netfront_rx_info *rinfo, RING_IDX rp,
	struct mbuf  **list,
	int *pages_flipped_p)
{
	int pages_flipped = *pages_flipped_p;
	struct mmu_update *mmu;
	struct multicall_entry *mcl;
	struct netif_rx_response *rx = &rinfo->rx;
	struct netif_extra_info *extras = rinfo->extras;
	RING_IDX cons = np->rx.rsp_cons;
	struct mbuf *m, *m0, *m_prev;
	grant_ref_t ref = xennet_get_rx_ref(np, cons);
	int max = 5 /* MAX_SKB_FRAGS + (rx->status <= RX_COPY_THRESHOLD) */;
	int frags = 1;
	int err = 0;
	u_long ret;

	m0 = m = m_prev = xennet_get_rx_mbuf(np, cons);

	
	if (rx->flags & NETRXF_extra_info) {
		err = xennet_get_extras(np, extras, rp);
		cons = np->rx.rsp_cons;
	}


	if (m0 != NULL) {
			m0->m_pkthdr.len = 0;
			m0->m_next = NULL;
	}
	
	for (;;) {
		u_long mfn;

#if 0		
		printf("rx->status=%hd rx->offset=%hu frags=%u\n",
			rx->status, rx->offset, frags);
#endif
		if (unlikely(rx->status < 0 ||
			rx->offset + rx->status > PAGE_SIZE)) {
#if 0						
			if (net_ratelimit())
				WPRINTK("rx->offset: %x, size: %u\n",
					rx->offset, rx->status);
#endif						
			xennet_move_rx_slot(np, m, ref);
			err = -EINVAL;
			goto next;
		}
		
		/*
		 * This definitely indicates a bug, either in this driver or in
		 * the backend driver. In future this should flag the bad
		 * situation to the system controller to reboot the backed.
		 */
		if (ref == GRANT_INVALID_REF) {
#if 0 				
			if (net_ratelimit())
				WPRINTK("Bad rx response id %d.\n", rx->id);
#endif			
			err = -EINVAL;
			goto next;
		}

		if (!np->copying_receiver) {
			/* Memory pressure, insufficient buffer
			 * headroom, ...
			 */
			if (!(mfn = gnttab_end_foreign_transfer_ref(ref))) {
				if (net_ratelimit())
					WPRINTK("Unfulfilled rx req "
						"(id=%d, st=%d).\n",
						rx->id, rx->status);
				xennet_move_rx_slot(np, m, ref);
				err = -ENOMEM;
				goto next;
			}

			if (!xen_feature( XENFEAT_auto_translated_physmap)) {
				/* Remap the page. */
				void *vaddr = mtod(m, void *);
				uint32_t pfn;

				mcl = np->rx_mcl + pages_flipped;
				mmu = np->rx_mmu + pages_flipped;

				MULTI_update_va_mapping(mcl, (u_long)vaddr,
				    (((vm_paddr_t)mfn) << PAGE_SHIFT) | PG_RW |
				    PG_V | PG_M | PG_A, 0);
				pfn = (uintptr_t)m->m_ext.ext_arg1;
				mmu->ptr = ((vm_paddr_t)mfn << PAGE_SHIFT) |
				    MMU_MACHPHYS_UPDATE;
				mmu->val = pfn;

				set_phys_to_machine(pfn, mfn);
			}
			pages_flipped++;
		} else {
			ret = gnttab_end_foreign_access_ref(ref);
			KASSERT(ret, ("ret != 0"));
		}

		gnttab_release_grant_reference(&np->gref_rx_head, ref);

next:
		if (m == NULL)
			break;

		m->m_len = rx->status;
		m->m_data += rx->offset;
		m0->m_pkthdr.len += rx->status;
		
		if (!(rx->flags & NETRXF_more_data))
			break;

		if (cons + frags == rp) {
			if (net_ratelimit())
				WPRINTK("Need more frags\n");
			err = -ENOENT;
				break;
		}
		m_prev = m;
		
		rx = RING_GET_RESPONSE(&np->rx, cons + frags);
		m = xennet_get_rx_mbuf(np, cons + frags);

		m_prev->m_next = m;
		m->m_next = NULL;
		ref = xennet_get_rx_ref(np, cons + frags);
		frags++;
	}
	*list = m0;

	if (unlikely(frags > max)) {
		if (net_ratelimit())
			WPRINTK("Too many frags\n");
		err = -E2BIG;
	}

	if (unlikely(err))
		np->rx.rsp_cons = cons + frags;

	*pages_flipped_p = pages_flipped;

	return err;
}

static void
xn_tick_locked(struct netfront_info *sc) 
{
	XN_RX_LOCK_ASSERT(sc);
	callout_reset(&sc->xn_stat_ch, hz, xn_tick, sc);

	/* XXX placeholder for printing debug information */
     
}


static void
xn_tick(void *xsc) 
{
	struct netfront_info *sc;
    
	sc = xsc;
	XN_RX_LOCK(sc);
	xn_tick_locked(sc);
	XN_RX_UNLOCK(sc);
     
}
static void
xn_start_locked(struct ifnet *ifp) 
{
	int otherend_id;
	unsigned short id;
	struct mbuf *m_head, *m;
	struct netfront_info *sc;
	netif_tx_request_t *tx;
	netif_extra_info_t *extra;
	RING_IDX i;
	grant_ref_t ref;
	u_long mfn, tx_bytes;
	int notify, nfrags;

	sc = ifp->if_softc;
	otherend_id = xenbus_get_otherend_id(sc->xbdev);
	tx_bytes = 0;

	if (!netfront_carrier_ok(sc))
		return;
	
	for (i = sc->tx.req_prod_pvt; TRUE; i++) {
		IF_DEQUEUE(&ifp->if_snd, m_head);
		if (m_head == NULL) 
			break;
		
		/*
		 * netfront_tx_slot_available() tries to do some math to
		 * ensure that there'll be enough xenbus ring slots available
		 * for the maximum number of packet fragments (and a couple more
		 * for what I guess are TSO and other ring entry items.)
		 */
		if (!netfront_tx_slot_available(sc)) {
			IF_PREPEND(&ifp->if_snd, m_head);
			ifp->if_drv_flags |= IFF_DRV_OACTIVE;
			break;
		}

		/*
		 * Defragment the mbuf if necessary.
		 */
		for (m = m_head, nfrags = 0; m; m = m->m_next)
			nfrags++;
		if (nfrags > MAX_SKB_FRAGS) {
			m = m_defrag(m_head, M_DONTWAIT);
			if (!m) {
				m_freem(m_head);
				break;
			}
			m_head = m;
		}

		/* Determine how many fragments now exist */
		for (m = m_head, nfrags = 0; m; m = m->m_next)
			nfrags++;

		/*
		 * Don't attempt to queue this packet if there aren't
		 * enough free entries in the chain.
		 *
		 * There isn't a 1:1 correspondance between the mbuf TX ring
		 * and the xenbus TX ring.
		 * xn_txeof() may need to be called to free up some slots.
		 *
		 * It is quite possible that this can be later eliminated if
		 * it turns out that partial * packets can be pushed into
		 * the ringbuffer, with fragments pushed in when further slots
		 * free up.
		 *
		 * It is also quite possible that the driver will lock up
		 * if the TX queue fills up with no RX traffic, and
		 * the mbuf ring is exhausted. The queue may need
		 * a swift kick to continue.
		 */

		/*
		 * It is not +1 like the allocation because we need to keep
		 * slot [0] free for the freelist head
		 */
		if (sc->xn_cdata.xn_tx_chain_cnt + nfrags >= NET_TX_RING_SIZE) {
			printf("xn_start_locked: xn_tx_chain_cnt (%d) + nfrags %d >= NET_TX_RING_SIZE (%d); must be full!\n",
			    (int) sc->xn_cdata.xn_tx_chain_cnt,
			    (int) nfrags, (int) NET_TX_RING_SIZE);
			IF_PREPEND(&ifp->if_snd, m_head);
			ifp->if_drv_flags |= IFF_DRV_OACTIVE;
			break;
		}

		/*
		 * Make sure there's actually space available in the
		 * Xen TX ring for this. Overcompensate for the possibility
		 * of having a TCP offload fragment just in case for now
		 * (the +1) rather than adding logic to accurately calculate
		 * the required size.
		 */
		if (RING_FREE_REQUESTS(&sc->tx) < (nfrags + 1)) {
			printf("xn_start_locked: free ring slots (%d) < (nfrags + 1) (%d); must be full!\n",
			    (int) RING_FREE_REQUESTS(&sc->tx),
			    (int) (nfrags + 1));
			IF_PREPEND(&ifp->if_snd, m_head);
			ifp->if_drv_flags |= IFF_DRV_OACTIVE;
			break;
		}

		/*
		 * Start packing the mbufs in this chain into
		 * the fragment pointers. Stop when we run out
		 * of fragments or hit the end of the mbuf chain.
		 */
		m = m_head;
		extra = NULL;
		for (m = m_head; m; m = m->m_next) {
			tx = RING_GET_REQUEST(&sc->tx, i);
			id = get_id_from_freelist(sc->xn_cdata.xn_tx_chain);
			if (id == 0)
				panic("xn_start_locked: was allocated the freelist head!\n");
			sc->xn_cdata.xn_tx_chain_cnt++;
			if (sc->xn_cdata.xn_tx_chain_cnt >= NET_TX_RING_SIZE+1)
				panic("xn_start_locked: tx_chain_cnt must be < NET_TX_RING_SIZE+1\n");
			sc->xn_cdata.xn_tx_chain[id] = m;
			tx->id = id;
			ref = gnttab_claim_grant_reference(&sc->gref_tx_head);
			KASSERT((short)ref >= 0, ("Negative ref"));
			mfn = virt_to_mfn(mtod(m, vm_offset_t));
			gnttab_grant_foreign_access_ref(ref, otherend_id,
			    mfn, GNTMAP_readonly);
			tx->gref = sc->grant_tx_ref[id] = ref;
			tx->offset = mtod(m, vm_offset_t) & (PAGE_SIZE - 1);
			tx->flags = 0;
			if (m == m_head) {
				/*
				 * The first fragment has the entire packet
				 * size, subsequent fragments have just the
				 * fragment size. The backend works out the
				 * true size of the first fragment by
				 * subtracting the sizes of the other
				 * fragments.
				 */
				tx->size = m->m_pkthdr.len;

				/*
				 * The first fragment contains the
				 * checksum flags and is optionally
				 * followed by extra data for TSO etc.
				 */
				if (m->m_pkthdr.csum_flags
				    & CSUM_DELAY_DATA) {
					tx->flags |= (NETTXF_csum_blank
					    | NETTXF_data_validated);
				}
#if __FreeBSD_version >= 700000
				if (m->m_pkthdr.csum_flags & CSUM_TSO) {
					struct netif_extra_info *gso =
						(struct netif_extra_info *)
						RING_GET_REQUEST(&sc->tx, ++i);

					if (extra)
						extra->flags |= XEN_NETIF_EXTRA_FLAG_MORE;
					else
						tx->flags |= NETTXF_extra_info;

					gso->u.gso.size = m->m_pkthdr.tso_segsz;
					gso->u.gso.type =
						XEN_NETIF_GSO_TYPE_TCPV4;
					gso->u.gso.pad = 0;
					gso->u.gso.features = 0;

					gso->type = XEN_NETIF_EXTRA_TYPE_GSO;
					gso->flags = 0;
					extra = gso;
				}
#endif
			} else {
				tx->size = m->m_len;
			}
			if (m->m_next) {
				tx->flags |= NETTXF_more_data;
				i++;
			}
		}

		BPF_MTAP(ifp, m_head);

		sc->stats.tx_bytes += m_head->m_pkthdr.len;
		sc->stats.tx_packets++;
	}

	sc->tx.req_prod_pvt = i;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&sc->tx, notify);
	if (notify)
		notify_remote_via_irq(sc->irq);

	xn_txeof(sc);

	if (RING_FULL(&sc->tx)) {
		sc->tx_full = 1;
#if 0
		netif_stop_queue(dev);
#endif
	}

	return;
}    

static void
xn_start(struct ifnet *ifp)
{
	struct netfront_info *sc;
	sc = ifp->if_softc;
	XN_TX_LOCK(sc);
	xn_start_locked(ifp);
	XN_TX_UNLOCK(sc);
}

/* equivalent of network_open() in Linux */
static void 
xn_ifinit_locked(struct netfront_info *sc) 
{
	struct ifnet *ifp;
	
	XN_LOCK_ASSERT(sc);
	
	ifp = sc->xn_ifp;
	
	if (ifp->if_drv_flags & IFF_DRV_RUNNING) 
		return;
	
	xn_stop(sc);
	
	network_alloc_rx_buffers(sc);
	sc->rx.sring->rsp_event = sc->rx.rsp_cons + 1;
	
	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
	
	callout_reset(&sc->xn_stat_ch, hz, xn_tick, sc);

}


static void 
xn_ifinit(void *xsc)
{
	struct netfront_info *sc = xsc;
    
	XN_LOCK(sc);
	xn_ifinit_locked(sc);
	XN_UNLOCK(sc);

}


static int
xn_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct netfront_info *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *) data;
	struct ifaddr *ifa = (struct ifaddr *)data;

	int mask, error = 0;
	switch(cmd) {
	case SIOCSIFADDR:
	case SIOCGIFADDR:
		XN_LOCK(sc);
		if (ifa->ifa_addr->sa_family == AF_INET) {
			ifp->if_flags |= IFF_UP;
			if (!(ifp->if_drv_flags & IFF_DRV_RUNNING)) 
				xn_ifinit_locked(sc);
			arp_ifinit(ifp, ifa);
			XN_UNLOCK(sc);	
		} else {
			XN_UNLOCK(sc);	
			error = ether_ioctl(ifp, cmd, data);
		}
		break;
	case SIOCSIFMTU:
		/* XXX can we alter the MTU on a VN ?*/
#ifdef notyet
		if (ifr->ifr_mtu > XN_JUMBO_MTU)
			error = EINVAL;
		else 
#endif
		{
			ifp->if_mtu = ifr->ifr_mtu;
			ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
			xn_ifinit(sc);
		}
		break;
	case SIOCSIFFLAGS:
		XN_LOCK(sc);
		if (ifp->if_flags & IFF_UP) {
			/*
			 * If only the state of the PROMISC flag changed,
			 * then just use the 'set promisc mode' command
			 * instead of reinitializing the entire NIC. Doing
			 * a full re-init means reloading the firmware and
			 * waiting for it to start up, which may take a
			 * second or two.
			 */
#ifdef notyet
			/* No promiscuous mode with Xen */
			if (ifp->if_drv_flags & IFF_DRV_RUNNING &&
			    ifp->if_flags & IFF_PROMISC &&
			    !(sc->xn_if_flags & IFF_PROMISC)) {
				XN_SETBIT(sc, XN_RX_MODE,
					  XN_RXMODE_RX_PROMISC);
			} else if (ifp->if_drv_flags & IFF_DRV_RUNNING &&
				   !(ifp->if_flags & IFF_PROMISC) &&
				   sc->xn_if_flags & IFF_PROMISC) {
				XN_CLRBIT(sc, XN_RX_MODE,
					  XN_RXMODE_RX_PROMISC);
			} else
#endif
				xn_ifinit_locked(sc);
		} else {
			if (ifp->if_drv_flags & IFF_DRV_RUNNING) {
				xn_stop(sc);
			}
		}
		sc->xn_if_flags = ifp->if_flags;
		XN_UNLOCK(sc);
		error = 0;
		break;
	case SIOCSIFCAP:
		mask = ifr->ifr_reqcap ^ ifp->if_capenable;
		if (mask & IFCAP_TXCSUM) {
			if (IFCAP_TXCSUM & ifp->if_capenable) {
				ifp->if_capenable &= ~(IFCAP_TXCSUM|IFCAP_TSO4);
				ifp->if_hwassist &= ~(CSUM_TCP | CSUM_UDP
				    | CSUM_IP | CSUM_TSO);
			} else {
				ifp->if_capenable |= IFCAP_TXCSUM;
				ifp->if_hwassist |= (CSUM_TCP | CSUM_UDP
				    | CSUM_IP);
			}
		}
		if (mask & IFCAP_RXCSUM) {
			ifp->if_capenable ^= IFCAP_RXCSUM;
		}
#if __FreeBSD_version >= 700000
		if (mask & IFCAP_TSO4) {
			if (IFCAP_TSO4 & ifp->if_capenable) {
				ifp->if_capenable &= ~IFCAP_TSO4;
				ifp->if_hwassist &= ~CSUM_TSO;
			} else if (IFCAP_TXCSUM & ifp->if_capenable) {
				ifp->if_capenable |= IFCAP_TSO4;
				ifp->if_hwassist |= CSUM_TSO;
			} else {
				IPRINTK("Xen requires tx checksum offload"
				    " be enabled to use TSO\n");
				error = EINVAL;
			}
		}
		if (mask & IFCAP_LRO) {
			ifp->if_capenable ^= IFCAP_LRO;
			
		}
#endif
		error = 0;
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
#ifdef notyet
		if (ifp->if_drv_flags & IFF_DRV_RUNNING) {
			XN_LOCK(sc);
			xn_setmulti(sc);
			XN_UNLOCK(sc);
			error = 0;
		}
#endif
		/* FALLTHROUGH */
	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
		error = EINVAL;
		break;
	default:
		error = ether_ioctl(ifp, cmd, data);
	}
    
	return (error);
}

static void
xn_stop(struct netfront_info *sc)
{	
	struct ifnet *ifp;

	XN_LOCK_ASSERT(sc);
    
	ifp = sc->xn_ifp;

	callout_stop(&sc->xn_stat_ch);

	xn_free_rx_ring(sc);
	xn_free_tx_ring(sc);
    
	ifp->if_drv_flags &= ~(IFF_DRV_RUNNING | IFF_DRV_OACTIVE);
}

/* START of Xenolinux helper functions adapted to FreeBSD */
int
network_connect(struct netfront_info *np)
{
	int i, requeue_idx, error;
	grant_ref_t ref;
	netif_rx_request_t *req;
	u_int feature_rx_copy, feature_rx_flip;

	error = xenbus_scanf(XBT_NIL, xenbus_get_otherend_path(np->xbdev),
	    "feature-rx-copy", NULL, "%u", &feature_rx_copy);
	if (error)
		feature_rx_copy = 0;
	error = xenbus_scanf(XBT_NIL, xenbus_get_otherend_path(np->xbdev),
	    "feature-rx-flip", NULL, "%u", &feature_rx_flip);
	if (error)
		feature_rx_flip = 1;

	/*
	 * Copy packets on receive path if:
	 *  (a) This was requested by user, and the backend supports it; or
	 *  (b) Flipping was requested, but this is unsupported by the backend.
	 */
	np->copying_receiver = ((MODPARM_rx_copy && feature_rx_copy) ||
				(MODPARM_rx_flip && !feature_rx_flip));

	XN_LOCK(np);
	/* Recovery procedure: */
	error = talk_to_backend(np->xbdev, np);
	if (error) 
		return (error);
	
	/* Step 1: Reinitialise variables. */
	netif_release_tx_bufs(np);

	/* Step 2: Rebuild the RX buffer freelist and the RX ring itself. */
	for (requeue_idx = 0, i = 0; i < NET_RX_RING_SIZE; i++) {
		struct mbuf *m;
		u_long pfn;

		if (np->rx_mbufs[i] == NULL)
			continue;

		m = np->rx_mbufs[requeue_idx] = xennet_get_rx_mbuf(np, i);
		ref = np->grant_rx_ref[requeue_idx] = xennet_get_rx_ref(np, i);
		req = RING_GET_REQUEST(&np->rx, requeue_idx);
		pfn = vtophys(mtod(m, vm_offset_t)) >> PAGE_SHIFT;

		if (!np->copying_receiver) {
			gnttab_grant_foreign_transfer_ref(ref,
			    xenbus_get_otherend_id(np->xbdev),
			    pfn);
		} else {
			gnttab_grant_foreign_access_ref(ref,
			    xenbus_get_otherend_id(np->xbdev),
			    PFNTOMFN(pfn), 0);
		}
		req->gref = ref;
		req->id   = requeue_idx;

		requeue_idx++;
	}

	np->rx.req_prod_pvt = requeue_idx;
	
	/* Step 3: All public and private state should now be sane.  Get
	 * ready to start sending and receiving packets and give the driver
	 * domain a kick because we've probably just requeued some
	 * packets.
	 */
	netfront_carrier_on(np);
	notify_remote_via_irq(np->irq);
	XN_TX_LOCK(np);
	xn_txeof(np);
	XN_TX_UNLOCK(np);
	network_alloc_rx_buffers(np);
	XN_UNLOCK(np);

	return (0);
}

static void 
show_device(struct netfront_info *sc)
{
#ifdef DEBUG
	if (sc) {
		IPRINTK("<vif handle=%u %s(%s) evtchn=%u irq=%u tx=%p rx=%p>\n",
			sc->xn_ifno,
			be_state_name[sc->xn_backend_state],
			sc->xn_user_state ? "open" : "closed",
			sc->xn_evtchn,
			sc->xn_irq,
			sc->xn_tx_if,
			sc->xn_rx_if);
	} else {
		IPRINTK("<vif NULL>\n");
	}
#endif
}

/** Create a network device.
 * @param handle device handle
 */
int 
create_netdev(device_t dev)
{
	int i;
	struct netfront_info *np;
	int err;
	struct ifnet *ifp;

	np = device_get_softc(dev);
	
	np->xbdev         = dev;
    
	XN_LOCK_INIT(np, xennetif);
	np->rx_target     = RX_MIN_TARGET;
	np->rx_min_target = RX_MIN_TARGET;
	np->rx_max_target = RX_MAX_TARGET;
	
	/* Initialise {tx,rx}_skbs to be a free chain containing every entry. */
	for (i = 0; i <= NET_TX_RING_SIZE; i++) {
		np->tx_mbufs[i] = (void *) ((u_long) i+1);
		np->grant_tx_ref[i] = GRANT_INVALID_REF;	
	}
	for (i = 0; i <= NET_RX_RING_SIZE; i++) {
		np->rx_mbufs[i] = NULL;
		np->grant_rx_ref[i] = GRANT_INVALID_REF;
	}
	/* A grant for every tx ring slot */
	if (gnttab_alloc_grant_references(TX_MAX_TARGET,
					  &np->gref_tx_head) < 0) {
		printf("#### netfront can't alloc tx grant refs\n");
		err = ENOMEM;
		goto exit;
	}
	/* A grant for every rx ring slot */
	if (gnttab_alloc_grant_references(RX_MAX_TARGET,
					  &np->gref_rx_head) < 0) {
		printf("#### netfront can't alloc rx grant refs\n");
		gnttab_free_grant_references(np->gref_tx_head);
		err = ENOMEM;
		goto exit;
	}
	
	err = xen_net_read_mac(dev, np->mac);
	if (err) {
		xenbus_dev_fatal(dev, err, "parsing %s/mac",
		    xenbus_get_node(dev));
		goto out;
	}
	
	/* Set up ifnet structure */
	ifp = np->xn_ifp = if_alloc(IFT_ETHER);
    	ifp->if_softc = np;
    	if_initname(ifp, "xn",  device_get_unit(dev));
    	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
    	ifp->if_ioctl = xn_ioctl;
    	ifp->if_output = ether_output;
    	ifp->if_start = xn_start;
#ifdef notyet
    	ifp->if_watchdog = xn_watchdog;
#endif
    	ifp->if_init = xn_ifinit;
    	ifp->if_mtu = ETHERMTU;
    	ifp->if_snd.ifq_maxlen = NET_TX_RING_SIZE - 1;
	
    	ifp->if_hwassist = XN_CSUM_FEATURES;
    	ifp->if_capabilities = IFCAP_HWCSUM;
#if __FreeBSD_version >= 700000
	ifp->if_capabilities |= IFCAP_TSO4;
	if (xn_enable_lro) {
		int err = tcp_lro_init(&np->xn_lro);
		if (err) {
			device_printf(dev, "LRO initialization failed\n");
			goto exit;
		}
		np->xn_lro.ifp = ifp;
		ifp->if_capabilities |= IFCAP_LRO;
	}
#endif
    	ifp->if_capenable = ifp->if_capabilities;
	
    	ether_ifattach(ifp, np->mac);
    	callout_init(&np->xn_stat_ch, CALLOUT_MPSAFE);
	netfront_carrier_off(np);

	return (0);

exit:
	gnttab_free_grant_references(np->gref_tx_head);
out:
	panic("do something smart");

}

/**
 * Handle the change of state of the backend to Closing.  We must delete our
 * device-layer structures now, to ensure that writes are flushed through to
 * the backend.  Once is this done, we can switch to Closed in
 * acknowledgement.
 */
#if 0
static void netfront_closing(device_t dev)
{
#if 0
	struct netfront_info *info = dev->dev_driver_data;

	DPRINTK("netfront_closing: %s removed\n", dev->nodename);

	close_netdev(info);
#endif
	xenbus_switch_state(dev, XenbusStateClosed);
}
#endif

static int netfront_detach(device_t dev)
{
	struct netfront_info *info = device_get_softc(dev);

	DPRINTK("%s\n", xenbus_get_node(dev));

	netif_free(info);

	return 0;
}


static void netif_free(struct netfront_info *info)
{
	netif_disconnect_backend(info);
#if 0
	close_netdev(info);
#endif
}

static void netif_disconnect_backend(struct netfront_info *info)
{
	XN_RX_LOCK(info);
	XN_TX_LOCK(info);
	netfront_carrier_off(info);
	XN_TX_UNLOCK(info);
	XN_RX_UNLOCK(info);

	end_access(info->tx_ring_ref, info->tx.sring);
	end_access(info->rx_ring_ref, info->rx.sring);
	info->tx_ring_ref = GRANT_INVALID_REF;
	info->rx_ring_ref = GRANT_INVALID_REF;
	info->tx.sring = NULL;
	info->rx.sring = NULL;

	if (info->irq)
		unbind_from_irqhandler(info->irq);

	info->irq = 0;
}


static void end_access(int ref, void *page)
{
	if (ref != GRANT_INVALID_REF)
		gnttab_end_foreign_access(ref, page);
}

/* ** Driver registration ** */
static device_method_t netfront_methods[] = { 
	/* Device interface */ 
	DEVMETHOD(device_probe,         netfront_probe), 
	DEVMETHOD(device_attach,        netfront_attach), 
	DEVMETHOD(device_detach,        netfront_detach), 
	DEVMETHOD(device_shutdown,      bus_generic_shutdown), 
	DEVMETHOD(device_suspend,       bus_generic_suspend), 
	DEVMETHOD(device_resume,        netfront_resume), 
 
	/* Xenbus interface */
	DEVMETHOD(xenbus_backend_changed, netfront_backend_changed),

	{ 0, 0 } 
}; 

static driver_t netfront_driver = { 
	"xn", 
	netfront_methods, 
	sizeof(struct netfront_info),                      
}; 
devclass_t netfront_devclass; 
 
DRIVER_MODULE(xe, xenbus, netfront_driver, netfront_devclass, 0, 0); 
