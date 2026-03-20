// SPDX-License-Identifier: GPL-2.0+
/*
 * Broadcom BCM6846 MDIO bus driver for U-Boot
 * Genexis XG6846B (BCM68460 SoC)
 *
 * MDIO controller: PERF_PHYS_BASE + 0x2060 = 0xff802060
 * Supports Clause 22 (PHYs) and Clause 45 (88E6320 indirect access).
 *
 * Fits the U-Boot DM MDIO framework (UCLASS_MDIO).
 *
 * Copyright (C) 2024 - Genexis XG6846B port
 */

#include <common.h>
#include <dm.h>
#include <linux/delay.h>
#include <miiphy.h>
#include <phy.h>

/* Register offsets */
#define MDIO_CMD	0x00
#define MDIO_CFG	0x04

/* CMD fields */
#define MDIO_CMD_DATA_MASK	0x0000ffff
#define MDIO_CMD_REG_SHIFT	16
#define MDIO_CMD_PHY_SHIFT	21
#define MDIO_CMD_OP_SHIFT	26
#define MDIO_CMD_FAIL		BIT(28)
#define MDIO_CMD_BUSY		BIT(29)

/* Clause 22 ops */
#define C22_WRITE	1
#define C22_READ	2

/* Clause 45 ops */
#define C45_ADDR	0
#define C45_WRITE	1
#define C45_READ	3

/* CFG fields */
#define MDIO_CFG_CLAUSE		BIT(0)	/* 1 = C22 */
#define MDIO_CFG_CLK_SHIFT	4
#define MDIO_CFG_FREE_CLK	BIT(12)

#define MDIO_RETRIES		1000
#define MDIO_DEFAULT_CLK	12

struct bcm6846_mdio_priv {
	void __iomem *base;
};

