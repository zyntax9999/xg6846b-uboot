// SPDX-License-Identifier: GPL-2.0+
/*
 * Board initialisation for Genexis XG6846B
 * BCM68460 (BCM6846) SoC
 *
 * Execution sequence in U-Boot:
 *   board_init_f()  → lowlevel_init → spl_init → relocate
 *   board_init_r()  → board_init() → board_late_init()
 *                  → eth_initialize() → net ops
 *
 * Copyright (C) 2024 - Genexis XG6846B port
 */

#include <common.h>
#include <command.h>
#include <dm.h>
#include <env.h>
#include <init.h>
#include <linux/delay.h>
#include <malloc.h>
#include <miiphy.h>
#include <net.h>
#include <phy.h>
#include <asm/io.h>

/* ============================================================
 * BCM6846 register addresses (physical = virtual in U-Boot)
 * ============================================================ */
#define BCM6846_PERF_BASE		0xff800000UL
#define BCM6846_PERF1_BASE		0xff858000UL
#define BCM6846_XRDP_BASE		0x82000000UL

/* WAN_MISC inside XRDP */
#define BCM6846_WAN_MISC		(BCM6846_XRDP_BASE + 0x00db2000)
#define  WAN_CFG_REG			0x00
#define  WAN_CFG_AE_SEL			BIT(0)
#define  WAN_CFG_SGMII			BIT(1)

/* QEGPHY */
#define BCM6846_QEGPHY			(BCM6846_XRDP_BASE + 0x00db2200)
#define  QEGPHY_CNTRL			0x00
#define  QEGPHY_IDDQ_BIAS		BIT(0)
#define  QEGPHY_EXT_PWR_DOWN		BIT(1)
#define  QEGPHY_IDDQ_GLOBAL		BIT(3)
#define  QEGPHY_PHY_RESET		BIT(5)

/* SFP TX disable GPIO (GPIO 15, active-high) */
#define BCM6846_GPIO_BASE		(BCM6846_PERF_BASE + 0x500)
#define  GPIO_DATA_HI			0x08
#define  GPIO_DIR_HI			0x10
#define SFP_TX_DIS_GPIO			15	/* bit 15 in low word */

/* Top control register — used to strap WAN mode */
#define BCM6846_TOP_CNTRL		(BCM6846_PERF1_BASE + 0x2000)
#define  TOP_CNTRL_WAN_MODE_MASK	BIT(8)	/* 1 = RJ45, 0 = fiber */

/* ============================================================
 * XRDP data-path init (implemented in data_path_6846.c)
 * ============================================================ */
extern int xrdp_data_path_init(void);

/* ============================================================
 * Helpers
 * ============================================================ */
static inline u32 rd32(ulong addr)
{
	return readl((void *)addr);
}

static inline void wr32(ulong addr, u32 val)
{
	writel(val, (void *)addr);
}

/* ============================================================
 * board_init — called early in board_init_r
 * ============================================================ */
int board_init(void)
{
	/* Ensure CFI / gd->bd is set */
	gd->bd->bi_arch_number = 0x6846;
	gd->bd->bi_boot_params = 0x00000100;

	printf("Genexis XG6846B (BCM68460)\n");
	return 0;
}

/* ============================================================
 * DRAM info
 * ============================================================ */
int dram_init(void)
{
	gd->ram_size = CONFIG_SYS_SDRAM_SIZE;
	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = CONFIG_SYS_SDRAM_BASE;
	gd->bd->bi_dram[0].size  = CONFIG_SYS_SDRAM_SIZE;
	return 0;
}

/* ============================================================
 * WAN mode detection
 *
 * On XG6846B the WAN mode (fiber vs copper) is read from a
 * hardware strap bit in TOP_CNTRL.  The bootloader may also
 * override it via the "wan_mode" environment variable.
 * ============================================================ */
static int xg6846b_wan_is_rj45(void)
{
	const char *env = env_get("wan_mode");

	if (env) {
		if (!strcmp(env, "rj45") || !strcmp(env, "copper"))
			return 1;
		if (!strcmp(env, "fiber") || !strcmp(env, "sfp"))
			return 0;
	}

	/* Fall back to hardware strap */
	return !!(rd32(BCM6846_TOP_CNTRL) & TOP_CNTRL_WAN_MODE_MASK);
}

/* ============================================================
 * QEGPHY power management (copper WAN)
 * ============================================================ */
static void qegphy_power_up(void)
{
	u32 v;

	v = rd32(BCM6846_QEGPHY + QEGPHY_CNTRL);
	v &= ~(QEGPHY_IDDQ_BIAS | QEGPHY_EXT_PWR_DOWN | QEGPHY_IDDQ_GLOBAL);
	v |= QEGPHY_PHY_RESET;
	wr32(BCM6846_QEGPHY + QEGPHY_CNTRL, v);
	udelay(50);
	v &= ~QEGPHY_PHY_RESET;
	wr32(BCM6846_QEGPHY + QEGPHY_CNTRL, v);
	mdelay(1);
	printf("QEGPHY: powered up\n");
}

static void qegphy_power_down(void)
{
	u32 v = rd32(BCM6846_QEGPHY + QEGPHY_CNTRL);

	v |= QEGPHY_IDDQ_BIAS | QEGPHY_IDDQ_GLOBAL;
	wr32(BCM6846_QEGPHY + QEGPHY_CNTRL, v);
}

/* ============================================================
 * SFP TX disable GPIO
 * ============================================================ */
