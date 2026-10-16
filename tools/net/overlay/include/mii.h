#ifndef _MII_H_
#define _MII_H_

/** @file
 *
 * Media Independent Interface (IEEE 802.3 clause 22 / 28 / 40) register map
 *
 * Replacement written for the BASIC-OS port of iPXE.  The upstream file is a copy of Linux's
 * include/linux/mii.h, which is GPL-2.0-only (incompatible with GPLv3).  The register numbers and
 * bit positions below come from the IEEE 802.3 standard; only the macro names follow the usual
 * Linux/iPXE convention so that the unmodified drivers keep compiling.
 *
 * Copyright (C) 2026 BASIC-OS contributors.  GPL-2.0-or-later.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

/* Register numbers (clause 22 management registers, then common vendor extensions) */
#define MII_BMCR		0x00	/* basic mode control */
#define MII_BMSR		0x01	/* basic mode status */
#define MII_PHYSID1		0x02	/* PHY identifier, high part */
#define MII_PHYSID2		0x03	/* PHY identifier, low part */
#define MII_ADVERTISE		0x04	/* auto-negotiation advertisement */
#define MII_LPA			0x05	/* link partner ability */
#define MII_EXPANSION		0x06	/* auto-negotiation expansion */
#define MII_CTRL1000		0x09	/* 1000BASE-T control */
#define MII_STAT1000		0x0a	/* 1000BASE-T status */
#define MII_MMD_CTRL		0x0d	/* MMD access control */
#define MII_MMD_DATA		0x0e	/* MMD access data */
#define MII_ESTATUS		0x0f	/* extended status */
#define MII_DCOUNTER		0x12	/* disconnect counter */
#define MII_FCSCOUNTER		0x13	/* false carrier counter */
#define MII_NWAYTEST		0x14	/* N-way test */
#define MII_RERRCOUNTER		0x15	/* receive error counter */
#define MII_SREVISION		0x16	/* silicon revision */
#define MII_RESV1		0x17
#define MII_LBRERROR		0x18	/* loopback, receive, bypass error */
#define MII_PHYADDR		0x19	/* PHY address */
#define MII_RESV2		0x1a
#define MII_TPISTATUS		0x1b	/* 10 Mbps TPI status */
#define MII_NCONFIG		0x1c	/* network interface configuration */

/* Basic mode control register */
#define BMCR_RESV		0x003f
#define BMCR_SPEED1000		0x0040	/* speed select, MSB */
#define BMCR_CTST		0x0080	/* collision test */
#define BMCR_FULLDPLX		0x0100	/* full duplex */
#define BMCR_ANRESTART		0x0200	/* restart auto-negotiation */
#define BMCR_ISOLATE		0x0400	/* electrically isolate the PHY */
#define BMCR_PDOWN		0x0800	/* power down */
#define BMCR_ANENABLE		0x1000	/* auto-negotiation enable */
#define BMCR_SPEED100		0x2000	/* speed select, LSB */
#define BMCR_LOOPBACK		0x4000
#define BMCR_RESET		0x8000

/* Basic mode status register */
#define BMSR_ERCAP		0x0001	/* extended register capability */
#define BMSR_JCD		0x0002	/* jabber detected */
#define BMSR_LSTATUS		0x0004	/* link up */
#define BMSR_ANEGCAPABLE	0x0008
#define BMSR_RFAULT		0x0010	/* remote fault */
#define BMSR_ANEGCOMPLETE	0x0020
#define BMSR_RESV		0x00c0
#define BMSR_ESTATEN		0x0100	/* extended status present */
#define BMSR_100HALF2		0x0200
#define BMSR_100FULL2		0x0400
#define BMSR_10HALF		0x0800
#define BMSR_10FULL		0x1000
#define BMSR_100HALF		0x2000
#define BMSR_100FULL		0x4000
#define BMSR_100BASE4		0x8000

