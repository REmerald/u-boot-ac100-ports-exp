// SPDX-License-Identifier: GPL-2.0
/*
 * Broadcom STB PCIe controller driver
 *
 * Copyright (C) 2020 Samsung Electronics Co., Ltd.
 *
 * Based on upstream Linux kernel driver:
 * drivers/pci/controller/pcie-brcmstb.c
 * Copyright (C) 2009 - 2017 Broadcom
 *
 * Based driver by Nicolas Saenz Julienne
 * Copyright (C) 2020 Nicolas Saenz Julienne <nsaenzjulienne@suse.de>
 */

#include <asm/io.h>
#include <common.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <errno.h>
#include <linux/bitfield.h>
#include <linux/log2.h>
#include <pci.h>

/* BRCM_PCIE_CAP_REGS - Offset for the mandatory capability config regs */
#define BRCM_PCIE_CAP_REGS				0x00ac

/* Broadcom STB PCIe Register Offsets */
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1				0x0188
#define  PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK	0xc
#define  PCIE_RC_CFG_VENDOR_SPCIFIC_REG1_LITTLE_ENDIAN			0x0

#define PCIE_RC_CFG_PRIV1_ID_VAL3			0x043c
#define  PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK	0xffffff

#define PCIE_RC_DL_MDIO_ADDR				0x1100
#define PCIE_RC_DL_MDIO_WR_DATA				0x1104
#define PCIE_RC_DL_MDIO_RD_DATA				0x1108

#define PCIE_MISC_MISC_CTRL				0x4008
#define  PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK		0x1000
#define  PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK	0x2000
#define  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK	0x300000
#define  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_128		0x0
#define  PCIE_MISC_MISC_CTRL_SCB0_SIZE_MASK		0xf8000000

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO		0x400c
#define PCIE_MEM_WIN0_LO(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO + ((win) * 4)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI		0x4010
#define PCIE_MEM_WIN0_HI(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI + ((win) * 4)

#define PCIE_MISC_RC_BAR1_CONFIG_LO			0x402c
#define  PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK		0x1f

#define PCIE_MISC_RC_BAR2_CONFIG_LO			0x4034
#define  PCIE_MISC_RC_BAR2_CONFIG_LO_SIZE_MASK		0x1f
#define PCIE_MISC_RC_BAR2_CONFIG_HI			0x4038

#define PCIE_MISC_RC_BAR3_CONFIG_LO			0x403c
#define  PCIE_MISC_RC_BAR3_CONFIG_LO_SIZE_MASK		0x1f

#define PCIE_MISC_PCIE_STATUS				0x4068
#define  PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK		0x80
#define  PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK	0x20
#define  PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK	0x10
#define  PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK	0x40

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT		 0x4070
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK	 0xfff00000
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK	 0xfff0
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_HI_SHIFT	 12
#define PCIE_MEM_WIN0_BASE_LIMIT(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT + ((win) * 4)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI			 0x4080
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK	 0xff
#define PCIE_MEM_WIN0_BASE_HI(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI + ((win) * 8)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI			 0x4084
#define  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK	 0xff
#define PCIE_MEM_WIN0_LIMIT_HI(win)	\
		PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI + ((win) * 8)

#define PCIE_MISC_HARD_PCIE_HARD_DEBUG				 0x4204
#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK 0x2
#define  PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK	 0x08000000

#define PCIE_MSI_INTR2_CLR				0x4508
#define PCIE_MSI_INTR2_MASK_SET				0x4510

#define PCIE_EXT_CFG_DATA				0x8000

#define PCIE_EXT_CFG_INDEX				0x9000
#define  PCIE_EXT_BUSNUM_SHIFT				20
#define  PCIE_EXT_SLOT_SHIFT				15
#define  PCIE_EXT_FUNC_SHIFT				12

