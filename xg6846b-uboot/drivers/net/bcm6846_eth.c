// SPDX-License-Identifier: GPL-2.0+
/*
 * Broadcom BCM6846 XRDP Ethernet driver for U-Boot
 * Genexis XG6846B (BCM68460 SoC)
 *
 * This driver implements the U-Boot DM ethernet ops using:
 *   - BCM6846 UniMAC for MAC-layer configuration
 *   - BCM6846 RGMII pads connected to 88E6320 CPU port (P6)
 *   - XRDP Runner data-path for TX/RX via SBPM packet buffers
 *
 * TX path:  eth_send() → SBPM alloc → copy → RDD BBH TX → Runner wakeup
 * RX path:  eth_recv() → poll SRAM PD FIFO → SBPM copy → free → deliver
 *
 * The XRDP data-path register init sequence (data_path_6846.c) must be
 * replayed before this driver can move traffic; call xrdp_data_path_init()
 * once during board_init_r().
 *
 * BCM6846-specific constants:
 *   SBPM_ADDRS           = 0x82d98000
 *   PSRAM_MEM_ADDRS      = 0x82600000
 *   SBPM_MAX_BUF_NUM     = 0x5FF  (1536 buffers × 128 B each)
 *   RNR_REGS_ADDR_CORE0  = 0x82d00000
 *   BB_ID_TX_LAN         = 32
 *
 * Copyright (C) 2024 - Genexis XG6846B port
 */

#include <common.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <env.h>
#include <linux/delay.h>
#include <malloc.h>
#include <miiphy.h>
#include <net.h>
#include <phy.h>

/* ============================================================
 * BCM6846 XRDP / SBPM constants
 * ============================================================ */
#define BCM6846_SBPM_BASE		0x82d98000UL
#define BCM6846_PSRAM_BASE		0x82600000UL
#define BCM6846_RNR_CORE0_BASE		0x82d00000UL
#define BCM6846_SBPM_MAX_BN		0x5ff
#define BCM6846_SBPM_BUF_SIZE		128
#define BCM6846_SBPM_HEADROOM		18

/* SBPM register offsets */
#define SBPM_BN_ALLOC_REG		0x0004
#define SBPM_BN_ALLOC_RPLY_REG		0x0008
#define SBPM_BN_CONNECT_REG		0x001c
#define SBPM_BN_CONNECT_RPLY_REG	0x0020
#define SBPM_GET_NEXT_REG		0x0024
#define SBPM_GET_NEXT_RPLY_REG		0x0028
#define SBPM_BN_FREE_REG		0x0038
#define SBPM_BN_FREE_RPLY_REG		0x003c
#define SBPM_INVALID_BN			0x3fff

/* Runner SRAM layout offsets (from RNR_CORE0_BASE) */
#define RNR_BBH_TX_RING_OFF		0x0050
#define RNR_BB_DEST_TABLE_OFF		0x00dc
#define RNR_BBH_TX_EGRESS_CTR_OFF	0x0180
#define RNR_SRAM_PD_FIFO_OFF		0x0800
#define RNR_SRAM_PD_FIFO_SIZE		64	/* 64 × 4 × u32 descriptors */
#define RNR_CPU_TX_THREAD		1
#define RNR_REGS_CPU_WAKEUP_OFF		0x0004

/* BBH TX descriptor field positions (32-bit words 0..3) */
#define BBH_TX_W0_PKT_LEN_SHIFT		0
#define BBH_TX_W0_PKT_LEN_MASK		0x00003fff
#define BBH_TX_W0_ABS			BIT(14)
#define BBH_TX_W0_LAST			BIT(15)
#define BBH_TX_W0_TGT_MEM_0		BIT(16)
#define BBH_TX_W0_TGT_MEM_1		BIT(17)
#define BBH_TX_W0_BN0_SHIFT		18
#define BBH_TX_W0_BN0_MASK		0xfffc0000
#define BBH_TX_W1_BN1_SHIFT		0
#define BBH_TX_W1_BN1_MASK		0x0003ffff

