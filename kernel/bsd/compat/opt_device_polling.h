/* opt_device_polling.h - GENERATED upstream, empty here. See opt_ifmedia.h.
 *
 * DEVICE_POLLING off is the right answer and not just the easy one: it
 * replaces interrupt-driven receive with a timer-driven poll, and there is
 * no polling loop in Genesis to register with. ether_poll_register returns
 * ENXIO for the same reason.
 */