#define PCIE_RGR1_SW_INIT_1				0x9210
#define  PCIE_RGR1_SW_INIT_1_PERST_MASK			0x1
#define  PCIE_RGR1_SW_INIT_1_INIT_MASK			0x2

/* PCIe parameters */
#define BRCM_NUM_PCIE_OUT_WINS		0x4

/* MDIO registers */
#define MDIO_PORT0			0x0
#define MDIO_DATA_MASK			0x7fffffff
#define MDIO_PORT_MASK			0xf0000
#define MDIO_REGAD_MASK			0xffff
#define MDIO_CMD_MASK			0xfff00000
#define MDIO_CMD_READ			0x1
#define MDIO_CMD_WRITE			0x0
#define MDIO_DATA_DONE_MASK		0x80000000
#define MDIO_RD_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 1 : 0)
#define MDIO_WT_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 0 : 1)
#define SSC_REGS_ADDR			0x1100
#define SET_ADDR_OFFSET			0x1f
#define SSC_CNTL_OFFSET			0x2
#define SSC_CNTL_OVRD_EN_MASK		0x8000
#define SSC_CNTL_OVRD_VAL_MASK		0x4000
#define SSC_STATUS_OFFSET		0x1
#define SSC_STATUS_SSC_MASK		0x400
#define SSC_STATUS_PLL_LOCK_MASK	0x800

struct brcm_pcie {
	void __iomem		*base;

	int			gen;
	bool			ssc;
};

#define msleep(a) udelay((a) * 1000)

/*
 * This is to convert the size of the inbound "BAR" region to the
 * non-linear values of PCIE_X_MISC_RC_BAR[123]_CONFIG_LO.SIZE
 */
static int brcm_pcie_encode_ibar_size(u64 size)
{
	int log2_in = ilog2(size);

	if (log2_in >= 12 && log2_in <= 15)
		/* Covers 4KB to 32KB (inclusive) */
		return (log2_in - 12) + 0x1c;
	else if (log2_in >= 16 && log2_in <= 37)
		/* Covers 64KB to 32GB, (inclusive) */
		return log2_in - 15;
	/* Something is awry so disable */
	return 0;
}

/* Configuration space read/write support */
static inline int brcm_pcie_cfg_index(pci_dev_t bdf, int reg)
{
	return (PCI_DEV(bdf) << PCIE_EXT_SLOT_SHIFT)
		| (PCI_FUNC(bdf) << PCIE_EXT_FUNC_SHIFT)
		| (PCI_BUS(bdf) << PCIE_EXT_BUSNUM_SHIFT)
		| (reg & ~3);
}

/* The controller is capable of serving in both RC and EP roles */
static bool brcm_pcie_rc_mode(struct brcm_pcie *pcie)
{
	u32 val = readl(pcie->base + PCIE_MISC_PCIE_STATUS);

	return !!FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK, val);
}

static bool brcm_pcie_link_up(struct brcm_pcie *pcie)
{
	u32 val = readl(pcie->base + PCIE_MISC_PCIE_STATUS);
	u32 dla = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK, val);
	u32 plu = FIELD_GET(PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK, val);

	return dla && plu;
}

static int brcm_pcie_config_address(struct udevice *udev, pci_dev_t bdf,
				    uint offset, void **paddress)
{
	struct brcm_pcie *pcie = dev_get_priv(udev);
	unsigned int bus = PCI_BUS(bdf);
	unsigned int dev = PCI_DEV(bdf);
	int idx;

	/*
	 * Busses 0 (host PCIe bridge) and 1 (its immediate child)
	 * are limited to a single device each
	 */
	if ((bus == (udev->seq + 1)) && dev > 0)
		return -ENODEV;

	/* Accesses to the RC go right to the RC registers if PCI device == 0 */
	if (bus == udev->seq) {
		if (PCI_DEV(bdf))
			return -ENODEV;

		*paddress = pcie->base + offset;
		return 0;
	}

	/* For devices, write to the config space index register */
	idx = brcm_pcie_cfg_index(bdf, 0);

	writel(idx, pcie->base + PCIE_EXT_CFG_INDEX);
	*paddress = pcie->base + PCIE_EXT_CFG_DATA + offset;

	return 0;
}