/* Auto-negotiation advertisement register */
#define ADVERTISE_SLCT		0x001f	/* selector field */
#define ADVERTISE_CSMA		0x0001	/* IEEE 802.3 selector */
#define ADVERTISE_10HALF	0x0020
#define ADVERTISE_1000XFULL	0x0020	/* 1000BASE-X variant of the same bit */
#define ADVERTISE_10FULL	0x0040
#define ADVERTISE_1000XHALF	0x0040
#define ADVERTISE_100HALF	0x0080
#define ADVERTISE_1000XPAUSE	0x0080
#define ADVERTISE_100FULL	0x0100
#define ADVERTISE_1000XPSE_ASYM	0x0100
#define ADVERTISE_100BASE4	0x0200
#define ADVERTISE_PAUSE_CAP	0x0400
#define ADVERTISE_PAUSE_ASYM	0x0800
#define ADVERTISE_RESV		0x1000
#define ADVERTISE_RFAULT	0x2000
#define ADVERTISE_LPACK		0x4000
#define ADVERTISE_NPAGE		0x8000
#define ADVERTISE_FULL		( ADVERTISE_100FULL | ADVERTISE_10FULL | ADVERTISE_CSMA )
#define ADVERTISE_ALL		( ADVERTISE_10HALF | ADVERTISE_10FULL | \
				  ADVERTISE_100HALF | ADVERTISE_100FULL )

/* Link partner ability register (same layout as the advertisement register) */
#define LPA_SLCT		0x001f
#define LPA_10HALF		0x0020
#define LPA_1000XFULL		0x0020
#define LPA_10FULL		0x0040
#define LPA_1000XHALF		0x0040
#define LPA_100HALF		0x0080
#define LPA_1000XPAUSE		0x0080
#define LPA_100FULL		0x0100
#define LPA_1000XPAUSE_ASYM	0x0100
#define LPA_100BASE4		0x0200
#define LPA_PAUSE_CAP		0x0400
#define LPA_PAUSE_ASYM		0x0800
#define LPA_RESV		0x1000
#define LPA_RFAULT		0x2000
#define LPA_LPACK		0x4000
#define LPA_NPAGE		0x8000
#define LPA_DUPLEX		( LPA_10FULL | LPA_100FULL )
#define LPA_100			( LPA_100FULL | LPA_100HALF | LPA_100BASE4 )

/* Auto-negotiation expansion register */
#define EXPANSION_NWAY		0x0001	/* link partner can auto-negotiate */
#define EXPANSION_LCWP		0x0002	/* new link code word received */
#define EXPANSION_ENABLENPAGE	0x0004	/* local next page able */
#define EXPANSION_NPCAPABLE	0x0008	/* link partner next page able */
#define EXPANSION_MFAULTS	0x0010	/* multiple faults detected */
#define EXPANSION_RESV		0xffe0

/* Extended status register */
#define ESTATUS_1000_THALF	0x1000
#define ESTATUS_1000_TFULL	0x2000

/* N-way test register */
#define NWAYTEST_RESV1		0x00ff
#define NWAYTEST_LOOPBACK	0x0100
#define NWAYTEST_RESV2		0xfe00

/* 1000BASE-T control register */
#define ADVERTISE_1000HALF	0x0100
#define ADVERTISE_1000FULL	0x0200

/* 1000BASE-T status register */
#define LPA_1000HALF		0x0400
#define LPA_1000FULL		0x0800
#define LPA_1000REMRXOK		0x1000
#define LPA_1000LOCALRXOK	0x2000

#include <ipxe/netdevice.h>

/* Estado de una interfaz MII (interfaz interna de los drivers). */
struct mii_if_info {
	int phy_id;
	int advertising;
	int phy_id_mask;
	int reg_num_mask;
	unsigned int full_duplex : 1;
	unsigned int force_media : 1;
	unsigned int supports_gmii : 1;
	struct net_device *dev;
	int (*mdio_read)(struct net_device *dev, int phy_id, int location);
	void (*mdio_write)(struct net_device *dev, int phy_id, int location, int val);
};

#endif /* _MII_H_ */
