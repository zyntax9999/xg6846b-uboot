# Genexis XG6846B — U-Boot Port
## For OpenWrt Development

---

## File Layout

```
xg6846b-uboot/
├── board/genexis/xg6846b/
│   ├── xg6846b.c          Board init, WAN mode, XRDP bringup, custom commands
│   ├── xg6846b.dts        U-Boot device tree (minimal)
│   ├── Makefile
│   └── Kconfig
├── configs/
│   └── xg6846b_defconfig  U-Boot defconfig
├── drivers/
│   ├── mdio/bcm6846_mdio.c   DM MDIO driver (UCLASS_MDIO)
│   └── net/bcm6846_eth.c     DM Ethernet driver (UCLASS_ETH, XRDP SBPM)
└── include/configs/
    └── xg6846b.h             Board configuration header
```

---

## Integration into U-Boot source tree

### 1. Copy files

```bash
UBOOT=/path/to/u-boot-openwrt   # or mainline u-boot

# Board files
cp -r board/genexis            $UBOOT/board/genexis

# Include
cp include/configs/xg6846b.h   $UBOOT/include/configs/

# Defconfig
cp configs/xg6846b_defconfig   $UBOOT/configs/

# Drivers
cp drivers/mdio/bcm6846_mdio.c $UBOOT/drivers/mdio/
cp drivers/net/bcm6846_eth.c   $UBOOT/drivers/net/
```

### 2. Wire Broadcom SDK files into the board build

The XRDP data-path init relies on three files from the Broadcom BCA SDK
archive (provided separately). Copy them into the board directory:

```bash
# From the bcmbca_tar.xz archive:
cp bcmbca/xrdp/data_path_6846.c  $UBOOT/board/genexis/xg6846b/
cp bcmbca/xrdp/access_logging.c  $UBOOT/board/genexis/xg6846b/
cp bcmbca/xrdp/rdp_drv_sbpm.c    $UBOOT/board/genexis/xg6846b/

# Headers needed by those files:
cp bcmbca/xrdp/access_logging.h  $UBOOT/board/genexis/xg6846b/
cp bcmbca/xrdp/access_macros.h   $UBOOT/board/genexis/xg6846b/
cp bcmbca/xrdp/rdd_data_structures.h $UBOOT/board/genexis/xg6846b/
cp bcmbca/xrdp/rdpa_types.h      $UBOOT/board/genexis/xg6846b/
```

The `data_path_6846.c` file provides `init_data[]` and the wrapper:

```c
// Add this shim to data_path_6846.c (or a new file) to export the
// standard U-Boot entry point:
int xrdp_data_path_init(void)
{
    return access_log_restore(init_data);
}
```

### 3. Register drivers in U-Boot Kconfig/Makefile

**drivers/mdio/Makefile** — append:
```makefile
obj-$(CONFIG_BCM6846_MDIO) += bcm6846_mdio.o
```

**drivers/mdio/Kconfig** — append:
```kconfig
config BCM6846_MDIO
    bool "BCM6846 MDIO bus driver"
    depends on DM_MDIO
    help
      MDIO driver for BCM6846 (BCM68460) SoC.
```

**drivers/net/Makefile** — append:
```makefile
obj-$(CONFIG_BCM6846_ETH) += bcm6846_eth.o
```

**drivers/net/Kconfig** — append:
```kconfig
config BCM6846_ETH
    bool "BCM6846 XRDP Ethernet driver"
    depends on DM_ETH
    select BCM6846_MDIO
    help
      Ethernet driver for BCM6846 using XRDP SBPM packet buffers.
```

**arch/arm/Kconfig** (or platform Kconfig) — append:
```kconfig
source "board/genexis/xg6846b/Kconfig"
```

### 4. Build

```bash
cd $UBOOT
export CROSS_COMPILE=arm-linux-gnueabihf-
export ARCH=arm

make xg6846b_defconfig
make -j$(nproc)
```

Output: `u-boot.bin` — flash to the CFE partition or load via JTAG.

---

## OpenWrt Target Integration

In your OpenWrt buildroot, create:

