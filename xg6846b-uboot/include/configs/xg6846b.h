/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * include/configs/xg6846b.h
 *
 * U-Boot configuration for Genexis XG6846B
 * Broadcom BCM68460 (BCM6846) SoC
 *
 * Copyright (C) 2024 - Genexis XG6846B port
 */

#ifndef __XG6846B_H
#define __XG6846B_H

/*
 * ================================================================
 * SoC / CPU
 * ================================================================
 */
#define CONFIG_BCM6846		1
#define CONFIG_BCM96846		1
#define CONFIG_ARM		1

/* Clock frequencies */
#define CONFIG_SYS_CLK_FREQ		1500000000	/* 1.5 GHz CPU */
#define CONFIG_SYS_HZ			1000		/* timer tick: 1 ms */

/*
 * ================================================================
 * Memory map
 * ================================================================
 */
#define CONFIG_SYS_SDRAM_BASE		0x00000000
#define CONFIG_SYS_SDRAM_SIZE		(256 * 1024 * 1024)	/* 256 MB */

/* U-Boot text/data location (loaded by CFE / SPL) */
#define CONFIG_SYS_TEXT_BASE		0x81f00000

/* Stack and heap */
#define CONFIG_SYS_INIT_SP_ADDR		(CONFIG_SYS_SDRAM_BASE + \
					 CONFIG_SYS_SDRAM_SIZE - \
					 GENERATED_GBL_DATA_SIZE)
#define CONFIG_SYS_MALLOC_LEN		(4 * 1024 * 1024)	/* 4 MB heap */

/* Load address for kernels / dtb */
#define CONFIG_SYS_LOAD_ADDR		0x81000000

/*
 * ================================================================
 * NAND Flash
 * ================================================================
 */
#define CONFIG_SYS_MAX_NAND_DEVICE	1
#define CONFIG_SYS_NAND_BASE		0xffe00000	/* NANDFLASH_PHYS_BASE */
#define CONFIG_SYS_NAND_SELF_INIT

/* MTD partition layout (must match DTS and OpenWrt target) */
#define MTDPARTS_DEFAULT		\
	"mtdparts=brcmnand.0:"		\
	"1m(cfe)ro,"			\
	"30m(linux),"			\
	"1m(nvram)"

/*
 * ================================================================
 * UART (console)
 * ================================================================
 */
#define CONFIG_SYS_BAUDRATE_TABLE	{ 9600, 19200, 38400, 57600, 115200 }

/* BCM6345 UART at PERF_PHYS_BASE + 0x640 = 0xff800640 */
#define CONFIG_SYS_NS16550_COM1		0xff800640
#define CONFIG_SYS_NS16550_CLK		50000000	/* 50 MHz UART ref */

/*
 * ================================================================
 * Ethernet
 * ================================================================
 */
#define CONFIG_BCMBCA_XRDP_ETH		/* use XRDP path */
#define CONFIG_XRDP_SBPM		/* BCM6846 uses SBPM (not FPM) */

/* Default active port (switch port to use for boot-time networking) */
#define CONFIG_BCM6846_ACTIVE_PORT	0	/* LAN1 = switch port 0 */

/* TFTP / DHCP settings */
#define CONFIG_NET_RETRY_COUNT		10

/*
 * ================================================================
 * Environment storage (in NVRAM NAND partition)
 * ================================================================
 */
#define CONFIG_ENV_IS_IN_NAND		1
#define CONFIG_ENV_OFFSET		0x1f00000	/* 31 MB into NAND */
#define CONFIG_ENV_SIZE			0x20000		/* 128 KB */
#define CONFIG_ENV_OFFSET_REDUND	(CONFIG_ENV_OFFSET + CONFIG_ENV_SIZE)

/*
 * ================================================================
 * Boot command defaults
 * ================================================================
 */
#define CONFIG_BOOTDELAY		3

#define CONFIG_BOOTCOMMAND					\
	"nand read 0x81000000 0x100000 0x1e00000; "		\
	"bootm 0x81000000"

