#ifndef _DEV_MII_MIIVAR_H_
#define _DEV_MII_MIIVAR_H_

#include <sys/queue.h>
#include <sys/bus.h>
#include <net/if.h>
#include <net/if_media.h>

/* <dev/mii/miivar.h> - the MII PHY framework, as a MAC driver sees it.
 *
 * --- what mii actually is -------------------------------------------------
 * An Ethernet controller is two chips' worth of function: the MAC, which
 * moves frames, and the PHY, which drives the wire and negotiates speed and
 * duplex. They talk over a two-wire management bus (MDC/MDIO), and the PHY's
 * registers are standardised (IEEE 802.3 clause 22) even when the MAC is not.
 *
 * So FreeBSD splits them: a MAC driver implements three methods for reading
 * and writing PHY registers over whatever bit-banging its hardware needs,
 * and a SEPARATE per-PHY driver - attached as a child device - does the
 * actual negotiation. That is why if_rl.c has rl_miibus_readreg/writereg/
 * statchg and then never touches a PHY register itself.
 *
 * --- what is here and what is not -----------------------------------------
 * The MAC-facing half is real: the three methods dispatch, struct mii_data
 * has the three fields a MAC driver reads, and mii_attach/mii_mediachg/
 * mii_tick/mii_pollstat exist.
 *
 * The PHY-driver half is NOT here. There are no per-PHY drivers (upstream
 * ships about forty), and no generic autonegotiation state machine. So
 * mii_attach finds no PHY driver to attach and reports link state as
 * unknown. A MAC driver compiles, attaches, and runs; what it does not get
 * is a negotiated link.
 *
 * That is a real limitation and it is the honest boundary: the PHY drivers
 * are their own vendoring job (dev/mii/*phy.c, each a real driver), and
 * porting them needs this framework to exist first. This is that framework.
 */

/* The per-PHY driver's softc. There are no PHY drivers here (see the file
 * comment), so nothing ever allocates one - but a MAC driver walks the PHY
 * list to find its instance, so the type has to be complete and the list has
 * to exist and be empty. An incomplete type would be a compile error in
 * perfectly ordinary driver code. */
struct mii_softc {
    LIST_ENTRY(mii_softc) mii_list;   /* linkage on mii_data's mii_phys */
    int               mii_phy;      /* this PHY's MII address */
    int               mii_inst;
    int               mii_flags;
    uint32            mii_mpd_oui;
    uint32            mii_mpd_model;
    uint32            mii_mpd_rev;
    int               mii_anegticks;
    device_t          mii_dev;
};

/* How many one-second ticks to wait for autonegotiation before giving up
 * and retrying. Upstream's values; a driver sets mii_anegticks from one of
 * them according to link speed. */
#define MII_ANEGTICKS       5
#define MII_ANEGTICKS_GIGE  17

/* Reset one PHY. A KOBJ method on the PHY driver upstream; there are no PHY
 * drivers here, so it writes BMCR_RESET directly - which is clause 22 and
 * therefore correct on any PHY - through the MAC's own register methods. */
void PHY_RESET(struct mii_softc *sc);

/* What a MAC driver reads. The three fields if_rl.c touches are
 * mii_media, mii_media_active and mii_media_status - the ifmedia the PHY
 * layer owns, and the negotiated result. */
struct mii_data {
    struct ifmedia mii_media;      /* the media list, PHY-owned            */
    if_t           mii_ifp;        /* the interface this belongs to        */
    int            mii_media_active;  /* IFM_* of what is actually running */
    int            mii_media_status;  /* IFM_AVALID | IFM_ACTIVE etc.      */
    int            mii_instance;
    struct mii_softc *mii_phy;     /* the attached PHY, NULL if none       */
    /* Which device this belongs to. Upstream reaches the MAC through the
     * miibus child's parent; there is no child device here, so it is
     * recorded directly. Without it mii_tick had no way to read a PHY
     * register and every link check said DOWN. */
    device_t          mii_dev;
    /* Upstream's PHY list head - a LIST_HEAD, not a pointer, because driver
     * source calls LIST_FIRST on it. Always empty here. */
    LIST_HEAD(mii_listhead, mii_softc) mii_phys;
};

typedef struct mii_data mii_data_t;

/* ifm_change_cb_t and ifm_stat_cb_t - the callbacks a MAC driver passes to
 * mii_attach - are NOT declared here. <net/if_media.h>, included above, is
 * where upstream declares them and it is vendored verbatim; a second typedef
 * of the same names here is a conflicting declaration, not a harmless
 * duplicate, because that header spells them with struct ifnet * and this
 * one would spell them with if_t. They are the same type either way, which
 * is exactly why the compiler complaining about it is useful. */

#define MII_PHY_ANY     (-1)
#define MII_OFFSET_ANY  (-1)

/* BMSR_DEFCAPMASK - the capability mask mii_attach is given - comes from
 * <dev/mii/mii.h>, which is vendored verbatim and which driver source
 * includes alongside this file. Not repeated here. */

#define MIIF_NOISOLATE  0x0002
#define MIIF_DOPAUSE    0x0100
#define MIIF_FORCEANEG  0x0040

int  mii_attach(device_t dev, device_t *miibus, if_t ifp,
                ifm_change_cb_t ifmedia_upd, ifm_stat_cb_t ifmedia_sts,
                int capmask, int phyloc, int offloc, int flags);
void mii_detach(device_t miibus);

/* Poll the PHY. Called from a MAC driver's one-second tick. */
void mii_tick(struct mii_data *mii);
/* Push a media change from ifmedia down to the PHY. */
int  mii_mediachg(struct mii_data *mii);
/* Read the current media state up from the PHY. */
void mii_pollstat(struct mii_data *mii);

/* device_get_softc on the miibus child gives the struct mii_data. Upstream
 * spells this as a macro for exactly this reason. */
#define device_get_softc_mii(d) ((struct mii_data *)device_get_softc(d))

extern driver_t miibus_driver;

#endif