**target/linux/bcm68xx/xg6846b/**

```
config
    - CONFIG_TARGET_bcm68xx_xg6846b=y
    - CONFIG_BCM6846_ETH=y
    - CONFIG_BCM6846_MDIO=y

base-files/
    etc/
        board.d/02_network
            # define LAN/WAN interface names

patches-5.15/
    0001-dts-add-genexis-xg6846b.patch   # adds genexis-xg6846b.dts
```

**target/linux/bcm68xx/image/Makefile** — add XG6846B image target.

---

## Boot Flow

```
Power on
  └─ CFE (Broadcom bootloader in flash)
       └─ loads u-boot.bin from NAND offset 0 (or 0x10000)
            └─ board_init_f()
                 ├─ lowlevel_init (clock/DDR)
                 └─ relocate to CONFIG_SYS_TEXT_BASE (0x81f00000)
            └─ board_init_r()
                 ├─ board_init()       → SoC ident
                 ├─ board_late_init()  → WAN mode, XRDP init, switch init
                 ├─ eth_initialize()   → DM probe MDIO + ETH drivers
                 └─ autoboot (3 s)
                      ├─ [interrupted] → XG6846B> prompt
                      └─ CONFIG_BOOTCOMMAND → nand read → bootm
```

---

## U-Boot Commands

### Standard network commands

```sh
# DHCP + TFTP boot
dhcp
tftpboot 0x81000000 openwrt-xg6846b.bin
bootm 0x81000000

# Static IP TFTP
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.254
tftpboot 0x81000000 openwrt-xg6846b.bin
```

### Firmware update via TFTP

```sh
# Update kernel+rootfs partition (linux, 30 MB)
run update_fw
```

Or step-by-step:
```sh
tftpboot 0x81000000 openwrt-xg6846b-nand.bin
nand erase 0x100000 0x1e00000
nand write 0x81000000 0x100000 ${filesize}
reset
```

### WAN mode

```sh
# Show current WAN mode
wan_mode

# Switch to RJ45/copper
wan_mode rj45
saveenv

# Switch to SFP/fiber
wan_mode fiber
saveenv
```

### XRDP debug

```sh
# Re-run XRDP init (use if network stops working)
xrdp_reinit

# Check MDIO bus / PHY
mii device
mii info
mdio list

# Read 88E6320 CPU port status (port 6, reg 0)
mdio read 0x16 0
```

### NAND partitions

```sh
nand info
mtdparts
nand dump 0x1f00000   # dump NVRAM partition
```

---

## XRDP Data-Path Architecture in U-Boot

```
send():
  eth_send(buf, len)
    └─ sbpm_write_packet()   ← copies buf into SBPM PSRAM buffers
         ├─ sbpm_alloc()       allocate buffer node(s) from 0x82d98000
         └─ sbpm_connect()     link multi-fragment chains
    └─ write BBH TX PD        at RNR_CORE0 + 0x50
    └─ rnr_cpu_wakeup()       kick Runner CPU TX thread (thread 1)

recv():
  eth_recv()
    └─ poll SRAM PD FIFO      at RNR_CORE0 + 0x800
         └─ non-zero word1 → packet available
    └─ sbpm_read_packet()     copy PSRAM chain into rx_buf
    └─ sbpm_free()            release SBPM buffer nodes
    └─ return pointer + len   to net stack

Key addresses (BCM6846):
  SBPM_ADDRS           = 0x82d98000   (alloc/free/connect)
  PSRAM_MEM_ADDRS      = 0x82600000   (128-byte-stride packet data)
  RNR_REGS_ADDR_CORE0  = 0x82d00000   (Runner register file)
  SBPM_MAX_BUFFER_NUM  = 0x5ff        (1536 buffers)
  BB_ID_TX_LAN         = 32           (LAN TX BB destination)
```

---

## Differences from Linux driver

| Aspect | Linux | U-Boot |
|---|---|---|
| XRDP init | `platform_driver` module | `board_late_init()` / `xrdp_reinit` cmd |
| TX/RX | IRQ + DMA ring | Polled SBPM |
| MDIO | `UCLASS_MDIO` + phylink | `UCLASS_MDIO` direct |
| Switch | DSA framework + mv88e6xxx | Minimal reg writes in `sw_init()` |
| MAC | `register_netdev` | `UCLASS_ETH` ops |
| WAN | `platform_driver` + SFP bus | Board init GPIO toggle |

---

## Known Issues / TODO

1. **SBPM headroom**: The `data_offset` in CPU RX descriptors may vary
   by firmware version. If received packets are offset, adjust
   `BCM6846_SBPM_HEADROOM` in `bcm6846_eth.c`.

2. **Multi-fragment TX**: Packets > 128 bytes require chained SBPM
   buffers (`sbpm_connect`). This is implemented but has not been
   tested with jumbo frames — keep MTU at 1500 during development.

3. **88E6320 VLAN isolation**: In U-Boot all ports share one VLAN.
   This is fine for firmware update but means a device on any LAN
   port can reach the tftp server. Acceptable for development.

4. **CFE handoff**: If CFE has already initialised XRDP, calling
   `xrdp_data_path_init()` again is safe (idempotent register writes).
   Use `xrdp_reinit` if the network seems stale after `reset`.