static int brcm_pcie_read_config(struct udevice *bus, pci_dev_t bdf,
				 uint offset, ulong *valuep,
				 enum pci_size_t size)
{
	return pci_generic_mmap_read_config(bus, brcm_pcie_config_address,
					    bdf, offset, valuep, size);
}

static int brcm_pcie_write_config(struct udevice *bus, pci_dev_t bdf,
				  uint offset, ulong value,
				  enum pci_size_t size)
{
	return pci_generic_mmap_write_config(bus, brcm_pcie_config_address,
					     bdf, offset, value, size);
}

static const char *link_speed_to_str(unsigned int s)
{
	static const char * const speed_str[] = { "??", "2.5", "5.0", "8.0" };

	if (s >= ARRAY_SIZE(speed_str))
		s = 0;

	return speed_str[s];
}

static inline void brcm_pcie_bridge_sw_init_set(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp;

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1);
	u32p_replace_bits(&tmp, val, PCIE_RGR1_SW_INIT_1_INIT_MASK);
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1);
}

static inline void brcm_pcie_perst_set(struct brcm_pcie *pcie, u32 val)
{
	u32 tmp;

	tmp = readl(pcie->base + PCIE_RGR1_SW_INIT_1);
	u32p_replace_bits(&tmp, val, PCIE_RGR1_SW_INIT_1_PERST_MASK);
	writel(tmp, pcie->base + PCIE_RGR1_SW_INIT_1);
}

static u32 brcm_pcie_mdio_form_pkt(int port, int regad, int cmd)
{
	u32 pkt = 0;

	pkt |= FIELD_PREP(MDIO_PORT_MASK, port);
	pkt |= FIELD_PREP(MDIO_REGAD_MASK, regad);
	pkt |= FIELD_PREP(MDIO_CMD_MASK, cmd);

	return pkt;
}

/* Negative return value indicates error */
static int brcm_pcie_mdio_read(void __iomem *base, u8 port, u8 regad, u32 *val)
{
	int tries;
	u32 data;

	writel(brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_READ),
	       base + PCIE_RC_DL_MDIO_ADDR);
	readl(base + PCIE_RC_DL_MDIO_ADDR);

	data = readl(base + PCIE_RC_DL_MDIO_RD_DATA);
	for (tries = 0; !MDIO_RD_DONE(data) && tries < 10; tries++) {
		udelay(10);
		data = readl(base + PCIE_RC_DL_MDIO_RD_DATA);
	}

	*val = FIELD_GET(MDIO_DATA_MASK, data);
	return MDIO_RD_DONE(data) ? 0 : -EIO;
}

/* Negative return value indicates error */
static int brcm_pcie_mdio_write(void __iomem *base, u8 port,
				u8 regad, u16 wrdata)
{
	int tries;
	u32 data;

	writel(brcm_pcie_mdio_form_pkt(port, regad, MDIO_CMD_WRITE),
	       base + PCIE_RC_DL_MDIO_ADDR);
	readl(base + PCIE_RC_DL_MDIO_ADDR);
	writel(MDIO_DATA_DONE_MASK | wrdata, base + PCIE_RC_DL_MDIO_WR_DATA);

	data = readl(base + PCIE_RC_DL_MDIO_WR_DATA);
	for (tries = 0; !MDIO_WT_DONE(data) && tries < 10; tries++) {
		udelay(10);
		data = readl(base + PCIE_RC_DL_MDIO_WR_DATA);
	}

	return MDIO_WT_DONE(data) ? 0 : -EIO;
}

/*
 * Configures device for Spread Spectrum Clocking (SSC) mode; negative
 * return value indicates error.
 */
