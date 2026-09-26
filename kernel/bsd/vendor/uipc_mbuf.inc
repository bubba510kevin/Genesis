/* mb_dupcl - vendored from uipc_mbuf.c */
void
mb_dupcl(struct mbuf *n, struct mbuf *m)
{
	volatile u_int *refcnt;

	KASSERT(m->m_flags & (M_EXT | M_EXTPG),
	    ("%s: M_EXT | M_EXTPG not set on %p", __func__, m));
	KASSERT(!(n->m_flags & (M_EXT | M_EXTPG)),
	    ("%s: M_EXT | M_EXTPG set on %p", __func__, n));

	/*
	 * Cache access optimization.
	 *
	 * o Regular M_EXT storage doesn't need full copy of m_ext, since
	 *   the holder of the 'ext_count' is responsible to carry the free
	 *   routine and its arguments.
	 * o M_EXTPG data is split between main part of mbuf and m_ext, the
	 *   main part is copied in full, the m_ext part is similar to M_EXT.
	 * o EXT_EXTREF, where 'ext_cnt' doesn't point into mbuf at all, is
	 *   special - it needs full copy of m_ext into each mbuf, since any
	 *   copy could end up as the last to free.
	 */
	if (m->m_flags & M_EXTPG) {
		bcopy(&m->m_epg_startcopy, &n->m_epg_startcopy,
		    __rangeof(struct mbuf, m_epg_startcopy, m_epg_endcopy));
		bcopy(&m->m_ext, &n->m_ext, m_epg_ext_copylen);
	} else if (m->m_ext.ext_type == EXT_EXTREF)
		bcopy(&m->m_ext, &n->m_ext, sizeof(struct m_ext));
	else
		bcopy(&m->m_ext, &n->m_ext, m_ext_copylen);

	n->m_flags |= m->m_flags & (M_RDONLY | M_EXT | M_EXTPG);

	/* See if this is the mbuf that holds the embedded refcount. */
	if (m->m_ext.ext_flags & EXT_FLAG_EMBREF) {
		refcnt = n->m_ext.ext_cnt = &m->m_ext.ext_count;
		n->m_ext.ext_flags &= ~EXT_FLAG_EMBREF;
	} else {
		KASSERT(m->m_ext.ext_cnt != NULL,
		    ("%s: no refcounting pointer on %p", __func__, m));
		refcnt = m->m_ext.ext_cnt;
	}

	if (*refcnt == 1)
		*refcnt += 1;
	else
		atomic_add_int(refcnt, 1);
}

/* m_append - vendored from uipc_mbuf.c */
int
m_append(struct mbuf *m0, int len, c_caddr_t cp)
{
	struct mbuf *m, *n;
	int remainder, space;

	for (m = m0; m->m_next != NULL; m = m->m_next)
		;
	remainder = len;
	space = M_TRAILINGSPACE(m);
	if (space > 0) {
		/*
		 * Copy into available space.
		 */
		if (space > remainder)
			space = remainder;
		bcopy(cp, mtod(m, caddr_t) + m->m_len, space);
		m->m_len += space;
		cp += space, remainder -= space;
	}
	while (remainder > 0) {
		/*
		 * Allocate a new mbuf; could check space
		 * and allocate a cluster instead.
		 */
		n = m_get(M_NOWAIT, m->m_type);
		if (n == NULL)
			break;
		n->m_len = min(MLEN, remainder);
		bcopy(cp, mtod(n, caddr_t), n->m_len);
		cp += n->m_len, remainder -= n->m_len;
		m->m_next = n;
		m = n;
	}
	if (m0->m_flags & M_PKTHDR)
		m0->m_pkthdr.len += len - remainder;
	return (remainder == 0);
}

static int
m_apply_extpg_one(struct mbuf *m, int off, int len,
    int (*f)(void *, void *, u_int), void *arg)
{
	void *p;
	u_int i, count, pgoff, pglen;
	int rval;

	KASSERT(PMAP_HAS_DMAP,
	    ("m_apply_extpg_one does not support unmapped mbufs"));
	off += mtod(m, vm_offset_t);
	if (off < m->m_epg_hdrlen) {
		count = min(m->m_epg_hdrlen - off, len);
		rval = f(arg, m->m_epg_hdr + off, count);
		if (rval)
			return (rval);
		len -= count;
		off = 0;
	} else
		off -= m->m_epg_hdrlen;
	pgoff = m->m_epg_1st_off;
	for (i = 0; i < m->m_epg_npgs && len > 0; i++) {
		pglen = m_epg_pagelen(m, i, pgoff);
		if (off < pglen) {
			count = min(pglen - off, len);
			p = PHYS_TO_DMAP(m->m_epg_pa[i] + pgoff + off);
			rval = f(arg, p, count);
			if (rval)
				return (rval);
			len -= count;
			off = 0;
		} else
			off -= pglen;
		pgoff = 0;
	}
	if (len > 0) {
		KASSERT(off < m->m_epg_trllen,
		    ("m_apply_extpg_one: offset beyond trailer"));
		KASSERT(len <= m->m_epg_trllen - off,
		    ("m_apply_extpg_one: length beyond trailer"));
		return (f(arg, m->m_epg_trail + off, len));
	}
	return (0);
}



/* Apply function f to the data in a single mbuf. */
static int
m_apply_one(struct mbuf *m, int off, int len,
    int (*f)(void *, void *, u_int), void *arg)
{
	if ((m->m_flags & M_EXTPG) != 0)
		return (m_apply_extpg_one(m, off, len, f, arg));
	else
		return (f(arg, mtod(m, caddr_t) + off, len));
}

/*
 * Apply function f to the data in an mbuf chain starting "off" bytes from
 * the beginning, continuing for "len" bytes.
 */