static void sfp_tx_disable(int disable)
{
	u32 dir, dat;

	/* Set GPIO 15 as output */
	dir = rd32(BCM6846_GPIO_BASE + GPIO_DIR_HI);
	dir |= BIT(SFP_TX_DIS_GPIO - 0);  /* word 0 covers GPIO 0..31 */
	wr32(BCM6846_GPIO_BASE + GPIO_DIR_HI, dir);

	dat = rd32(BCM6846_GPIO_BASE + GPIO_DATA_HI);
	if (disable)
		dat |= BIT(SFP_TX_DIS_GPIO);
	else
		dat &= ~BIT(SFP_TX_DIS_GPIO);
	wr32(BCM6846_GPIO_BASE + GPIO_DATA_HI, dat);
}

/* ============================================================
 * WAN SERDES / XRDP AE mux
 * ============================================================ */
static void wan_ae_select(void)
{
	u32 v = rd32(BCM6846_WAN_MISC + WAN_CFG_REG);

	v |= WAN_CFG_AE_SEL | WAN_CFG_SGMII;
	wr32(BCM6846_WAN_MISC + WAN_CFG_REG, v);
}

/* ============================================================
 * board_late_init — network bringup
 * ============================================================ */
int board_late_init(void)
{
	int rj45 = xg6846b_wan_is_rj45();

	printf("XG6846B: WAN mode = %s\n", rj45 ? "RJ45/copper" : "Fiber/SFP");

	/* Select AE (SERDES) on XRDP WAN mux */
	wan_ae_select();

	if (rj45) {
		sfp_tx_disable(1);
		qegphy_power_up();
	} else {
		sfp_tx_disable(0);	/* enable SFP laser */
		qegphy_power_down();
	}

	/* XRDP data-path init — must run before any ethernet I/O */
	printf("XG6846B: initialising XRDP data path...\n");
	if (xrdp_data_path_init())
		printf("XG6846B: WARNING - XRDP init returned error\n");
	else
		printf("XG6846B: XRDP ready\n");

	/* Set default environment values */
	if (!env_get("bootdelay"))
		env_set("bootdelay", "3");

	if (!env_get("bootcmd"))
		env_set("bootcmd",
			"nand read 0x81000000 0x100000 0x1e00000; "
			"bootm 0x81000000");

	if (!env_get("bootargs"))
		env_set("bootargs",
			"console=ttyS0,115200 "
			"root=/dev/mtdblock2 rootfstype=squashfs "
			"init=/etc/preinit");

	return 0;
}

/* ============================================================
 * Ethernet environment helpers
 * ============================================================ */
int board_eth_init(struct bd_info *bis)
{
	return eth_initialize();
}

/* ============================================================
 * misc_init_r — miscellaneous late hardware init
 * ============================================================ */
int misc_init_r(void)
{
	/* Print SoC family */
	printf("SoC: BCM68460 (BCM6846 family)\n");
	return 0;
}

/* ============================================================
 * reset — software-triggered SoC reset
 * ============================================================ */
void reset_cpu(void)
{
	/* BCM6846 watchdog reset: write magic to WD timer */
#define BCM6846_TIMR_BASE	(BCM6846_PERF_BASE + 0x400)
#define WD_WATCHDOG_DEFCOUNT	0x00000001
#define WD_TIMER_CTL		0x0c
#define WD_TIMER_EN		0x10
	wr32(BCM6846_TIMR_BASE + WD_TIMER_CTL,  0xee0f6e77);
	wr32(BCM6846_TIMR_BASE + WD_TIMER_CTL,  0x00000001);
	wr32(BCM6846_TIMR_BASE + WD_WATCHDOG_DEFCOUNT, 1);
	wr32(BCM6846_TIMR_BASE + WD_TIMER_EN, 1);

	while (1)
		;
}

/* ============================================================
 * U-Boot commands: wan_mode, xrdp_reinit
 * ============================================================ */
static int do_wan_mode(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	if (argc < 2) {
		printf("WAN mode: %s\n",
		       xg6846b_wan_is_rj45() ? "rj45" : "fiber");
		return 0;
	}

	if (!strcmp(argv[1], "fiber") || !strcmp(argv[1], "sfp")) {
		env_set("wan_mode", "fiber");
		sfp_tx_disable(0);
		qegphy_power_down();
		printf("WAN mode set to Fiber/SFP\n");
	} else if (!strcmp(argv[1], "rj45") || !strcmp(argv[1], "copper")) {
		env_set("wan_mode", "rj45");
		sfp_tx_disable(1);
		qegphy_power_up();
		printf("WAN mode set to RJ45/copper\n");
	} else {
		return CMD_RET_USAGE;
	}

	return 0;
}

U_BOOT_CMD(wan_mode, 2, 0, do_wan_mode,
	   "get/set WAN port mode",
	   "[fiber|rj45]  - query or set WAN mode (fiber/SFP or RJ45/copper)\n"
	   "  Changes take effect immediately. Use 'saveenv' to persist.\n");

static int do_xrdp_reinit(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	printf("Re-initialising XRDP data path...\n");
	if (xrdp_data_path_init())
		printf("ERROR: XRDP init failed\n");
	else
		printf("XRDP data path reinitialised OK\n");
	return 0;
}

U_BOOT_CMD(xrdp_reinit, 1, 0, do_xrdp_reinit,
	   "re-run BCM6846 XRDP data-path init sequence",
	   "  Replays the XRDP register init table.\n"
	   "  Use after a partial reset or to diagnose network issues.\n");