static int brcm_pcie_set_ssc(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	int pll, ssc;
	int ret;
	u32 tmp;

	ret = brcm_pcie_mdio_write(base, MDIO_PORT0, SET_ADDR_OFFSET,
				   SSC_REGS_ADDR);
	if (ret < 0)
		return ret;

	ret = brcm_pcie_mdio_read(base, MDIO_PORT0, SSC_CNTL_OFFSET, &tmp);
	if (ret < 0)
		return ret;

	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_EN_MASK);
	u32p_replace_bits(&tmp, 1, SSC_CNTL_OVRD_VAL_MASK);
	ret = brcm_pcie_mdio_write(base, MDIO_PORT0, SSC_CNTL_OFFSET, tmp);
	if (ret < 0)
		return ret;

	udelay(1000);
	ret = brcm_pcie_mdio_read(base, MDIO_PORT0, SSC_STATUS_OFFSET, &tmp);
	if (ret < 0)
		return ret;

	ssc = FIELD_GET(SSC_STATUS_SSC_MASK, tmp);
	pll = FIELD_GET(SSC_STATUS_PLL_LOCK_MASK, tmp);

	return ssc && pll ? 0 : -EIO;
}

/* Limits operation to a specific generation (1, 2, or 3) */
static void brcm_pcie_set_gen(struct brcm_pcie *pcie, int gen)
{
	void __iomem *base = pcie->base;

	u16 lnkctl2 = readw(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);
	u32 lnkcap = readl(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP);

	lnkcap = (lnkcap & ~PCI_EXP_LNKCAP_SLS) | gen;
	writel(lnkcap, base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP);

	lnkctl2 = (lnkctl2 & ~0xf) | gen;
	writew(lnkctl2, base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);
}

static void brcm_pcie_set_outbound_win(struct brcm_pcie *pcie,
				       unsigned int win, u64 phys_addr,
				       u64 pcie_addr, u64 size)
{
	void __iomem *base = pcie->base;
	u32 phys_addr_mb_high, limit_addr_mb_high;
	phys_addr_t phys_addr_mb, limit_addr_mb;
	int high_addr_shift;
	u32 tmp;

	/* Set the base of the pcie_addr window */
	writel(lower_32_bits(pcie_addr), base + PCIE_MEM_WIN0_LO(win));
	writel(upper_32_bits(pcie_addr), base + PCIE_MEM_WIN0_HI(win));

	/* Write the addr base & limit lower bits (in MBs) */
	phys_addr_mb = phys_addr / SZ_1M;
	limit_addr_mb = (phys_addr + size - 1) / SZ_1M;

	tmp = readl(base + PCIE_MEM_WIN0_BASE_LIMIT(win));
	u32p_replace_bits(&tmp, phys_addr_mb,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK);
	u32p_replace_bits(&tmp, limit_addr_mb,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK);
	writel(tmp, base + PCIE_MEM_WIN0_BASE_LIMIT(win));

	/* Write the cpu & limit addr upper bits */
	high_addr_shift = PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_HI_SHIFT;
	phys_addr_mb_high = phys_addr_mb >> high_addr_shift;
	tmp = readl(base + PCIE_MEM_WIN0_BASE_HI(win));
	u32p_replace_bits(&tmp, phys_addr_mb_high,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK);
	writel(tmp, base + PCIE_MEM_WIN0_BASE_HI(win));

	limit_addr_mb_high = limit_addr_mb >> high_addr_shift;
	tmp = readl(base + PCIE_MEM_WIN0_LIMIT_HI(win));
	u32p_replace_bits(&tmp, limit_addr_mb_high,
			  PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK);
	writel(tmp, base + PCIE_MEM_WIN0_LIMIT_HI(win));
}