#define CONFIG_EXTRA_ENV_SETTINGS				\
	"bootargs=console=ttyS0,115200 "			\
	    "root=/dev/mtdblock2 rootfstype=squashfs "		\
	    "init=/etc/preinit\0"				\
	"wan_mode=fiber\0"					\
	"active_port=0\0"					\
	"ipaddr=192.168.1.1\0"				\
	"serverip=192.168.1.254\0"				\
	"netmask=255.255.255.0\0"				\
	"tftpfile=openwrt-xg6846b.bin\0"			\
	"update_fw="						\
	    "tftp 0x81000000 ${tftpfile}; "			\
	    "nand erase 0x100000 0x1e00000; "			\
	    "nand write 0x81000000 0x100000 ${filesize}\0"

/*
 * ================================================================
 * Boot device selection
 * ================================================================
 */
#define CONFIG_BOOTARGS_NAND						\
	"setenv bootargs console=ttyS0,115200 "				\
	"root=/dev/mtdblock2 rootfstype=squashfs "			\
	"init=/etc/preinit mtdparts=" MTDPARTS_DEFAULT

/*
 * ================================================================
 * DM / Device Model
 * ================================================================
 */
#define CONFIG_DM			1
#define CONFIG_DM_ETH			1
#define CONFIG_DM_MDIO			1
#define CONFIG_DM_GPIO			1
#define CONFIG_DM_SERIAL		1

/*
 * ================================================================
 * BCM6846-specific register addresses used in board code
 * (duplicated here for standalone board header use)
 * ================================================================
 */
#define BCM6846_PERF_PHYS_BASE		0xff800000UL
#define BCM6846_XRDP_PHYS_BASE		0x82000000UL
#define BCM6846_MDIO_PHYS_BASE		(BCM6846_PERF_PHYS_BASE + 0x2060)
#define BCM6846_UNIMAC_PHYS_BASE	(BCM6846_XRDP_PHYS_BASE + 0x00da0000)
#define BCM6846_RGMII_PHYS_BASE		(BCM6846_XRDP_PHYS_BASE + 0x00d97300)
#define BCM6846_WAN_MISC_PHYS_BASE	(BCM6846_XRDP_PHYS_BASE + 0x00db2000)
#define BCM6846_QEGPHY_PHYS_BASE	(BCM6846_XRDP_PHYS_BASE + 0x00db2200)
#define BCM6846_SBPM_PHYS_BASE		(BCM6846_XRDP_PHYS_BASE + 0x00d98000)
#define BCM6846_RNR_CORE0_PHYS_BASE	(BCM6846_XRDP_PHYS_BASE + 0x00d00000)

/*
 * ================================================================
 * SPL (Secondary Program Loader) — optional
 * ================================================================
 */
#ifdef CONFIG_SPL_BUILD
#define CONFIG_SPL_TEXT_BASE		0x80100000
#define CONFIG_SPL_STACK		(CONFIG_SYS_SDRAM_BASE + 0x10000)
#define CONFIG_SPL_MALLOC_SIZE		(64 * 1024)
#define CONFIG_SPL_NAND_SUPPORT		1
#define CONFIG_SPL_NAND_SIMPLE		1
#endif

/*
 * ================================================================
 * Misc
 * ================================================================
 */
#define CONFIG_SYS_MAXARGS		32
#define CONFIG_SYS_CBSIZE		1024	/* Console I/O buffer */
#define CONFIG_SYS_PBSIZE		(CONFIG_SYS_CBSIZE + 64)
#define CONFIG_SYS_LONGHELP		1

/* LZO decompression for squashfs */
#define CONFIG_LZO			1

/* FIT image support */
#define CONFIG_FIT			1
#define CONFIG_FIT_VERBOSE		1

/* Allow booting unsigned images during development */
#define CONFIG_LEGACY_IMAGE_FORMAT	1

#endif /* __XG6846B_H */