static int bcm6846_mdio_wait(struct bcm6846_mdio_priv *p)
{
	u32 v;
	int i;

	for (i = 0; i < MDIO_RETRIES; i++) {
		v = readl(p->base + MDIO_CMD);
		if (!(v & MDIO_CMD_BUSY))
			return (v & MDIO_CMD_FAIL) ? -EIO : 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void bcm6846_mdio_clause(struct bcm6846_mdio_priv *p, int c22)
{
	u32 cfg = readl(p->base + MDIO_CFG);

	if (c22)
		cfg |= MDIO_CFG_CLAUSE;
	else
		cfg &= ~MDIO_CFG_CLAUSE;
	writel(cfg, p->base + MDIO_CFG);
}

static int bcm6846_mdio_read(struct udevice *dev, int addr, int devad, int reg)
{
	struct bcm6846_mdio_priv *p = dev_get_priv(dev);
	u32 cmd;
	int rc;

	if (devad == MDIO_DEVAD_NONE) {
		/* Clause 22 */
		bcm6846_mdio_clause(p, 1);
		cmd = MDIO_CMD_BUSY |
		      (C22_READ << MDIO_CMD_OP_SHIFT) |
		      ((addr & 0x1f) << MDIO_CMD_PHY_SHIFT) |
		      ((reg  & 0x1f) << MDIO_CMD_REG_SHIFT);
		writel(cmd, p->base + MDIO_CMD);
		rc = bcm6846_mdio_wait(p);
		if (rc)
			return rc;
		return readl(p->base + MDIO_CMD) & MDIO_CMD_DATA_MASK;
	}

	/* Clause 45 */
	bcm6846_mdio_clause(p, 0);

	/* Address cycle */
	cmd = MDIO_CMD_BUSY |
	      (C45_ADDR << MDIO_CMD_OP_SHIFT) |
	      ((addr  & 0x1f) << MDIO_CMD_PHY_SHIFT) |
	      ((devad & 0x1f) << MDIO_CMD_REG_SHIFT) |
	      (reg & 0xffff);
	writel(cmd, p->base + MDIO_CMD);
	rc = bcm6846_mdio_wait(p);
	if (rc)
		return rc;

	/* Read cycle */
	cmd = MDIO_CMD_BUSY |
	      (C45_READ << MDIO_CMD_OP_SHIFT) |
	      ((addr  & 0x1f) << MDIO_CMD_PHY_SHIFT) |
	      ((devad & 0x1f) << MDIO_CMD_REG_SHIFT);
	writel(cmd, p->base + MDIO_CMD);
	rc = bcm6846_mdio_wait(p);
	if (rc)
		return rc;

	return readl(p->base + MDIO_CMD) & MDIO_CMD_DATA_MASK;
}

static int bcm6846_mdio_write(struct udevice *dev, int addr, int devad,
			      int reg, u16 val)
{
	struct bcm6846_mdio_priv *p = dev_get_priv(dev);
	u32 cmd;
	int rc;

	if (devad == MDIO_DEVAD_NONE) {
		/* Clause 22 */
		bcm6846_mdio_clause(p, 1);
		cmd = MDIO_CMD_BUSY |
		      (C22_WRITE << MDIO_CMD_OP_SHIFT) |
		      ((addr & 0x1f) << MDIO_CMD_PHY_SHIFT) |
		      ((reg  & 0x1f) << MDIO_CMD_REG_SHIFT) |
		      val;
		writel(cmd, p->base + MDIO_CMD);
		return bcm6846_mdio_wait(p);
	}

	/* Clause 45 address */
	bcm6846_mdio_clause(p, 0);
	cmd = MDIO_CMD_BUSY |
	      (C45_ADDR << MDIO_CMD_OP_SHIFT) |
	      ((addr  & 0x1f) << MDIO_CMD_PHY_SHIFT) |
	      ((devad & 0x1f) << MDIO_CMD_REG_SHIFT) |
	      (reg & 0xffff);
	writel(cmd, p->base + MDIO_CMD);
	rc = bcm6846_mdio_wait(p);
	if (rc)
		return rc;

	/* Clause 45 write */
	cmd = MDIO_CMD_BUSY |
	      (C45_WRITE << MDIO_CMD_OP_SHIFT) |
	      ((addr  & 0x1f) << MDIO_CMD_PHY_SHIFT) |
	      ((devad & 0x1f) << MDIO_CMD_REG_SHIFT) |
	      val;
	writel(cmd, p->base + MDIO_CMD);
	return bcm6846_mdio_wait(p);
}

static int bcm6846_mdio_probe(struct udevice *dev)
{
	struct bcm6846_mdio_priv *p = dev_get_priv(dev);
	u32 clk_div, cfg;

	p->base = dev_remap_addr(dev);
	if (!p->base)
		return -EINVAL;

	clk_div = dev_read_u32_default(dev, "clock-divider", MDIO_DEFAULT_CLK);

	cfg = readl(p->base + MDIO_CFG);
	cfg |= MDIO_CFG_FREE_CLK;
	cfg &= ~(0xff << MDIO_CFG_CLK_SHIFT);
	cfg |= (clk_div & 0xff) << MDIO_CFG_CLK_SHIFT;
	writel(cfg, p->base + MDIO_CFG);

	printf("BCM6846 MDIO: base=%p clk_div=%u\n", p->base, clk_div);
	return 0;
}

static const struct mdio_ops bcm6846_mdio_ops = {
	.read  = bcm6846_mdio_read,
	.write = bcm6846_mdio_write,
};

static const struct udevice_id bcm6846_mdio_ids[] = {
	{ .compatible = "brcm,bcm6846-mdio" },
	{ }
};

U_BOOT_DRIVER(bcm6846_mdio) = {
	.name     = "bcm6846-mdio",
	.id       = UCLASS_MDIO,
	.of_match = bcm6846_mdio_ids,
	.probe    = bcm6846_mdio_probe,
	.ops      = &bcm6846_mdio_ops,
	.priv_auto = sizeof(struct bcm6846_mdio_priv),
};