/* BB destination ID for LAN TX on BCM6846 */
#define BB_ID_TX_LAN			32

/* CPU RX descriptor (SBPM path) */
#define CPU_RX_W0_BN0_SHIFT		14
#define CPU_RX_W0_BN0_MASK		0xfffc000
#define CPU_RX_W1_PKT_LEN_SHIFT		0
#define CPU_RX_W1_PKT_LEN_MASK		0x00003fff
#define CPU_RX_W2_DATA_OFFSET_SHIFT	6
#define CPU_RX_W2_DATA_OFFSET_MASK	0x00001fc0
#define CPU_RX_W2_SRC_PORT_SHIFT	27
#define CPU_RX_W2_SRC_PORT_MASK		0xf8000000

/* ============================================================
 * BCM6846 UniMAC registers (EMAC0 at 0x82da0000)
 * ============================================================ */
#define BCM6846_UNIMAC_BASE		0x82da0000UL
#define UMAC_CMD			0x008
#define  CMD_TX_EN			BIT(0)
#define  CMD_RX_EN			BIT(1)
#define  CMD_SPEED_SHIFT		2
#define  CMD_SPEED_1000			2
#define  CMD_PAD_EN			BIT(5)
#define  CMD_CRC_FWD			BIT(6)
#define  CMD_SW_RESET			BIT(13)
#define UMAC_MAC0			0x00c
#define UMAC_MAC1			0x010
#define UMAC_MAX_FRAME_LEN		0x014
#define UMAC_TX_IPG_LEN			0x05c

/* ============================================================
 * BCM6846 RGMII control (at 0x82d97300)
 * ============================================================ */
#define BCM6846_RGMII_BASE		0x82d97300UL
#define RGMII_CTRL			0x00
#define  RGMII_MODE_EN			BIT(0)
#define  RGMII_ID_MODE_DIS		BIT(1)
#define  RGMII_PORT_1000		(2 << 2)

/* ============================================================
 * 88E6320 switch addresses (via BCM6846 MDIO)
 * CPU port P6 is fixed-link 1G to BCM6846 UniMAC0
 * ============================================================ */
#define SW_PORT_BASE			0x10	/* MDIO addr of port 0 */
#define SW_CPU_PORT			6
#define SW_G1_ADDR			0x1b
#define SW_G2_ADDR			0x1c

/* Port control reg */
#define SW_PORT_CTRL_REG		0x04
#define  SW_PORT_FWD			3
#define  SW_PORT_DISABLED		0
#define  SW_PORT_EGRESS_UNMOD		(0 << 12)
#define  SW_PORT_EGRESS_UNTAGGED	(2 << 12)

/* Global1 */
#define SW_G1_STATUS_REG		0x00
#define  SW_G1_PPU_RDY			BIT(15)
#define SW_G1_CTRL_REG			0x04
#define  SW_G1_SW_RESET			BIT(15)
#define  SW_G1_PPU_EN			BIT(14)

/* Global2 SMI proxy */
#define SW_G2_SMI_CMD_REG		0x18
#define SW_G2_SMI_DATA_REG		0x19
#define  G2_SMI_BUSY			BIT(15)
#define  G2_SMI_C22			BIT(12)
#define  G2_SMI_C22_WRITE		(1 << 10)
#define  G2_SMI_C22_READ		(2 << 10)

/* LAN port list */
static const int sw_lan_ports[] = { 0, 1, 4 };

/* ============================================================
 * Driver private state
 * ============================================================ */
#define RX_BUF_SIZE	2048

struct bcm6846_eth_priv {
	/* Hardware bases (direct-mapped, no MMU in U-Boot) */
	volatile u32	*sbpm;
	volatile u32	*rnr;
	volatile u32	*umac;
	volatile u32	*rgmii;

	/* MDIO bus for switch management */
	struct udevice	*mdio_dev;

	/* RX packet buffer (reused across poll calls) */
	u8		rx_buf[RX_BUF_SIZE + BCM6846_SBPM_HEADROOM];

