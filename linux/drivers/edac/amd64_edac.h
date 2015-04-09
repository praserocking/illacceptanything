/*
 * AMD64 class Memory Controller kernel module
 *
 * Copyright (c) 2009 SoftwareBitMaker.
 * Copyright (c) 2009 Advanced Micro Devices, Inc.
 *
 * This file may be distributed under the terms of the
 * GNU General Public License.
 *
 *	Originally Written by Thayne Harbaugh
 *
 *      Changes by Douglas "norsk" Thompson  <dougthompson@xmission.com>:
 *		- K8 CPU Revision D and greater support
 *
 *      Changes by Dave Peterson <dsp@llnl.gov> <dave_peterson@pobox.com>:
 *		- Module largely rewritten, with new (and hopefully correct)
 *		code for dealing with node and chip select interleaving,
 *		various code cleanup, and bug fixes
 *		- Added support for memory hoisting using DRAM hole address
 *		register
 *
 *	Changes by Douglas "norsk" Thompson <dougthompson@xmission.com>:
 *		-K8 Rev (1207) revision support added, required Revision
 *		specific mini-driver code to support Rev F as well as
 *		prior revisions
 *
 *	Changes by Douglas "norsk" Thompson <dougthompson@xmission.com>:
 *		-Family 10h revision support added. New PCI Device IDs,
 *		indicating new changes. Actual registers modified
 *		were slight, less than the Rev E to Rev F transition
 *		but changing the PCI Device ID was the proper thing to
 *		do, as it provides for almost automactic family
 *		detection. The mods to Rev F required more family
 *		information detection.
 *
 *	Changes/Fixes by Borislav Petkov <bp@alien8.de>:
 *		- misc fixes and code cleanups
 *
 * This module is based on the following documents
 * (available from http://www.amd.com/):
 *
 *	Title:	BIOS and Kernel Developer's Guide for AMD Athlon 64 and AMD
 *		Opteron Processors
 *	AMD publication #: 26094
 *`	Revision: 3.26
 *
 *	Title:	BIOS and Kernel Developer's Guide for AMD NPT Family 0Fh
 *		Processors
 *	AMD publication #: 32559
 *	Revision: 3.00
 *	Issue Date: May 2006
 *
 *	Title:	BIOS and Kernel Developer's Guide (BKDG) For AMD Family 10h
 *		Processors
 *	AMD publication #: 31116
 *	Revision: 3.00
 *	Issue Date: September 07, 2007
 *
 * Sections in the first 2 documents are no longer in sync with each other.
 * The Family 10h BKDG was totally re-written from scratch with a new
 * presentation model.
 * Therefore, comments that refer to a Document section might be off.
 */

#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/slab.h>
#include <linux/mmzone.h>
#include <linux/edac.h>
#include <asm/msr.h>
#include "edac_core.h"
#include "mce_amd.h"

