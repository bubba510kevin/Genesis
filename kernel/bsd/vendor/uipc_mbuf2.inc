/* VENDORED - FreeBSD source, copied verbatim from
 * vendsrc/sys/kern/uipc_mbuf2.c. See kernel/bsd/vendor/kern_mbuf.inc's
 * header for why these are fragments and how they are built.
 *
 * Packet tags: the per-mbuf key/value list protocols hang out-of-band state
 * off. m_free() deletes the tag chain on every packet-header mbuf it frees,
 * so this is not optional even with no protocol in the tree to use it -
 * without m_tag_delete_chain(), the vendored allocation core does not link.
 *
 * WHY A FRAGMENT AND NOT THE WHOLE FILE
 * uipc_mbuf2.c also carries m_pulldown() and the MAC-label tag helpers;
 * m_pulldown wants the M_EXTPG paths and mac_framework.h wants the whole
 * MAC subsystem.
 *
 * Functions taken, in upstream order:
 *   m_tag_free_default   m_tag_alloc            m_tag_delete
 *   m_tag_delete_chain   m_tag_delete_nonpersistent
 *   m_tag_locate         m_tag_copy             m_tag_copy_chain
 */

/* Free a packet tag. */
void
m_tag_free_default(struct m_tag *t)
{
#ifdef MAC
	if (t->m_tag_id == PACKET_TAG_MACLABEL)
		mac_mbuf_tag_destroy(t);
#endif
	free(t, M_PACKET_TAGS);
}

/* Get a packet tag structure along with specified data following. */
struct m_tag *
m_tag_alloc(uint32_t cookie, uint16_t type, int len, int wait)
{
	struct m_tag *t;

	MBUF_CHECKSLEEP(wait);
	if (len < 0)
		return NULL;
	t = malloc(len + sizeof(struct m_tag), M_PACKET_TAGS, wait);
	if (t == NULL)
		return NULL;
	m_tag_setup(t, cookie, type, len);
	t->m_tag_free = m_tag_free_default;
	return t;
}

/* Unlink and free a packet tag. */
void
m_tag_delete(struct mbuf *m, struct m_tag *t)
{

	KASSERT(m && t, ("m_tag_delete: null argument, m %p t %p", m, t));
	m_tag_unlink(m, t);
	m_tag_free(t);
}

/* Unlink and free a packet tag chain, starting from given tag. */
void
m_tag_delete_chain(struct mbuf *m, struct m_tag *t)
{
	struct m_tag *p, *q;

	KASSERT(m, ("m_tag_delete_chain: null mbuf"));
	if (t != NULL)
		p = t;
	else
		p = SLIST_FIRST(&m->m_pkthdr.tags);
	if (p == NULL)
		return;
	while ((q = SLIST_NEXT(p, m_tag_link)) != NULL)
		m_tag_delete(m, q);
	m_tag_delete(m, p);
}

/*
 * Strip off all tags that would normally vanish when
 * passing through a network interface.  Only persistent
 * tags will exist after this; these are expected to remain
 * so long as the mbuf chain exists, regardless of the
 * path the mbufs take.
 */
void
m_tag_delete_nonpersistent(struct mbuf *m)
{
	struct m_tag *p, *q;

	SLIST_FOREACH_SAFE(p, &m->m_pkthdr.tags, m_tag_link, q)
		if ((p->m_tag_id & MTAG_PERSISTENT) == 0)
			m_tag_delete(m, p);
}

/* Find a tag, starting from a given position. */
struct m_tag *
m_tag_locate(struct mbuf *m, uint32_t cookie, uint16_t type, struct m_tag *t)
{
	struct m_tag *p;

	KASSERT(m, ("m_tag_locate: null mbuf"));
	if (t == NULL)
		p = SLIST_FIRST(&m->m_pkthdr.tags);
	else
		p = SLIST_NEXT(t, m_tag_link);
	while (p != NULL) {
		if (p->m_tag_cookie == cookie && p->m_tag_id == type)
			return p;
		p = SLIST_NEXT(p, m_tag_link);
	}
	return NULL;
}

/* Copy a single tag. */
struct m_tag *
m_tag_copy(struct m_tag *t, int how)
{
	struct m_tag *p;

	MBUF_CHECKSLEEP(how);
	KASSERT(t, ("m_tag_copy: null tag"));
	p = m_tag_alloc(t->m_tag_cookie, t->m_tag_id, t->m_tag_len, how);
	if (p == NULL)
		return (NULL);
#ifdef MAC
	/*
	 * XXXMAC: we should probably pass off the initialization, and
	 * copying here?  can we hide that PACKET_TAG_MACLABEL is
	 * special from the mbuf code?
	 */
	if (t->m_tag_id == PACKET_TAG_MACLABEL) {
		if (mac_mbuf_tag_init(p, how) != 0) {
			m_tag_free(p);
			return (NULL);
		}
		mac_mbuf_tag_copy(t, p);
	} else
#endif
		bcopy(t + 1, p + 1, t->m_tag_len); /* Copy the data */
	return p;
}

/*
 * Copy two tag chains. The destination mbuf (to) loses any attached
 * tags even if the operation fails. This should not be a problem, as
 * m_tag_copy_chain() is typically called with a newly-allocated
 * destination mbuf.
 */
int
m_tag_copy_chain(struct mbuf *to, const struct mbuf *from, int how)
{
	struct m_tag *p, *t, *tprev = NULL;

	MBUF_CHECKSLEEP(how);
	KASSERT(to && from,
		("m_tag_copy_chain: null argument, to %p from %p", to, from));
	m_tag_delete_chain(to, NULL);
	SLIST_FOREACH(p, &from->m_pkthdr.tags, m_tag_link) {
		t = m_tag_copy(p, how);
		if (t == NULL) {
			m_tag_delete_chain(to, NULL);
			return 0;
		}
		if (tprev == NULL)
			SLIST_INSERT_HEAD(&to->m_pkthdr.tags, t, m_tag_link);
		else
			SLIST_INSERT_AFTER(tprev, t, m_tag_link);
		tprev = t;
	}
	return 1;
}