int
m_apply(struct mbuf *m, int off, int len,
    int (*f)(void *, void *, u_int), void *arg)
{
	u_int count;
	int rval;

	KASSERT(off >= 0, ("m_apply, negative off %d", off));
	KASSERT(len >= 0, ("m_apply, negative len %d", len));
	while (off > 0) {
		KASSERT(m != NULL, ("m_apply, offset > size of mbuf chain "
		    "(%d extra)", off));
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	while (len > 0) {
		KASSERT(m != NULL, ("m_apply, length > size of mbuf chain "
		    "(%d extra)", len));
		count = min(m->m_len - off, len);
		rval = m_apply_one(m, off, count, f, arg);
		if (rval)
			return (rval);
		len -= count;
		off = 0;
		m = m->m_next;
	}
	return (0);
}

/* m_prepend - vendored from uipc_mbuf.c */
struct mbuf *
m_prepend(struct mbuf *m, int len, int how)
{
	struct mbuf *mn;

	if (m->m_flags & M_PKTHDR)
		mn = m_gethdr(how, m->m_type);
	else
		mn = m_get(how, m->m_type);
	if (mn == NULL) {
		m_freem(m);
		return (NULL);
	}
	if (m->m_flags & M_PKTHDR)
		m_move_pkthdr(mn, m);
	mn->m_next = m;
	m = mn;
	if (len < M_SIZE(m))
		M_ALIGN(m, len);
	m->m_len = len;
	return (m);
}

/* m_adj - vendored from uipc_mbuf.c */
void
m_adj(struct mbuf *mp, int req_len)
{
	int len = req_len;
	struct mbuf *m;
	int count;

	if ((m = mp) == NULL)
		return;
	if (len >= 0) {
		/*
		 * Trim from head.
		 */
		while (m != NULL && len > 0) {
			if (m->m_len <= len) {
				len -= m->m_len;
				m->m_len = 0;
				m = m->m_next;
			} else {
				m->m_len -= len;
				m->m_data += len;
				len = 0;
			}
		}
		if (mp->m_flags & M_PKTHDR)
			mp->m_pkthdr.len -= (req_len - len);
	} else {
		/*
		 * Trim from tail.  Scan the mbuf chain,
		 * calculating its length and finding the last mbuf.
		 * If the adjustment only affects this mbuf, then just
		 * adjust and return.  Otherwise, rescan and truncate
		 * after the remaining size.
		 */
		len = -len;
		count = 0;
		for (;;) {
			count += m->m_len;
			if (m->m_next == (struct mbuf *)0)
				break;
			m = m->m_next;
		}
		if (m->m_len >= len) {
			m->m_len -= len;
			if (mp->m_flags & M_PKTHDR)
				mp->m_pkthdr.len -= len;
			return;
		}
		count -= len;
		if (count < 0)
			count = 0;
		/*
		 * Correct length for chain is "count".
		 * Find the mbuf with last data, adjust its length,
		 * and toss data from remaining mbufs on chain.
		 */
		m = mp;
		if (m->m_flags & M_PKTHDR)
			m->m_pkthdr.len = count;
		for (; m; m = m->m_next) {
			if (m->m_len >= count) {
				m->m_len = count;
				if (m->m_next != NULL) {
					m_freem(m->m_next);
					m->m_next = NULL;
				}
				break;
			}
			count -= m->m_len;
		}
	}
}

/* m_pullup - vendored from uipc_mbuf.c */
struct mbuf *
m_pullup(struct mbuf *n, int len)
{
	struct mbuf *m;
	int count;
	int space;

	KASSERT((n->m_flags & M_EXTPG) == 0,
	    ("%s: unmapped mbuf %p", __func__, n));

	/*
	 * If first mbuf has no cluster, and has room for len bytes
	 * without shifting current data, pullup into it,
	 * otherwise allocate a new mbuf to prepend to the chain.
	 */
	if ((n->m_flags & M_EXT) == 0 &&
	    n->m_data + len < &n->m_dat[MLEN] && n->m_next) {
		if (n->m_len >= len)
			return (n);
		m = n;
		n = n->m_next;
		len -= m->m_len;
	} else {
		if (len > MHLEN)
			goto bad;
		m = m_get(M_NOWAIT, n->m_type);
		if (m == NULL)
			goto bad;
		if (n->m_flags & M_PKTHDR)
			m_move_pkthdr(m, n);
	}
	space = &m->m_dat[MLEN] - (m->m_data + m->m_len);
	do {
		count = min(min(max(len, max_protohdr), space), n->m_len);
		bcopy(mtod(n, caddr_t), mtod(m, caddr_t) + m->m_len,
		  (u_int)count);
		len -= count;
		m->m_len += count;
		n->m_len -= count;
		space -= count;
		if (n->m_len)
			n->m_data += count;
		else
			n = m_free(n);
	} while (len > 0 && n);
	if (len > 0) {
		(void) m_free(m);
		goto bad;
	}
	m->m_next = n;
	return (m);
bad:
	m_freem(n);
	return (NULL);
}

/* m_dup - vendored from uipc_mbuf.c */
struct mbuf *
m_dup(const struct mbuf *m, int how)
{
	struct mbuf **p, *top = NULL;
	int remain, moff, nsize;

	MBUF_CHECKSLEEP(how);
	/* Sanity check */
	if (m == NULL)
		return (NULL);
	M_ASSERTPKTHDR(m);

	/* While there's more data, get a new mbuf, tack it on, and fill it */
	remain = m->m_pkthdr.len;
	moff = 0;
	p = &top;
	while (remain > 0 || top == NULL) {	/* allow m->m_pkthdr.len == 0 */
		struct mbuf *n;

		/* Get the next new mbuf */
		if (remain >= MINCLSIZE) {
			n = m_getcl(how, m->m_type, 0);
			nsize = MCLBYTES;
		} else {
			n = m_get(how, m->m_type);
			nsize = MLEN;
		}
		if (n == NULL)
			goto nospace;

		if (top == NULL) {		/* First one, must be PKTHDR */
			if (!m_dup_pkthdr(n, m, how)) {
				m_free(n);
				goto nospace;
			}
			if ((n->m_flags & M_EXT) == 0)
				nsize = MHLEN;
			n->m_flags &= ~M_RDONLY;
		}
		n->m_len = 0;

		/* Link it into the new chain */
		*p = n;
		p = &n->m_next;

		/* Copy data from original mbuf(s) into new mbuf */
		while (n->m_len < nsize && m != NULL) {
			int chunk = min(nsize - n->m_len, m->m_len - moff);

			m_copydata(m, moff, chunk, n->m_data + n->m_len);
			moff += chunk;
			n->m_len += chunk;
			remain -= chunk;
			if (moff == m->m_len) {
				m = m->m_next;
				moff = 0;
			}
		}

		/* Check correct total mbuf length */
		KASSERT((remain > 0 && m != NULL) || (remain == 0 && m == NULL),
		    	("%s: bogus m_pkthdr.len", __func__));
	}
	return (top);

nospace:
	m_freem(top);
	return (NULL);
}

/* m_length - vendored from uipc_mbuf.c */
u_int
m_length(struct mbuf *m0, struct mbuf **last)
{
	struct mbuf *m;
	u_int len;

	len = 0;
	for (m = m0; m != NULL; m = m->m_next) {
		len += m->m_len;
		if (m->m_next == NULL)
			break;
	}
	if (last != NULL)
		*last = m;
	return (len);
}

/* m_copydata - vendored from uipc_mbuf.c */
void
m_copydata(const struct mbuf *m, int off, int len, caddr_t cp)
{
	u_int count;

	KASSERT(off >= 0, ("m_copydata, negative off %d", off));
	KASSERT(len >= 0, ("m_copydata, negative len %d", len));
	while (off > 0) {
		KASSERT(m != NULL, ("m_copydata, offset > size of mbuf chain"));
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	while (len > 0) {
		KASSERT(m != NULL, ("m_copydata, length > size of mbuf chain"));
		count = min(m->m_len - off, len);
		if ((m->m_flags & M_EXTPG) != 0)
			m_copyfromunmapped(m, off, count, cp);
		else
			bcopy(mtod(m, caddr_t) + off, cp, count);
		len -= count;
		cp += count;
		off = 0;
		m = m->m_next;
	}
}

/* m_copym - vendored from uipc_mbuf.c */
struct mbuf *
m_copym(struct mbuf *m, int off0, int len, int wait)
{
	struct mbuf *n, **np;
	int off = off0;
	struct mbuf *top;
	int copyhdr = 0;

	KASSERT(off >= 0, ("m_copym, negative off %d", off));
	KASSERT(len >= 0, ("m_copym, negative len %d", len));
	MBUF_CHECKSLEEP(wait);
	if (off == 0 && m->m_flags & M_PKTHDR)
		copyhdr = 1;
	while (off > 0) {
		KASSERT(m != NULL, ("m_copym, offset > size of mbuf chain"));
		if (off < m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	np = &top;
	top = NULL;
	while (len > 0) {
		if (m == NULL) {
			KASSERT(len == M_COPYALL,
			    ("m_copym, length > size of mbuf chain"));
			break;
		}
		if (copyhdr)
			n = m_gethdr(wait, m->m_type);
		else
			n = m_get(wait, m->m_type);
		*np = n;
		if (n == NULL)
			goto nospace;
		if (copyhdr) {
			if (!m_dup_pkthdr(n, m, wait))
				goto nospace;
			if (len == M_COPYALL)
				n->m_pkthdr.len -= off0;
			else
				n->m_pkthdr.len = len;
			copyhdr = 0;
		}
		n->m_len = min(len, m->m_len - off);
		if (m->m_flags & (M_EXT | M_EXTPG)) {
			n->m_data = m->m_data + off;
			mb_dupcl(n, m);
		} else
			bcopy(mtod(m, caddr_t)+off, mtod(n, caddr_t),
			    (u_int)n->m_len);
		if (len != M_COPYALL)
			len -= n->m_len;
		off = 0;
		m = m->m_next;
		np = &n->m_next;
	}

	return (top);
nospace:
	m_freem(top);
	return (NULL);
}

/* m_cat - vendored from uipc_mbuf.c */
void
m_cat(struct mbuf *m, struct mbuf *n)
{
	while (m->m_next)
		m = m->m_next;
	while (n) {
		if (!M_WRITABLE(m) ||
		    (n->m_flags & M_EXTPG) != 0 ||
		    M_TRAILINGSPACE(m) < n->m_len) {
			/* just join the two chains */
			m->m_next = n;
			return;
		}
		/* splat the data from one into the other */
		bcopy(mtod(n, caddr_t), mtod(m, caddr_t) + m->m_len,
		    (u_int)n->m_len);
		m->m_len += n->m_len;
		n = m_free(n);
	}
}

/* m_pkthdr_init - vendored from uipc_mbuf.c */
int
m_pkthdr_init(struct mbuf *m, int how)
{
#ifdef MAC
	int error;
#endif
	m->m_data = m->m_pktdat;
	bzero(&m->m_pkthdr, sizeof(m->m_pkthdr));
#ifdef NUMA
	m->m_pkthdr.numa_domain = M_NODOM;
#endif
#ifdef MAC
	/* If the label init fails, fail the alloc */
	error = mac_mbuf_init(m, how);
	if (error)
		return (error);
#endif

	return (0);
}

/* m_move_pkthdr - vendored from uipc_mbuf.c */
void
m_move_pkthdr(struct mbuf *to, struct mbuf *from)
{

#if 0
	/* see below for why these are not enabled */
	M_ASSERTPKTHDR(to);
	/* Note: with MAC, this may not be a good assertion. */
	KASSERT(SLIST_EMPTY(&to->m_pkthdr.tags),
	    ("m_move_pkthdr: to has tags"));
#endif
#ifdef MAC
	/*
	 * XXXMAC: It could be this should also occur for non-MAC?
	 */
	if (to->m_flags & M_PKTHDR)
		m_tag_delete_chain(to, NULL);
#endif
	to->m_flags = (from->m_flags & M_COPYFLAGS) |
	    (to->m_flags & (M_EXT | M_EXTPG));
	if ((to->m_flags & M_EXT) == 0)
		to->m_data = to->m_pktdat;
	to->m_pkthdr = from->m_pkthdr;		/* especially tags */
	SLIST_INIT(&from->m_pkthdr.tags);	/* purge tags from src */
	from->m_flags &= ~M_PKTHDR;
	if (from->m_pkthdr.csum_flags & CSUM_SND_TAG) {
		from->m_pkthdr.csum_flags &= ~CSUM_SND_TAG;
		from->m_pkthdr.snd_tag = NULL;
	}
}

/* m_dup_pkthdr - vendored from uipc_mbuf.c */
int
m_dup_pkthdr(struct mbuf *to, const struct mbuf *from, int how)
{

#if 0
	/*
	 * The mbuf allocator only initializes the pkthdr
	 * when the mbuf is allocated with m_gethdr(). Many users
	 * (e.g. m_copy*, m_prepend) use m_get() and then
	 * smash the pkthdr as needed causing these
	 * assertions to trip.  For now just disable them.
	 */
	M_ASSERTPKTHDR(to);
	/* Note: with MAC, this may not be a good assertion. */
	KASSERT(SLIST_EMPTY(&to->m_pkthdr.tags), ("m_dup_pkthdr: to has tags"));
#endif
	MBUF_CHECKSLEEP(how);
#ifdef MAC
	if (to->m_flags & M_PKTHDR)
		m_tag_delete_chain(to, NULL);
#endif
	to->m_flags = (from->m_flags & M_COPYFLAGS) |
	    (to->m_flags & (M_EXT | M_EXTPG));
	if ((to->m_flags & M_EXT) == 0)
		to->m_data = to->m_pktdat;
	to->m_pkthdr = from->m_pkthdr;
	if (from->m_pkthdr.csum_flags & CSUM_SND_TAG)
		m_snd_tag_ref(from->m_pkthdr.snd_tag);
	SLIST_INIT(&to->m_pkthdr.tags);
	return (m_tag_copy_chain(to, from, how));
}

/* m_demote_pkthdr - vendored from uipc_mbuf.c */
void
m_demote_pkthdr(struct mbuf *m)
{

	M_ASSERTPKTHDR(m);
	M_ASSERT_NO_SND_TAG(m);

	m_tag_delete_chain(m, NULL);
	m->m_flags &= ~M_PKTHDR;
	bzero(&m->m_pkthdr, sizeof(struct pkthdr));
}

/* m_adj_decap - vendored from uipc_mbuf.c */
void
m_adj_decap(struct mbuf *mp, int len)
{
	uint8_t rsstype;

	m_adj(mp, len);
	if ((mp->m_flags & M_PKTHDR) != 0) {
		/*
		 * If flowid was calculated by card from the inner
		 * headers, move flowid to the decapsulated mbuf
		 * chain, otherwise clear.  This depends on the
		 * internals of m_adj, which keeps pkthdr as is, in
		 * particular not changing rsstype and flowid.
		 */
		rsstype = mp->m_pkthdr.rsstype;
		if ((rsstype & M_HASHTYPE_INNER) != 0) {
			M_HASHTYPE_SET(mp, rsstype & ~M_HASHTYPE_INNER);
		} else {
			M_HASHTYPE_CLEAR(mp);
		}
	}
}

/*
 * Return the number of fragments an mbuf will use.  This is usually
 * used as a proxy for the number of scatter/gather elements needed by
 * a DMA engine to access an mbuf.  In general mapped mbufs are
 * assumed to be backed by physically contiguous buffers that only
 * need a single fragment.  Unmapped mbufs, on the other hand, can
 * span disjoint physical pages.
 */
static int
frags_per_mbuf(struct mbuf *m)
{
	int frags;

	if ((m->m_flags & M_EXTPG) == 0)
		return (1);

	/*
	 * The header and trailer are counted as a single fragment
	 * each when present.
	 *
	 * XXX: This overestimates the number of fragments by assuming
	 * all the backing physical pages are disjoint.
	 */
	frags = 0;
	if (m->m_epg_hdrlen != 0)
		frags++;
	frags += m->m_epg_npgs;
	if (m->m_epg_trllen != 0)
		frags++;

	return (frags);
}

/*
 * Copy an mbuf chain into a uio limited by len if set.
 */
int
m_mbuftouio(struct uio *uio, const struct mbuf *m, int len)
{
	int error, length, total;
	int progress = 0;

	if (len > 0)
		total = min(uio->uio_resid, len);
	else
		total = uio->uio_resid;

	/* Fill the uio with data from the mbufs. */
	for (; m != NULL; m = m->m_next) {
		length = min(m->m_len, total - progress);

		if ((m->m_flags & M_EXTPG) != 0)
			error = m_unmapped_uiomove(m, 0, uio, length);
		else
			error = uiomove(mtod(m, void *), length, uio);
		if (error)
			return (error);

		progress += length;
	}

	return (0);
}


/*
 * Copy data from a buffer back into the indicated mbuf chain,
 * starting "off" bytes from the beginning, extending the mbuf
 * chain if necessary.
 */
void
m_copyback(struct mbuf *m0, int off, int len, c_caddr_t cp)
{
	int mlen;
	struct mbuf *m = m0, *n;
	int totlen = 0;

	if (m0 == NULL)
		return;
	while (off > (mlen = m->m_len)) {
		off -= mlen;
		totlen += mlen;
		if (m->m_next == NULL) {
			n = m_get(M_NOWAIT, m->m_type);
			if (n == NULL)
				goto out;
			bzero(mtod(n, caddr_t), MLEN);
			n->m_len = min(MLEN, len + off);
			m->m_next = n;
		}
		m = m->m_next;
	}
	while (len > 0) {
		if (m->m_next == NULL && (len > m->m_len - off)) {
			m->m_len += min(len - (m->m_len - off),
			    M_TRAILINGSPACE(m));
		}
		mlen = min (m->m_len - off, len);
		if ((m->m_flags & M_EXTPG) != 0)
			m_copytounmapped(m, off, mlen, cp);
		else
			bcopy(cp, off + mtod(m, caddr_t), (u_int)mlen);
		cp += mlen;
		len -= mlen;
		mlen += off;
		off = 0;
		totlen += mlen;
		if (len == 0)
			break;
		if (m->m_next == NULL) {
			n = m_get(M_NOWAIT, m->m_type);
			if (n == NULL)
				break;
			n->m_len = min(MLEN, len);
			m->m_next = n;
		}
		m = m->m_next;
	}
out:	if (((m = m0)->m_flags & M_PKTHDR) && (m->m_pkthdr.len < totlen))
		m->m_pkthdr.len = totlen;
}

/*
 * Clean up mbuf (chain) from any tags and packet headers.
 * If "all" is set then the first mbuf in the chain will be
 * cleaned too.
 */
void
m_demote(struct mbuf *m0, int all, int flags)
{
	struct mbuf *m;

	flags |= M_DEMOTEFLAGS;

	for (m = all ? m0 : m0->m_next; m != NULL; m = m->m_next) {
		KASSERT(m->m_nextpkt == NULL, ("%s: m_nextpkt in m %p, m0 %p",
		    __func__, m, m0));
		if (m->m_flags & M_PKTHDR)
			m_demote_pkthdr(m);
		m->m_flags &= flags;
	}
}

u_int
m_fixhdr(struct mbuf *m0)
{
	u_int len;

	len = m_length(m0, NULL);
	m0->m_pkthdr.len = len;
	return (len);
}

/*
 * Return a pointer to mbuf/offset of location in mbuf chain.
 */
struct mbuf *
m_getptr(struct mbuf *m, int loc, int *off)
{

	while (loc >= 0) {
		/* Normal end of search. */
		if (m->m_len > loc) {
			*off = loc;
			return (m);
		} else {
			loc -= m->m_len;
			if (m->m_next == NULL) {
				if (loc == 0) {
					/* Point at the end of valid data. */
					*off = m->m_len;
					return (m);
				}
				return (NULL);
			}
			m = m->m_next;
		}
	}
	return (NULL);
}

/*
 * Copy the contents of uio into a properly sized mbuf chain.
 * A compat KPI.  Users are recommended to use direct calls to backing
 * functions.
 */
struct mbuf *
m_uiotombuf(struct uio *uio, int how, int len, int lspace, int flags)
{

	if (flags & M_EXTPG) {
		/* XXX: 'lspace' magically becomes maxseg! */
		return (m_uiotombuf_nomap(uio, how, len, lspace, flags));
	} else if (__predict_false(uio->uio_resid == 0)) {
		struct mbuf *m;

		/*
		 * m_uiotombuf() is known to return zero length buffer, keep
		 * this compatibility. mc_uiotomc() won't do that.
		 */
		if (flags & M_PKTHDR) {
			m = m_gethdr(how, MT_DATA);
			m->m_pkthdr.memlen = MSIZE;
		} else
			m = m_get(how, MT_DATA);
		if (m != NULL)
			m->m_data += lspace;
		return (m);
	} else {
		struct mchain mc;
		int error;

		error = mc_uiotomc(&mc, uio, len, lspace, how, flags);
		if (__predict_true(error == 0)) {
			if (flags & M_PKTHDR) {
				mc_first(&mc)->m_pkthdr.len = mc.mc_len;
				mc_first(&mc)->m_pkthdr.memlen = mc.mc_mlen;
			}
			return (mc_first(&mc));
		} else
			return (NULL);
	}
}


/*
 * Copy the contents of uio into a properly sized mbuf chain.
 * @param length Limit copyout length.  If 0 entire uio_resid is copied.
 * @param lspace Provide leading space in the first mbuf in the chain.
 */
int
mc_uiotomc(struct mchain *mc, struct uio *uio, u_int length, u_int lspace,
    int how, int flags)
{
	struct mbuf *mb;
	u_int total;
	int error;

	MPASS(lspace < MHLEN);
	MPASS(UINT_MAX - lspace >= length);
	MPASS(uio->uio_rw == UIO_WRITE);
	MPASS(uio->uio_resid >= 0);

	if (length > 0) {
		if (uio->uio_resid > length) {
			total = length;
			flags &= ~M_EOR;
		} else
			total = uio->uio_resid;
	} else if (__predict_false(uio->uio_resid + lspace > UINT_MAX))
		return (EOVERFLOW);
	else
		total = uio->uio_resid;

	if (__predict_false(total + lspace == 0)) {
		*mc = MCHAIN_INITIALIZER(mc);
		return (0);
	}

	error = mc_get(mc, total + lspace, how, MT_DATA, flags);
	if (__predict_false(error))
		return (error);
	mc_first(mc)->m_data += lspace;

	/* Fill all mbufs with uio data and update header information. */
	STAILQ_FOREACH(mb, &mc->mc_q, m_stailq) {
		u_int mlen;

		mlen = min(M_TRAILINGSPACE(mb), total - mc->mc_len);
		error = uiomove(mtod(mb, void *), mlen, uio);
		if (__predict_false(error)) {
			mc_freem(mc);
			*mc = MCHAIN_INITIALIZER(mc);
			return (error);
		}
		mb->m_len = mlen;
		mc->mc_len += mlen;
	}
	MPASS(mc->mc_len == total);

	return (0);
}


/*
 * Concatenate two pkthdr mbuf chains.
 */
void
m_catpkt(struct mbuf *m, struct mbuf *n)
{

	M_ASSERTPKTHDR(m);
	M_ASSERTPKTHDR(n);

	m->m_pkthdr.len += n->m_pkthdr.len;
	m_demote(n, 1, 0);

	m_cat(m, n);
}

/*
 * Defragment an mbuf chain, returning at most maxfrags separate
 * mbufs+clusters.  If this is not possible NULL is returned and
 * the original mbuf chain is left in its present (potentially
 * modified) state.  We use two techniques: collapsing consecutive
 * mbufs and replacing consecutive mbufs by a cluster.
 *
 * NB: this should really be named m_defrag but that name is taken
 */
struct mbuf *
m_collapse(struct mbuf *m0, int how, int maxfrags)
{
	struct mbuf *m, *n, *n2, **prev;
	u_int curfrags;

	/*
	 * Calculate the current number of frags.
	 */
	curfrags = 0;
	for (m = m0; m != NULL; m = m->m_next)
		curfrags += frags_per_mbuf(m);
	/*
	 * First, try to collapse mbufs.  Note that we always collapse
	 * towards the front so we don't need to deal with moving the
	 * pkthdr.  This may be suboptimal if the first mbuf has much
	 * less data than the following.
	 */
	m = m0;
again:
	for (;;) {
		n = m->m_next;
		if (n == NULL)
			break;
		if (M_WRITABLE(m) &&
		    n->m_len < M_TRAILINGSPACE(m)) {
			m_copydata(n, 0, n->m_len,
			    mtod(m, char *) + m->m_len);
			m->m_len += n->m_len;
			m->m_next = n->m_next;
			curfrags -= frags_per_mbuf(n);
			m_free(n);
			if (curfrags <= maxfrags)
				return m0;
		} else
			m = n;
	}
	KASSERT(maxfrags > 1,
		("maxfrags %u, but normal collapse failed", maxfrags));
	/*
	 * Collapse consecutive mbufs to a cluster.
	 */
	prev = &m0->m_next;		/* NB: not the first mbuf */
	while ((n = *prev) != NULL) {
		if ((n2 = n->m_next) != NULL &&
		    n->m_len + n2->m_len < MCLBYTES) {
			m = m_getcl(how, MT_DATA, 0);
			if (m == NULL)
				goto bad;
			m_copydata(n, 0,  n->m_len, mtod(m, char *));
			m_copydata(n2, 0,  n2->m_len,
			    mtod(m, char *) + n->m_len);
			m->m_len = n->m_len + n2->m_len;
			m->m_next = n2->m_next;
			*prev = m;
			curfrags += 1;  /* For the new cluster */
			curfrags -= frags_per_mbuf(n);
			curfrags -= frags_per_mbuf(n2);
			m_free(n);
			m_free(n2);
			if (curfrags <= maxfrags)
				return m0;
			/*
			 * Still not there, try the normal collapse
			 * again before we allocate another cluster.
			 */
			goto again;
		}
		prev = &n->m_next;
	}
	/*
	 * No place where we can collapse to a cluster; punt.
	 * This can occur if, for example, you request 2 frags
	 * but the packet requires that both be clusters (we
	 * never reallocate the first mbuf to avoid moving the
	 * packet header).
	 */
bad:
	return NULL;
}

/*
 * Copy an entire packet, including header (which must be present).
 * An optimization of the common case `m_copym(m, 0, M_COPYALL, how)'.
 * Note that the copy is read-only, because clusters are not copied,
 * only their reference counts are incremented.
 * Preserve alignment of the first mbuf so if the creator has left
 * some room at the beginning (e.g. for inserting protocol headers)
 * the copies still have the room available.
 */
struct mbuf *
m_copypacket(struct mbuf *m, int how)
{
	struct mbuf *top, *n, *o;

	MBUF_CHECKSLEEP(how);
	n = m_get(how, m->m_type);
	top = n;
	if (n == NULL)
		goto nospace;

	if (!m_dup_pkthdr(n, m, how))
		goto nospace;
	n->m_len = m->m_len;
	if (m->m_flags & (M_EXT | M_EXTPG)) {
		n->m_data = m->m_data;
		mb_dupcl(n, m);
	} else {
		n->m_data = n->m_pktdat + (m->m_data - m->m_pktdat );
		bcopy(mtod(m, char *), mtod(n, char *), n->m_len);
	}

	m = m->m_next;
	while (m) {
		o = m_get(how, m->m_type);
		if (o == NULL)
			goto nospace;

		n->m_next = o;
		n = n->m_next;

		n->m_len = m->m_len;
		if (m->m_flags & (M_EXT | M_EXTPG)) {
			n->m_data = m->m_data;
			mb_dupcl(n, m);
		} else {
			bcopy(mtod(m, char *), mtod(n, char *), n->m_len);
		}

		m = m->m_next;
	}
	return top;
nospace:
	m_freem(top);
	return (NULL);
}

/*
 * Like m_pullup(), except a new mbuf is always allocated, and we allow
 * the amount of empty space before the data in the new mbuf to be specified
 * (in the event that the caller expects to prepend later).
 */
struct mbuf *
m_copyup(struct mbuf *n, int len, int dstoff)
{
	struct mbuf *m;
	int count, space;

	if (len > (MHLEN - dstoff))
		goto bad;
	m = m_get(M_NOWAIT, n->m_type);
	if (m == NULL)
		goto bad;
	if (n->m_flags & M_PKTHDR)
		m_move_pkthdr(m, n);
	m->m_data += dstoff;
	space = &m->m_dat[MLEN] - (m->m_data + m->m_len);
	do {
		count = min(min(max(len, max_protohdr), space), n->m_len);
		memcpy(mtod(m, caddr_t) + m->m_len, mtod(n, caddr_t),
		    (unsigned)count);
		len -= count;
		m->m_len += count;
		n->m_len -= count;
		space -= count;
		if (n->m_len)
			n->m_data += count;
		else
			n = m_free(n);
	} while (len > 0 && n);
	if (len > 0) {
		(void) m_free(m);
		goto bad;
	}
	m->m_next = n;
	return (m);
 bad:
	m_freem(n);
	return (NULL);
}

/*
 * Defragment a mbuf chain, returning the shortest possible
 * chain of mbufs and clusters.  If allocation fails and
 * this cannot be completed, NULL will be returned, but
 * the passed in chain will be unchanged.  Upon success,
 * the original chain will be freed, and the new chain
 * will be returned.
 *
 * If a non-packet header is passed in, the original
 * mbuf (chain?) will be returned unharmed.
 */
struct mbuf *
m_defrag(struct mbuf *m0, int how)
{
	struct mbuf *m_new = NULL, *m_final = NULL;
	int progress = 0, length;

	MBUF_CHECKSLEEP(how);
	if (!(m0->m_flags & M_PKTHDR))
		return (m0);

	m_fixhdr(m0); /* Needed sanity check */

#ifdef MBUF_STRESS_TEST
	if (m_defragrandomfailures) {
		int temp = arc4random() & 0xff;
		if (temp == 0xba)
			goto nospace;
	}
#endif

	if (m0->m_pkthdr.len > MHLEN)
		m_final = m_getcl(how, MT_DATA, M_PKTHDR);
	else
		m_final = m_gethdr(how, MT_DATA);

	if (m_final == NULL)
		goto nospace;

	if (m_dup_pkthdr(m_final, m0, how) == 0)
		goto nospace;

	m_new = m_final;

	while (progress < m0->m_pkthdr.len) {
		length = m0->m_pkthdr.len - progress;
		if (length > MCLBYTES)
			length = MCLBYTES;

		if (m_new == NULL) {
			if (length > MLEN)
				m_new = m_getcl(how, MT_DATA, 0);
			else
				m_new = m_get(how, MT_DATA);
			if (m_new == NULL)
				goto nospace;
		}

		m_copydata(m0, progress, length, mtod(m_new, caddr_t));
		progress += length;
		m_new->m_len = length;
		if (m_new != m_final)
			m_cat(m_final, m_new);
		m_new = NULL;
	}
#ifdef MBUF_STRESS_TEST
	if (m0->m_next == NULL)
		m_defraguseless++;
#endif
	m_freem(m0);
	m0 = m_final;
#ifdef MBUF_STRESS_TEST
	m_defragpackets++;
	m_defragbytes += m0->m_pkthdr.len;
#endif
	return (m0);
nospace:
#ifdef MBUF_STRESS_TEST
	m_defragfailure++;
#endif
	if (m_final)
		m_freem(m_final);
	return (NULL);
}

/*
 * Routine to copy from device local memory into mbufs.
 * Note that `off' argument is offset into first mbuf of target chain from
 * which to begin copying the data to.
 */
struct mbuf *
m_devget(char *buf, int totlen, int off, struct ifnet *ifp,
    void (*copy)(char *from, caddr_t to, u_int len))
{
	struct mbuf *m;
	struct mbuf *top = NULL, **mp = &top;
	int len;

	if (off < 0 || off > MHLEN)
		return (NULL);

	while (totlen > 0) {
		if (top == NULL) {	/* First one, must be PKTHDR */
			if (totlen + off >= MINCLSIZE) {
				m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
				len = MCLBYTES;
			} else {
				m = m_gethdr(M_NOWAIT, MT_DATA);
				len = MHLEN;

				/* Place initial small packet/header at end of mbuf */
				if (m && totlen + off + max_linkhdr <= MHLEN) {
					m->m_data += max_linkhdr;
					len -= max_linkhdr;
				}
			}
			if (m == NULL)
				return NULL;
			m->m_pkthdr.rcvif = ifp;
			m->m_pkthdr.len = totlen;
		} else {
			if (totlen + off >= MINCLSIZE) {
				m = m_getcl(M_NOWAIT, MT_DATA, 0);
				len = MCLBYTES;
			} else {
				m = m_get(M_NOWAIT, MT_DATA);
				len = MLEN;
			}
			if (m == NULL) {
				m_freem(top);
				return NULL;
			}
		}
		if (off) {
			m->m_data += off;
			len -= off;
			off = 0;
		}
		m->m_len = len = min(totlen, len);
		if (copy)
			copy(buf, mtod(m, caddr_t), (u_int)len);
		else
			bcopy(buf, mtod(m, caddr_t), (u_int)len);
		buf += len;
		*mp = m;
		mp = &m->m_next;
		totlen -= len;
	}
	return (top);
}

/*
 * Partition an mbuf chain in two pieces, returning the tail --
 * all but the first len0 bytes.  In case of failure, it returns NULL and
 * attempts to restore the chain to its original state.
 *
 * Note that the resulting mbufs might be read-only, because the new
 * mbuf can end up sharing an mbuf cluster with the original mbuf if
 * the "breaking point" happens to lie within a cluster mbuf. Use the
 * M_WRITABLE() macro to check for this case.
 */
struct mbuf *
m_split(struct mbuf *m0, int len0, int wait)
{
	struct mbuf *m, *n;
	u_int len = len0, remain;

	MBUF_CHECKSLEEP(wait);
	for (m = m0; m && len > m->m_len; m = m->m_next)
		len -= m->m_len;
	if (m == NULL)
		return (NULL);
	remain = m->m_len - len;
	if (m0->m_flags & M_PKTHDR && remain == 0) {
		n = m_gethdr(wait, m0->m_type);
		if (n == NULL)
			return (NULL);
		n->m_next = m->m_next;
		m->m_next = NULL;
		if (m0->m_pkthdr.csum_flags & CSUM_SND_TAG) {
			n->m_pkthdr.snd_tag =
			    m_snd_tag_ref(m0->m_pkthdr.snd_tag);
			n->m_pkthdr.csum_flags |= CSUM_SND_TAG;
		} else
			n->m_pkthdr.rcvif = m0->m_pkthdr.rcvif;
		n->m_pkthdr.len = m0->m_pkthdr.len - len0;
		m0->m_pkthdr.len = len0;
		return (n);
	} else if (m0->m_flags & M_PKTHDR) {
		n = m_gethdr(wait, m0->m_type);
		if (n == NULL)
			return (NULL);
		if (m0->m_pkthdr.csum_flags & CSUM_SND_TAG) {
			n->m_pkthdr.snd_tag =
			    m_snd_tag_ref(m0->m_pkthdr.snd_tag);
			n->m_pkthdr.csum_flags |= CSUM_SND_TAG;
		} else
			n->m_pkthdr.rcvif = m0->m_pkthdr.rcvif;
		n->m_pkthdr.len = m0->m_pkthdr.len - len0;
		m0->m_pkthdr.len = len0;
		if (m->m_flags & (M_EXT | M_EXTPG))
			goto extpacket;
		if (remain > MHLEN) {
			/* m can't be the lead packet */
			M_ALIGN(n, 0);
			n->m_next = m_split(m, len, wait);
			if (n->m_next == NULL) {
				(void) m_free(n);
				return (NULL);
			} else {
				n->m_len = 0;
				return (n);
			}
		} else
			M_ALIGN(n, remain);
	} else if (remain == 0) {
		n = m->m_next;
		m->m_next = NULL;
		return (n);
	} else {
		n = m_get(wait, m->m_type);
		if (n == NULL)
			return (NULL);
		M_ALIGN(n, remain);
	}
extpacket:
	if (m->m_flags & (M_EXT | M_EXTPG)) {
		n->m_data = m->m_data + len;
		mb_dupcl(n, m);
	} else {
		bcopy(mtod(m, caddr_t) + len, mtod(n, caddr_t), remain);
	}
	n->m_len = remain;
	m->m_len = len;
	n->m_next = m->m_next;
	m->m_next = NULL;
	return (n);
}

/*
 * Create a writable copy of the mbuf chain.  While doing this
 * we compact the chain with a goal of producing a chain with
 * at most two mbufs.  The second mbuf in this chain is likely
 * to be a cluster.  The primary purpose of this work is to create
 * a writable packet for encryption, compression, etc.  The
 * secondary goal is to linearize the data so the data can be
 * passed to crypto hardware in the most efficient manner possible.
 */
struct mbuf *
m_unshare(struct mbuf *m0, int how)
{
	struct mbuf *m, *mprev;
	struct mbuf *n, *mfirst, *mlast;
	int len, off;

	mprev = NULL;
	for (m = m0; m != NULL; m = mprev->m_next) {
		/*
		 * m_unshare() can not process KTLS mbufs because they must
		 * neither be linearized nor converted to mapped.
		 */
		if (mbuf_has_tls_session(m)) {
			m_freem(m0);
			return (NULL);
		}

		/*
		 * Regular mbufs are ignored unless there's a cluster
		 * in front of it that we can use to coalesce.  We do
		 * the latter mainly so later clusters can be coalesced
		 * also w/o having to handle them specially (i.e. convert
		 * mbuf+cluster -> cluster).  This optimization is heavily
		 * influenced by the assumption that we're running over
		 * Ethernet where MCLBYTES is large enough that the max
		 * packet size will permit lots of coalescing into a
		 * single cluster.  This in turn permits efficient
		 * crypto operations, especially when using hardware.
		 */
		if ((m->m_flags & M_EXT) == 0) {
			if (mprev &&
			    (mprev->m_flags & (M_EXT | M_EXTPG)) == M_EXT &&
			    m->m_len <= M_TRAILINGSPACE(mprev)) {
				/* XXX: this ignores mbuf types */
				memcpy(mtod(mprev, caddr_t) + mprev->m_len,
				    mtod(m, caddr_t), m->m_len);
				mprev->m_len += m->m_len;
				mprev->m_next = m->m_next;	/* unlink from chain */
				m_free(m);			/* reclaim mbuf */
			} else {
				mprev = m;
			}
			continue;
		}
		/*
		 * Writable mbufs are left alone (for now).
		 */
		if (M_WRITABLE(m)) {
			mprev = m;
			continue;
		}

		/*
		 * Not writable, replace with a copy or coalesce with
		 * the previous mbuf if possible (since we have to copy
		 * it anyway, we try to reduce the number of mbufs and
		 * clusters so that future work is easier).
		 */
		KASSERT(m->m_flags & M_EXT, ("m_flags 0x%x", m->m_flags));
		/* NB: we only coalesce into a cluster or larger */
		if (mprev != NULL &&
		    (mprev->m_flags & (M_EXT | M_EXTPG)) == M_EXT &&
		    m->m_len <= M_TRAILINGSPACE(mprev)) {
			/* XXX: this ignores mbuf types */
			m_copydata(m, 0, m->m_len,
			    mtod(mprev, caddr_t) + mprev->m_len);
			mprev->m_len += m->m_len;
			mprev->m_next = m->m_next;	/* unlink from chain */
			m_free(m);			/* reclaim mbuf */
			continue;
		}

		/*
		 * Allocate new space to hold the copy and copy the data.
		 * We deal with jumbo mbufs (i.e. m_len > MCLBYTES) by
		 * splitting them into clusters.  We could just malloc a
		 * buffer and make it external but too many device drivers
		 * don't know how to break up the non-contiguous memory when
		 * doing DMA.
		 */
		n = m_getcl(how, m->m_type, m->m_flags & M_COPYFLAGS);
		if (n == NULL) {
			m_freem(m0);
			return (NULL);
		}
		if (m->m_flags & M_PKTHDR) {
			KASSERT(mprev == NULL, ("%s: m0 %p, m %p has M_PKTHDR",
			    __func__, m0, m));
			m_move_pkthdr(n, m);
		}
		len = m->m_len;
		off = 0;
		mfirst = n;
		mlast = NULL;
		for (;;) {
			int cc = min(len, MCLBYTES);
			m_copydata(m, off, cc, mtod(n, caddr_t));
			n->m_len = cc;
			if (mlast != NULL)
				mlast->m_next = n;
			mlast = n;
#if 0
			newipsecstat.ips_clcopied++;
#endif

			len -= cc;
			if (len <= 0)
				break;
			off += cc;

			n = m_getcl(how, m->m_type, m->m_flags & M_COPYFLAGS);
			if (n == NULL) {
				m_freem(mfirst);
				m_freem(m0);
				return (NULL);
			}
		}
		n->m_next = m->m_next;
		if (mprev == NULL)
			m0 = mfirst;		/* new head of chain */
		else
			mprev->m_next = mfirst;	/* replace old mbuf */
		m_free(m);			/* release old mbuf */
		mprev = mfirst;
	}
	return (m0);
}