#define amd64_debug(fmt, arg...) \
	edac_printk(KERN_DEBUG, "amd64", fmt, ##arg)

#define amd64_info(fmt, arg...) \
	edac_printk(KERN_INFO, "amd64", fmt, ##arg)

#define amd64_notice(fmt, arg...) \
	edac_printk(KERN_NOTICE, "amd64", fmt, ##arg)

#define amd64_warn(fmt, arg...) \
	edac_printk(KERN_WARNING, "amd64", fmt, ##arg)

#define amd64_err(fmt, arg...) \
	edac_printk(KERN_ERR, "amd64", fmt, ##arg)

#define amd64_mc_warn(mci, fmt, arg...) \
	edac_mc_chipset_printk(mci, KERN_WARNING, "amd64", fmt, ##arg)

#define amd64_mc_err(mci, fmt, arg...) \
	edac_mc_chipset_printk(mci, KERN_ERR, "amd64", fmt, ##arg)

/*
 * Throughout the comments in this code, the following terms are used:
 *
 *	SysAddr, DramAddr, and InputAddr
 *
 *  These terms come directly from the amd64 documentation
 * (AMD publication #26094).  They are defined as follows:
 *
 *     SysAddr:
 *         This is a physical address generated by a CPU core or a device
 *         doing DMA.  If generated by a CPU core, a SysAddr is the result of
 *         a virtual to physical address translation by the CPU core's address
 *         translation mechanism (MMU).
 *
 *     DramAddr:
 *         A DramAddr is derived from a SysAddr by subtracting an offset that
 *         depends on which node the SysAddr maps to and whether the SysAddr
 *         is within a range affected by memory hoisting.  The DRAM Base
 *         (section 3.4.4.1) and DRAM Limit (section 3.4.4.2) registers
 *         determine which node a SysAddr maps to.
 *
 *         If the DRAM Hole Address Register (DHAR) is enabled and the SysAddr
 *         is within the range of addresses specified by this register, then
 *         a value x from the DHAR is subtracted from the SysAddr to produce a
 *         DramAddr.  Here, x represents the base address for the node that
 *         the SysAddr maps to plus an offset due to memory hoisting.  See
 *         section 3.4.8 and the comments in amd64_get_dram_hole_info() and
 *         sys_addr_to_dram_addr() below for more information.
 *
 *         If the SysAddr is not affected by the DHAR then a value y is
 *         subtracted from the SysAddr to produce a DramAddr.  Here, y is the
 *         base address for the node that the SysAddr maps to.  See section
 *         3.4.4 and the comments in sys_addr_to_dram_addr() below for more
 *         information.
 *
 *     InputAddr:
 *         A DramAddr is translated to an InputAddr before being passed to the
 *         memory controller for the node that the DramAddr is associated
 *         with.  The memory controller then maps the InputAddr to a csrow.
 *         If node interleaving is not in use, then the InputAddr has the same
 *         value as the DramAddr.  Otherwise, the InputAddr is produced by
 *         discarding the bits used for node interleaving from the DramAddr.
 *         See section 3.4.4 for more information.
 *
 *         The memory controller for a given node uses its DRAM CS Base and
 *         DRAM CS Mask registers to map an InputAddr to a csrow.  See
 *         sections 3.5.4 and 3.5.5 for more information.
 */

#define EDAC_AMD64_VERSION		"3.4.0"
#define EDAC_MOD_STR			"amd64_edac"

/* Extended Model from CPUID, for CPU Revision numbers */
#define K8_REV_D			1
#define K8_REV_E			2
#define K8_REV_F			4

/* Hardware limit on ChipSelect rows per MC and processors per system */
#define NUM_CHIPSELECTS			8
#define DRAM_RANGES			8

#define ON true
#define OFF false

/*
 * PCI-defined configuration space registers
 */
#define PCI_DEVICE_ID_AMD_15H_NB_F1	0x1601
#define PCI_DEVICE_ID_AMD_15H_NB_F2	0x1602
#define PCI_DEVICE_ID_AMD_15H_M30H_NB_F1 0x141b
#define PCI_DEVICE_ID_AMD_15H_M30H_NB_F2 0x141c
#define PCI_DEVICE_ID_AMD_15H_M60H_NB_F1 0x1571
#define PCI_DEVICE_ID_AMD_15H_M60H_NB_F2 0x1572
#define PCI_DEVICE_ID_AMD_16H_NB_F1	0x1531
#define PCI_DEVICE_ID_AMD_16H_NB_F2	0x1532
#define PCI_DEVICE_ID_AMD_16H_M30H_NB_F1 0x1581
#define PCI_DEVICE_ID_AMD_16H_M30H_NB_F2 0x1582

/*
 * Function 1 - Address Map
 */
#define DRAM_BASE_LO			0x40
#define DRAM_LIMIT_LO			0x44

/*
 * F15 M30h D18F1x2[1C:00]
 */
#define DRAM_CONT_BASE			0x200
#define DRAM_CONT_LIMIT			0x204

/*
 * F15 M30h D18F1x2[4C:40]
 */
#define DRAM_CONT_HIGH_OFF		0x240

#define dram_rw(pvt, i)			((u8)(pvt->ranges[i].base.lo & 0x3))
#define dram_intlv_sel(pvt, i)		((u8)((pvt->ranges[i].lim.lo >> 8) & 0x7))
#define dram_dst_node(pvt, i)		((u8)(pvt->ranges[i].lim.lo & 0x7))

#define DHAR				0xf0
#define dhar_mem_hoist_valid(pvt)	((pvt)->dhar & BIT(1))
#define dhar_base(pvt)			((pvt)->dhar & 0xff000000)
#define k8_dhar_offset(pvt)		(((pvt)->dhar & 0x0000ff00) << 16)

					/* NOTE: Extra mask bit vs K8 */
#define f10_dhar_offset(pvt)		(((pvt)->dhar & 0x0000ff80) << 16)

#define DCT_CFG_SEL			0x10C

#define DRAM_LOCAL_NODE_BASE		0x120
#define DRAM_LOCAL_NODE_LIM		0x124

#define DRAM_BASE_HI			0x140
#define DRAM_LIMIT_HI			0x144


/*
 * Function 2 - DRAM controller
 */
#define DCSB0				0x40
#define DCSB1				0x140
#define DCSB_CS_ENABLE			BIT(0)

#define DCSM0				0x60
#define DCSM1				0x160

#define csrow_enabled(i, dct, pvt)	((pvt)->csels[(dct)].csbases[(i)] & DCSB_CS_ENABLE)

#define DRAM_CONTROL			0x78

#define DBAM0				0x80
#define DBAM1				0x180

/* Extract the DIMM 'type' on the i'th DIMM from the DBAM reg value passed */
#define DBAM_DIMM(i, reg)		((((reg) >> (4*(i)))) & 0xF)

#define DBAM_MAX_VALUE			11

#define DCLR0				0x90
#define DCLR1				0x190
#define REVE_WIDTH_128			BIT(16)
#define WIDTH_128			BIT(11)

#define DCHR0				0x94
#define DCHR1				0x194
#define DDR3_MODE			BIT(8)

#define DCT_SEL_LO			0x110
#define dct_high_range_enabled(pvt)	((pvt)->dct_sel_lo & BIT(0))
#define dct_interleave_enabled(pvt)	((pvt)->dct_sel_lo & BIT(2))

#define dct_ganging_enabled(pvt)	((boot_cpu_data.x86 == 0x10) && ((pvt)->dct_sel_lo & BIT(4)))

#define dct_data_intlv_enabled(pvt)	((pvt)->dct_sel_lo & BIT(5))
#define dct_memory_cleared(pvt)		((pvt)->dct_sel_lo & BIT(10))

#define SWAP_INTLV_REG			0x10c

#define DCT_SEL_HI			0x114

/*
 * Function 3 - Misc Control
 */
#define NBCTL				0x40

#define NBCFG				0x44
#define NBCFG_CHIPKILL			BIT(23)
#define NBCFG_ECC_ENABLE		BIT(22)

/* F3x48: NBSL */
#define F10_NBSL_EXT_ERR_ECC		0x8
#define NBSL_PP_OBS			0x2

#define SCRCTRL				0x58

#define F10_ONLINE_SPARE		0xB0
#define online_spare_swap_done(pvt, c)	(((pvt)->online_spare >> (1 + 2 * (c))) & 0x1)
#define online_spare_bad_dramcs(pvt, c)	(((pvt)->online_spare >> (4 + 4 * (c))) & 0x7)

#define F10_NB_ARRAY_ADDR		0xB8
#define F10_NB_ARRAY_DRAM		BIT(31)

/* Bits [2:1] are used to select 16-byte section within a 64-byte cacheline  */
#define SET_NB_ARRAY_ADDR(section)	(((section) & 0x3) << 1)

#define F10_NB_ARRAY_DATA		0xBC
#define F10_NB_ARR_ECC_WR_REQ		BIT(17)
#define SET_NB_DRAM_INJECTION_WRITE(inj)  \
					(BIT(((inj.word) & 0xF) + 20) | \
					F10_NB_ARR_ECC_WR_REQ | inj.bit_map)
#define SET_NB_DRAM_INJECTION_READ(inj)  \
					(BIT(((inj.word) & 0xF) + 20) | \
					BIT(16) |  inj.bit_map)


#define NBCAP				0xE8
#define NBCAP_CHIPKILL			BIT(4)
#define NBCAP_SECDED			BIT(3)
#define NBCAP_DCT_DUAL			BIT(0)

#define EXT_NB_MCA_CFG			0x180

/* MSRs */
#define MSR_MCGCTL_NBE			BIT(4)

enum amd_families {
	K8_CPUS = 0,
	F10_CPUS,
	F15_CPUS,
	F15_M30H_CPUS,
	F15_M60H_CPUS,
	F16_CPUS,
	F16_M30H_CPUS,
	NUM_FAMILIES,
};

/* Error injection control structure */
struct error_injection {
	u32	 section;
	u32	 word;
	u32	 bit_map;
};

/* low and high part of PCI config space regs */
struct reg_pair {
	u32 lo, hi;
};

/*
 * See F1x[1, 0][7C:40] DRAM Base/Limit Registers
 */
struct dram_range {
	struct reg_pair base;
	struct reg_pair lim;
};

/* A DCT chip selects collection */
struct chip_select {
	u32 csbases[NUM_CHIPSELECTS];
	u8 b_cnt;

	u32 csmasks[NUM_CHIPSELECTS];
	u8 m_cnt;
};

struct amd64_pvt {
	struct low_ops *ops;

	/* pci_device handles which we utilize */
	struct pci_dev *F1, *F2, *F3;

	u16 mc_node_id;		/* MC index of this MC node */
	u8 fam;			/* CPU family */
	u8 model;		/* ... model */
	u8 stepping;		/* ... stepping */

	int ext_model;		/* extended model value of this node */
	int channel_count;

	/* Raw registers */
	u32 dclr0;		/* DRAM Configuration Low DCT0 reg */
	u32 dclr1;		/* DRAM Configuration Low DCT1 reg */
	u32 dchr0;		/* DRAM Configuration High DCT0 reg */
	u32 dchr1;		/* DRAM Configuration High DCT1 reg */
	u32 nbcap;		/* North Bridge Capabilities */
	u32 nbcfg;		/* F10 North Bridge Configuration */
	u32 ext_nbcfg;		/* Extended F10 North Bridge Configuration */
	u32 dhar;		/* DRAM Hoist reg */
	u32 dbam0;		/* DRAM Base Address Mapping reg for DCT0 */
	u32 dbam1;		/* DRAM Base Address Mapping reg for DCT1 */

	/* one for each DCT */
	struct chip_select csels[2];

	/* DRAM base and limit pairs F1x[78,70,68,60,58,50,48,40] */
	struct dram_range ranges[DRAM_RANGES];

	u64 top_mem;		/* top of memory below 4GB */
	u64 top_mem2;		/* top of memory above 4GB */

	u32 dct_sel_lo;		/* DRAM Controller Select Low */
	u32 dct_sel_hi;		/* DRAM Controller Select High */
	u32 online_spare;	/* On-Line spare Reg */

	/* x4 or x8 syndromes in use */
	u8 ecc_sym_sz;

	/* place to store error injection parameters prior to issue */
	struct error_injection injection;

	/* cache the dram_type */
	enum mem_type dram_type;
};

enum err_codes {
	DECODE_OK	=  0,
	ERR_NODE	= -1,
	ERR_CSROW	= -2,
	ERR_CHANNEL	= -3,
};

struct err_info {
	int err_code;
	struct mem_ctl_info *src_mci;
	int csrow;
	int channel;
	u16 syndrome;
	u32 page;
	u32 offset;
};

static inline u64 get_dram_base(struct amd64_pvt *pvt, u8 i)
{
	u64 addr = ((u64)pvt->ranges[i].base.lo & 0xffff0000) << 8;

	if (boot_cpu_data.x86 == 0xf)
		return addr;

	return (((u64)pvt->ranges[i].base.hi & 0x000000ff) << 40) | addr;
}

static inline u64 get_dram_limit(struct amd64_pvt *pvt, u8 i)
{
	u64 lim = (((u64)pvt->ranges[i].lim.lo & 0xffff0000) << 8) | 0x00ffffff;

	if (boot_cpu_data.x86 == 0xf)
		return lim;

	return (((u64)pvt->ranges[i].lim.hi & 0x000000ff) << 40) | lim;
}

static inline u16 extract_syndrome(u64 status)
{
	return ((status >> 47) & 0xff) | ((status >> 16) & 0xff00);
}

static inline u8 dct_sel_interleave_addr(struct amd64_pvt *pvt)
{
	if (pvt->fam == 0x15 && pvt->model >= 0x30)
		return (((pvt->dct_sel_hi >> 9) & 0x1) << 2) |
			((pvt->dct_sel_lo >> 6) & 0x3);

	return	((pvt)->dct_sel_lo >> 6) & 0x3;
}
/*
 * per-node ECC settings descriptor
 */
struct ecc_settings {
	u32 old_nbctl;
	bool nbctl_valid;

	struct flags {
		unsigned long nb_mce_enable:1;
		unsigned long nb_ecc_prev:1;
	} flags;
};

#ifdef CONFIG_EDAC_DEBUG
int amd64_create_sysfs_dbg_files(struct mem_ctl_info *mci);
void amd64_remove_sysfs_dbg_files(struct mem_ctl_info *mci);

#else
static inline int amd64_create_sysfs_dbg_files(struct mem_ctl_info *mci)
{
	return 0;
}
static void inline amd64_remove_sysfs_dbg_files(struct mem_ctl_info *mci)
{
}
#endif

#ifdef CONFIG_EDAC_AMD64_ERROR_INJECTION
int amd64_create_sysfs_inject_files(struct mem_ctl_info *mci);
void amd64_remove_sysfs_inject_files(struct mem_ctl_info *mci);

#else
static inline int amd64_create_sysfs_inject_files(struct mem_ctl_info *mci)
{
	return 0;
}
static inline void amd64_remove_sysfs_inject_files(struct mem_ctl_info *mci)
{
}
#endif

/*
 * Each of the PCI Device IDs types have their own set of hardware accessor
 * functions and per device encoding/decoding logic.
 */
struct low_ops {
	int (*early_channel_count)	(struct amd64_pvt *pvt);
	void (*map_sysaddr_to_csrow)	(struct mem_ctl_info *mci, u64 sys_addr,
					 struct err_info *);
	int (*dbam_to_cs)		(struct amd64_pvt *pvt, u8 dct,
					 unsigned cs_mode, int cs_mask_nr);
};

struct amd64_family_type {
	const char *ctl_name;
	u16 f1_id, f3_id;
	struct low_ops ops;
};

int __amd64_read_pci_cfg_dword(struct pci_dev *pdev, int offset,
			       u32 *val, const char *func);
int __amd64_write_pci_cfg_dword(struct pci_dev *pdev, int offset,
				u32 val, const char *func);

#define amd64_read_pci_cfg(pdev, offset, val)	\
	__amd64_read_pci_cfg_dword(pdev, offset, val, __func__)

#define amd64_write_pci_cfg(pdev, offset, val)	\
	__amd64_write_pci_cfg_dword(pdev, offset, val, __func__)

int amd64_get_dram_hole_info(struct mem_ctl_info *mci, u64 *hole_base,
			     u64 *hole_offset, u64 *hole_size);

#define to_mci(k) container_of(k, struct mem_ctl_info, dev)

/* Injection helpers */
static inline void disable_caches(void *dummy)
{
	write_cr0(read_cr0() | X86_CR0_CD);
	wbinvd();
}

static inline void enable_caches(void *dummy)
{
	write_cr0(read_cr0() & ~X86_CR0_CD);
}

static inline u8 dram_intlv_en(struct amd64_pvt *pvt, unsigned int i)
{
	if (pvt->fam == 0x15 && pvt->model >= 0x30) {
		u32 tmp;
		amd64_read_pci_cfg(pvt->F1, DRAM_CONT_LIMIT, &tmp);
		return (u8) tmp & 0xF;
	}
	return (u8) (pvt->ranges[i].base.lo >> 8) & 0x7;
}

static inline u8 dhar_valid(struct amd64_pvt *pvt)
{
	if (pvt->fam == 0x15 && pvt->model >= 0x30) {
		u32 tmp;
		amd64_read_pci_cfg(pvt->F1, DRAM_CONT_BASE, &tmp);
		return (tmp >> 1) & BIT(0);
	}
	return (pvt)->dhar & BIT(0);
}

static inline u32 dct_sel_baseaddr(struct amd64_pvt *pvt)
{
	if (pvt->fam == 0x15 && pvt->model >= 0x30) {
		u32 tmp;
		amd64_read_pci_cfg(pvt->F1, DRAM_CONT_BASE, &tmp);
		return (tmp >> 11) & 0x1FFF;
	}
	return (pvt)->dct_sel_lo & 0xFFFFF800;
}