	/* TX BBH FIFO tracking */
	u8		bbh_ingress[16];

	/* Runner SRAM PD FIFO read index */
	int		pd_rd_idx;

	int		active_port;	/* switch port to send/recv on */
};

/* ============================================================
 * SBPM helpers
 * ============================================================ */
static int sbpm_wait_alloc(volatile u32 *sbpm, u16 *bn)
{
	u32 rply;
	int i;

	for (i = 0; i < 1000; i++) {
		rply = sbpm[SBPM_BN_ALLOC_RPLY_REG / 4];
		if (rply & BIT(31)) {
			if (!(rply & BIT(0)))
				return -ENOMEM;
			*bn = (rply >> 1) & 0x3fff;
			return 0;
		}
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int sbpm_alloc(volatile u32 *sbpm, u16 *bn)
{
	/* Trigger allocation (SA=0) */
	sbpm[SBPM_BN_ALLOC_REG / 4] = 0;
	return sbpm_wait_alloc(sbpm, bn);
}

static int sbpm_connect(volatile u32 *sbpm, u16 head, u16 next)
{
	u32 rply;
	int i;

	/* Connect head → next, request ack */
	sbpm[SBPM_BN_CONNECT_REG / 4] = head | BIT(14) | BIT(15) | (next << 16);

	for (i = 0; i < 1000; i++) {
		rply = sbpm[SBPM_BN_CONNECT_RPLY_REG / 4];
		if (rply & BIT(31))
			return (rply & BIT(0)) ? 0 : -EIO;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void sbpm_free(volatile u32 *sbpm, u16 head)
{
	/* Free without context, no ack needed */
	sbpm[SBPM_BN_FREE_REG / 4] = head;
}

/*
 * Write packet data into SBPM buffers.
 * Each buffer holds BCM6846_SBPM_BUF_SIZE bytes.
 * Returns number of BNs allocated, fills bn0/bn1.
 */
static int sbpm_write_packet(volatile u32 *sbpm, const u8 *data, int len,
			     u16 *bn0_out, u16 *bn1_out, u8 *num_bns)
{
	u32 psram_stride = BCM6846_SBPM_BUF_SIZE;
	u8 *psram = (u8 *)BCM6846_PSRAM_BASE;
	u16 bn_prev = SBPM_INVALID_BN, bn_curr;
	u16 first_bn = SBPM_INVALID_BN;
	int offset = 0;
	int buffers = 0;
	int chunk, rc;

	while (offset < len) {
		rc = sbpm_alloc(sbpm, &bn_curr);
		if (rc) {
			if (first_bn != SBPM_INVALID_BN)
				sbpm_free(sbpm, first_bn);
			return rc;
		}

		chunk = min(len - offset, (int)psram_stride);
		memcpy(psram + bn_curr * psram_stride, data + offset, chunk);
		flush_dcache_range(
			(ulong)(psram + bn_curr * psram_stride),
			(ulong)(psram + bn_curr * psram_stride + chunk));

		if (first_bn == SBPM_INVALID_BN) {
			first_bn = bn_curr;
			*bn0_out = bn_curr;
		} else {
			sbpm_connect(sbpm, bn_prev, bn_curr);
		}

		if (buffers == 1)
			*bn1_out = bn_curr;

		bn_prev = bn_curr;
		offset += chunk;
		buffers++;
	}

	*num_bns = (u8)buffers;
	return 0;
}

/*
 * Read packet from SBPM linked list into dst.
 * Returns packet length.
 */
static int sbpm_read_packet(volatile u32 *sbpm, u16 head_bn, u8 *dst,
			    int max_len)
{
	u8 *psram = (u8 *)BCM6846_PSRAM_BASE;
	u32 psram_stride = BCM6846_SBPM_BUF_SIZE;
	u32 rply;
	u16 bn = head_bn;
	int copied = 0;

	while (bn != SBPM_INVALID_BN && copied < max_len) {
		int chunk = min(max_len - copied, (int)psram_stride);
		u8 *src = psram + bn * psram_stride;

		invalidate_dcache_range((ulong)src, (ulong)(src + chunk));
		memcpy(dst + copied, src, chunk);
		copied += chunk;

		/* Get next BN in chain */
		sbpm[SBPM_GET_NEXT_REG / 4] = bn;
		udelay(2);
		rply = sbpm[SBPM_GET_NEXT_RPLY_REG / 4];
		if (!(rply & BIT(0)) || (rply & BIT(3))) /* bn_null set */
			break;
		bn = (rply >> 4) & 0x3fff;
	}

	return copied;
}

/* ============================================================
 * Runner helpers
 * ============================================================ */
static void rnr_cpu_wakeup(volatile u32 *rnr)
{
	rnr[RNR_REGS_CPU_WAKEUP_OFF / 4] = RNR_CPU_TX_THREAD;
}

static int rnr_tx_poll(volatile u32 *rnr, u8 tx_port,
		       u8 *ingress_counter, int timeout_us)
{
	volatile u32 *tx_pd = rnr + RNR_BBH_TX_RING_OFF / 4;
	volatile u8  *egr_ctr = (volatile u8 *)(rnr + RNR_BBH_TX_EGRESS_CTR_OFF / 4)
				 + tx_port;
	u32 pkt_len;
	int i;

	for (i = 0; i < timeout_us; i++) {
		pkt_len = *tx_pd & BBH_TX_W0_PKT_LEN_MASK;
		if (pkt_len == 0 &&
		    (ingress_counter[tx_port] - *egr_ctr) < 8)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* ============================================================
 * 88E6320 switch management
 * ============================================================ */
static int sw_mdio_rd(struct udevice *mdio, int port_off, int reg)
{
	return dm_mdio_read(mdio, SW_PORT_BASE + port_off,
			    MDIO_DEVAD_NONE, reg);
}

static int sw_mdio_wr(struct udevice *mdio, int port_off, int reg, u16 val)
{
	return dm_mdio_write(mdio, SW_PORT_BASE + port_off,
			     MDIO_DEVAD_NONE, reg, val);
}

/*
 * Wait for 88E6320 PPU (Polling PHY Unit) to become ready after reset.
 * PPU ready = Global1 status bit[15] set.
 */
static int sw_wait_ppu(struct udevice *mdio)
{
	int i, v;

	for (i = 0; i < 100; i++) {
		v = sw_mdio_rd(mdio, SW_G1_ADDR - SW_PORT_BASE,
			       SW_G1_STATUS_REG);
		if (v >= 0 && (v & SW_G1_PPU_RDY))
			return 0;
		mdelay(10);
	}
	return -ETIMEDOUT;
}

static int sw_global2_phy_wait(struct udevice *mdio)
{
	int v, i;

	for (i = 0; i < 100; i++) {
		v = sw_mdio_rd(mdio, SW_G2_ADDR - SW_PORT_BASE,
			       SW_G2_SMI_CMD_REG);
		if (v >= 0 && !(v & G2_SMI_BUSY))
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

/* Indirect PHY write via Global2 SMI proxy */
static int sw_phy_write(struct udevice *mdio, int phy, int reg, u16 val)
{
	int rc;

	rc = sw_global2_phy_wait(mdio);
	if (rc)
		return rc;

	sw_mdio_wr(mdio, SW_G2_ADDR - SW_PORT_BASE, SW_G2_SMI_DATA_REG, val);

	return sw_mdio_wr(mdio, SW_G2_ADDR - SW_PORT_BASE, SW_G2_SMI_CMD_REG,
			  G2_SMI_BUSY | G2_SMI_C22 | G2_SMI_C22_WRITE |
			  ((phy & 0x1f) << 5) | (reg & 0x1f));
}

static int sw_init(struct udevice *mdio)
{
	int i, rc;

	printf("88E6320: resetting switch\n");

	/* Software reset via Global1 */
	sw_mdio_wr(mdio, SW_G1_ADDR - SW_PORT_BASE,
		   SW_G1_CTRL_REG, SW_G1_SW_RESET);

	mdelay(20);

	rc = sw_wait_ppu(mdio);
	if (rc) {
		printf("88E6320: PPU not ready after reset\n");
		return rc;
	}

	/* Enable PPU */
	sw_mdio_wr(mdio, SW_G1_ADDR - SW_PORT_BASE,
		   SW_G1_CTRL_REG, SW_G1_PPU_EN);

	/* LAN ports: forwarding, egress untagged */
	for (i = 0; i < ARRAY_SIZE(sw_lan_ports); i++) {
		int p = sw_lan_ports[i];
		u16 ctrl = SW_PORT_FWD | SW_PORT_EGRESS_UNTAGGED;

		sw_mdio_wr(mdio, p, SW_PORT_CTRL_REG, ctrl);
		/* Enable auto-neg for internal PHYs: write MII ctrl reg */
		sw_phy_write(mdio, p, 0, 0x1200); /* AN enable + restart */
		printf("88E6320: LAN port %d forwarding\n", p);
	}

	/* CPU port P6: forwarding, egress unmodified (for DSA-tag-free path) */
	sw_mdio_wr(mdio, SW_CPU_PORT, SW_PORT_CTRL_REG,
		   SW_PORT_FWD | SW_PORT_EGRESS_UNMOD);

	/* Disable unused ports P2, P3, P5 */
	for (i = 2; i <= 5; i++) {
		if (i == 4)
			continue;
		sw_mdio_wr(mdio, i, SW_PORT_CTRL_REG, SW_PORT_DISABLED);
	}

	printf("88E6320: switch init complete\n");
	return 0;
}

/* ============================================================
 * UniMAC + RGMII init
 * ============================================================ */
static void unimac_init(volatile u32 *umac, volatile u32 *rgmii,
			const u8 *mac_addr)
{
	u32 cmd;

	/* Reset */
	cmd = readl(umac + UMAC_CMD / 4);
	writel(cmd | CMD_SW_RESET, umac + UMAC_CMD / 4);
	udelay(10);
	writel(cmd & ~CMD_SW_RESET, umac + UMAC_CMD / 4);
	udelay(10);

	/* MAC address */
	writel((mac_addr[0] << 24) | (mac_addr[1] << 16) |
	       (mac_addr[2] << 8)  |  mac_addr[3],
	       umac + UMAC_MAC0 / 4);
	writel((mac_addr[4] << 8) | mac_addr[5],
	       umac + UMAC_MAC1 / 4);

	/* 1G, full-duplex, pad enable, CRC forward */
	writel((CMD_SPEED_1000 << CMD_SPEED_SHIFT) | CMD_PAD_EN | CMD_CRC_FWD,
	       umac + UMAC_CMD / 4);

	/* IPG and max frame */
	writel(12, umac + UMAC_TX_IPG_LEN / 4);
	writel(1522 + 8, umac + UMAC_MAX_FRAME_LEN / 4);

	/* RGMII: enable, 1G mode, TX-side internal delay (TXID) */
	writel(RGMII_MODE_EN | RGMII_PORT_1000, rgmii + RGMII_CTRL / 4);

	printf("UniMAC0: init done (MAC %pM)\n", mac_addr);
}

static void unimac_set_txrx(volatile u32 *umac, int enable)
{
	u32 cmd = readl(umac + UMAC_CMD / 4);

	if (enable)
		cmd |= CMD_TX_EN | CMD_RX_EN;
	else
		cmd &= ~(CMD_TX_EN | CMD_RX_EN);

	writel(cmd, umac + UMAC_CMD / 4);
}

/* ============================================================
 * XRDP data-path init (links to existing SDK file)
 * ============================================================ */
extern int xrdp_data_path_init(void);

/* ============================================================
 * U-Boot eth_ops
 * ============================================================ */
static int bcm6846_eth_start(struct udevice *dev)
{
	struct bcm6846_eth_priv *priv = dev_get_priv(dev);
	struct eth_pdata *pdata = dev_get_plat(dev);

	/* XRDP Runner data-path (idempotent) */
	if (xrdp_data_path_init()) {
		dev_err(dev, "XRDP data path init failed\n");
		return -EIO;
	}

	/* UniMAC + RGMII */
	unimac_init(priv->umac, priv->rgmii, pdata->enetaddr);

	/* Switch */
	if (sw_init(priv->mdio_dev))
		dev_warn(dev, "Switch init failed - continuing\n");

	/* Enable MAC RX/TX */
	unimac_set_txrx(priv->umac, 1);

	/* Reset SRAM PD FIFO read index */
	priv->pd_rd_idx = 0;

	printf("BCM6846: eth started, port=%d\n", priv->active_port);
	return 0;
}

static void bcm6846_eth_stop(struct udevice *dev)
{
	struct bcm6846_eth_priv *priv = dev_get_priv(dev);

	unimac_set_txrx(priv->umac, 0);
}

static int bcm6846_eth_send(struct udevice *dev, void *packet, int length)
{
	struct bcm6846_eth_priv *priv = dev_get_priv(dev);
	volatile u32 *rnr  = priv->rnr;
	volatile u32 *sbpm = priv->sbpm;
	volatile u32 *tx_pd, *bb_dest;
	u16 bn0 = 0, bn1 = 0;
	u8  bns_num = 0;
	u32 bb_id;
	int rc;

	if (length < 60)
		length = 60;

	rc = rnr_tx_poll(rnr, priv->active_port, priv->bbh_ingress, 2000);
	if (rc) {
		dev_warn(dev, "TX BBH not ready\n");
		return rc;
	}

	rc = sbpm_write_packet(sbpm, (u8 *)packet, length,
			       &bn0, &bn1, &bns_num);
	if (rc) {
		dev_err(dev, "SBPM alloc failed\n");
		return rc;
	}

	tx_pd   = rnr + RNR_BBH_TX_RING_OFF / 4;
	bb_dest = rnr + RNR_BB_DEST_TABLE_OFF / 4;

	/* Build TX PD (word 0 and 1) */
	memset((void *)tx_pd, 0, 32);
	/* word 0: packet length, LAST=1, ABS=0, TGT_MEM=1, bn0 in bits[31:18] */
	tx_pd[0] = ((u32)length & BBH_TX_W0_PKT_LEN_MASK) |
		   BBH_TX_W0_LAST |
		   BBH_TX_W0_TGT_MEM_0 |
		   ((u32)bn0 << BBH_TX_W0_BN0_SHIFT);
	/* word 1: bn1 */
	tx_pd[1] = (u32)bn1;
	/* word 2: bn_num */
	tx_pd[2] = bns_num;

	wmb();

	/* Set BB destination for this port */
	bb_id = BB_ID_TX_LAN + (priv->active_port << 6);
	*bb_dest = bb_id;

	wmb();

	/* Wake up Runner CPU TX thread */
	rnr_cpu_wakeup(rnr);

	priv->bbh_ingress[priv->active_port]++;
	return 0;
}

static int bcm6846_eth_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct bcm6846_eth_priv *priv = dev_get_priv(dev);
	volatile u32 *rnr = priv->rnr;
	volatile u32 *fifo = rnr + RNR_SRAM_PD_FIFO_OFF / 4;
	volatile u32 *desc;
	u32 w0, w1, w2, w3;
	u16 bn0;
	u8  data_offset;
	int pkt_len;

	/* Poll SRAM PD FIFO */
	desc = fifo + priv->pd_rd_idx;

	invalidate_dcache_range((ulong)desc, (ulong)(desc + 4));

	w1 = desc[1];
	if (!w1)
		return -EAGAIN;

	w0 = desc[0];
	w2 = desc[2];
	w3 = desc[3];

	/* Clear descriptor to mark consumed */
	desc[3] = 0;
	desc[2] = 0;
	desc[0] = 0;
	desc[1] = 0;
	wmb();

	priv->pd_rd_idx = (priv->pd_rd_idx + 4) %
			  (RNR_SRAM_PD_FIFO_SIZE * 4);

	/* Extract fields */
	bn0        = (w0 >> CPU_RX_W0_BN0_SHIFT) & 0x3fff;
	pkt_len    = (w1 >> CPU_RX_W1_PKT_LEN_SHIFT) & CPU_RX_W1_PKT_LEN_MASK;
	data_offset = (w2 >> CPU_RX_W2_DATA_OFFSET_SHIFT) & 0x7f;

	if (!pkt_len || bn0 == SBPM_INVALID_BN)
		return -EAGAIN;

	if (pkt_len > RX_BUF_SIZE) {
		printf("BCM6846 RX: oversized frame %d\n", pkt_len);
		sbpm_free(priv->sbpm, bn0);
		return -EINVAL;
	}

	/* Copy packet out of SBPM */
	sbpm_read_packet(priv->sbpm, bn0,
			 priv->rx_buf + data_offset, pkt_len);
	sbpm_free(priv->sbpm, bn0);

	*packetp = priv->rx_buf + data_offset;
	return pkt_len;
}

static int bcm6846_eth_free_pkt(struct udevice *dev, uchar *packet, int length)
{
	/* RX buffer is internal — nothing to free */
	return 0;
}

static const struct eth_ops bcm6846_eth_ops = {
	.start    = bcm6846_eth_start,
	.stop     = bcm6846_eth_stop,
	.send     = bcm6846_eth_send,
	.recv     = bcm6846_eth_recv,
	.free_pkt = bcm6846_eth_free_pkt,
};

/* ============================================================
 * DM probe / of_to_plat
 * ============================================================ */
static int bcm6846_eth_of_to_plat(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	const char *mac;

	pdata->iobase = dev_read_addr(dev);

	mac = env_get("ethaddr");
	if (mac)
		string_to_enetaddr(mac, pdata->enetaddr);
	else
		net_random_ethaddr(pdata->enetaddr);

	return 0;
}

static int bcm6846_eth_probe(struct udevice *dev)
{
	struct bcm6846_eth_priv *priv = dev_get_priv(dev);
	struct udevice *mdio;
	int rc;

	/* Hardware bases — direct physical addresses work in U-Boot
	 * (no MMU, 1:1 mapping on BCM6846). */
	priv->sbpm  = (volatile u32 *)BCM6846_SBPM_BASE;
	priv->rnr   = (volatile u32 *)BCM6846_RNR_CORE0_BASE;
	priv->umac  = (volatile u32 *)BCM6846_UNIMAC_BASE;
	priv->rgmii = (volatile u32 *)BCM6846_RGMII_BASE;

	/* Active port: from DT or env, default 0 */
	priv->active_port = dev_read_u32_default(dev, "active-port", 0);

	/* Find the BCM6846 MDIO bus */
	rc = uclass_get_device_by_name(UCLASS_MDIO, "bcm6846-mdio", &mdio);
	if (rc) {
		dev_err(dev, "BCM6846 MDIO bus not found: %d\n", rc);
		return rc;
	}
	priv->mdio_dev = mdio;

	dev_info(dev, "BCM6846 Ethernet probed (active_port=%d)\n",
		 priv->active_port);
	return 0;
}

static const struct udevice_id bcm6846_eth_ids[] = {
	{ .compatible = "brcm,bcm6846-eth" },
	{ }
};

U_BOOT_DRIVER(bcm6846_eth) = {
	.name         = "bcm6846-eth",
	.id           = UCLASS_ETH,
	.of_match     = bcm6846_eth_ids,
	.of_to_plat   = bcm6846_eth_of_to_plat,
	.probe        = bcm6846_eth_probe,
	.ops          = &bcm6846_eth_ops,
	.priv_auto    = sizeof(struct bcm6846_eth_priv),
	.plat_auto    = sizeof(struct eth_pdata),
};