static int brcm_pcie_probe(struct udevice *dev)
{
	struct udevice *ctlr = pci_get_controller(dev);
	struct pci_controller *hose = dev_get_uclass_priv(ctlr);
	struct brcm_pcie *pcie = dev_get_priv(dev);
	void __iomem *base = pcie->base;
	bool ssc_good = false;
	int num_out_wins = 0;
	u64 rc_bar2_offset, rc_bar2_size;
	unsigned int scb_size_val;
	int i, ret;
	u16 nlw, cls, lnksta;
	u32 tmp;

	/* Reset the bridge */
	brcm_pcie_bridge_sw_init_set(pcie, 1);

	udelay(150);

	/* Take the bridge out of reset */
	brcm_pcie_bridge_sw_init_set(pcie, 0);

	tmp = readl(base + PCIE_MISC_HARD_PCIE_HARD_DEBUG);
	tmp &= ~PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK;
	writel(tmp, base + PCIE_MISC_HARD_PCIE_HARD_DEBUG);
	/* Wait for SerDes to be stable */
	udelay(150);

	/* Set SCB_MAX_BURST_SIZE, CFG_READ_UR_MODE, SCB_ACCESS_EN */
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK);
	u32p_replace_bits(&tmp, 1, PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK);
	u32p_replace_bits(&tmp, PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_128,
			  PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK);
	writel(tmp, base + PCIE_MISC_MISC_CTRL);

	/*
	 * TODO: When support for other SoCs than BCM2711 is added we may
	 * need to use the base address and size(s) provided in the dma-ranges
	 * property.
	 */
	rc_bar2_offset = 0;
	rc_bar2_size = 0xc0000000;

	tmp = lower_32_bits(rc_bar2_offset);
	u32p_replace_bits(&tmp, brcm_pcie_encode_ibar_size(rc_bar2_size),
			  PCIE_MISC_RC_BAR2_CONFIG_LO_SIZE_MASK);
	writel(tmp, base + PCIE_MISC_RC_BAR2_CONFIG_LO);
	writel(upper_32_bits(rc_bar2_offset),
	       base + PCIE_MISC_RC_BAR2_CONFIG_HI);

	scb_size_val = rc_bar2_size ?
		       ilog2(rc_bar2_size) - 15 : 0xf; /* 0xf is 1GB */
	tmp = readl(base + PCIE_MISC_MISC_CTRL);
	u32p_replace_bits(&tmp, scb_size_val,
			  PCIE_MISC_MISC_CTRL_SCB0_SIZE_MASK);
	writel(tmp, base + PCIE_MISC_MISC_CTRL);

	/* Disable the PCIe->GISB memory window (RC_BAR1) */
	tmp = readl(base + PCIE_MISC_RC_BAR1_CONFIG_LO);
	tmp &= ~PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK;
	writel(tmp, base + PCIE_MISC_RC_BAR1_CONFIG_LO);

	/* Disable the PCIe->SCB memory window (RC_BAR3) */
	tmp = readl(base + PCIE_MISC_RC_BAR3_CONFIG_LO);
	tmp &= ~PCIE_MISC_RC_BAR3_CONFIG_LO_SIZE_MASK;
	writel(tmp, base + PCIE_MISC_RC_BAR3_CONFIG_LO);

	/* Mask all interrupts since we are not handling any yet */
	writel(0xffffffff, base + PCIE_MSI_INTR2_MASK_SET);

	/* Clear any interrupts we find on boot */
	writel(0xffffffff, base + PCIE_MSI_INTR2_CLR);

	if (pcie->gen)
		brcm_pcie_set_gen(pcie, pcie->gen);

	/* Unassert the fundamental reset */
	brcm_pcie_perst_set(pcie, 0);

	/* Give the RC/EP time to wake up, before trying to configure RC.
	 * Intermittently check status for link-up, up to a total of 100ms.
	 */
	for (i = 0; i < 100 && !brcm_pcie_link_up(pcie); i += 5)
		msleep(5);

	if (!brcm_pcie_link_up(pcie)) {
		printf("PCIe BRCM: link down\n");
		return -ENODEV;
	}

	if (!brcm_pcie_rc_mode(pcie)) {
		printf("PCIe misconfigured; is in EP mode\n");
		return -EINVAL;
	}

	for (i = 0; i < hose->region_count; i++) {
		struct pci_region *reg = &hose->regions[i];

		if (reg->flags != PCI_REGION_MEM)
			continue;

		if (num_out_wins >= BRCM_NUM_PCIE_OUT_WINS)
			return -EINVAL;

		brcm_pcie_set_outbound_win(pcie, num_out_wins, reg->phys_start,
					   reg->bus_start, reg->size);

		num_out_wins++;
	}

	/*
	 * For config space accesses on the RC, show the right class for
	 * a PCIe-PCIe bridge (the default setting is to be EP mode).
	 */
	tmp = readl(base + PCIE_RC_CFG_PRIV1_ID_VAL3);
	u32p_replace_bits(&tmp, 0x060400,
			  PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK);
	writel(tmp, base + PCIE_RC_CFG_PRIV1_ID_VAL3);

	if (pcie->ssc) {
		ret = brcm_pcie_set_ssc(pcie);
		if (ret == 0)
			ssc_good = true;
		else
			printf("PCIe BRCM: failed attempt to enter SSC mode\n");
	}

	lnksta = readw(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKSTA);
	cls = FIELD_GET(PCI_EXP_LNKSTA_CLS, lnksta);
	nlw = FIELD_GET(PCI_EXP_LNKSTA_NLW, lnksta);

	printf("PCIe BRCM: link up, %s Gbps x%u %s\n", link_speed_to_str(cls),
	       nlw, ssc_good ? "(SSC)" : "(!SSC)");

	/* PCIe->SCB endian mode for BAR */
	tmp = readl(base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1);
	u32p_replace_bits(&tmp, PCIE_RC_CFG_VENDOR_SPCIFIC_REG1_LITTLE_ENDIAN,
		PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK);
	writel(tmp, base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1);

	/*
	 * Refclk from RC should be gated with CLKREQ# input when ASPM L0s,L1
	 * is enabled => setting the CLKREQ_DEBUG_ENABLE field to 1.
	 */
	tmp = readl(base + PCIE_MISC_HARD_PCIE_HARD_DEBUG);
	tmp |= PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK;
	writel(tmp, base + PCIE_MISC_HARD_PCIE_HARD_DEBUG);

	return 0;
}

