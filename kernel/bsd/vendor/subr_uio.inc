int
uiomove(void *cp, int n, struct uio *uio)
{

	return (uiomove_faultflag(cp, n, uio, 0));
}

int
uiomove_nofault(void *cp, int n, struct uio *uio)
{

	return (uiomove_faultflag(cp, n, uio, 1));
}

int
uiomove_step(void *cp, void *base, size_t len, struct uio *uio)
{
	int error = 0;

	switch (uio->uio_segflg) {
	case UIO_USERSPACE:
		maybe_yield();
		switch (uio->uio_rw) {
		case UIO_READ:
			error = copyout(cp, base, len);
			break;
		case UIO_WRITE:
			error = copyin(base, cp, len);
			break;
		}
		break;
	case UIO_SYSSPACE:
		switch (uio->uio_rw) {
		case UIO_READ:
			memcpy(base, cp, len);
			break;
		case UIO_WRITE:
			memcpy(cp, base, len);
			break;
		}
		break;
	case UIO_NOCOPY:
		break;
	}

	return (error);
}

static int
uiomove_faultflag(void *cp, int n, struct uio *uio, int nofault)
{
	struct iovec *iov;
	size_t cnt;
	int error, newflags, save;

	error = 0;

	KASSERT(uio->uio_rw == UIO_READ || uio->uio_rw == UIO_WRITE,
	    ("uiomove: mode"));
	KASSERT(uio->uio_segflg != UIO_USERSPACE || uio->uio_td == curthread,
	    ("uiomove proc"));
	KASSERT(uio->uio_resid >= 0,
	    ("%s: uio %p resid underflow", __func__, uio));

	if (uio->uio_segflg == UIO_USERSPACE) {
		newflags = TDP_DEADLKTREAT;
		if (nofault) {
			/*
			 * Fail if a non-spurious page fault occurs.
			 */
			newflags |= TDP_NOFAULTING | TDP_RESETSPUR;
		} else {
			WITNESS_WARN(WARN_GIANTOK | WARN_SLEEPOK, NULL,
			    "Calling uiomove()");
		}
		save = curthread_pflags_set(newflags);
	} else {
		KASSERT(nofault == 0, ("uiomove: nofault"));
		save = ~0;
	}

	while (n > 0 && uio->uio_resid) {
		KASSERT(uio->uio_iovcnt > 0,
		    ("%s: uio %p iovcnt underflow", __func__, uio));

		iov = uio->uio_iov;
		cnt = iov->iov_len;
		if (cnt == 0) {
			uio->uio_iov++;
			uio->uio_iovcnt--;
			continue;
		}
		if (cnt > n)
			cnt = n;

		error = uiomove_step(cp, iov->iov_base, cnt, uio);
		if (error != 0)
			goto out;
		iov->iov_base = (char *)iov->iov_base + cnt;
		iov->iov_len -= cnt;
		uio->uio_resid -= cnt;
		uio->uio_offset += cnt;
		cp = (char *)cp + cnt;
		n -= cnt;
	}
out:
	curthread_pflags_restore(save);
	return (error);
}

/*
 * Wrapper for uiomove() that validates the arguments against a known-good
 * kernel buffer.  Currently, uiomove accepts a signed (n) argument, which
 * is almost definitely a bad thing, so we catch that here as well.  We
 * return a runtime failure, but it might be desirable to generate a runtime
 * assertion failure instead.
 */
int
uiomove_frombuf(void *buf, int buflen, struct uio *uio)
{
	size_t offset, n;

	if (uio->uio_offset < 0 || uio->uio_resid < 0 ||
	    (offset = uio->uio_offset) != uio->uio_offset)
		return (EINVAL);
	if (buflen <= 0 || offset >= buflen)
		return (0);
	if ((n = buflen - offset) > IOSIZE_MAX)
		return (EINVAL);
	return (uiomove((char *)buf + offset, n, uio));
}
