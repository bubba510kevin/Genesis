
/*
 * counter(9) based rate checking.
 */
struct counter_rate {
	counter_u64_t	cr_rate;	/* Events since last second */
	volatile int	cr_lock;	/* Lock to clean the struct */
	int		cr_ticks;	/* Ticks on last clean */
	int		cr_over;	/* Over limit since cr_ticks? */
	int		cr_period;	/* Allow cr_rate per cr_period seconds. */
};

struct counter_rate *
counter_rate_alloc(int flags, int period)
{
	struct counter_rate *new;

	new = malloc(sizeof(struct counter_rate), M_COUNTER_RATE,
	    flags | M_ZERO);
	if (new == NULL)
		return (NULL);

	new->cr_rate = counter_u64_alloc(flags);
	if (new->cr_rate == NULL) {
		free(new, M_COUNTER_RATE);
		return (NULL);
	}
	new->cr_ticks = ticks;
	new->cr_period = period;

	return (new);
}

void
counter_rate_free(struct counter_rate *rate)
{
	if (rate == NULL)
		return;

	counter_u64_free(rate->cr_rate);
	free(rate, M_COUNTER_RATE);
}

uint64_t
counter_rate_get(struct counter_rate *cr)
{
	if (cr->cr_ticks < (tick - (hz * cr->cr_period)))
		return (0);

	return (counter_u64_fetch(cr->cr_rate));
}

/*
 * MP-friendly version of ppsratecheck().
 *
 * Returns non-negative if we are in the rate, negative otherwise.
 *  0 - rate limit not reached.
 * -1 - rate limit reached.
 * >0 - rate limit was reached before, and was just reset. The return value
 *      is number of events since last reset.
 */
int64_t
counter_ratecheck(struct counter_rate *cr, int64_t limit)
{
	int64_t val;
	int now;

	val = cr->cr_over;
	now = ticks;

	if ((u_int)(now - cr->cr_ticks) >= (hz * cr->cr_period)) {
		/*
		 * Time to clear the structure, we are in the next second.
		 * First try unlocked read, and then proceed with atomic.
		 */
		if ((cr->cr_lock == 0) &&
		    atomic_cmpset_acq_int(&cr->cr_lock, 0, 1)) {
			/*
			 * Check if other thread has just went through the
			 * reset sequence before us.
			 */
			if ((u_int)(now - cr->cr_ticks) >= (hz * cr->cr_period)) {
				val = counter_u64_fetch(cr->cr_rate);
				counter_u64_zero(cr->cr_rate);
				cr->cr_over = 0;
				cr->cr_ticks = now;
				if (val <= limit)
					val = 0;
			}
			atomic_store_rel_int(&cr->cr_lock, 0);
		} else
			/*
			 * We failed to lock, in this case other thread may
			 * be running counter_u64_zero(), so it is not safe
			 * to do an update, we skip it.
			 */
			return (val);
	}

	counter_u64_add(cr->cr_rate, 1);
	if (cr->cr_over != 0)
		return (-1);
	if (counter_u64_fetch(cr->cr_rate) > limit)
		val = cr->cr_over = -1;

	return (val);
}