static int brcm_pcie_ofdata_to_platdata(struct udevice *dev)
{
	struct brcm_pcie *pcie = dev_get_priv(dev);
	ofnode dn = dev_ofnode(dev);
	u32 max_link_speed;
	int ret;

	/* Get the controller base address */
	pcie->base = dev_read_addr_ptr(dev);
	if (!pcie->base)
		return -EINVAL;

	pcie->ssc = ofnode_read_bool(dn, "brcm,enable-ssc");

	ret = ofnode_read_u32(dn, "max-link-speed", &max_link_speed);
	if (ret < 0 || max_link_speed > 4)
		pcie->gen = 0;
	else
		pcie->gen = max_link_speed;

	return 0;
}

static const struct dm_pci_ops brcm_pcie_ops = {
	.read_config	= brcm_pcie_read_config,
	.write_config	= brcm_pcie_write_config,
};

static const struct udevice_id brcm_pcie_ids[] = {
	{ .compatible = "brcm,bcm2711-pcie" },
	{ }
};

U_BOOT_DRIVER(pcie_brcm_base) = {
	.name			= "pcie_brcm",
	.id			= UCLASS_PCI,
	.ops			= &brcm_pcie_ops,
	.of_match		= brcm_pcie_ids,
	.probe			= brcm_pcie_probe,
	.ofdata_to_platdata	= brcm_pcie_ofdata_to_platdata,
	.priv_auto_alloc_size	= sizeof(struct brcm_pcie),
};