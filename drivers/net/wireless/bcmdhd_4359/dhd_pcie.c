/*
 * DHD Bus Module for PCIE
 *
 * Copyright (C) 1999-2016, Broadcom Corporation
 * 
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 * 
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 * 
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 *
 * <<Broadcom-WL-IPTag/Open:>>
 *
 * $Id: dhd_pcie.c 619254 2016-02-16 04:17:55Z $
 */


#include <typedefs.h>
#include <bcmutils.h>
#include <bcmdevs.h>
#include <siutils.h>
#include <hndsoc.h>
#include <hndpmu.h>
#include <hnd_debug.h>
#include <sbchipc.h>
#include <hnd_armtrap.h>
#if defined(DHD_DEBUG)
#include <hnd_cons.h>
#endif 
#include <dngl_stats.h>
#include <pcie_core.h>
#include <dhd.h>
#include <dhd_bus.h>
#include <dhd_flowring.h>
#include <dhd_proto.h>
#include <dhd_dbg.h>
#include <dhdioctl.h>
#include <sdiovar.h>
#include <bcmmsgbuf.h>
#include <pcicfg.h>
#include <dhd_pcie.h>
#include <bcmpcie.h>
#include <bcmendian.h>
#ifdef DHDTCPACK_SUPPRESS
#include <dhd_ip.h>
#endif 

#ifdef CUSTOMER_HW_ONE
#include <linux/htc_flags.h>
#include <linux/irq.h>
#endif 


#ifdef BCMEMBEDIMAGE
#include BCMEMBEDIMAGE
#endif 

#ifdef PCIE_OOB
#include "ftdi_sio_external.h"
#endif 

#define MEMBLOCK	2048		
#define MAX_WKLK_IDLE_CHECK	3	

#define ARMCR4REG_BANKIDX	(0x40/sizeof(uint32))
#define ARMCR4REG_BANKPDA	(0x4C/sizeof(uint32))

#if defined(SUPPORT_MULTIPLE_BOARD_REV)
	extern unsigned int system_rev;
#endif 

int otp_write = 0;

int dhd_dongle_memsize;
int dhd_dongle_ramsize;
static int dhdpcie_checkdied(dhd_bus_t *bus, char *data, uint size);
#ifdef DHD_DEBUG
static int dhdpcie_bus_readconsole(dhd_bus_t *bus);
#endif 
#if defined(DHD_FW_COREDUMP)
static int dhdpcie_mem_dump(dhd_bus_t *bus);
#endif 

static int dhdpcie_bus_membytes(dhd_bus_t *bus, bool write, ulong address, uint8 *data, uint size);
static int dhdpcie_bus_doiovar(dhd_bus_t *bus, const bcm_iovar_t *vi, uint32 actionid,
	const char *name, void *params,
	int plen, void *arg, int len, int val_size);
static int dhdpcie_bus_lpback_req(struct  dhd_bus *bus, uint32 intval);
static int dhdpcie_bus_dmaxfer_req(struct  dhd_bus *bus,
	uint32 len, uint32 srcdelay, uint32 destdelay);
static int dhdpcie_bus_download_state(dhd_bus_t *bus, bool enter);
static int _dhdpcie_download_firmware(struct dhd_bus *bus);
static int dhdpcie_download_firmware(dhd_bus_t *bus, osl_t *osh);
static int dhdpcie_bus_write_vars(dhd_bus_t *bus);
static bool dhdpcie_bus_process_mailbox_intr(dhd_bus_t *bus, uint32 intstatus);
static bool dhdpci_bus_read_frames(dhd_bus_t *bus);
static int dhdpcie_readshared(dhd_bus_t *bus);
static void dhdpcie_init_shared_addr(dhd_bus_t *bus);
static bool dhdpcie_dongle_attach(dhd_bus_t *bus);
static void dhdpcie_bus_dongle_setmemsize(dhd_bus_t *bus, int mem_size);
static void dhdpcie_bus_release_dongle(dhd_bus_t *bus, osl_t *osh,
	bool dongle_isolation, bool reset_flag);
static void dhdpcie_bus_release_malloc(dhd_bus_t *bus, osl_t *osh);
static int dhdpcie_downloadvars(dhd_bus_t *bus, void *arg, int len);
static uint8 dhdpcie_bus_rtcm8(dhd_bus_t *bus, ulong offset);
static void dhdpcie_bus_wtcm8(dhd_bus_t *bus, ulong offset, uint8 data);
static void dhdpcie_bus_wtcm16(dhd_bus_t *bus, ulong offset, uint16 data);
static uint16 dhdpcie_bus_rtcm16(dhd_bus_t *bus, ulong offset);
static void dhdpcie_bus_wtcm32(dhd_bus_t *bus, ulong offset, uint32 data);
static uint32 dhdpcie_bus_rtcm32(dhd_bus_t *bus, ulong offset);
static void dhdpcie_bus_wtcm64(dhd_bus_t *bus, ulong offset, uint64 data);
static uint64 dhdpcie_bus_rtcm64(dhd_bus_t *bus, ulong offset);
static void dhdpcie_bus_cfg_set_bar0_win(dhd_bus_t *bus, uint32 data);
static void dhdpcie_bus_reg_unmap(osl_t *osh, ulong addr, int size);
static int dhdpcie_cc_nvmshadow(dhd_bus_t *bus, struct bcmstrbuf *b);
static void dhdpcie_send_mb_data(dhd_bus_t *bus, uint32 h2d_mb_data);
static void dhd_fillup_ring_sharedptr_info(dhd_bus_t *bus, ring_info_t *ring_info);
extern void dhd_dpc_kill(dhd_pub_t *dhdp);

#ifdef BCMEMBEDIMAGE
static int dhdpcie_download_code_array(dhd_bus_t *bus);
#endif 


#define     PCI_VENDOR_ID_BROADCOM          0x14e4

static void dhd_bus_set_device_wake(struct dhd_bus *bus, bool val);
extern void wl_nddbg_wpp_log(const char *format, ...);
#ifdef PCIE_OOB
static void dhd_bus_doorbell_timeout_reset(struct dhd_bus *bus);

#define DHD_DEFAULT_DOORBELL_TIMEOUT 200	
static uint dhd_doorbell_timeout = DHD_DEFAULT_DOORBELL_TIMEOUT;

#define HOST_WAKE 4   
#define DEVICE_WAKE 5  
#define BIT_WL_REG_ON 6
#define BIT_BT_REG_ON 7

int gpio_handle_val = 0;
unsigned char gpio_port = 0;
unsigned char gpio_direction = 0;
#define OOB_PORT "ttyUSB0"
#endif 
static bool dhdpcie_check_firmware_compatible(uint32 f_api_version, uint32 h_api_version);

enum {
	IOV_INTR = 1,
	IOV_MEMBYTES,
	IOV_MEMSIZE,
	IOV_SET_DOWNLOAD_STATE,
	IOV_DEVRESET,
	IOV_VARS,
	IOV_MSI_SIM,
	IOV_PCIE_LPBK,
	IOV_CC_NVMSHADOW,
	IOV_RAMSIZE,
	IOV_RAMSTART,
	IOV_SLEEP_ALLOWED,
	IOV_PCIE_DMAXFER,
	IOV_PCIE_SUSPEND,
	IOV_PCIEREG,
	IOV_PCIECFGREG,
	IOV_PCIECOREREG,
	IOV_PCIESERDESREG,
	IOV_BAR0_SECWIN_REG,
	IOV_SBREG,
	IOV_DONGLEISOLATION,
	IOV_LTRSLEEPON_UNLOOAD,
	IOV_METADATA_DBG,
	IOV_RX_METADATALEN,
	IOV_TX_METADATALEN,
	IOV_TXP_THRESHOLD,
	IOV_BUZZZ_DUMP,
	IOV_DUMP_RINGUPD_BLOCK,
	IOV_DMA_RINGINDICES,
	IOV_DB1_FOR_MB,
	IOV_FLOW_PRIO_MAP,
#ifdef DHD_PCIE_RUNTIMEPM
	IOV_IDLETIME,
#endif 
	IOV_RXBOUND,
	IOV_TXBOUND,
	IOV_HANGREPORT,
#ifdef PCIE_OOB
	IOV_OOB_BT_REG_ON,
	IOV_OOB_ENABLE
#endif 
};


const bcm_iovar_t dhdpcie_iovars[] = {
	{"intr",	IOV_INTR,	0,	IOVT_BOOL,	0 },
	{"membytes",	IOV_MEMBYTES,	0,	IOVT_BUFFER,	2 * sizeof(int) },
	{"memsize",	IOV_MEMSIZE,	0,	IOVT_UINT32,	0 },
	{"dwnldstate",	IOV_SET_DOWNLOAD_STATE,	0,	IOVT_BOOL,	0 },
	{"vars",	IOV_VARS,	0,	IOVT_BUFFER,	0 },
	{"devreset",	IOV_DEVRESET,	0,	IOVT_BOOL,	0 },
	{"pcie_lpbk",	IOV_PCIE_LPBK,	0,	IOVT_UINT32,	0 },
	{"cc_nvmshadow", IOV_CC_NVMSHADOW, 0, IOVT_BUFFER, 0 },
	{"ramsize",	IOV_RAMSIZE,	0,	IOVT_UINT32,	0 },
	{"ramstart",	IOV_RAMSTART,	0,	IOVT_UINT32,	0 },
	{"pciereg",	IOV_PCIEREG,	0,	IOVT_BUFFER,	2 * sizeof(int32) },
	{"pciecfgreg",	IOV_PCIECFGREG,	0,	IOVT_BUFFER,	2 * sizeof(int32) },
	{"pciecorereg",	IOV_PCIECOREREG,	0,	IOVT_BUFFER,	2 * sizeof(int32) },
	{"pcieserdesreg",	IOV_PCIESERDESREG,	0,	IOVT_BUFFER,	3 * sizeof(int32) },
	{"bar0secwinreg",	IOV_BAR0_SECWIN_REG,	0,	IOVT_BUFFER,	sizeof(sdreg_t) },
	{"sbreg",	IOV_SBREG,	0,	IOVT_BUFFER,	sizeof(sdreg_t) },
	{"pcie_dmaxfer",	IOV_PCIE_DMAXFER,	0,	IOVT_BUFFER,	3 * sizeof(int32) },
	{"pcie_suspend", IOV_PCIE_SUSPEND,	0,	IOVT_UINT32,	0 },
#ifdef PCIE_OOB
	{"oob_bt_reg_on", IOV_OOB_BT_REG_ON,    0,  IOVT_UINT32,    0 },
	{"oob_enable",   IOV_OOB_ENABLE,    0,  IOVT_UINT32,    0 },
#endif 
	{"sleep_allowed",	IOV_SLEEP_ALLOWED,	0,	IOVT_BOOL,	0 },
	{"dngl_isolation", IOV_DONGLEISOLATION,	0,	IOVT_UINT32,	0 },
	{"ltrsleep_on_unload", IOV_LTRSLEEPON_UNLOOAD,	0,	IOVT_UINT32,	0 },
	{"dump_ringupdblk", IOV_DUMP_RINGUPD_BLOCK,	0,	IOVT_BUFFER,	0 },
	{"dma_ring_indices", IOV_DMA_RINGINDICES,	0,	IOVT_UINT32,	0},
	{"metadata_dbg", IOV_METADATA_DBG,	0,	IOVT_BOOL,	0 },
	{"rx_metadata_len", IOV_RX_METADATALEN,	0,	IOVT_UINT32,	0 },
	{"tx_metadata_len", IOV_TX_METADATALEN,	0,	IOVT_UINT32,	0 },
	{"db1_for_mb", IOV_DB1_FOR_MB,	0,	IOVT_UINT32,	0 },
	{"txp_thresh", IOV_TXP_THRESHOLD,	0,	IOVT_UINT32,	0 },
	{"buzzz_dump", IOV_BUZZZ_DUMP,		0,	IOVT_UINT32,	0 },
	{"flow_prio_map", IOV_FLOW_PRIO_MAP,	0,	IOVT_UINT32,	0 },
#ifdef DHD_PCIE_RUNTIMEPM
	{"idletime",    IOV_IDLETIME,   0,      IOVT_INT32,     0 },
#endif 
	{"rxbound",     IOV_RXBOUND,    0,      IOVT_UINT32,    0 },
	{"txbound",     IOV_TXBOUND,    0,      IOVT_UINT32,    0 },
	{"fw_hang_report", IOV_HANGREPORT,	0,	IOVT_BOOL,	0 },
	{NULL, 0, 0, 0, 0 }
};


#define MAX_READ_TIMEOUT	5 * 1000 * 1000

#ifndef DHD_RXBOUND
#define DHD_RXBOUND		64
#endif
#ifndef DHD_TXBOUND
#define DHD_TXBOUND		64
#endif
uint dhd_rxbound = DHD_RXBOUND;
uint dhd_txbound = DHD_TXBOUND;


int
dhd_bus_register(void)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	return dhdpcie_bus_register();
}

void
dhd_bus_unregister(void)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	dhdpcie_bus_unregister();
	return;
}


uint32 *
dhdpcie_bus_reg_map(osl_t *osh, ulong addr, int size)
{
	return (uint32 *)REG_MAP(addr, size);
}

void
dhdpcie_bus_reg_unmap(osl_t *osh, ulong addr, int size)
{
	REG_UNMAP((void*)(uintptr)addr);
	return;
}

#ifdef CONFIG_PHYS_ADDR_T_64BIT
#define PRINTF_RESOURCE	"0x%016llx"
#else
#define PRINTF_RESOURCE	"0x%08x"
#endif

dhd_bus_t* dhdpcie_bus_attach(osl_t *osh,
	volatile char *regs, volatile char *tcm, uint32 tcm_size, void *pci_dev)
{
	dhd_bus_t *bus;

	DHD_TRACE(("%s: ENTER\n", __FUNCTION__));

	do {
		if (!(bus = MALLOCZ(osh, sizeof(dhd_bus_t)))) {
			DHD_ERROR(("%s: MALLOC of dhd_bus_t failed\n", __FUNCTION__));
			break;
		}

		bus->regs = regs;
		bus->tcm = tcm;
		bus->tcm_size = tcm_size;
		bus->osh = osh;
		
		bus->dev = (struct pci_dev *)pci_dev;
#ifdef CUSTOMER_HW_ONE
		bus->bar0_addr = pci_resource_start(bus->dev, 0);
		bus->bar1_addr = pci_resource_start(bus->dev, 2);
		DHD_ERROR(("%s: bar0_addr 0x" PRINTF_RESOURCE " bar1_addr 0x" PRINTF_RESOURCE " \n",
			__FUNCTION__, bus->bar0_addr, bus->bar1_addr));
#endif 

		dll_init(&bus->const_flowring);

		
		if (!(bus->pcie_sh = MALLOCZ(osh, sizeof(pciedev_shared_t)))) {
			DHD_ERROR(("%s: MALLOC of bus->pcie_sh failed\n", __FUNCTION__));
			break;
		}

		

		if (dhdpcie_dongle_attach(bus)) {
			DHD_ERROR(("%s: dhdpcie_probe_attach failed\n", __FUNCTION__));
			break;
		}

		
		if (!(bus->dhd = dhd_attach(osh, bus, PCMSGBUF_HDRLEN))) {
			DHD_ERROR(("%s: dhd_attach failed\n", __FUNCTION__));

			break;
		}
		bus->dhd->busstate = DHD_BUS_DOWN;
		bus->db1_for_mb = TRUE;
		bus->dhd->hang_report = TRUE;
		bus->d3_ack_war_cnt = 0;

		DHD_TRACE(("%s: EXIT SUCCESS\n",
			__FUNCTION__));

		return bus;
	} while (0);

	DHD_TRACE(("%s: EXIT FAILURE\n", __FUNCTION__));

	if (bus && bus->pcie_sh) {
		MFREE(osh, bus->pcie_sh, sizeof(pciedev_shared_t));
	}

	if (bus) {
		MFREE(osh, bus, sizeof(dhd_bus_t));
	}
	return NULL;
}

uint
dhd_bus_chip(struct dhd_bus *bus)
{
	ASSERT(bus->sih != NULL);
	return bus->sih->chip;
}

uint
dhd_bus_chiprev(struct dhd_bus *bus)
{
	ASSERT(bus);
	ASSERT(bus->sih != NULL);
	return bus->sih->chiprev;
}

void *
dhd_bus_pub(struct dhd_bus *bus)
{
	return bus->dhd;
}

void *
dhd_bus_sih(struct dhd_bus *bus)
{
	return (void *)bus->sih;
}

void *
dhd_bus_txq(struct dhd_bus *bus)
{
	return &bus->txq;
}

uint dhd_bus_chip_id(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = dhdp->bus;
	return  bus->sih->chip;
}

uint dhd_bus_chiprev_id(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = dhdp->bus;
	return bus->sih->chiprev;
}

uint dhd_bus_chippkg_id(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = dhdp->bus;
	return bus->sih->chippkg;
}

uint32
dhdpcie_bus_intstatus(dhd_bus_t *bus)
{
	uint32 intstatus = 0;
	uint32 intmask = 0;

	if ((bus->dhd->busstate == DHD_BUS_SUSPEND || bus->d3_suspend_pending) && bus->wait_for_d3_ack) {
		DHD_ERROR(("%s: trying to clear intstatus during suspend (%d)"
			" or suspend in progress %d\n",
			__FUNCTION__, bus->dhd->busstate, bus->d3_suspend_pending));
		return intstatus;
	}
#ifdef CUSTOMER_HW_ONE
	if (bus->dhd->hang_was_sent)
		return intstatus;
#endif 

	if ((bus->sih->buscorerev == 6) || (bus->sih->buscorerev == 4) ||
		(bus->sih->buscorerev == 2)) {
		intstatus = dhdpcie_bus_cfg_read_dword(bus, PCIIntstatus, 4);
		dhdpcie_bus_cfg_write_dword(bus, PCIIntstatus, 4, intstatus);
		intstatus &= I_MB;
	} else {
		
		intstatus = si_corereg(bus->sih, bus->sih->buscoreidx, PCIMailBoxInt, 0, 0);

		
		intmask = si_corereg(bus->sih, bus->sih->buscoreidx, PCIMailBoxMask, 0, 0);

		si_corereg(bus->sih, bus->sih->buscoreidx, PCIMailBoxInt, bus->def_intmask,
			intstatus);

		intstatus &= intmask;

		
		if (intstatus == (uint32)-1) {
			DHD_ERROR(("%s: !!!!!!Device Removed or dead chip.\n", __FUNCTION__));
			intstatus = 0;
#ifdef CUSTOMER_HW_ONE
			
			if (bus && bus->dev && bus->dev->irq) {
				struct irq_desc *desc;

				desc = irq_to_desc(bus->dev->irq);
				if (desc != NULL && desc->depth == 0) {
					DHD_INTR(("%s disable_irq_nosync irq=%d\n", __FUNCTION__,
						bus->dev->irq));
					disable_irq_nosync(bus->dev->irq);
					bus->intdis = TRUE;
				}
			}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && 1
			dhd_os_send_hang_message(bus->dhd);
#endif 
#endif 
		}

		intstatus &= bus->def_intmask;
	}

	return intstatus;
}

int32
dhdpcie_bus_isr(dhd_bus_t *bus)
{
	uint32 intstatus = 0;

	do {
		DHD_TRACE(("%s: Enter\n", __FUNCTION__));
		
		if (!bus) {
			DHD_ERROR(("%s : bus is null pointer, exit \n", __FUNCTION__));
			break;
		}

		if (bus->dhd->dongle_reset) {
			DHD_ERROR(("%s : dongle reset , exit \n", __FUNCTION__));
			break;
		}

#ifndef CUSTOMER_HW_ONE
		if (bus->dhd->busstate == DHD_BUS_DOWN) {
			DHD_ERROR(("%s: BUS is down, not processing the interrupt \r\n",
				__FUNCTION__));
			break;
		}
#endif 

		intstatus = dhdpcie_bus_intstatus(bus);

		
		if (intstatus == 0) {
			break;
		}

		
		bus->intstatus = intstatus;


		
		bus->intrcount++;
#ifdef CUSTOMER_HW_ONE
			if (bus->ipend && bus->intdis) {
				if (bus->lastintrs == 0) {
					bus->lastintrs = bus->intrcount;
				} else if (bus->intrcount > bus->lastintrs + 10) {
					DHD_ERROR(("%s : hang recover, lastintrs %d intrcount %d\n",
						__FUNCTION__, bus->lastintrs, bus->intrcount));
					bus->lastintrs = 0;
					dhd_os_send_hang_message(bus->dhd);
					break;
				}
			} else {
				bus->lastintrs = 0;
			}
#endif 

		
		bus->ipend = TRUE;
		dhdpcie_bus_intr_disable(bus); 

#if defined(PCIE_ISR_THREAD)

		DHD_TRACE(("Calling dhd_bus_dpc() from %s\n", __FUNCTION__));
		DHD_OS_WAKE_LOCK(bus->dhd);
		while (dhd_bus_dpc(bus));
		DHD_OS_WAKE_UNLOCK(bus->dhd);
#else
		bus->dpc_sched = TRUE;
		dhd_sched_dpc(bus->dhd);     
#endif 

		DHD_TRACE(("%s: Exit Success DPC Queued\n", __FUNCTION__));
		return TRUE;

	} while (0);

	DHD_TRACE(("%s: Exit Failure\n", __FUNCTION__));
	return FALSE;
}

static bool
dhdpcie_dongle_attach(dhd_bus_t *bus)
{

	osl_t *osh = bus->osh;
	void *regsva = (void*)bus->regs;
	uint16 devid = bus->cl_devid;
	uint32 val;
	sbpcieregs_t *sbpcieregs;

	DHD_TRACE(("%s: ENTER\n", __FUNCTION__));


	bus->alp_only = TRUE;
	bus->sih = NULL;

	
	dhdpcie_bus_cfg_set_bar0_win(bus, SI_ENUM_BASE);

	
	val = OSL_PCI_READ_CONFIG(osh, PCI_CFG_VID, sizeof(uint32));
	if ((val & 0xFFFF) != VENDOR_BROADCOM) {
		DHD_ERROR(("%s : failed to read PCI configuration space!\n", __FUNCTION__));
		goto fail;
	}

	val = OSL_PCI_READ_CONFIG(osh, PCI_SPROM_CONTROL, sizeof(uint32));
	if (val == 0xffffffff) {
		DHD_ERROR(("%s : failed to read SPROM control register\n", __FUNCTION__));
		goto fail;
	}

	
	if (!(bus->sih = si_attach((uint)devid, osh, regsva, PCI_BUS, bus,
	                           &bus->vars, &bus->varsz))) {
		DHD_ERROR(("%s: si_attach failed!\n", __FUNCTION__));
		
#ifdef SUPPORT_LINKDOWN_RECOVERY
#ifdef CONFIG_ARCH_MSM
		bus->islinkdown = 1;
#endif 
#endif 
		
		goto fail;
	}

	si_setcore(bus->sih, PCIE2_CORE_ID, 0);
	sbpcieregs = (sbpcieregs_t*)(bus->regs);

	
	W_REG(osh, &sbpcieregs->configaddr, 0x4e0);
	val = R_REG(osh, &sbpcieregs->configdata);
	W_REG(osh, &sbpcieregs->configdata, val);

	
	
	if ((si_setcore(bus->sih, ARM7S_CORE_ID, 0)) ||
	    (si_setcore(bus->sih, ARMCM3_CORE_ID, 0)) ||
	    (si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) ||
	    (si_setcore(bus->sih, ARMCA7_CORE_ID, 0))) {
		bus->armrev = si_corerev(bus->sih);
	} else {
		DHD_ERROR(("%s: failed to find ARM core!\n", __FUNCTION__));
		goto fail;
	}

	if (si_setcore(bus->sih, SYSMEM_CORE_ID, 0)) {
		if (!(bus->orig_ramsize = si_sysmem_size(bus->sih))) {
			DHD_ERROR(("%s: failed to find SYSMEM memory!\n", __FUNCTION__));
			goto fail;
		}
		
		bus->dongle_ram_base = CA7_4365_RAM_BASE;
		
		bus->orig_ramsize = 0x1c0000;
	} else if (!si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
		if (!(bus->orig_ramsize = si_socram_size(bus->sih))) {
			DHD_ERROR(("%s: failed to find SOCRAM memory!\n", __FUNCTION__));
			goto fail;
		}
	} else {
		
		if (!(bus->orig_ramsize = si_tcm_size(bus->sih))) {
			DHD_ERROR(("%s: failed to find CR4-TCM memory!\n", __FUNCTION__));
			goto fail;
		}
		
		switch ((uint16)bus->sih->chip) {
		case BCM4339_CHIP_ID:
		case BCM4335_CHIP_ID:
			bus->dongle_ram_base = CR4_4335_RAM_BASE;
			break;
		case BCM4358_CHIP_ID:
		case BCM4356_CHIP_ID:
		case BCM4354_CHIP_ID:
		case BCM43567_CHIP_ID:
		case BCM43569_CHIP_ID:
		case BCM4350_CHIP_ID:
		case BCM43570_CHIP_ID:
			bus->dongle_ram_base = CR4_4350_RAM_BASE;
			break;
		case BCM4360_CHIP_ID:
			bus->dongle_ram_base = CR4_4360_RAM_BASE;
			break;
		CASE_BCM4345_CHIP:
			bus->dongle_ram_base = (bus->sih->chiprev < 6)  
				? CR4_4345_LT_C0_RAM_BASE : CR4_4345_GE_C0_RAM_BASE;
			break;
		CASE_BCM43602_CHIP:
			bus->dongle_ram_base = CR4_43602_RAM_BASE;
			break;
		case BCM4349_CHIP_GRPID:
			
			bus->dongle_ram_base = ((bus->sih->chiprev < 9) ?
			CR4_4349_RAM_BASE : CR4_4349_RAM_BASE_FROM_REV_9);
			break;
		default:
			bus->dongle_ram_base = 0;
			DHD_ERROR(("%s: WARNING: Using default ram base at 0x%x\n",
			           __FUNCTION__, bus->dongle_ram_base));
		}
	}
	bus->ramsize = bus->orig_ramsize;
	if (dhd_dongle_memsize)
		dhdpcie_bus_dongle_setmemsize(bus, dhd_dongle_memsize);

	DHD_ERROR(("DHD: dongle ram size is set to %d(orig %d) at 0x%x\n",
	           bus->ramsize, bus->orig_ramsize, bus->dongle_ram_base));

	bus->srmemsize = si_socram_srmem_size(bus->sih);


	bus->def_intmask = PCIE_MB_D2H_MB_MASK | PCIE_MB_TOPCIE_FN0_0 | PCIE_MB_TOPCIE_FN0_1;

	
	bus->intr = (bool)dhd_intr;

	bus->wait_for_d3_ack = 1;
	bus->suspended = FALSE;
	bus->force_suspend = 0;

#ifdef PCIE_OOB
	gpio_handle_val = get_handle(OOB_PORT);
	if (gpio_handle_val < 0)
	{
		DHD_ERROR(("%s: Could not get GPIO handle.\n", __FUNCTION__));
		ASSERT(FALSE);
	}

	gpio_direction = 0;
	ftdi_set_bitmode(gpio_handle_val, 0, BITMODE_BITBANG);

	
	gpio_port = 1 << BIT_WL_REG_ON | 1 << BIT_BT_REG_ON | 1 << DEVICE_WAKE;
	gpio_write_port(gpio_handle_val, gpio_port);

	gpio_direction = 1 << BIT_WL_REG_ON | 1 << BIT_BT_REG_ON | 1 << DEVICE_WAKE;
	ftdi_set_bitmode(gpio_handle_val, gpio_direction, BITMODE_BITBANG);

	bus->oob_enabled = TRUE;

	
	bus->device_wake_state = TRUE;
	dhd_bus_set_device_wake(bus, FALSE);
	dhd_bus_doorbell_timeout_reset(bus);
#endif 

	DHD_TRACE(("%s: EXIT: SUCCESS\n", __FUNCTION__));
	return 0;

fail:
	if (bus->sih != NULL) {
		si_detach(bus->sih);
		bus->sih = NULL;
	}
	DHD_TRACE(("%s: EXIT: FAILURE\n", __FUNCTION__));
	return -1;
}

int
dhpcie_bus_unmask_interrupt(dhd_bus_t *bus)
{
	dhdpcie_bus_cfg_write_dword(bus, PCIIntmask, 4, I_MB);
	return 0;
}
int
dhpcie_bus_mask_interrupt(dhd_bus_t *bus)
{
	dhdpcie_bus_cfg_write_dword(bus, PCIIntmask, 4, 0x0);
	return 0;
}

void
dhdpcie_bus_intr_enable(dhd_bus_t *bus)
{
	DHD_TRACE(("enable interrupts\n"));
	if (bus && bus->sih && !bus->islinkdown) {
		bus->intdis = FALSE;
#ifdef CUSTOMER_HW_ONE
		if (bus && bus->dev && bus->dev->irq) {
			struct irq_desc *desc = irq_to_desc(bus->dev->irq);
			if (desc != NULL && desc->depth > 0) {
				DHD_INTR(("%s enable_irq irq=%d\n", __FUNCTION__,
					bus->dev->irq));
				enable_irq(bus->dev->irq);
			}
		}
#endif 
		if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
			(bus->sih->buscorerev == 4)) {
			dhpcie_bus_unmask_interrupt(bus);
		} else {
			
			if ((bus->dhd->busstate == DHD_BUS_SUSPEND || bus->d3_suspend_pending) &&
				bus->wait_for_d3_ack) {
				return;
			}
			si_corereg(bus->sih, bus->sih->buscoreidx, PCIMailBoxMask,
				bus->def_intmask, bus->def_intmask);
		}
	}
}

void
dhdpcie_bus_intr_disable(dhd_bus_t *bus)
{
	DHD_TRACE(("%s Enter\n", __FUNCTION__));
	if (bus && bus->sih && !bus->islinkdown) {
		if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
			(bus->sih->buscorerev == 4)) {
			dhpcie_bus_mask_interrupt(bus);
		} else {
			
			if ((bus->dhd->busstate == DHD_BUS_SUSPEND || bus->d3_suspend_pending) &&
				bus->wait_for_d3_ack) {
				return;
			}
			si_corereg(bus->sih, bus->sih->buscoreidx, PCIMailBoxMask,
				bus->def_intmask, 0);
		}
#ifdef CUSTOMER_HW_ONE
		if (bus && bus->dev && bus->dev->irq) {
			struct irq_desc *desc = irq_to_desc(bus->dev->irq);
			if (desc->depth == 0) {
				DHD_INTR(("%s disable_irq_nosync irq=%d\n", __FUNCTION__,
					bus->dev->irq));
				disable_irq_nosync(bus->dev->irq);
			}
		}
#endif 
		bus->intdis = TRUE;
	}
	DHD_TRACE(("%s Exit\n", __FUNCTION__));
}

static void
dhdpcie_advertise_bus_cleanup(dhd_pub_t	 *dhdp)
{
	unsigned long flags;
	int timeleft;

	DHD_GENERAL_LOCK(dhdp, flags);
	dhdp->busstate = DHD_BUS_DOWN_IN_PROGRESS;
	DHD_GENERAL_UNLOCK(dhdp, flags);

	timeleft = dhd_os_busbusy_wait_negation(dhdp, &dhdp->dhd_bus_busy_state);
	if (timeleft == 0) {
		DHD_ERROR(("%s : Timeout due to dhd_bus_busy_state=0x%x\n",
				__FUNCTION__, dhdp->dhd_bus_busy_state));
#ifdef CUSTOMER_HW_ONE
		dhdp->dhd_bus_busy_state = 0;
		DHD_ERROR(("%s : Clear dhd_bus_busy_state=0x%x\n",
			__FUNCTION__, dhdp->dhd_bus_busy_state));
#else
		BUG_ON(1);
#endif 
	}

	return;
}

static void
dhdpcie_bus_remove_prep(dhd_bus_t *bus)
{
	unsigned long flags;
	DHD_TRACE(("%s Enter\n", __FUNCTION__));

	DHD_GENERAL_LOCK(bus->dhd, flags);
	bus->dhd->busstate = DHD_BUS_DOWN;
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	dhd_os_sdlock(bus->dhd);

	dhdpcie_bus_intr_disable(bus);
	if (!bus->dhd->dongle_isolation) {
		pcie_watchdog_reset(bus->osh, bus->sih, (sbpcieregs_t *)(bus->regs));
	}

	dhd_os_sdunlock(bus->dhd);

	DHD_TRACE(("%s Exit\n", __FUNCTION__));
}

void
dhdpcie_bus_release(dhd_bus_t *bus)
{
	bool dongle_isolation = FALSE;
	osl_t *osh = NULL;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus) {

		osh = bus->osh;
		ASSERT(osh);

		if (bus->dhd) {
			dhdpcie_advertise_bus_cleanup(bus->dhd);
			dongle_isolation = bus->dhd->dongle_isolation;
			dhdpcie_bus_remove_prep(bus);

			if (bus->intr) {
				dhdpcie_bus_intr_disable(bus);
				dhdpcie_free_irq(bus);
			}
			dhdpcie_bus_release_dongle(bus, osh, dongle_isolation, TRUE);
			dhd_detach(bus->dhd);
			dhd_free(bus->dhd);
			bus->dhd = NULL;
		}

		
		if (bus->regs) {
			dhdpcie_bus_reg_unmap(osh, (ulong)bus->regs, DONGLE_REG_MAP_SIZE);
			bus->regs = NULL;
		}
		if (bus->tcm) {
			dhdpcie_bus_reg_unmap(osh, (ulong)bus->tcm, bus->tcm_size);
			bus->tcm = NULL;
		}

		dhdpcie_bus_release_malloc(bus, osh);
		
		if (bus->pcie_sh) {
			MFREE(osh, bus->pcie_sh, sizeof(pciedev_shared_t));
			bus->pcie_sh = NULL;
		}

#ifdef DHD_DEBUG

		if (bus->console.buf != NULL)
			MFREE(osh, bus->console.buf, bus->console.bufsize);
#endif


		
		MFREE(osh, bus, sizeof(dhd_bus_t));

	}

	DHD_TRACE(("%s: Exit\n", __FUNCTION__));
} 


void
dhdpcie_bus_release_dongle(dhd_bus_t *bus, osl_t *osh, bool dongle_isolation, bool reset_flag)
{
	DHD_TRACE(("%s: Enter bus->dhd %p bus->dhd->dongle_reset %d \n", __FUNCTION__,
		bus->dhd, bus->dhd->dongle_reset));

	if ((bus->dhd && bus->dhd->dongle_reset) && reset_flag) {
		DHD_TRACE(("%s Exit\n", __FUNCTION__));
		return;
	}

	if (bus->sih) {

		if (!dongle_isolation)
			pcie_watchdog_reset(bus->osh, bus->sih, (sbpcieregs_t *)(bus->regs));

		if (bus->ltrsleep_on_unload) {
			si_corereg(bus->sih, bus->sih->buscoreidx,
				OFFSETOF(sbpcieregs_t, u.pcie2.ltr_state), ~0, 0);
		}

		if (bus->sih->buscorerev == 13)
			 pcie_serdes_iddqdisable(bus->osh, bus->sih, (sbpcieregs_t *)(bus->regs));

		if (bus->sih != NULL) {
			si_detach(bus->sih);
			bus->sih = NULL;
		}
		if (bus->vars && bus->varsz)
			MFREE(osh, bus->vars, bus->varsz);
		bus->vars = NULL;
	}

	DHD_TRACE(("%s Exit\n", __FUNCTION__));
}

uint32
dhdpcie_bus_cfg_read_dword(dhd_bus_t *bus, uint32 addr, uint32 size)
{
	uint32 data = OSL_PCI_READ_CONFIG(bus->osh, addr, size);
	return data;
}

void
dhdpcie_bus_cfg_write_dword(dhd_bus_t *bus, uint32 addr, uint32 size, uint32 data)
{
	OSL_PCI_WRITE_CONFIG(bus->osh, addr, size, data);
}

void
dhdpcie_bus_cfg_set_bar0_win(dhd_bus_t *bus, uint32 data)
{
	OSL_PCI_WRITE_CONFIG(bus->osh, PCI_BAR0_WIN, 4, data);
}

void
dhdpcie_bus_dongle_setmemsize(struct dhd_bus *bus, int mem_size)
{
	int32 min_size =  DONGLE_MIN_MEMSIZE;
	
	DHD_ERROR(("user: Restrict the dongle ram size to %d, min accepted %d\n",
		dhd_dongle_memsize, min_size));
	if ((dhd_dongle_memsize > min_size) &&
		(dhd_dongle_memsize < (int32)bus->orig_ramsize))
		bus->ramsize = dhd_dongle_memsize;
}

void
dhdpcie_bus_release_malloc(dhd_bus_t *bus, osl_t *osh)
{
	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus->dhd && bus->dhd->dongle_reset)
		return;

	if (bus->vars && bus->varsz) {
		MFREE(osh, bus->vars, bus->varsz);
		bus->vars = NULL;
	}

	DHD_TRACE(("%s: Exit\n", __FUNCTION__));
	return;

}

void dhd_bus_stop(struct dhd_bus *bus, bool enforce_mutex)
{
	uint32 status;
	unsigned long flags;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (!bus->dhd)
		return;

	if (bus->dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: already down by net_dev_reset\n", __FUNCTION__));
		goto done;
	}

	DHD_DISABLE_RUNTIME_PM(bus->dhd);

	DHD_GENERAL_LOCK(bus->dhd, flags);
	bus->dhd->busstate = DHD_BUS_DOWN;
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	dhdpcie_bus_intr_disable(bus);
	status =  dhdpcie_bus_cfg_read_dword(bus, PCIIntstatus, 4);
	dhdpcie_bus_cfg_write_dword(bus, PCIIntstatus, 4, status);

	if (!dhd_download_fw_on_driverload) {
		dhd_dpc_kill(bus->dhd);
	}

	
	dhd_os_set_ioctl_resp_timeout(IOCTL_DISABLE_TIMEOUT);
	dhd_wakeup_ioctl_event(bus->dhd, IOCTL_RETURN_ON_BUS_STOP);

done:
	return;
}

bool dhd_bus_watchdog(dhd_pub_t *dhd)
{
	unsigned long flags;
#ifdef DHD_DEBUG
	dhd_bus_t *bus;
	bus = dhd->bus;

	DHD_GENERAL_LOCK(dhd, flags);
	if (dhd->busstate == DHD_BUS_DOWN ||
			dhd->busstate == DHD_BUS_DOWN_IN_PROGRESS) {
		DHD_ERROR(("%s: Bus down, ret\n", __FUNCTION__));
		DHD_GENERAL_UNLOCK(dhd, flags);
		return TRUE;
	}
	DHD_GENERAL_UNLOCK(dhd, flags);



	
	if (dhd_console_ms != 0) {
		bus->console.count += dhd_watchdog_ms;
		if (bus->console.count >= dhd_console_ms) {
			DHD_GENERAL_LOCK(dhd, flags);
			if (dhd->busstate != DHD_BUS_DATA) {
				DHD_GENERAL_UNLOCK(dhd, flags);
			} else {
				
				dhd->dhd_bus_busy_state |= DHD_BUS_BUSY_IN_WD;
				DHD_GENERAL_UNLOCK(dhd, flags);

#ifdef DHD_PCIE_RUNTIMEPM
				dhdpcie_runtime_bus_wake(dhd, TRUE, __builtin_return_address(0));
#endif 

				bus->console.count -= dhd_console_ms;
				
				if (dhdpcie_bus_readconsole(bus) < 0)
					dhd_console_ms = 0;	

				DHD_GENERAL_LOCK(dhd, flags);
				dhd->dhd_bus_busy_state &= ~DHD_BUS_BUSY_IN_WD;
				DHD_GENERAL_UNLOCK(dhd, flags);
			}
		}
	}
#endif 

#ifdef PCIE_OOB
	
	if (dhd_doorbell_timeout != 0 && !(bus->dhd->busstate == DHD_BUS_SUSPEND) &&
		dhd_timeout_expired(&bus->doorbell_timer)) {
		dhd_bus_set_device_wake(bus, FALSE);
	}
#endif 

	return FALSE;
} 

int
dhd_bus_download_firmware(struct dhd_bus *bus, osl_t *osh,
                          char *pfw_path, char *pnv_path)
{
	int ret;

	bus->fw_path = pfw_path;
	bus->nv_path = pnv_path;

	DHD_ERROR(("%s: firmware path=%s, nvram path=%s\n",
		__FUNCTION__, bus->fw_path, bus->nv_path));

	ret = dhdpcie_download_firmware(bus, osh);

	return ret;
}

#define CONFIG_ANDROID_BCMDHD_FW_PATH "/vendor/firmware/broadcom/dhd/firmware/fw.bin.trx"
#define CONFIG_ANDROID_BCMDHD_NVRAM_PATH "/vendor/firmware/broadcom/dhd/nvrams/nvm.txt"

static int
dhdpcie_download_firmware(struct dhd_bus *bus, osl_t *osh)
{
	int ret = 0;
#if defined(BCM_REQUEST_FW)
	char fw_path[64] = "/lib/firmware/brcm/bcm";	
	char nv_path[64];		
	bus->fw_path = fw_path;
	bus->nv_path = nv_path;
	
	snprintf(bus->nv_path, sizeof(nv_path), "%s", CONFIG_ANDROID_BCMDHD_NVRAM_PATH);
	
	snprintf(bus->fw_path, sizeof(fw_path), "%s", CONFIG_ANDROID_BCMDHD_FW_PATH);
#endif 

	DHD_OS_WAKE_LOCK(bus->dhd);

	ret = _dhdpcie_download_firmware(bus);

	DHD_OS_WAKE_UNLOCK(bus->dhd);
	return ret;
}

static int
dhdpcie_download_code_file(struct dhd_bus *bus, char *pfw_path)
{
	int bcmerror = BCME_ERROR;
	int offset = 0;
	int len = 0;
	char *imgbuf = NULL;
	uint8 *memblock = NULL, *memptr;
#ifdef DHD_DEBUG
	uint32 crc32 = 0;
#endif

	int offset_end = bus->ramsize;

	DHD_ERROR(("%s: download firmware %s\n", __FUNCTION__, pfw_path));

	imgbuf = dhd_os_open_image(pfw_path);
	if (imgbuf == NULL)
		goto err;

	memptr = memblock = MALLOC(bus->dhd->osh, MEMBLOCK + DHD_SDALIGN);
	if (memblock == NULL) {
		DHD_ERROR(("%s: Failed to allocate memory %d bytes\n", __FUNCTION__, MEMBLOCK));
		goto err;
	}
	if ((uint32)(uintptr)memblock % DHD_SDALIGN)
		memptr += (DHD_SDALIGN - ((uint32)(uintptr)memblock % DHD_SDALIGN));

	
	while ((len = dhd_os_get_image_block((char*)memptr, MEMBLOCK, imgbuf))) {
		if (len < 0) {
			DHD_ERROR(("%s: dhd_os_get_image_block failed (%d)\n", __FUNCTION__, len));
			bcmerror = BCME_ERROR;
			goto err;
		}
		
		if (si_setcore(bus->sih, ARMCR4_CORE_ID, 0) ||
			si_setcore(bus->sih, ARMCA7_CORE_ID, 0)) {
			/* if address is 0, store the reset instruction to be written in 0 */
			if (offset == 0) {
				bus->resetinstr = *(((uint32*)memptr));
				
				offset += bus->dongle_ram_base;
				offset_end += offset;
			}
		}
		bcmerror = dhdpcie_bus_membytes(bus, TRUE, offset, (uint8 *)memptr, len);
		if (bcmerror) {
			DHD_ERROR(("%s: error %d on writing %d membytes at 0x%08x\n",
				__FUNCTION__, bcmerror, MEMBLOCK, offset));
			goto err;
		}
#ifdef DHD_DEBUG
		crc32 = hndcrc32(memptr, len, crc32);
#endif
		offset += MEMBLOCK;

		if (offset >= offset_end) {
			break;
		}
	}

err:
	if (memblock)
		MFREE(bus->dhd->osh, memblock, MEMBLOCK + DHD_SDALIGN);

	if (imgbuf)
		dhd_os_close_image(imgbuf);

#ifdef DHD_DEBUG
	DHD_ERROR(("%s: firmware crc32 = 0x%x\n", __FUNCTION__, crc32));
#endif
	return bcmerror;
} 

static int
dhdpcie_download_nvram(struct dhd_bus *bus)
{
	int bcmerror = BCME_ERROR;
	uint len;
	char * memblock = NULL;
	char *bufp;
	char *pnv_path;
	bool nvram_file_exists;
	bool nvram_uefi_exists = FALSE;
	bool local_alloc = FALSE;
	pnv_path = bus->nv_path;

	nvram_file_exists = ((pnv_path != NULL) && (pnv_path[0] != '\0'));

	
	len = MAX_NVRAMBUF_SIZE;
	dhd_get_download_buffer(bus->dhd, NULL, NVRAM, &memblock, &len);

	
	if ((len == 0) || (memblock[0] == '\0')) {

		if (nvram_file_exists) {
			len = MAX_NVRAMBUF_SIZE;
			dhd_get_download_buffer(bus->dhd, pnv_path, NVRAM, &memblock, &len);
			if ((len <= 0 || len > MAX_NVRAMBUF_SIZE)) {
				goto err;
			}
		}
		else {
			
			bcmerror = BCME_OK;
		}
	} else {
		nvram_uefi_exists = TRUE;
	}


	if (len > 0 && len <= MAX_NVRAMBUF_SIZE) {
		bufp = (char *) memblock;

#ifdef CACHE_FW_IMAGES
		if (bus->processed_nvram_params_len) {
			len = bus->processed_nvram_params_len;
		}

		if (!bus->processed_nvram_params_len) {
			bufp[len] = 0;
			if (nvram_uefi_exists || nvram_file_exists) {
				len = process_nvram_vars(bufp, len);
				bus->processed_nvram_params_len = len;
			}
		} else
#else
		{
			bufp[len] = 0;
			if (nvram_uefi_exists || nvram_file_exists) {
				len = process_nvram_vars(bufp, len);
			}
		}
#endif 


		if (len % 4) {
			len += 4 - (len % 4);
		}
		bufp += len;
		*bufp++ = 0;
		if (len)
			bcmerror = dhdpcie_downloadvars(bus, memblock, len + 1);
		if (bcmerror) {
			DHD_ERROR(("%s: error downloading vars: %d\n",
				__FUNCTION__, bcmerror));
		}
	}
	else {
		DHD_ERROR(("%s: error reading nvram file: %d\n",
		           __FUNCTION__, len));
		bcmerror = BCME_ERROR;
	}

err:
	if (memblock) {
		if (local_alloc) {
			MFREE(bus->dhd->osh, memblock, MAX_NVRAMBUF_SIZE);
		} else {
			dhd_free_download_buffer(bus->dhd, memblock, MAX_NVRAMBUF_SIZE);
		}
	}

	return bcmerror;
}


#ifdef BCMEMBEDIMAGE
int
dhdpcie_download_code_array(struct dhd_bus *bus)
{
	int bcmerror = -1;
	int offset = 0;
	unsigned char *p_dlarray  = NULL;
	unsigned int dlarray_size = 0;
	unsigned int downloded_len, remaining_len, len;
	char *p_dlimagename, *p_dlimagever, *p_dlimagedate;
	uint8 *memblock = NULL, *memptr;

	downloded_len = 0;
	remaining_len = 0;
	len = 0;

	p_dlarray = dlarray;
	dlarray_size = sizeof(dlarray);
	p_dlimagename = dlimagename;
	p_dlimagever  = dlimagever;
	p_dlimagedate = dlimagedate;

	if ((p_dlarray == 0) ||	(dlarray_size == 0) ||(dlarray_size > bus->ramsize) ||
		(p_dlimagename == 0) ||	(p_dlimagever  == 0) ||	(p_dlimagedate == 0))
		goto err;

	memptr = memblock = MALLOC(bus->dhd->osh, MEMBLOCK + DHD_SDALIGN);
	if (memblock == NULL) {
		DHD_ERROR(("%s: Failed to allocate memory %d bytes\n", __FUNCTION__, MEMBLOCK));
		goto err;
	}
	if ((uint32)(uintptr)memblock % DHD_SDALIGN)
		memptr += (DHD_SDALIGN - ((uint32)(uintptr)memblock % DHD_SDALIGN));

	while (downloded_len  < dlarray_size) {
		remaining_len = dlarray_size - downloded_len;
		if (remaining_len >= MEMBLOCK)
			len = MEMBLOCK;
		else
			len = remaining_len;

		memcpy(memptr, (p_dlarray + downloded_len), len);
		
		if (si_setcore(bus->sih, ARMCR4_CORE_ID, 0) ||
			si_setcore(bus->sih, SYSMEM_CORE_ID, 0)) {
			/* if address is 0, store the reset instruction to be written in 0 */
			if (offset == 0) {
				bus->resetinstr = *(((uint32*)memptr));
				
				offset += bus->dongle_ram_base;
			}
		}
		bcmerror = dhdpcie_bus_membytes(bus, TRUE, offset, (uint8 *)memptr, len);
		downloded_len += len;
		if (bcmerror) {
			DHD_ERROR(("%s: error %d on writing %d membytes at 0x%08x\n",
				__FUNCTION__, bcmerror, MEMBLOCK, offset));
			goto err;
		}
		offset += MEMBLOCK;
	}

#ifdef DHD_DEBUG
	
	{
		unsigned char *ularray = NULL;
		unsigned int uploded_len;
		uploded_len = 0;
		bcmerror = -1;
		ularray = MALLOC(bus->dhd->osh, dlarray_size);
		if (ularray == NULL)
			goto upload_err;
		
		offset = bus->dongle_ram_base;
		memset(ularray, 0xaa, dlarray_size);
		while (uploded_len  < dlarray_size) {
			remaining_len = dlarray_size - uploded_len;
			if (remaining_len >= MEMBLOCK)
				len = MEMBLOCK;
			else
				len = remaining_len;
			bcmerror = dhdpcie_bus_membytes(bus, FALSE, offset,
				(uint8 *)(ularray + uploded_len), len);
			if (bcmerror) {
				DHD_ERROR(("%s: error %d on reading %d membytes at 0x%08x\n",
					__FUNCTION__, bcmerror, MEMBLOCK, offset));
				goto upload_err;
			}

			uploded_len += len;
			offset += MEMBLOCK;
		}

		if (memcmp(p_dlarray, ularray, dlarray_size)) {
			DHD_ERROR(("%s: Downloaded image is corrupted (%s, %s, %s).\n",
				__FUNCTION__, p_dlimagename, p_dlimagever, p_dlimagedate));
			goto upload_err;

		} else
			DHD_ERROR(("%s: Download, Upload and compare succeeded (%s, %s, %s).\n",
				__FUNCTION__, p_dlimagename, p_dlimagever, p_dlimagedate));
upload_err:
		if (ularray)
			MFREE(bus->dhd->osh, ularray, dlarray_size);
	}
#endif 
err:

	if (memblock)
		MFREE(bus->dhd->osh, memblock, MEMBLOCK + DHD_SDALIGN);

	return bcmerror;
} 
#endif 


static int
dhdpcie_ramsize_read_image(struct dhd_bus *bus, char *buf, int len)
{
	int bcmerror = BCME_ERROR;
	char *imgbuf = NULL;

	if (buf == NULL || len == 0)
		goto err;

	
	if ((bus->fw_path != NULL) && (bus->fw_path[0] != '\0')) {
		imgbuf = dhd_os_open_image(bus->fw_path);
		if (imgbuf == NULL) {
			DHD_ERROR(("%s: Failed to open firmware file\n", __FUNCTION__));
			goto err;
		}

		
		if (len != dhd_os_get_image_block(buf, len, imgbuf)) {
			DHD_ERROR(("%s: Failed to read %d bytes data\n", __FUNCTION__, len));
			goto err;
		}

		bcmerror = BCME_OK;
	}

err:
	if (imgbuf)
		dhd_os_close_image(imgbuf);

	return bcmerror;
}

static int
dhdpcie_ramsize_adj(struct dhd_bus *bus)
{
	int i, search_len = 0;
	uint8 *memptr = NULL;
	uint8 *ramsizeptr = NULL;
	uint ramsizelen;
	uint32 ramsize_ptr_ptr[] = {RAMSIZE_PTR_PTR_LIST};
	hnd_ramsize_ptr_t ramsize_info;

	DHD_ERROR(("%s: Enter\n", __FUNCTION__));

	
	if (dhd_dongle_memsize) {
		DHD_ERROR(("%s: user restrict dongle ram size to %d.\n", __FUNCTION__,
			dhd_dongle_memsize));
		return BCME_ERROR;
	}

#ifndef BCMEMBEDIMAGE
	
	if ((bus->fw_path == NULL) || (bus->fw_path[0] == '\0')) {
		DHD_ERROR(("%s: no fimrware file\n", __FUNCTION__));
		return BCME_ERROR;
	}
#endif 

	
	for (i = 0; ; i++) {
		if (ramsize_ptr_ptr[i] == RAMSIZE_PTR_PTR_END)
			break;

		if (search_len < ramsize_ptr_ptr[i])
			search_len = ramsize_ptr_ptr[i];
	}

	if (!search_len)
		return BCME_ERROR;

	search_len += sizeof(hnd_ramsize_ptr_t);

	memptr = MALLOC(bus->dhd->osh, search_len);
	if (memptr == NULL) {
		DHD_ERROR(("%s: Failed to allocate memory %d bytes\n", __FUNCTION__, search_len));
		return BCME_ERROR;
	}

	
	if (dhdpcie_ramsize_read_image(bus, memptr, search_len) != BCME_OK) {
#ifdef BCMEMBEDIMAGE
		unsigned char *p_dlarray  = NULL;
		unsigned int dlarray_size = 0;
		char *p_dlimagename, *p_dlimagever, *p_dlimagedate;

		p_dlarray = dlarray;
		dlarray_size = sizeof(dlarray);
		p_dlimagename = dlimagename;
		p_dlimagever  = dlimagever;
		p_dlimagedate = dlimagedate;

		if ((p_dlarray == 0) ||	(dlarray_size == 0) || (p_dlimagename == 0) ||
			(p_dlimagever  == 0) ||	(p_dlimagedate == 0))
			goto err;

		ramsizeptr = p_dlarray;
		ramsizelen = dlarray_size;
#else
		goto err;
#endif 
	}
	else {
		ramsizeptr = memptr;
		ramsizelen = search_len;
	}

	if (ramsizeptr) {
		
		for (i = 0; ; i++) {
			if (ramsize_ptr_ptr[i] == RAMSIZE_PTR_PTR_END)
				break;

			if (ramsize_ptr_ptr[i] + sizeof(hnd_ramsize_ptr_t) > ramsizelen)
				continue;

			memcpy((char *)&ramsize_info, ramsizeptr + ramsize_ptr_ptr[i],
				sizeof(hnd_ramsize_ptr_t));

			if (ramsize_info.magic == HND_RAMSIZE_PTR_MAGIC) {
				bus->orig_ramsize = ramsize_info.ram_size;
				bus->ramsize = ramsize_info.ram_size;
				DHD_ERROR(("%s: Adjust dongle RAMSIZE to 0x%x\n", __FUNCTION__,
					bus->ramsize));
				break;
			}
		}
	}

	return BCME_OK;
err:
	if (memptr)
		MFREE(bus->dhd->osh, memptr, search_len);

	return BCME_ERROR;
} 

static int
_dhdpcie_download_firmware(struct dhd_bus *bus)
{
	int bcmerror = -1;

	bool embed = FALSE;	
	bool dlok = FALSE;	
#ifdef CUSTOMER_HW_ONE
	char *p = NULL;
#endif 

	
	if ((bus->fw_path == NULL) || (bus->fw_path[0] == '\0')) {
#ifdef BCMEMBEDIMAGE
		embed = TRUE;
#else
		DHD_ERROR(("%s: no fimrware file\n", __FUNCTION__));
		return 0;
#endif
	}
#ifdef CUSTOMER_HW_ONE
	DHD_ERROR(("%s: dongle_ram_base 0x%x\n", __FUNCTION__, bus->dongle_ram_base));
	if (bus->dongle_ram_base == CR4_4349_RAM_BASE) {
		if ((p = strstr(bus->fw_path, "4359.bin")) != NULL) {
			sprintf(p, "%s", "4359b1.bin");
		} else if ((p = strstr(bus->fw_path, "4359_apsta.bin")) != NULL) {
			sprintf(p, "%s", "4359b1_apsta.bin");
		} else if ((p = strstr(bus->fw_path, "bcm_mfg.bin")) != NULL) {
			sprintf(p, "%s", "bcm4359b1_mfg.bin");
		}
		DHD_ERROR(("%s: fw_path %s\n", __FUNCTION__, bus->fw_path));
	} else {
		DHD_ERROR(("%s: fw_path %s\n", __FUNCTION__, bus->fw_path));
	}
#endif 
	
	if (dhdpcie_ramsize_adj(bus)) {
		DHD_ERROR(("%s: dhdpcie_ramsize_adj failed\n", __FUNCTION__));
		goto err;
	}

	
	if (dhdpcie_bus_download_state(bus, TRUE)) {
		DHD_ERROR(("%s: error placing ARM core in reset\n", __FUNCTION__));
		goto err;
	}

	
	if ((bus->fw_path != NULL) && (bus->fw_path[0] != '\0')) {
		if (dhdpcie_download_code_file(bus, bus->fw_path)) {
			DHD_ERROR(("%s: dongle image file download failed\n", __FUNCTION__));
#ifdef BCMEMBEDIMAGE
			embed = TRUE;
#else
			goto err;
#endif
		} else {
			embed = FALSE;
			dlok = TRUE;
		}
	}

#ifdef BCMEMBEDIMAGE
	if (embed) {
		if (dhdpcie_download_code_array(bus)) {
			DHD_ERROR(("%s: dongle image array download failed\n", __FUNCTION__));
			goto err;
		} else {
			dlok = TRUE;
		}
	}
#else
	BCM_REFERENCE(embed);
#endif
	if (!dlok) {
		DHD_ERROR(("%s: dongle image download failed\n", __FUNCTION__));
		goto err;
	}

	
	
	


	
	if (dhdpcie_download_nvram(bus)) {
		DHD_ERROR(("%s: dongle nvram file download failed\n", __FUNCTION__));
		goto err;
	}

	
	if (dhdpcie_bus_download_state(bus, FALSE)) {
		DHD_ERROR(("%s: error getting out of ARM core reset\n", __FUNCTION__));
		goto err;
	}

	bcmerror = 0;

err:
	return bcmerror;
} 

#define CONSOLE_LINE_MAX	192

#ifdef DHD_DEBUG
static int
dhdpcie_bus_readconsole(dhd_bus_t *bus)
{
	dhd_console_t *c = &bus->console;
	uint8 line[CONSOLE_LINE_MAX], ch;
	uint32 n, idx, addr;
	int rv;

	
	if (bus->console_addr == 0)
		return -1;

	
	addr = bus->console_addr + OFFSETOF(hnd_cons_t, log);

	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr, (uint8 *)&c->log, sizeof(c->log))) < 0)
		return rv;

	
	if (c->buf == NULL) {
		c->bufsize = ltoh32(c->log.buf_size);
		if ((c->buf = MALLOC(bus->dhd->osh, c->bufsize)) == NULL)
			return BCME_NOMEM;
	}
	idx = ltoh32(c->log.idx);

	
	if (idx > c->bufsize)
		return BCME_ERROR;

	
	if (idx == c->last)
		return BCME_OK;

	
	addr = ltoh32(c->log.buf);
	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr, c->buf, c->bufsize)) < 0)
		return rv;

	while (c->last != idx) {
		for (n = 0; n < CONSOLE_LINE_MAX - 2; n++) {
			if (c->last == idx) {
				if (c->last >= n)
					c->last -= n;
				else
					c->last = c->bufsize - n;
				goto break2;
			}
			ch = c->buf[c->last];
			c->last = (c->last + 1) % c->bufsize;
			if (ch == '\n')
				break;
			line[n] = ch;
		}

		if (n > 0) {
			if (line[n - 1] == '\r')
				n--;
			line[n] = 0;
			printf("CONSOLE: %s\n", line);
#if 0 && (NDISVER >= 0x0630)
			wl_nddbg_wpp_log("CONSOLE: %s", line);
#endif

		}
	}
break2:

	return BCME_OK;
} 
#endif 

static int
dhdpcie_checkdied(dhd_bus_t *bus, char *data, uint size)
{
	int bcmerror = 0;
	uint msize = 512;
	char *mbuffer = NULL;
	char *console_buffer = NULL;
	uint maxstrlen = 256;
	char *str = NULL;
	trap_t tr;
	pciedev_shared_t *pciedev_shared = bus->pcie_sh;
	struct bcmstrbuf strbuf;
	uint32 console_ptr, console_size, console_index;
	uint8 line[CONSOLE_LINE_MAX], ch;
	uint32 n, i, addr;
	int rv;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (DHD_NOCHECKDIED_ON()) {
		return 0;
	}

	if (data == NULL) {
		size = msize;
		mbuffer = data = MALLOC(bus->dhd->osh, msize);

		if (mbuffer == NULL) {
			DHD_ERROR(("%s: MALLOC(%d) failed \n", __FUNCTION__, msize));
			bcmerror = BCME_NOMEM;
			goto done;
		}
	}

	if ((str = MALLOC(bus->dhd->osh, maxstrlen)) == NULL) {
		DHD_ERROR(("%s: MALLOC(%d) failed \n", __FUNCTION__, maxstrlen));
		bcmerror = BCME_NOMEM;
		goto done;
	}

	if (bus->shared_addr != 0) {
		
		if ((rv = dhdpcie_bus_membytes(bus, FALSE, bus->shared_addr,
			(uint8 *)pciedev_shared, sizeof(pciedev_shared_t))) < 0) {
			DHD_ERROR(("Failed to read PCIe shared struct with %d\n", rv));
			goto done;
		}
	}
	else {
		if ((bcmerror = dhdpcie_readshared(bus)) < 0)
			goto done;
	}

	bcm_binit(&strbuf, data, size);

	bcm_bprintf(&strbuf, "msgtrace address : 0x%08X\nconsole address  : 0x%08X\n",
	            pciedev_shared->msgtrace_addr, pciedev_shared->console_addr);

	if ((pciedev_shared->flags & PCIE_SHARED_ASSERT_BUILT) == 0) {
		bcm_bprintf(&strbuf, "Assrt not built in dongle\n");
	}

	if ((bus->pcie_sh->flags & (PCIE_SHARED_ASSERT|PCIE_SHARED_TRAP)) == 0) {
		bcm_bprintf(&strbuf, "No trap%s in dongle",
		          (bus->pcie_sh->flags & PCIE_SHARED_ASSERT_BUILT)
		          ?"/assrt" :"");
	} else {
		if (bus->pcie_sh->flags & PCIE_SHARED_ASSERT) {
			
			bcm_bprintf(&strbuf, "Dongle assert");
			if (bus->pcie_sh->assert_exp_addr != 0 &&
				bus->pcie_sh->assert_exp_addr != 0xffffffff) {
				str[0] = '\0';
				if ((bcmerror = dhdpcie_bus_membytes(bus, FALSE,
					bus->pcie_sh->assert_exp_addr,
					(uint8 *)str, maxstrlen)) < 0) {
					goto done;
				}

				str[maxstrlen - 1] = '\0';
				bcm_bprintf(&strbuf, " expr \"%s\"", str);
			}

			if (bus->pcie_sh->assert_file_addr != 0 &&
				bus->pcie_sh->assert_file_addr != 0xffffffff) {
				str[0] = '\0';
				if ((bcmerror = dhdpcie_bus_membytes(bus, FALSE,
					bus->pcie_sh->assert_file_addr,
					(uint8 *)str, maxstrlen)) < 0) {
					goto done;
				}

				str[maxstrlen - 1] = '\0';
				bcm_bprintf(&strbuf, " file \"%s\"", str);
			}

			bcm_bprintf(&strbuf, " line %d ",  bus->pcie_sh->assert_line);
		}

		if (bus->pcie_sh->flags & PCIE_SHARED_TRAP &&
			bus->pcie_sh->trap_addr != 0xffffffff) {
			bus->dhd->dongle_trap_occured = TRUE;
			if ((bcmerror = dhdpcie_bus_membytes(bus, FALSE,
				bus->pcie_sh->trap_addr, (uint8*)&tr, sizeof(trap_t))) < 0) {
				goto done;
			}

			bcm_bprintf(&strbuf,
			"\nTRAP type 0x%x @ epc 0x%x, cpsr 0x%x, spsr 0x%x, sp 0x%x,"
			" lp 0x%x, rpc 0x%x"
			"\nTrap offset 0x%x, r0 0x%x, r1 0x%x, r2 0x%x, r3 0x%x, "
			"r4 0x%x, r5 0x%x, r6 0x%x, r7 0x%x\n\n",
			ltoh32(tr.type), ltoh32(tr.epc), ltoh32(tr.cpsr), ltoh32(tr.spsr),
			ltoh32(tr.r13), ltoh32(tr.r14), ltoh32(tr.pc),
			ltoh32(bus->pcie_sh->trap_addr),
			ltoh32(tr.r0), ltoh32(tr.r1), ltoh32(tr.r2), ltoh32(tr.r3),
			ltoh32(tr.r4), ltoh32(tr.r5), ltoh32(tr.r6), ltoh32(tr.r7));

			addr =  bus->pcie_sh->console_addr + OFFSETOF(hnd_cons_t, log);
			if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr,
				(uint8 *)&console_ptr, sizeof(console_ptr))) < 0) {
				goto printbuf;
			}

			addr =  bus->pcie_sh->console_addr + OFFSETOF(hnd_cons_t, log.buf_size);
			if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr,
				(uint8 *)&console_size, sizeof(console_size))) < 0) {
				goto printbuf;
			}

			addr =  bus->pcie_sh->console_addr + OFFSETOF(hnd_cons_t, log.idx);
			if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr,
				(uint8 *)&console_index, sizeof(console_index))) < 0) {
				goto printbuf;
			}

			console_ptr = ltoh32(console_ptr);
			console_size = ltoh32(console_size);
			console_index = ltoh32(console_index);

			if (console_size > CONSOLE_BUFFER_MAX ||
				!(console_buffer = MALLOC(bus->dhd->osh, console_size))) {
				goto printbuf;
			}

			if ((rv = dhdpcie_bus_membytes(bus, FALSE, console_ptr,
				(uint8 *)console_buffer, console_size)) < 0) {
				goto printbuf;
			}

			for (i = 0, n = 0; i < console_size; i += n + 1) {
				for (n = 0; n < CONSOLE_LINE_MAX - 2; n++) {
					ch = console_buffer[(console_index + i + n) % console_size];
					if (ch == '\n')
						break;
					line[n] = ch;
				}


				if (n > 0) {
					if (line[n - 1] == '\r')
						n--;
					line[n] = 0;

					printf("CONSOLE: %s\n", line);
				}
			}
		}
	}

printbuf:
	if (bus->pcie_sh->flags & (PCIE_SHARED_ASSERT | PCIE_SHARED_TRAP)) {
		printf("%s: %s\n", __FUNCTION__, strbuf.origbuf);

		
		dhd_wakeup_ioctl_event(bus->dhd, IOCTL_RETURN_ON_TRAP);

#if defined(DHD_FW_COREDUMP)
		
		if (bus->dhd->memdump_enabled) {
			bus->dhd->memdump_type = DUMP_TYPE_DONGLE_TRAP;
#ifdef CUSTOMER_HW_ONE
			dhd_bus_mem_dump(bus->dhd);
#else
			dhdpcie_mem_dump(bus);
#endif 
		}
#endif 


	}

done:
	if (mbuffer)
		MFREE(bus->dhd->osh, mbuffer, msize);
	if (str)
		MFREE(bus->dhd->osh, str, maxstrlen);

	if (console_buffer)
		MFREE(bus->dhd->osh, console_buffer, console_size);

	return bcmerror;
} 


void dhdpcie_mem_dump_bugcheck(dhd_bus_t *bus, uint8 *buf)
{
	int ret = 0;
	int size; 
	int start; 
	int read_size = 0; 
	uint8 *databuf = buf;

	if (bus == NULL) {
		return;
	}

	start = bus->dongle_ram_base;
	
	size = bus->ramsize;
	
	while (size)
	{
		read_size = MIN(MEMBLOCK, size);
		if ((ret = dhdpcie_bus_membytes(bus, FALSE, start, databuf, read_size))) {
			return;
		}

		
		size -= read_size;
		start += read_size;
		databuf += read_size;
	}
	bus->dhd->soc_ram = buf;
	bus->dhd->soc_ram_length = bus->ramsize;
	return;
}


#if defined(DHD_FW_COREDUMP)
static int
dhdpcie_mem_dump(dhd_bus_t *bus)
{
	int ret = BCME_OK;
	int size; 
	int start = bus->dongle_ram_base; 
	int read_size = 0; 
	uint8 *buf = NULL, *databuf = NULL;

	if(bus->islinkdown) {
		DHD_ERROR(("%s: PCIe link was down\n", __FUNCTION__));
		return BCME_ERROR;
	}

	
	size = bus->ramsize;
#if defined(CONFIG_DHD_USE_STATIC_BUF) && defined(DHD_USE_STATIC_MEMDUMP)
	buf = DHD_OS_PREALLOC(bus->dhd, DHD_PREALLOC_MEMDUMP_BUF, size);
	bzero(buf, size);
#else
	buf = MALLOC(bus->dhd->osh, size);
#endif 
	if (!buf) {
		DHD_ERROR(("%s: Out of memory (%d bytes)\n", __FUNCTION__, size));
		return BCME_ERROR;
	}

	
	DHD_TRACE_HW4(("Dump dongle memory"));
	databuf = buf;
	while (size)
	{
		read_size = MIN(MEMBLOCK, size);
		if ((ret = dhdpcie_bus_membytes(bus, FALSE, start, databuf, read_size)))
		{
			DHD_ERROR(("%s: Error membytes %d\n", __FUNCTION__, ret));
			if (buf) {
				MFREE(bus->dhd->osh, buf, size);
			}
			return BCME_ERROR;
		}
		DHD_TRACE(("."));

		
		size -= read_size;
		start += read_size;
		databuf += read_size;
	}

	DHD_TRACE_HW4(("%s FUNC: Copy fw image to the embedded buffer \n", __FUNCTION__));

	dhd_save_fwdump(bus->dhd, buf, bus->ramsize);
	ret = dhd_schedule_memdump(bus->dhd, buf, bus->ramsize);
	return ret;
}

int
dhd_bus_mem_dump(dhd_pub_t *dhdp)
{
	dhd_bus_t *bus = dhdp->bus;
#ifdef CUSTOMER_HW_ONE
	int ret = 0;
	unsigned long flags;

	
	if (!(get_radio_flag() & BIT(3))) {
		DHD_ERROR(("%s: radio flag 8 8 disabled\n", __FUNCTION__));
		if (bus->dhd->dongle_trap_occured) {
			DHD_ERROR(("%s dongle trap occured issue hang procedure.\n", __FUNCTION__));
			dhd_os_send_hang_message(bus->dhd);
		}
		return BCME_OK;
	}

	DHD_GENERAL_LOCK(bus->dhd, flags);
	if (bus->dhd->busstate == DHD_BUS_DOWN ||
		bus->dhd->busstate == DHD_BUS_DOWN_IN_PROGRESS ||
		bus->dhd->busstate == DHD_BUS_SUSPEND ||
		bus->dhd->memdump_in_progress) {
		DHD_ERROR(("%s: returning as busstate=%d, memdump_in_progress=%d\n",
			__FUNCTION__, bus->dhd->busstate, bus->dhd->memdump_in_progress));
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
		ret = -ENODEV;
		return ret;
	}
	bus->dhd->memdump_in_progress = 1;
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	if ((ret = dhdpcie_mem_dump(bus)) != 0) {
		DHD_ERROR(("%s: dhdpcie_mem_dump error=%d\n", __FUNCTION__, ret));
		DHD_GENERAL_LOCK(bus->dhd, flags);
		bus->dhd->memdump_in_progress = 0;
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
	}

	return ret;
#else
	if (bus->suspended) {
		DHD_ERROR(("%s: Bus is suspend so skip\n", __FUNCTION__));
		return 0;
	}

	return dhdpcie_mem_dump(bus);
#endif 
}
#endif 

int
dhd_socram_dump(dhd_bus_t *bus)
{
#if defined(DHD_FW_COREDUMP)
#ifdef CUSTOMER_HW_ONE
	return (dhd_bus_mem_dump(bus->dhd));
#else
	return (dhdpcie_mem_dump(bus));
#endif 
#else
	return -1;
#endif
}

static int
dhdpcie_bus_membytes(dhd_bus_t *bus, bool write, ulong address, uint8 *data, uint size)
{
	uint dsize;
	int detect_endian_flag = 0x01;
	bool little_endian;

	if (write && bus->islinkdown) {
		DHD_ERROR(("%s: PCIe link was down\n", __FUNCTION__));
		return BCME_ERROR;
	}
#ifdef CUSTOMER_HW_ONE
	if (address + size > bus->tcm_size) {
		DHD_ERROR(("%s: invalid addr %lx size %d exceeds %x",
			__FUNCTION__, address, size, bus->tcm_size));
		return BCME_BADADDR;
	}
#endif 
	
	little_endian = *(char *)&detect_endian_flag;


	
	dsize = sizeof(uint64);

	
	if (write) {
		while (size) {
			if (size >= sizeof(uint64) && little_endian &&
#ifdef CONFIG_64BIT
				!(address % 8) &&
#endif 
				1) {
				dhdpcie_bus_wtcm64(bus, address, *((uint64 *)data));
			} else {
				dsize = sizeof(uint8);
				dhdpcie_bus_wtcm8(bus, address, *data);
			}

			
			if ((size -= dsize)) {
				data += dsize;
				address += dsize;
			}
		}
	} else {
		while (size) {
			if (size >= sizeof(uint64) && little_endian &&
#ifdef CONFIG_64BIT
				!(address % 8) &&
#endif 
				1) {
				*(uint64 *)data = dhdpcie_bus_rtcm64(bus, address);
			} else {
				dsize = sizeof(uint8);
				*data = dhdpcie_bus_rtcm8(bus, address);
			}

			
			if ((size -= dsize) > 0) {
				data += dsize;
				address += dsize;
			}
		}
	}
	return BCME_OK;
} 

int BCMFASTPATH
dhd_bus_schedule_queue(struct dhd_bus  *bus, uint16 flow_id, bool txs)
{
	flow_ring_node_t *flow_ring_node;
	int ret = BCME_OK;
#ifdef DHD_LOSSLESS_ROAMING
	dhd_pub_t *dhdp = bus->dhd;
#endif
	DHD_INFO(("%s: flow_id is %d\n", __FUNCTION__, flow_id));

	
	if (flow_id >= bus->max_sub_queues) {
		DHD_ERROR(("%s: flow_id is invalid %d, max %d\n", __FUNCTION__,
			flow_id, bus->max_sub_queues));
		return 0;
	}

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flow_id);

#ifdef DHD_LOSSLESS_ROAMING
	if ((dhdp->dequeue_prec_map & (1 << flow_ring_node->flow_info.tid)) == 0) {
		DHD_INFO(("%s: tid %d is not in precedence map. block scheduling\n",
			__FUNCTION__, flow_ring_node->flow_info.tid));
		return BCME_OK;
	}
#endif 

	{
		unsigned long flags;
		void *txp = NULL;
		flow_queue_t *queue;
#ifdef DHD_LOSSLESS_ROAMING
		struct ether_header *eh;
		uint8 *pktdata;
#endif 

		queue = &flow_ring_node->queue; 

		DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);

		if (flow_ring_node->status != FLOW_RING_STATUS_OPEN) {
			DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
			return BCME_NOTREADY;
		}

		while ((txp = dhd_flow_queue_dequeue(bus->dhd, queue)) != NULL) {
			PKTORPHAN(txp);


#ifdef DHDTCPACK_SUPPRESS
			if (bus->dhd->tcpack_sup_mode != TCPACK_SUP_HOLD) {
				ret = dhd_tcpack_check_xmit(bus->dhd, txp);
				if (ret != BCME_OK) {
					DHD_ERROR(("%s: dhd_tcpack_check_xmit() error.\n",
						__FUNCTION__));
				}
			}
#endif 
#ifdef DHD_LOSSLESS_ROAMING
			pktdata = (uint8 *)PKTDATA(OSH_NULL, txp);
			eh = (struct ether_header *) pktdata;
			if (eh->ether_type == hton16(ETHER_TYPE_802_1X)) {
				uint8 prio = (uint8)PKTPRIO(txp);

				
				if (prio == PRIO_8021D_NC) {
					PKTSETPRIO(txp, PRIO_8021D_BE);
				}
			}
#endif 

			
			ret = dhd_prot_txdata(bus->dhd, txp, flow_ring_node->flow_info.ifindex);
			if (ret != BCME_OK) { 
				DHD_INFO(("%s: Reinsert %d\n", __FUNCTION__, ret));
				dhd_prot_txdata_write_flush(bus->dhd, flow_id, FALSE);
				
				ret = dhd_flow_queue_reinsert(bus->dhd, queue, txp);
				DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

				
				return ret;
			}
		}

		dhd_prot_txdata_write_flush(bus->dhd, flow_id, FALSE);

		DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
	}

	return ret;
} 

int BCMFASTPATH
dhd_bus_txdata(struct dhd_bus *bus, void *txp, uint8 ifidx)
{
	uint16 flowid;
	flow_queue_t *queue;
	flow_ring_node_t *flow_ring_node;
	unsigned long flags;
	int ret = BCME_OK;
	void *txp_pend = NULL;

	if (!bus->dhd->flowid_allocator) {
		DHD_ERROR(("%s: Flow ring not intited yet  \n", __FUNCTION__));
		goto toss;
	}

	flowid = DHD_PKT_GET_FLOWID(txp);

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flowid);

	DHD_TRACE(("%s: pkt flowid %d, status %d active %d\n",
		__FUNCTION__, flowid, flow_ring_node->status,
		flow_ring_node->active));

	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);
	if ((flowid >= bus->dhd->num_flow_rings) ||
		(!flow_ring_node->active) ||
		(flow_ring_node->status == FLOW_RING_STATUS_DELETE_PENDING) ||
		(flow_ring_node->status == FLOW_RING_STATUS_STA_FREEING)) {
		DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
		DHD_INFO(("%s: Dropping pkt flowid %d, status %d active %d\n",
			__FUNCTION__, flowid, flow_ring_node->status,
			flow_ring_node->active));
		ret = BCME_ERROR;
		goto toss;
	}

	queue = &flow_ring_node->queue; 

	if ((ret = dhd_flow_queue_enqueue(bus->dhd, queue, txp)) != BCME_OK) {
		txp_pend = txp;
	}

	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	if (flow_ring_node->status) {
		DHD_INFO(("%s: Enq pkt flowid %d, status %d active %d\n",
			__FUNCTION__, flowid, flow_ring_node->status,
			flow_ring_node->active));
		if (txp_pend) {
			txp = txp_pend;
			goto toss;
		}
		return BCME_OK;
	}
	ret = dhd_bus_schedule_queue(bus, flowid, FALSE); 

	
	if (txp_pend) {
		DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);

		if ((ret = dhd_flow_queue_enqueue(bus->dhd, queue, txp_pend)) != BCME_OK) {
			DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
			txp = txp_pend;
			goto toss;
		}

		DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
	}

	return ret;

toss:
	DHD_INFO(("%s: Toss %d\n", __FUNCTION__, ret));
#ifdef CUSTOMER_HW_31_2
	dhd_txcomplete(bus->dhd->osh, txp, TRUE);
#else
	PKTCFREE(bus->dhd->osh, txp, TRUE);
#endif
	return ret;
} 


void
dhd_bus_stop_queue(struct dhd_bus *bus)
{
	dhd_txflowcontrol(bus->dhd, ALL_INTERFACES, ON);
	bus->bus_flowctrl = TRUE;
}

void
dhd_bus_start_queue(struct dhd_bus *bus)
{
	dhd_txflowcontrol(bus->dhd, ALL_INTERFACES, OFF);
	bus->bus_flowctrl = TRUE;
}

#if defined(DHD_DEBUG)
int dhd_bus_console_in(dhd_pub_t *dhd, uchar *msg, uint msglen)
{
	dhd_bus_t *bus = dhd->bus;
	uint32 addr, val;
	int rv;
	
	if (bus->console_addr == 0)
		return BCME_UNSUPPORTED;

	
	if (bus->dhd->dongle_reset) {
		dhd_os_sdunlock(bus->dhd);
		return BCME_NOTREADY;
	}

	
	addr = bus->console_addr + OFFSETOF(hnd_cons_t, cbuf_idx);
	val = htol32(0);
	if ((rv = dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *)&val, sizeof(val))) < 0)
		goto done;

	
	addr = bus->console_addr + OFFSETOF(hnd_cons_t, cbuf);
	if ((rv = dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *)msg, msglen)) < 0)
		goto done;

	
	addr = bus->console_addr + OFFSETOF(hnd_cons_t, vcons_in);
	val = htol32(msglen);
	if ((rv = dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *)&val, sizeof(val))) < 0)
		goto done;

	
	dhdpcie_send_mb_data(bus, H2D_HOST_CONS_INT);
done:
	return rv;
} 
#endif 

void BCMFASTPATH
dhd_bus_rx_frame(struct dhd_bus *bus, void* pkt, int ifidx, uint pkt_count)
{
	dhd_rx_frame(bus->dhd, ifidx, pkt, pkt_count, 0);
}

void
dhdpcie_bus_wtcm8(dhd_bus_t *bus, ulong offset, uint8 data)
{
#if defined(CUSTOMER_HW_ONE) && defined(PCIE_BUS_IO)
	if (!PHYSADDRISPCIE(bus->bar1_addr + offset)) {
		DHD_ERROR(("%s: invalid addr=0x" PRINTF_RESOURCE ", size=%zd\n",
			__FUNCTION__, bus->bar1_addr + offset, sizeof(data)));
		return;
	}
	writeb((uint8)(data), (volatile uint8 *)(bus->tcm + offset));
#else
	*(volatile uint8 *)(bus->tcm + offset) = (uint8)data;
#endif 
}

uint8
dhdpcie_bus_rtcm8(dhd_bus_t *bus, ulong offset)
{
	volatile uint8 data;

#if defined(CUSTOMER_HW_ONE) && defined(PCIE_BUS_IO)
	if (!PHYSADDRISPCIE(bus->bar1_addr + offset)) {
		DHD_ERROR(("%s: invalid addr=0x" PRINTF_RESOURCE ", size=%zd\n",
			__FUNCTION__, bus->bar1_addr + offset, sizeof(data)));
		return -1;
	}
	data = readb((volatile uint8 *)(bus->tcm + offset));
#else
		data = *(volatile uint8 *)(bus->tcm + offset);
#endif 

	return data;
}

void
dhdpcie_bus_wtcm32(dhd_bus_t *bus, ulong offset, uint32 data)
{
#if defined(CUSTOMER_HW_ONE) && defined(PCIE_BUS_IO)
	if (!PHYSADDRISPCIE(bus->bar1_addr + offset + 3)) {
		DHD_ERROR(("%s: invalid addr=0x" PRINTF_RESOURCE ", size=%zd\n",
			__FUNCTION__, bus->bar1_addr + offset, sizeof(data)));
		return;
	}
	writel((uint32)(data), (volatile uint32 *)(bus->tcm + offset));
#else
	*(volatile uint32 *)(bus->tcm + offset) = (uint32)data;
#endif 
}
void
dhdpcie_bus_wtcm16(dhd_bus_t *bus, ulong offset, uint16 data)
{
#if defined(CUSTOMER_HW_ONE) && defined(PCIE_BUS_IO)
	if (!PHYSADDRISPCIE(bus->bar1_addr + offset + 1)) {
		DHD_ERROR(("%s: invalid addr=0x" PRINTF_RESOURCE ", size=%zd\n",
			__FUNCTION__, bus->bar1_addr + offset, sizeof(data)));
		return;
	}
	writew((uint16)(data), (volatile uint16 *)(bus->tcm + offset));
#else
	*(volatile uint16 *)(bus->tcm + offset) = (uint16)data;
#endif 
}
void
dhdpcie_bus_wtcm64(dhd_bus_t *bus, ulong offset, uint64 data)
{
#if defined(CUSTOMER_HW_ONE) && defined(PCIE_BUS_IO)
	if (!PHYSADDRISPCIE(bus->bar1_addr + offset + 7)) {
		DHD_ERROR(("%s: invalid addr=0x" PRINTF_RESOURCE ", size=%zd\n",
			__FUNCTION__, bus->bar1_addr + offset, sizeof(data)));
		return;
	}
	writeq((uint64)(data), (volatile uint64 *)(bus->tcm + offset));
#else
	*(volatile uint64 *)(bus->tcm + offset) = (uint64)data;
#endif 
}

uint16
dhdpcie_bus_rtcm16(dhd_bus_t *bus, ulong offset)
{
	volatile uint16 data;

#if defined(CUSTOMER_HW_ONE) && defined(PCIE_BUS_IO)
	if (!PHYSADDRISPCIE(bus->bar1_addr + offset + 1)) {
		DHD_ERROR(("%s: invalid addr=0x" PRINTF_RESOURCE ", size=%zd\n",
			__FUNCTION__, bus->bar1_addr + offset, sizeof(data)));
		return -1;
	}
	data = readw((volatile uint16 *)(bus->tcm + offset));
#else
		data = *(volatile uint16 *)(bus->tcm + offset);
#endif 

	return data;
}

uint32
dhdpcie_bus_rtcm32(dhd_bus_t *bus, ulong offset)
{
	volatile uint32 data;

#if defined(CUSTOMER_HW_ONE) && defined(PCIE_BUS_IO)
	if (!PHYSADDRISPCIE(bus->bar1_addr + offset + 3)) {
		DHD_ERROR(("%s: invalid addr=0x" PRINTF_RESOURCE ", size=%zd\n",
			__FUNCTION__, bus->bar1_addr + offset, sizeof(data)));
		return -1;
	}
	data = readl((volatile uint32 *)(bus->tcm + offset));
#else
		data = *(volatile uint32 *)(bus->tcm + offset);
#endif 

	return data;
}

uint64
dhdpcie_bus_rtcm64(dhd_bus_t *bus, ulong offset)
{
	volatile uint64 data;

#if defined(CUSTOMER_HW_ONE) && defined(PCIE_BUS_IO)
	if (!PHYSADDRISPCIE(bus->bar1_addr + offset + 7)) {
		DHD_ERROR(("%s: invalid addr=0x" PRINTF_RESOURCE ", size=%zd\n",
			__FUNCTION__, bus->bar1_addr + offset, sizeof(data)));
		return -1;
	}
	data = readq((volatile uint64 *)(bus->tcm + offset));
#else
		data = *(volatile uint64 *)(bus->tcm + offset);
#endif 

	return data;
}

void
dhd_bus_cmn_writeshared(dhd_bus_t *bus, void *data, uint32 len, uint8 type, uint16 ringid)
{
	uint64 long_data;
	ulong tcm_offset;

	DHD_INFO(("%s: writing to dongle type %d len %d\n", __FUNCTION__, type, len));

	if (bus->islinkdown) {
		DHD_ERROR(("%s: PCIe link was down\n", __func__));
		return;
	}

	switch (type) {
		case D2H_DMA_SCRATCH_BUF:
		{
			pciedev_shared_t *sh = (pciedev_shared_t*)bus->shared_addr;
			long_data = HTOL64(*(uint64 *)data);
			tcm_offset = (ulong)&(sh->host_dma_scratch_buffer);
			dhdpcie_bus_membytes(bus, TRUE, tcm_offset, (uint8*) &long_data, len);
			prhex(__FUNCTION__, data, len);
			break;
		}

		case D2H_DMA_SCRATCH_BUF_LEN:
		{
			pciedev_shared_t *sh = (pciedev_shared_t*)bus->shared_addr;
			tcm_offset = (ulong)&(sh->host_dma_scratch_buffer_len);
			dhdpcie_bus_wtcm32(bus, tcm_offset, (uint32) HTOL32(*(uint32 *)data));
			prhex(__FUNCTION__, data, len);
			break;
		}

		case H2D_DMA_INDX_WR_BUF:
		{
			pciedev_shared_t *shmem = (pciedev_shared_t *)bus->pcie_sh;

			long_data = HTOL64(*(uint64 *)data);
			tcm_offset = (ulong)shmem->rings_info_ptr;
			tcm_offset += OFFSETOF(ring_info_t, h2d_w_idx_hostaddr);
			dhdpcie_bus_membytes(bus, TRUE, tcm_offset, (uint8*) &long_data, len);
			prhex(__FUNCTION__, data, len);
			break;
		}

		case H2D_DMA_INDX_RD_BUF:
		{
			pciedev_shared_t *shmem = (pciedev_shared_t *)bus->pcie_sh;
			long_data = HTOL64(*(uint64 *)data);
			tcm_offset = (ulong)shmem->rings_info_ptr;
			tcm_offset += OFFSETOF(ring_info_t, h2d_r_idx_hostaddr);
			dhdpcie_bus_membytes(bus, TRUE, tcm_offset, (uint8*) &long_data, len);
			prhex(__FUNCTION__, data, len);
			break;
		}

		case D2H_DMA_INDX_WR_BUF:
		{
			pciedev_shared_t *shmem = (pciedev_shared_t *)bus->pcie_sh;
			long_data = HTOL64(*(uint64 *)data);
			tcm_offset = (ulong)shmem->rings_info_ptr;
			tcm_offset += OFFSETOF(ring_info_t, d2h_w_idx_hostaddr);
			dhdpcie_bus_membytes(bus, TRUE, tcm_offset, (uint8*) &long_data, len);
			prhex(__FUNCTION__, data, len);
			break;
		}

		case D2H_DMA_INDX_RD_BUF:
		{
			pciedev_shared_t *shmem = (pciedev_shared_t *)bus->pcie_sh;
			long_data = HTOL64(*(uint64 *)data);
			tcm_offset = (ulong)shmem->rings_info_ptr;
			tcm_offset += OFFSETOF(ring_info_t, d2h_r_idx_hostaddr);
			dhdpcie_bus_membytes(bus, TRUE, tcm_offset, (uint8*) &long_data, len);
			prhex(__FUNCTION__, data, len);
			break;
		}

		case RING_ITEM_LEN:
			tcm_offset = bus->ring_sh[ringid].ring_mem_addr;
			tcm_offset += OFFSETOF(ring_mem_t, len_items);
			dhdpcie_bus_wtcm16(bus, tcm_offset, (uint16) HTOL16(*(uint16 *)data));
			break;

		case RING_MAX_ITEMS:
			tcm_offset = bus->ring_sh[ringid].ring_mem_addr;
			tcm_offset += OFFSETOF(ring_mem_t, max_item);
			dhdpcie_bus_wtcm16(bus, tcm_offset, (uint16) HTOL16(*(uint16 *)data));
			break;

		case RING_BUF_ADDR:
			long_data = HTOL64(*(uint64 *)data);
			tcm_offset = bus->ring_sh[ringid].ring_mem_addr;
			tcm_offset += OFFSETOF(ring_mem_t, base_addr);
			dhdpcie_bus_membytes(bus, TRUE, tcm_offset, (uint8 *) &long_data, len);
			prhex(__FUNCTION__, data, len);
			break;

		case RING_WR_UPD:
			tcm_offset = bus->ring_sh[ringid].ring_state_w;
			dhdpcie_bus_wtcm16(bus, tcm_offset, (uint16) HTOL16(*(uint16 *)data));
			break;

		case RING_RD_UPD:
			tcm_offset = bus->ring_sh[ringid].ring_state_r;
			dhdpcie_bus_wtcm16(bus, tcm_offset, (uint16) HTOL16(*(uint16 *)data));
			break;

		case D2H_MB_DATA:
			dhdpcie_bus_wtcm32(bus, bus->d2h_mb_data_ptr_addr,
				(uint32) HTOL32(*(uint32 *)data));
			break;

		case H2D_MB_DATA:
			dhdpcie_bus_wtcm32(bus, bus->h2d_mb_data_ptr_addr,
				(uint32) HTOL32(*(uint32 *)data));
			break;

		default:
			break;
	}
} 

void
dhd_bus_cmn_readshared(dhd_bus_t *bus, void* data, uint8 type, uint16 ringid)
{
	ulong tcm_offset;

	switch (type) {
		case RING_WR_UPD:
			tcm_offset = bus->ring_sh[ringid].ring_state_w;
			*(uint16*)data = LTOH16(dhdpcie_bus_rtcm16(bus, tcm_offset));
			break;
		case RING_RD_UPD:
			tcm_offset = bus->ring_sh[ringid].ring_state_r;
			*(uint16*)data = LTOH16(dhdpcie_bus_rtcm16(bus, tcm_offset));
			break;
		case TOTAL_LFRAG_PACKET_CNT:
		{
			pciedev_shared_t *sh = (pciedev_shared_t*)bus->shared_addr;
			*(uint16*)data = LTOH16(dhdpcie_bus_rtcm16(bus,
				(ulong) &sh->total_lfrag_pkt_cnt));
			break;
		}
		case H2D_MB_DATA:
			*(uint32*)data = LTOH32(dhdpcie_bus_rtcm32(bus, bus->h2d_mb_data_ptr_addr));
			break;
		case D2H_MB_DATA:
			*(uint32*)data = LTOH32(dhdpcie_bus_rtcm32(bus, bus->d2h_mb_data_ptr_addr));
			break;
		case MAX_HOST_RXBUFS:
		{
			pciedev_shared_t *sh = (pciedev_shared_t*)bus->shared_addr;
			*(uint16*)data = LTOH16(dhdpcie_bus_rtcm16(bus,
				(ulong) &sh->max_host_rxbufs));
			break;
		}
		default :
			break;
	}
}

uint32 dhd_bus_get_sharedflags(dhd_bus_t *bus)
{
	return ((pciedev_shared_t*)bus->pcie_sh)->flags;
}

void
dhd_bus_clearcounts(dhd_pub_t *dhdp)
{
}

int
dhd_bus_iovar_op(dhd_pub_t *dhdp, const char *name,
                 void *params, int plen, void *arg, int len, bool set)
{
	dhd_bus_t *bus = dhdp->bus;
	const bcm_iovar_t *vi = NULL;
	int bcmerror = 0;
	int val_size;
	uint32 actionid;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	ASSERT(name);
	ASSERT(len >= 0);

	
	ASSERT(set || (arg && len));

	
	ASSERT(!set || (!params && !plen));

	DHD_INFO(("%s: %s %s, len %d plen %d\n", __FUNCTION__,
	         name, (set ? "set" : "get"), len, plen));

	
	if ((vi = bcm_iovar_lookup(dhdpcie_iovars, name)) == NULL) {
		bcmerror = BCME_UNSUPPORTED;
		goto exit;
	}


	if (params == NULL) {
		params = arg;
		plen = len;
	}

	if (vi->type == IOVT_VOID)
		val_size = 0;
	else if (vi->type == IOVT_BUFFER)
		val_size = len;
	else
		
		val_size = sizeof(int);

	actionid = set ? IOV_SVAL(vi->varid) : IOV_GVAL(vi->varid);
	bcmerror = dhdpcie_bus_doiovar(bus, vi, actionid, name, params, plen, arg, len, val_size);

exit:
	return bcmerror;
} 

#ifdef BCM_BUZZZ
#include <bcm_buzzz.h>

int
dhd_buzzz_dump_cntrs(char *p, uint32 *core, uint32 *log,
	const int num_counters)
{
	int bytes = 0;
	uint32 ctr;
	uint32 curr[BCM_BUZZZ_COUNTERS_MAX], prev[BCM_BUZZZ_COUNTERS_MAX];
	uint32 delta[BCM_BUZZZ_COUNTERS_MAX];

	
	for (ctr = 0U; ctr < num_counters; ctr++) {
		prev[ctr] = core[ctr];
		curr[ctr] = *log++;
		core[ctr] = curr[ctr];  

		if (curr[ctr] < prev[ctr])
			delta[ctr] = curr[ctr] + (~0U - prev[ctr]);
		else
			delta[ctr] = (curr[ctr] - prev[ctr]);

		bytes += sprintf(p + bytes, "%12u ", delta[ctr]);
	}

	return bytes;
}

typedef union cm3_cnts { 
	uint32 u32;
	uint8  u8[4];
	struct {
		uint8 cpicnt;
		uint8 exccnt;
		uint8 sleepcnt;
		uint8 lsucnt;
	};
} cm3_cnts_t;

int
dhd_bcm_buzzz_dump_cntrs6(char *p, uint32 *core, uint32 *log)
{
	int bytes = 0;

	uint32 cyccnt, instrcnt;
	cm3_cnts_t cm3_cnts;
	uint8 foldcnt;

	{   
		uint32 curr, prev, delta;
		prev = core[0]; curr = *log++; core[0] = curr;
		if (curr < prev)
			delta = curr + (~0U - prev);
		else
			delta = (curr - prev);

		bytes += sprintf(p + bytes, "%12u ", delta);
		cyccnt = delta;
	}

	{	
		int i;
		uint8 max8 = ~0;
		cm3_cnts_t curr, prev, delta;
		prev.u32 = core[1]; curr.u32 = * log++; core[1] = curr.u32;
		for (i = 0; i < 4; i++) {
			if (curr.u8[i] < prev.u8[i])
				delta.u8[i] = curr.u8[i] + (max8 - prev.u8[i]);
			else
				delta.u8[i] = (curr.u8[i] - prev.u8[i]);
			bytes += sprintf(p + bytes, "%4u ", delta.u8[i]);
		}
		cm3_cnts.u32 = delta.u32;
	}

	{   
		uint8 curr, prev, delta, max8 = ~0;
		bcm_buzzz_arg0_t arg0; arg0.u32 = *log;
		prev = core[2]; curr = arg0.klog.cnt; core[2] = curr;
		if (curr < prev)
			delta = curr + (max8 - prev);
		else
			delta = (curr - prev);
		bytes += sprintf(p + bytes, "%4u ", delta);
		foldcnt = delta;
	}

	instrcnt = cyccnt - (cm3_cnts.u8[0] + cm3_cnts.u8[1] + cm3_cnts.u8[2]
		                 + cm3_cnts.u8[3]) + foldcnt;
	if (instrcnt > 0xFFFFFF00)
		bytes += sprintf(p + bytes, "[%10s] ", "~");
	else
		bytes += sprintf(p + bytes, "[%10u] ", instrcnt);
	return bytes;
}

int
dhd_buzzz_dump_log(char *p, uint32 *core, uint32 *log, bcm_buzzz_t *buzzz)
{
	int bytes = 0;
	bcm_buzzz_arg0_t arg0;
	static uint8 * fmt[] = BCM_BUZZZ_FMT_STRINGS;

	if (buzzz->counters == 6) {
		bytes += dhd_bcm_buzzz_dump_cntrs6(p, core, log);
		log += 2; 
	} else {
		bytes += dhd_buzzz_dump_cntrs(p, core, log, buzzz->counters);
		log += buzzz->counters; 
	}

	
	arg0.u32 = *log++;

	switch (arg0.klog.args) {
		case 0:
			bytes += sprintf(p + bytes, fmt[arg0.klog.id]);
			break;
		case 1:
		{
			uint32 arg1 = *log++;
			bytes += sprintf(p + bytes, fmt[arg0.klog.id], arg1);
			break;
		}
		case 2:
		{
			uint32 arg1, arg2;
			arg1 = *log++; arg2 = *log++;
			bytes += sprintf(p + bytes, fmt[arg0.klog.id], arg1, arg2);
			break;
		}
		case 3:
		{
			uint32 arg1, arg2, arg3;
			arg1 = *log++; arg2 = *log++; arg3 = *log++;
			bytes += sprintf(p + bytes, fmt[arg0.klog.id], arg1, arg2, arg3);
			break;
		}
		case 4:
		{
			uint32 arg1, arg2, arg3, arg4;
			arg1 = *log++; arg2 = *log++;
			arg3 = *log++; arg4 = *log++;
			bytes += sprintf(p + bytes, fmt[arg0.klog.id], arg1, arg2, arg3, arg4);
			break;
		}
		default:
			printf("Maximum one argument supported\n");
			break;
	}

	bytes += sprintf(p + bytes, "\n");

	return bytes;
}

void dhd_buzzz_dump(bcm_buzzz_t *buzzz_p, void *buffer_p, char *p)
{
	int i;
	uint32 total, part1, part2, log_sz, core[BCM_BUZZZ_COUNTERS_MAX];
	void * log;

	for (i = 0; i < BCM_BUZZZ_COUNTERS_MAX; i++) {
		core[i] = 0;
	}

	log_sz = buzzz_p->log_sz;

	part1 = ((uint32)buzzz_p->cur - (uint32)buzzz_p->log) / log_sz;

	if (buzzz_p->wrap == TRUE) {
		part2 = ((uint32)buzzz_p->end - (uint32)buzzz_p->cur) / log_sz;
		total = (buzzz_p->buffer_sz - BCM_BUZZZ_LOGENTRY_MAXSZ) / log_sz;
	} else {
		part2 = 0U;
		total = buzzz_p->count;
	}

	if (total == 0U) {
		printf("bcm_buzzz_dump total<%u> done\n", total);
		return;
	} else {
		printf("bcm_buzzz_dump total<%u> : part2<%u> + part1<%u>\n",
		       total, part2, part1);
	}

	if (part2) {   
		log = (void*)((size_t)buffer_p + (buzzz_p->cur - buzzz_p->log));
		while (part2--) {   
			p[0] = '\0';
			dhd_buzzz_dump_log(p, core, (uint32 *)log, buzzz_p);
			printf("%s", p);
			log = (void*)((size_t)log + buzzz_p->log_sz);
		}
	}

	log = (void*)buffer_p;
	while (part1--) {
		p[0] = '\0';
		dhd_buzzz_dump_log(p, core, (uint32 *)log, buzzz_p);
		printf("%s", p);
		log = (void*)((size_t)log + buzzz_p->log_sz);
	}

	printf("bcm_buzzz_dump done.\n");
}

int dhd_buzzz_dump_dngl(dhd_bus_t *bus)
{
	bcm_buzzz_t * buzzz_p = NULL;
	void * buffer_p = NULL;
	char * page_p = NULL;
	pciedev_shared_t *sh;
	int ret = 0;

	if (bus->dhd->busstate != DHD_BUS_DATA) {
		return BCME_UNSUPPORTED;
	}
	if ((page_p = (char *)MALLOC(bus->dhd->osh, 4096)) == NULL) {
		printf("Page memory allocation failure\n");
		goto done;
	}
	if ((buzzz_p = MALLOC(bus->dhd->osh, sizeof(bcm_buzzz_t))) == NULL) {
		printf("BCM BUZZZ memory allocation failure\n");
		goto done;
	}

	ret = dhdpcie_readshared(bus);
	if (ret < 0) {
		DHD_ERROR(("%s :Shared area read failed \n", __FUNCTION__));
		goto done;
	}

	sh = bus->pcie_sh;

	DHD_INFO(("%s buzzz:%08x\n", __FUNCTION__, sh->buzzz));

	if (sh->buzzz != 0U) {	

		dhdpcie_bus_membytes(bus, FALSE, (ulong)sh->buzzz,
		                     (uint8 *)buzzz_p, sizeof(bcm_buzzz_t));

		printf("BUZZZ[0x%08x]: log<0x%08x> cur<0x%08x> end<0x%08x> "
			"count<%u> status<%u> wrap<%u>\n"
			"cpu<0x%02X> counters<%u> group<%u> buffer_sz<%u> log_sz<%u>\n",
			(int)sh->buzzz,
			(int)buzzz_p->log, (int)buzzz_p->cur, (int)buzzz_p->end,
			buzzz_p->count, buzzz_p->status, buzzz_p->wrap,
			buzzz_p->cpu_idcode, buzzz_p->counters, buzzz_p->group,
			buzzz_p->buffer_sz, buzzz_p->log_sz);

		if (buzzz_p->count == 0) {
			printf("Empty dongle BUZZZ trace\n\n");
			goto done;
		}

		
		buffer_p = MALLOC(bus->dhd->osh, buzzz_p->buffer_sz);
		if (buffer_p == NULL) {
			printf("Buffer memory allocation failure\n");
			goto done;
		}

		
		dhdpcie_bus_membytes(bus, FALSE, (uint32)buzzz_p->log,   
		                     (uint8 *)buffer_p, buzzz_p->buffer_sz);

		

		{
			int ctr;
			for (ctr = 0; ctr < buzzz_p->counters; ctr++) {
				printf("<Evt[%02X]> ", buzzz_p->eventid[ctr]);
			}
			printf("<code execution point>\n");
		}

		dhd_buzzz_dump(buzzz_p, buffer_p, page_p);

		printf("----- End of dongle BCM BUZZZ Trace -----\n\n");

		MFREE(bus->dhd->osh, buffer_p, buzzz_p->buffer_sz); buffer_p = NULL;
	}

done:

	if (page_p)   MFREE(bus->dhd->osh, page_p, 4096);
	if (buzzz_p)  MFREE(bus->dhd->osh, buzzz_p, sizeof(bcm_buzzz_t));
	if (buffer_p) MFREE(bus->dhd->osh, buffer_p, buzzz_p->buffer_sz);

	return BCME_OK;
}
#endif 

#define PCIE_GEN2(sih) ((BUSTYPE((sih)->bustype) == PCI_BUS) &&	\
	((sih)->buscoretype == PCIE2_CORE_ID))

static bool
pcie2_mdiosetblock(dhd_bus_t *bus, uint blk)
{
	uint mdiodata, mdioctrl, i = 0;
	uint pcie_serdes_spinwait = 200;

	mdioctrl = MDIOCTL2_DIVISOR_VAL | (0x1F << MDIOCTL2_REGADDR_SHF);
	mdiodata = (blk << MDIODATA2_DEVADDR_SHF) | MDIODATA2_DONE;

	si_corereg(bus->sih, bus->sih->buscoreidx, PCIE2_MDIO_CONTROL, ~0, mdioctrl);
	si_corereg(bus->sih, bus->sih->buscoreidx, PCIE2_MDIO_WR_DATA, ~0, mdiodata);

	OSL_DELAY(10);
	
	while (i < pcie_serdes_spinwait) {
		uint mdioctrl_read = si_corereg(bus->sih, bus->sih->buscoreidx, PCIE2_MDIO_WR_DATA,
			0, 0);
		if (!(mdioctrl_read & MDIODATA2_DONE)) {
			break;
		}
		OSL_DELAY(1000);
		i++;
	}

	if (i >= pcie_serdes_spinwait) {
		DHD_ERROR(("pcie_mdiosetblock: timed out\n"));
		return FALSE;
	}

	return TRUE;
}

int
dhd_bus_stop_clock(dhd_pub_t *dhdpub)
{
	dhd_bus_t *bus = dhdpub->bus;

	if (!bus) {
		DHD_ERROR(("%s: bus is null!\n", __FUNCTION__));
		return BCME_ERROR;
	}

#ifdef CUSTOMER_HW_ONE
	
	bus->islinkdown = 1;
#endif

	return dhdpcie_stop_host_pcieclock(bus);
}

int
dhd_bus_start_clock(dhd_pub_t *dhdpub)
{
	dhd_bus_t *bus = dhdpub->bus;

	if (!bus) {
		DHD_ERROR(("%s: bus is null!\n", __FUNCTION__));
		return BCME_ERROR;
	}

	return dhdpcie_start_host_pcieclock(bus);
}

int
dhd_bus_devreset(dhd_pub_t *dhdp, uint8 flag)
{
	dhd_bus_t *bus = dhdp->bus;
	int bcmerror = 0;
	unsigned long flags;
#ifdef CONFIG_ARCH_MSM
	int retry = POWERUP_MAX_RETRY;
#endif 

	if (dhd_download_fw_on_driverload) {
		bcmerror = dhd_bus_start(dhdp);
	} else {
		if (flag == TRUE) { 
			
			DHD_ERROR(("%s: == Power OFF ==\n", __FUNCTION__));

			bus->dhd->up = FALSE;

			if (bus->dhd->busstate != DHD_BUS_DOWN) {
				dhdpcie_advertise_bus_cleanup(bus->dhd);
				if (bus->intr) {
					dhdpcie_bus_intr_disable(bus);
					dhdpcie_free_irq(bus);
				}
#ifdef BCMPCIE_OOB_HOST_WAKE
				
				dhd_bus_oob_intr_set(bus->dhd, FALSE);
				dhd_bus_oob_intr_unregister(bus->dhd);
#endif 
				dhd_os_wd_timer(dhdp, 0);
				dhd_bus_stop(bus, TRUE);
				dhd_prot_reset(dhdp);
				dhd_clear(dhdp);
				dhd_bus_release_dongle(bus);
				dhdpcie_bus_free_resource(bus);
				bcmerror = dhdpcie_bus_disable_device(bus);
				if (bcmerror) {
					DHD_ERROR(("%s: dhdpcie_bus_disable_device: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}
#ifdef CONFIG_ARCH_MSM
				bcmerror = dhdpcie_bus_clock_stop(bus);
				if (bcmerror) {
					DHD_ERROR(("%s: host clock stop failed: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}
#endif 
				DHD_GENERAL_LOCK(bus->dhd, flags);
				bus->dhd->busstate = DHD_BUS_DOWN;
				DHD_GENERAL_UNLOCK(bus->dhd, flags);
			} else {
				if (bus->intr) {
					dhdpcie_free_irq(bus);
				}
#ifdef BCMPCIE_OOB_HOST_WAKE
				
				dhd_bus_oob_intr_set(bus->dhd, FALSE);
				dhd_bus_oob_intr_unregister(bus->dhd);
#endif 
				dhd_prot_reset(dhdp);
				dhd_clear(dhdp);
				dhd_bus_release_dongle(bus);
				dhdpcie_bus_free_resource(bus);
				bcmerror = dhdpcie_bus_disable_device(bus);
				if (bcmerror) {
					DHD_ERROR(("%s: dhdpcie_bus_disable_device: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}

#ifdef CONFIG_ARCH_MSM
				bcmerror = dhdpcie_bus_clock_stop(bus);
				if (bcmerror) {
					DHD_ERROR(("%s: host clock stop failed: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}
#endif  
			}

			bus->dhd->dongle_reset = TRUE;
			DHD_ERROR(("%s:  WLAN OFF Done\n", __FUNCTION__));

		} else { 
			if (bus->dhd->busstate == DHD_BUS_DOWN) {
				
				DHD_ERROR(("%s: == Power ON ==\n", __FUNCTION__));
#ifdef CONFIG_ARCH_MSM
				while (--retry) {
					bcmerror = dhdpcie_bus_clock_start(bus);
					if (!bcmerror) {
						DHD_ERROR(("%s: dhdpcie_bus_clock_start OK\n",
							__FUNCTION__));
						break;
					} else {
						OSL_SLEEP(10);
					}
				}

				if (bcmerror && !retry) {
					DHD_ERROR(("%s: host pcie clock enable failed: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}
#endif 
				bus->pci_d3hot_done = 0;
				bcmerror = dhdpcie_bus_enable_device(bus);
				if (bcmerror) {
					DHD_ERROR(("%s: host configuration restore failed: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}

				bcmerror = dhdpcie_bus_alloc_resource(bus);
				if (bcmerror) {
					DHD_ERROR(("%s: dhdpcie_bus_resource_alloc failed: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}

				bcmerror = dhdpcie_bus_dongle_attach(bus);
				if (bcmerror) {
					DHD_ERROR(("%s: dhdpcie_bus_dongle_attach failed: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}

				bcmerror = dhd_bus_request_irq(bus);
				if (bcmerror) {
					DHD_ERROR(("%s: dhd_bus_request_irq failed: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}
#ifdef CUSTOMER_HW_ONE
				if (dhdpcie_chipmatch(bus->dev->vendor, bus->dev->device)) {
					DHD_ERROR(("%s: WIFI_OTP: chipmatch failed!!\n",
						__FUNCTION__));
				}
				DHD_ERROR(("%s: WIFI_OTP: bus->device: %X \n",
					__FUNCTION__, bus->dev->device));
#endif 
				bus->dhd->dongle_reset = FALSE;

				bcmerror = dhd_bus_start(dhdp);
				if (bcmerror) {
					DHD_ERROR(("%s: dhd_bus_start: %d\n",
						__FUNCTION__, bcmerror));
					goto done;
				}

				bus->dhd->up = TRUE;
				DHD_ERROR(("%s: WLAN Power On Done\n", __FUNCTION__));
			} else {
				DHD_ERROR(("%s: what should we do here\n", __FUNCTION__));
				goto done;
			}
		}
	}

done:
	if (bcmerror) {
		DHD_GENERAL_LOCK(bus->dhd, flags);
		bus->dhd->busstate = DHD_BUS_DOWN;
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
	}

	return bcmerror;
}

static int
pcie2_mdioop(dhd_bus_t *bus, uint physmedia, uint regaddr, bool write, uint *val,
	bool slave_bypass)
{
	uint pcie_serdes_spinwait = 200, i = 0, mdio_ctrl;
	uint32 reg32;

	pcie2_mdiosetblock(bus, physmedia);

	
	mdio_ctrl = MDIOCTL2_DIVISOR_VAL;
	mdio_ctrl |= (regaddr << MDIOCTL2_REGADDR_SHF);

	if (slave_bypass)
		mdio_ctrl |= MDIOCTL2_SLAVE_BYPASS;

	if (!write)
		mdio_ctrl |= MDIOCTL2_READ;

	si_corereg(bus->sih, bus->sih->buscoreidx, PCIE2_MDIO_CONTROL, ~0, mdio_ctrl);

	if (write) {
		reg32 =  PCIE2_MDIO_WR_DATA;
		si_corereg(bus->sih, bus->sih->buscoreidx, PCIE2_MDIO_WR_DATA, ~0,
			*val | MDIODATA2_DONE);
	} else
		reg32 =  PCIE2_MDIO_RD_DATA;

	
	while (i < pcie_serdes_spinwait) {
		uint done_val =  si_corereg(bus->sih, bus->sih->buscoreidx, reg32, 0, 0);
		if (!(done_val & MDIODATA2_DONE)) {
			if (!write) {
				*val = si_corereg(bus->sih, bus->sih->buscoreidx,
					PCIE2_MDIO_RD_DATA, 0, 0);
				*val = *val & MDIODATA2_MASK;
			}
			return 0;
		}
		OSL_DELAY(1000);
		i++;
	}
	return -1;
}

static int
dhdpcie_bus_doiovar(dhd_bus_t *bus, const bcm_iovar_t *vi, uint32 actionid, const char *name,
                void *params, int plen, void *arg, int len, int val_size)
{
	int bcmerror = 0;
	int32 int_val = 0;
	int32 int_val2 = 0;
	int32 int_val3 = 0;
	bool bool_val = 0;

	DHD_TRACE(("%s: Enter, action %d name %s params %p plen %d arg %p len %d val_size %d\n",
	           __FUNCTION__, actionid, name, params, plen, arg, len, val_size));

	if ((bcmerror = bcm_iovar_lencheck(vi, arg, len, IOV_ISSET(actionid))) != 0)
		goto exit;

	if (plen >= (int)sizeof(int_val))
		bcopy(params, &int_val, sizeof(int_val));

	if (plen >= (int)sizeof(int_val) * 2)
		bcopy((void*)((uintptr)params + sizeof(int_val)), &int_val2, sizeof(int_val2));

	if (plen >= (int)sizeof(int_val) * 3)
		bcopy((void*)((uintptr)params + 2 * sizeof(int_val)), &int_val3, sizeof(int_val3));

	bool_val = (int_val != 0) ? TRUE : FALSE;

	
	if (bus->dhd->dongle_reset && !(actionid == IOV_SVAL(IOV_DEVRESET) ||
	                                actionid == IOV_GVAL(IOV_DEVRESET))) {
		bcmerror = BCME_NOTREADY;
		goto exit;
	}

	switch (actionid) {


	case IOV_SVAL(IOV_VARS):
		bcmerror = dhdpcie_downloadvars(bus, arg, len);
		break;

	case IOV_SVAL(IOV_PCIEREG):
		si_corereg(bus->sih, bus->sih->buscoreidx, OFFSETOF(sbpcieregs_t, configaddr), ~0,
			int_val);
		si_corereg(bus->sih, bus->sih->buscoreidx, OFFSETOF(sbpcieregs_t, configdata), ~0,
			int_val2);
		break;

	case IOV_GVAL(IOV_PCIEREG):
		si_corereg(bus->sih, bus->sih->buscoreidx, OFFSETOF(sbpcieregs_t, configaddr), ~0,
			int_val);
		int_val = si_corereg(bus->sih, bus->sih->buscoreidx,
			OFFSETOF(sbpcieregs_t, configdata), 0, 0);
		bcopy(&int_val, arg, sizeof(int_val));
		break;

	case IOV_SVAL(IOV_PCIECOREREG):
		si_corereg(bus->sih, bus->sih->buscoreidx, int_val, ~0, int_val2);
		break;
	case IOV_GVAL(IOV_BAR0_SECWIN_REG):
	{
		sdreg_t sdreg;
		uint32 addr, size;

		bcopy(params, &sdreg, sizeof(sdreg));

		addr = sdreg.offset;
		size = sdreg.func;

		if (si_backplane_access(bus->sih, addr, size, &int_val, TRUE) != BCME_OK) {
			DHD_ERROR(("Invalid size/addr combination \n"));
			bcmerror = BCME_ERROR;
			break;
		}
		bcopy(&int_val, arg, sizeof(int32));
		break;
	}

	case IOV_SVAL(IOV_BAR0_SECWIN_REG):
	{
		sdreg_t sdreg;
		uint32 addr, size;

		bcopy(params, &sdreg, sizeof(sdreg));

		addr = sdreg.offset;
		size = sdreg.func;
		if (si_backplane_access(bus->sih, addr, size, &sdreg.value, FALSE) != BCME_OK) {
			DHD_ERROR(("Invalid size/addr combination \n"));
			bcmerror = BCME_ERROR;
		}
		break;
	}

	case IOV_GVAL(IOV_SBREG):
	{
		sdreg_t sdreg;
		uint32 addr, size;

		bcopy(params, &sdreg, sizeof(sdreg));

		addr = sdreg.offset | SI_ENUM_BASE;
		size = sdreg.func;

		if (si_backplane_access(bus->sih, addr, size, &int_val, TRUE) != BCME_OK) {
			DHD_ERROR(("Invalid size/addr combination \n"));
			bcmerror = BCME_ERROR;
			break;
		}
		bcopy(&int_val, arg, sizeof(int32));
		break;
	}

	case IOV_SVAL(IOV_SBREG):
	{
		sdreg_t sdreg;
		uint32 addr, size;

		bcopy(params, &sdreg, sizeof(sdreg));

		addr = sdreg.offset | SI_ENUM_BASE;
		size = sdreg.func;
		if (si_backplane_access(bus->sih, addr, size, &sdreg.value, FALSE) != BCME_OK) {
			DHD_ERROR(("Invalid size/addr combination \n"));
			bcmerror = BCME_ERROR;
		}
		break;
	}

	case IOV_GVAL(IOV_PCIESERDESREG):
	{
		uint val;
		if (!PCIE_GEN2(bus->sih)) {
			DHD_ERROR(("supported only in pcie gen2\n"));
			bcmerror = BCME_ERROR;
			break;
		}

		if (!pcie2_mdioop(bus, int_val, int_val2, FALSE, &val, FALSE)) {
			bcopy(&val, arg, sizeof(int32));
		} else {
			DHD_ERROR(("pcie2_mdioop failed.\n"));
			bcmerror = BCME_ERROR;
		}
		break;
	}

	case IOV_SVAL(IOV_PCIESERDESREG):
		if (!PCIE_GEN2(bus->sih)) {
			DHD_ERROR(("supported only in pcie gen2\n"));
			bcmerror = BCME_ERROR;
			break;
		}
		if (pcie2_mdioop(bus, int_val, int_val2, TRUE, &int_val3, FALSE)) {
			DHD_ERROR(("pcie2_mdioop failed.\n"));
			bcmerror = BCME_ERROR;
		}
		break;
	case IOV_GVAL(IOV_PCIECOREREG):
		int_val = si_corereg(bus->sih, bus->sih->buscoreidx, int_val, 0, 0);
		bcopy(&int_val, arg, sizeof(int_val));
		break;

	case IOV_SVAL(IOV_PCIECFGREG):
		OSL_PCI_WRITE_CONFIG(bus->osh, int_val, 4, int_val2);
		break;

	case IOV_GVAL(IOV_PCIECFGREG):
		int_val = OSL_PCI_READ_CONFIG(bus->osh, int_val, 4);
		bcopy(&int_val, arg, sizeof(int_val));
		break;

	case IOV_SVAL(IOV_PCIE_LPBK):
		bcmerror = dhdpcie_bus_lpback_req(bus, int_val);
		break;

	case IOV_SVAL(IOV_PCIE_DMAXFER):
		bcmerror = dhdpcie_bus_dmaxfer_req(bus, int_val, int_val2, int_val3);
		break;

	case IOV_GVAL(IOV_PCIE_SUSPEND):
		int_val = (bus->dhd->busstate == DHD_BUS_SUSPEND) ? 1 : 0;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_PCIE_SUSPEND):
		bus->force_suspend = 1;
		dhdpcie_bus_suspend(bus, bool_val);
		bus->force_suspend = 0;
		break;

	case IOV_GVAL(IOV_MEMSIZE):
		int_val = (int32)bus->ramsize;
		bcopy(&int_val, arg, val_size);
		break;
	case IOV_SVAL(IOV_MEMBYTES):
	case IOV_GVAL(IOV_MEMBYTES):
	{
		uint32 address;		
		uint size, dsize;
		uint8 *data;

		bool set = (actionid == IOV_SVAL(IOV_MEMBYTES));

		ASSERT(plen >= 2*sizeof(int));

		address = (uint32)int_val;
		bcopy((char *)params + sizeof(int_val), &int_val, sizeof(int_val));
		size = (uint)int_val;

		
		dsize = set ? plen - (2 * sizeof(int)) : len;
		if (dsize < size) {
			DHD_ERROR(("%s: error on %s membytes, addr 0x%08x size %d dsize %d\n",
			           __FUNCTION__, (set ? "set" : "get"), address, size, dsize));
			bcmerror = BCME_BADARG;
			break;
		}

		DHD_INFO(("%s: Request to %s %d bytes at address 0x%08x\n dsize %d ", __FUNCTION__,
		          (set ? "write" : "read"), size, address, dsize));

		
		if (si_setcore(bus->sih, ARMCR4_CORE_ID, 0) ||
		    si_setcore(bus->sih, SYSMEM_CORE_ID, 0)) {
			/* if address is 0, store the reset instruction to be written in 0 */
			if (set && address == bus->dongle_ram_base) {
				bus->resetinstr = *(((uint32*)params) + 2);
			}
		} else {
		
		if ((bus->orig_ramsize) &&
		    ((address > bus->orig_ramsize) || (address + size > bus->orig_ramsize)))
		{
			uint8 enable, protect, remap;
			si_socdevram(bus->sih, FALSE, &enable, &protect, &remap);
			if (!enable || protect) {
				DHD_ERROR(("%s: ramsize 0x%08x doesn't have %d bytes at 0x%08x\n",
					__FUNCTION__, bus->orig_ramsize, size, address));
				DHD_ERROR(("%s: socram enable %d, protect %d\n",
					__FUNCTION__, enable, protect));
				bcmerror = BCME_BADARG;
				break;
			}

			if (!REMAP_ENAB(bus) && (address >= SOCDEVRAM_ARM_ADDR)) {
				uint32 devramsize = si_socdevram_size(bus->sih);
				if ((address < SOCDEVRAM_ARM_ADDR) ||
					(address + size > (SOCDEVRAM_ARM_ADDR + devramsize))) {
					DHD_ERROR(("%s: bad address 0x%08x, size 0x%08x\n",
						__FUNCTION__, address, size));
					DHD_ERROR(("%s: socram range 0x%08x,size 0x%08x\n",
						__FUNCTION__, SOCDEVRAM_ARM_ADDR, devramsize));
					bcmerror = BCME_BADARG;
					break;
				}
				
				address -= SOCDEVRAM_ARM_ADDR;
				address += SOCDEVRAM_BP_ADDR;
				DHD_INFO(("%s: Request to %s %d bytes @ Mapped address 0x%08x\n",
					__FUNCTION__, (set ? "write" : "read"), size, address));
			} else if (REMAP_ENAB(bus) && REMAP_ISADDR(bus, address) && remap) {
				DHD_ERROR(("%s: Need to disable remap for address 0x%08x\n",
					__FUNCTION__, address));
				bcmerror = BCME_ERROR;
				break;
			}
		}
		}

		
		data = set ? (uint8*)params + 2 * sizeof(int): (uint8*)arg;

		
		bcmerror = dhdpcie_bus_membytes(bus, set, address, data, size);

		break;
	}

#ifdef BCM_BUZZZ
	
	case IOV_GVAL(IOV_BUZZZ_DUMP):
		bcmerror = dhd_buzzz_dump_dngl(bus);
		break;
#endif 

	case IOV_SVAL(IOV_SET_DOWNLOAD_STATE):
		bcmerror = dhdpcie_bus_download_state(bus, bool_val);
		break;

	case IOV_GVAL(IOV_RAMSIZE):
		int_val = (int32)bus->ramsize;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_RAMSIZE):
		bus->ramsize = int_val;
		bus->orig_ramsize = int_val;
		break;

	case IOV_GVAL(IOV_RAMSTART):
		int_val = (int32)bus->dongle_ram_base;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_CC_NVMSHADOW):
	{
		struct bcmstrbuf dump_b;

		bcm_binit(&dump_b, arg, len);
		bcmerror = dhdpcie_cc_nvmshadow(bus, &dump_b);
		break;
	}

	case IOV_GVAL(IOV_SLEEP_ALLOWED):
		bool_val = bus->sleep_allowed;
		bcopy(&bool_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_SLEEP_ALLOWED):
		bus->sleep_allowed = bool_val;
		break;

	case IOV_GVAL(IOV_DONGLEISOLATION):
		int_val = bus->dhd->dongle_isolation;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_DONGLEISOLATION):
		bus->dhd->dongle_isolation = bool_val;
		break;

	case IOV_GVAL(IOV_LTRSLEEPON_UNLOOAD):
		int_val = bus->ltrsleep_on_unload;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_LTRSLEEPON_UNLOOAD):
		bus->ltrsleep_on_unload = bool_val;
		break;

	case IOV_GVAL(IOV_DUMP_RINGUPD_BLOCK):
	{
		struct bcmstrbuf dump_b;
		bcm_binit(&dump_b, arg, len);
		bcmerror = dhd_prot_ringupd_dump(bus->dhd, &dump_b);
		break;
	}
	case IOV_GVAL(IOV_DMA_RINGINDICES):
	{	int h2d_support, d2h_support;

		d2h_support = DMA_INDX_ENAB(bus->dhd->dma_d2h_ring_upd_support) ? 1 : 0;
		h2d_support = DMA_INDX_ENAB(bus->dhd->dma_h2d_ring_upd_support) ? 1 : 0;
		int_val = d2h_support | (h2d_support << 1);
		bcopy(&int_val, arg, sizeof(int_val));
		break;
	}
	case IOV_SVAL(IOV_DMA_RINGINDICES):
		
		if (bus->dhd->busstate == DHD_BUS_DOWN) {
			if ((int_val > 3) || (int_val < 0)) {
				DHD_ERROR(("Bad argument. Possible values: 0, 1, 2 & 3\n"));
				bcmerror = BCME_BADARG;
			} else {
				bus->dhd->dma_d2h_ring_upd_support = (int_val & 1) ? TRUE : FALSE;
				bus->dhd->dma_h2d_ring_upd_support = (int_val & 2) ? TRUE : FALSE;
			}
		} else {
			DHD_ERROR(("%s: Can change only when bus down (before FW download)\n",
				__FUNCTION__));
			bcmerror = BCME_NOTDOWN;
		}
		break;

	case IOV_GVAL(IOV_METADATA_DBG):
		int_val = dhd_prot_metadata_dbg_get(bus->dhd);
		bcopy(&int_val, arg, val_size);
		break;
	case IOV_SVAL(IOV_METADATA_DBG):
		dhd_prot_metadata_dbg_set(bus->dhd, (int_val != 0));
		break;

	case IOV_GVAL(IOV_RX_METADATALEN):
		int_val = dhd_prot_metadatalen_get(bus->dhd, TRUE);
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_RX_METADATALEN):
		if (int_val > 64) {
			bcmerror = BCME_BUFTOOLONG;
			break;
		}
		dhd_prot_metadatalen_set(bus->dhd, int_val, TRUE);
		break;

	case IOV_SVAL(IOV_TXP_THRESHOLD):
		dhd_prot_txp_threshold(bus->dhd, TRUE, int_val);
		break;

	case IOV_GVAL(IOV_TXP_THRESHOLD):
		int_val = dhd_prot_txp_threshold(bus->dhd, FALSE, int_val);
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_DB1_FOR_MB):
		if (int_val)
			bus->db1_for_mb = TRUE;
		else
			bus->db1_for_mb = FALSE;
		break;

	case IOV_GVAL(IOV_DB1_FOR_MB):
		if (bus->db1_for_mb)
			int_val = 1;
		else
			int_val = 0;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_TX_METADATALEN):
		int_val = dhd_prot_metadatalen_get(bus->dhd, FALSE);
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_TX_METADATALEN):
		if (int_val > 64) {
			bcmerror = BCME_BUFTOOLONG;
			break;
		}
		dhd_prot_metadatalen_set(bus->dhd, int_val, FALSE);
		break;

	case IOV_SVAL(IOV_DEVRESET):
		dhd_bus_devreset(bus->dhd, (uint8)bool_val);
		break;

	case IOV_GVAL(IOV_FLOW_PRIO_MAP):
		int_val = bus->dhd->flow_prio_map_type;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_FLOW_PRIO_MAP):
		int_val = (int32)dhd_update_flow_prio_map(bus->dhd, (uint8)int_val);
		bcopy(&int_val, arg, val_size);
		break;

#ifdef DHD_PCIE_RUNTIMEPM
	case IOV_GVAL(IOV_IDLETIME):
		int_val = bus->idletime;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_IDLETIME):
		if (int_val < 0) {
			bcmerror = BCME_BADARG;
		} else {
			bus->idletime = int_val;
		}
		break;
#endif 

	case IOV_GVAL(IOV_TXBOUND):
		int_val = (int32)dhd_txbound;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_TXBOUND):
		dhd_txbound = (uint)int_val;
		break;

	case IOV_GVAL(IOV_RXBOUND):
		int_val = (int32)dhd_rxbound;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_RXBOUND):
		dhd_rxbound = (uint)int_val;
		break;

	case IOV_SVAL(IOV_HANGREPORT):
		bus->dhd->hang_report = bool_val;
		DHD_ERROR(("%s: Set hang_report as %d\n",
			__FUNCTION__, bus->dhd->hang_report));
		break;

	case IOV_GVAL(IOV_HANGREPORT):
		int_val = (int32)bus->dhd->hang_report;
		bcopy(&int_val, arg, val_size);
		break;

	default:
		bcmerror = BCME_UNSUPPORTED;
		break;
	}

exit:
	return bcmerror;
} 

static int
dhdpcie_bus_lpback_req(struct  dhd_bus *bus, uint32 len)
{
	if (bus->dhd == NULL) {
		DHD_ERROR(("bus not inited\n"));
		return 0;
	}
	if (bus->dhd->prot == NULL) {
		DHD_ERROR(("prot is not inited\n"));
		return 0;
	}
	if (bus->dhd->busstate != DHD_BUS_DATA) {
		DHD_ERROR(("not in a readystate to LPBK  is not inited\n"));
		return 0;
	}
	dhdmsgbuf_lpbk_req(bus->dhd, len);
	return 0;
}

int
dhdpcie_bus_suspend(struct dhd_bus *bus, bool state)
{
	int timeleft;
	unsigned long flags;
	int rc = 0;

	if (bus->dhd == NULL) {
		DHD_ERROR(("bus not inited\n"));
		return BCME_ERROR;
	}
	if (bus->dhd->prot == NULL) {
		DHD_ERROR(("prot is not inited\n"));
		return BCME_ERROR;
	}

	if (dhd_query_bus_erros(bus->dhd)) {
		return BCME_ERROR;
	}

	DHD_GENERAL_LOCK(bus->dhd, flags);
	if (bus->dhd->busstate != DHD_BUS_DATA && bus->dhd->busstate != DHD_BUS_SUSPEND) {
		DHD_ERROR(("not in a readystate to LPBK  is not inited\n"));
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
		return BCME_ERROR;
	}
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	if (bus->suspended == state) { 
		DHD_ERROR(("Bus is already in SUSPEND state.\n"));
		return BCME_OK;
	}

	if (bus->d3_suspend_pending) {
		DHD_ERROR(("D3 SUSPEND PENDING Suspend pending ...\n"));
		return BCME_ERROR;
	}

	if (state) {
		int idle_retry = 0;
		int active;

		if(bus->islinkdown) {
			DHD_ERROR(("%s: PCIe link was down, state=%d\n",
				__FUNCTION__, state));
			return BCME_ERROR;
		}

		
		DHD_ERROR_HW_ONE(("%s: Entering suspend state\n", __FUNCTION__));
		bus->wait_for_d3_ack = 0;
		bus->suspended = TRUE;

		DHD_GENERAL_LOCK(bus->dhd, flags);
		
		dhd_bus_stop_queue(bus);
		bus->dhd->busstate = DHD_BUS_SUSPEND;
		if (bus->dhd->dhd_bus_busy_state & DHD_BUS_BUSY_IN_TX) {
			DHD_ERROR(("Tx Request is not ended\n"));
			bus->dhd->busstate = DHD_BUS_DATA;
			
			dhd_bus_start_queue(bus);
			DHD_GENERAL_UNLOCK(bus->dhd, flags);
			bus->suspended = FALSE;
			return -EBUSY;
		}

		bus->dhd->dhd_bus_busy_state |= DHD_BUS_BUSY_IN_SUSPEND;
		DHD_GENERAL_UNLOCK(bus->dhd, flags);

		DHD_OS_WAKE_LOCK_WAIVE(bus->dhd);
		bus->d3_suspend_pending = TRUE;
		dhd_os_set_ioctl_resp_timeout(D3_ACK_RESP_TIMEOUT);
		dhdpcie_send_mb_data(bus, H2D_HOST_D3_INFORM);
		timeleft = dhd_os_d3ack_wait(bus->dhd, &bus->wait_for_d3_ack);
		dhd_os_set_ioctl_resp_timeout(IOCTL_RESP_TIMEOUT);
		DHD_OS_WAKE_LOCK_RESTORE(bus->dhd);

		
		{
			uint32 d2h_mb_data = 0;
			uint32 zero = 0;

			
			if (bus->wait_for_d3_ack == 0) {
				
				dhd_bus_cmn_readshared(bus, &d2h_mb_data, D2H_MB_DATA, 0);

				if (!D2H_DEV_MB_INVALIDATED(d2h_mb_data) &&
						(d2h_mb_data & D2H_DEV_D3_ACK)) {
					DHD_ERROR(("*** D3 WAR for missing interrupt ***\r\n"));
					
					dhd_bus_cmn_writeshared(bus, &zero, sizeof(uint32),
							D2H_MB_DATA, 0);

					
					bus->wait_for_d3_ack = 1;
					bus->d3_ack_war_cnt++;

				} 
			} 
		}

		while ((active = dhd_os_check_wakelock_all(bus->dhd)) &&
			(idle_retry < MAX_WKLK_IDLE_CHECK)) {
			msleep(1);
			idle_retry++;
		}

		if (bus->wait_for_d3_ack) {
			DHD_ERROR_HW_ONE(("%s: Got D3 Ack \n", __FUNCTION__));

			
			if (!bus->force_suspend && active) {
				DHD_ERROR_HW_ONE(("%s():Suspend failed because of wakelock restoring Dongle to D0\n",
					__FUNCTION__));


				
				bus->wait_for_d3_ack = 0;

				DHD_OS_WAKE_LOCK_WAIVE(bus->dhd);
				dhdpcie_send_mb_data(bus,
					(H2D_HOST_D0_INFORM_IN_USE|H2D_HOST_D0_INFORM));
				DHD_OS_WAKE_LOCK_RESTORE(bus->dhd);

				
				si_corereg(bus->sih, bus->sih->buscoreidx, PCIH2D_DB1, ~0, 0x12345678);

				bus->suspended = FALSE;
				DHD_GENERAL_LOCK(bus->dhd, flags);
				bus->d3_suspend_pending = FALSE;
				bus->dhd->busstate = DHD_BUS_DATA;
				
				dhd_bus_start_queue(bus);
				DHD_GENERAL_UNLOCK(bus->dhd, flags);
				rc = BCME_ERROR;
			} else {
				DHD_OS_WAKE_LOCK_WAIVE(bus->dhd);
				dhdpcie_send_mb_data(bus, (H2D_HOST_D0_INFORM_IN_USE));
				DHD_OS_WAKE_LOCK_RESTORE(bus->dhd);
				dhdpcie_bus_intr_disable(bus);
				bus->d3_suspend_pending = FALSE;
				rc = dhdpcie_pci_suspend_resume(bus, state);
				dhd_bus_set_device_wake(bus, FALSE);
#if defined(BCMPCIE_OOB_HOST_WAKE)
				dhdpcie_oob_intr_set(bus, TRUE);
#endif 
			}
			bus->dhd->d3ackcnt_timeout = 0;
		} else if (timeleft == 0) {
			bus->dhd->d3ack_timeout_occured = TRUE;
			bus->dhd->d3ackcnt_timeout++;
			DHD_ERROR(("%s: resumed on timeout for D3 ACK d3_inform_cnt %d \n",
				__FUNCTION__, bus->dhd->d3ackcnt_timeout));
			bus->suspended = FALSE;
			DHD_GENERAL_LOCK(bus->dhd, flags);
			bus->d3_suspend_pending = FALSE;
			bus->dhd->busstate = DHD_BUS_DATA;
			
			dhd_bus_start_queue(bus);
			DHD_GENERAL_UNLOCK(bus->dhd, flags);
			if (bus->dhd->d3ackcnt_timeout >= MAX_CNTL_D3ACK_TIMEOUT) {
				DHD_ERROR(("%s: Event HANG send up "
					"due to PCIe linkdown\n", __FUNCTION__));
#ifdef SUPPORT_LINKDOWN_RECOVERY
#ifdef CONFIG_ARCH_MSM
				bus->islinkdown = TRUE;
#endif 
#endif 
				dhd_os_check_hang(bus->dhd, 0, -ETIMEDOUT);
			}
			rc = -ETIMEDOUT;

		}
		bus->wait_for_d3_ack = 1;
		DHD_GENERAL_LOCK(bus->dhd, flags);
		bus->dhd->dhd_bus_busy_state &= ~DHD_BUS_BUSY_IN_SUSPEND;
		dhd_os_busbusy_wake(bus->dhd);
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
	} else {
		
#if defined(BCMPCIE_OOB_HOST_WAKE)
		DHD_OS_OOB_IRQ_WAKE_UNLOCK(bus->dhd);
#endif 
		DHD_GENERAL_LOCK(bus->dhd, flags);
		bus->dhd->dhd_bus_busy_state |= DHD_BUS_BUSY_IN_RESUME;
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
		rc = dhdpcie_pci_suspend_resume(bus, state);
		if (rc) {
			DHD_ERROR(("%s: PCIe resume failed %d\n",
				__FUNCTION__, rc));
			DHD_GENERAL_LOCK(bus->dhd, flags);
			bus->dhd->dhd_bus_busy_state &= ~DHD_BUS_BUSY_IN_RESUME;
			bus->dhd->busstate = DHD_BUS_DOWN;
			DHD_GENERAL_UNLOCK(bus->dhd, flags);
			return rc;
		}
		if (bus->dhd->busstate == DHD_BUS_SUSPEND) {
			DHD_OS_WAKE_LOCK_WAIVE(bus->dhd);
			dhdpcie_send_mb_data(bus, (H2D_HOST_D0_INFORM));
			DHD_OS_WAKE_LOCK_RESTORE(bus->dhd);
			dhd_bus_set_device_wake(bus, TRUE);
		}
		bus->suspended = FALSE;
		DHD_GENERAL_LOCK(bus->dhd, flags);
		bus->dhd->busstate = DHD_BUS_DATA;
		bus->dhd->dhd_bus_busy_state &= ~DHD_BUS_BUSY_IN_RESUME;
#ifdef DHD_PCIE_RUNTIMEPM
		if (bus->dhd->dhd_bus_busy_state & DHD_BUS_BUSY_RPM_SUSPEND_DONE) {
			bus->bus_wake = 1;
			OSL_SMP_WMB();
			wake_up_interruptible(&bus->rpm_queue);
		}
#endif 
		
		dhd_bus_start_queue(bus);
		dhd_os_busbusy_wake(bus->dhd);
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
		dhdpcie_bus_intr_enable(bus);
	}
	return rc;
}

static int
dhdpcie_bus_dmaxfer_req(struct  dhd_bus *bus, uint32 len, uint32 srcdelay, uint32 destdelay)
{
	if (bus->dhd == NULL) {
		DHD_ERROR(("bus not inited\n"));
		return BCME_ERROR;
	}
	if (bus->dhd->prot == NULL) {
		DHD_ERROR(("prot is not inited\n"));
		return BCME_ERROR;
	}
	if (bus->dhd->busstate != DHD_BUS_DATA) {
		DHD_ERROR(("not in a readystate to LPBK  is not inited\n"));
		return BCME_ERROR;
	}

	if (len < 5 || len > 4194296) {
		DHD_ERROR(("len is too small or too large\n"));
		return BCME_ERROR;
	}
	return dhdmsgbuf_dmaxfer_req(bus->dhd, len, srcdelay, destdelay);
}



static int
dhdpcie_bus_download_state(dhd_bus_t *bus, bool enter)
{
	int bcmerror = 0;
	uint32 *cr4_regs;

	if (!bus->sih) {
		DHD_ERROR(("%s: NULL sih!!\n", __FUNCTION__));
		return BCME_ERROR;
	}
	if (enter) {
		
		dhdpcie_bus_cfg_write_dword(bus, PCI_BAR1_WIN, 4, 0x00000000);
		bus->alp_only = TRUE;

		
		cr4_regs = si_setcore(bus->sih, ARMCR4_CORE_ID, 0);

		if (cr4_regs == NULL && !(si_setcore(bus->sih, ARM7S_CORE_ID, 0)) &&
		    !(si_setcore(bus->sih, ARMCM3_CORE_ID, 0)) &&
		    !(si_setcore(bus->sih, ARMCA7_CORE_ID, 0))) {
			DHD_ERROR(("%s: Failed to find ARM core!\n", __FUNCTION__));
			bcmerror = BCME_ERROR;
			goto fail;
		}

		if (si_setcore(bus->sih, ARMCA7_CORE_ID, 0)) {
			
			si_core_reset(bus->sih, SICF_CPUHALT, SICF_CPUHALT);
			if (!(si_setcore(bus->sih, SYSMEM_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find SYSMEM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
			si_core_reset(bus->sih, 0, 0);
			
			dhdpcie_init_shared_addr(bus);
		} else if (cr4_regs == NULL) { 
			si_core_disable(bus->sih, 0);

			if (!(si_setcore(bus->sih, SOCRAM_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find SOCRAM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			si_core_reset(bus->sih, 0, 0);

			
			if (bus->ramsize) {
				uint32 zeros = 0;
				if (dhdpcie_bus_membytes(bus, TRUE, bus->ramsize - 4,
				                     (uint8*)&zeros, 4) < 0) {
					bcmerror = BCME_ERROR;
					goto fail;
				}
			}
		} else {
			
			si_core_reset(bus->sih, SICF_CPUHALT, SICF_CPUHALT);
			if (BCM43602_CHIP(bus->sih->chip)) {
				W_REG(bus->pcie_mb_intr_osh, cr4_regs + ARMCR4REG_BANKIDX, 5);
				W_REG(bus->pcie_mb_intr_osh, cr4_regs + ARMCR4REG_BANKPDA, 0);
				W_REG(bus->pcie_mb_intr_osh, cr4_regs + ARMCR4REG_BANKIDX, 7);
				W_REG(bus->pcie_mb_intr_osh, cr4_regs + ARMCR4REG_BANKPDA, 0);
			}
			
			dhdpcie_init_shared_addr(bus);
		}
	} else {
		if (si_setcore(bus->sih, ARMCA7_CORE_ID, 0)) {
			
			if ((bcmerror = dhdpcie_bus_write_vars(bus))) {
				DHD_ERROR(("%s: could not write vars to RAM\n", __FUNCTION__));
				goto fail;
			}
			
			if (!(si_setcore(bus->sih, ARMCA7_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find ARM CA7 core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
			
			bcmerror = dhdpcie_bus_membytes(bus, TRUE, 0,
				(uint8 *)&bus->resetinstr, sizeof(bus->resetinstr));
			
		} else if (!si_setcore(bus->sih, ARMCR4_CORE_ID, 0)) {
			if (!(si_setcore(bus->sih, SOCRAM_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find SOCRAM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			if (!si_iscoreup(bus->sih)) {
				DHD_ERROR(("%s: SOCRAM core is down after reset?\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			if (!si_setcore(bus->sih, PCMCIA_CORE_ID, 0) &&
			    !si_setcore(bus->sih, SDIOD_CORE_ID, 0)) {
				DHD_ERROR(("%s: Can't change back to SDIO core?\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}


			if (!(si_setcore(bus->sih, ARM7S_CORE_ID, 0)) &&
			    !(si_setcore(bus->sih, ARMCM3_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find ARM core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}
		} else {
			if (BCM43602_CHIP(bus->sih->chip)) {
				
				if (!(si_setcore(bus->sih, SOCRAM_CORE_ID, 0))) {
					DHD_ERROR(("%s: Failed to find SOCRAM core!\n",
						__FUNCTION__));
					bcmerror = BCME_ERROR;
					goto fail;
				}
				si_core_reset(bus->sih, 0, 0);
				si_setcore(bus->sih, ARMCR4_CORE_ID, 0);
			}

			
			if ((bcmerror = dhdpcie_bus_write_vars(bus))) {
				DHD_ERROR(("%s: could not write vars to RAM\n", __FUNCTION__));
				goto fail;
			}

			
			if (!(si_setcore(bus->sih, ARMCR4_CORE_ID, 0))) {
				DHD_ERROR(("%s: Failed to find ARM CR4 core!\n", __FUNCTION__));
				bcmerror = BCME_ERROR;
				goto fail;
			}

			
			bcmerror = dhdpcie_bus_membytes(bus, TRUE, 0,
				(uint8 *)&bus->resetinstr, sizeof(bus->resetinstr));

			if (bcmerror == BCME_OK) {
				uint32 tmp;

				bcmerror = dhdpcie_bus_membytes(bus, FALSE, 0,
				                                (uint8 *)&tmp, sizeof(tmp));

				if (bcmerror == BCME_OK && tmp != bus->resetinstr) {
					DHD_ERROR(("%s: Failed to write 0x%08x to addr 0\n",
					          __FUNCTION__, bus->resetinstr));
					DHD_ERROR(("%s: contents of addr 0 is 0x%08x\n",
					          __FUNCTION__, tmp));
					bcmerror = BCME_ERROR;
					goto fail;
				}
			}

			
		}

		si_core_reset(bus->sih, 0, 0);

		
		bus->alp_only = FALSE;

		bus->dhd->busstate = DHD_BUS_LOAD;
	}

fail:
	
	si_setcore(bus->sih, PCIE2_CORE_ID, 0);

	return bcmerror;
} 

static int
dhdpcie_bus_write_vars(dhd_bus_t *bus)
{
	int bcmerror = 0;
	uint32 varsize, phys_size;
	uint32 varaddr;
	uint8 *vbuffer;
	uint32 varsizew;
#ifdef DHD_DEBUG
	uint8 *nvram_ularray;
#endif 

	/* Even if there are no vars are to be written, we still need to set the ramsize. */
	varsize = bus->varsz ? ROUNDUP(bus->varsz, 4) : 0;
	varaddr = (bus->ramsize - 4) - varsize;

	varaddr += bus->dongle_ram_base;

	if (bus->vars) {

		vbuffer = (uint8 *)MALLOC(bus->dhd->osh, varsize);
		if (!vbuffer)
			return BCME_NOMEM;

		bzero(vbuffer, varsize);
		bcopy(bus->vars, vbuffer, bus->varsz);
		
		bcmerror = dhdpcie_bus_membytes(bus, TRUE, varaddr, vbuffer, varsize);

		
#ifdef DHD_DEBUG
		
		DHD_INFO(("Compare NVRAM dl & ul; varsize=%d\n", varsize));
		nvram_ularray = (uint8*)MALLOC(bus->dhd->osh, varsize);
		if (!nvram_ularray)
			return BCME_NOMEM;

		
		memset(nvram_ularray, 0xaa, varsize);

		
		bcmerror = dhdpcie_bus_membytes(bus, FALSE, varaddr, nvram_ularray, varsize);
		if (bcmerror) {
				DHD_ERROR(("%s: error %d on reading %d nvram bytes at 0x%08x\n",
					__FUNCTION__, bcmerror, varsize, varaddr));
		}

		
		if (memcmp(vbuffer, nvram_ularray, varsize)) {
			DHD_ERROR(("%s: Downloaded NVRAM image is corrupted.\n", __FUNCTION__));
		} else
			DHD_ERROR(("%s: Download, Upload and compare of NVRAM succeeded.\n",
			__FUNCTION__));

		MFREE(bus->dhd->osh, nvram_ularray, varsize);
#endif 

		MFREE(bus->dhd->osh, vbuffer, varsize);
	}

	phys_size = REMAP_ENAB(bus) ? bus->ramsize : bus->orig_ramsize;

	phys_size += bus->dongle_ram_base;

	
	DHD_INFO(("Physical memory size: %d, usable memory size: %d\n",
		phys_size, bus->ramsize));
	DHD_INFO(("Vars are at %d, orig varsize is %d\n",
		varaddr, varsize));
	varsize = ((phys_size - 4) - varaddr);

	if (bcmerror) {
		varsizew = 0;
		bus->nvram_csm = varsizew;
	} else {
		varsizew = varsize / 4;
		varsizew = (~varsizew << 16) | (varsizew & 0x0000FFFF);
		bus->nvram_csm = varsizew;
		varsizew = htol32(varsizew);
	}

	DHD_INFO(("New varsize is %d, length token=0x%08x\n", varsize, varsizew));

	
	bcmerror = dhdpcie_bus_membytes(bus, TRUE, (phys_size - 4),
		(uint8*)&varsizew, 4);

	return bcmerror;
} 

int
dhdpcie_downloadvars(dhd_bus_t *bus, void *arg, int len)
{
	int bcmerror = BCME_OK;
#ifdef KEEP_JP_REGREV
	char *tmpbuf;
	uint tmpidx;
#endif 

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	
	if (bus->dhd->up) {
		bcmerror = BCME_NOTDOWN;
		goto err;
	}
	if (!len) {
		bcmerror = BCME_BUFTOOSHORT;
		goto err;
	}

	
	if (bus->vars)
		MFREE(bus->dhd->osh, bus->vars, bus->varsz);

	bus->vars = MALLOC(bus->dhd->osh, len);
	bus->varsz = bus->vars ? len : 0;
	if (bus->vars == NULL) {
		bcmerror = BCME_NOMEM;
		goto err;
	}

	
	bcopy(arg, bus->vars, bus->varsz);

#ifdef KEEP_JP_REGREV
	if (bus->vars != NULL && bus->varsz > 0) {
		char *pos = NULL;
		tmpbuf = MALLOCZ(bus->dhd->osh, bus->varsz + 1);
		if (tmpbuf == NULL) {
			goto err;
		}
		memcpy(tmpbuf, bus->vars, bus->varsz);
		for (tmpidx = 0; tmpidx < bus->varsz; tmpidx++) {
			if (tmpbuf[tmpidx] == 0) {
				tmpbuf[tmpidx] = '\n';
			}
		}
		bus->dhd->vars_ccode[0] = 0;
		bus->dhd->vars_regrev = 0;
		if ((pos = strstr(tmpbuf, "ccode"))) {
			sscanf(pos, "ccode=%s\n", bus->dhd->vars_ccode);
		}
		if ((pos = strstr(tmpbuf, "regrev"))) {
			sscanf(pos, "regrev=%u\n", &(bus->dhd->vars_regrev));
		}
		MFREE(bus->dhd->osh, tmpbuf, bus->varsz + 1);
	}
#endif 

err:
	return bcmerror;
}

#ifndef BCMPCIE_OOB_HOST_WAKE
uint8
dhdpcie_find_pci_capability(osl_t *osh, uint8 req_cap_id)
{
	uint8 cap_id;
	uint8 cap_ptr = 0;
	uint8 byte_val;

	
	byte_val = read_pci_cfg_byte(PCI_CFG_HDR);
	if ((byte_val & 0x7f) != PCI_HEADER_NORMAL) {
		DHD_ERROR(("%s : PCI config header not normal.\n", __FUNCTION__));
		goto end;
	}

	
	byte_val = read_pci_cfg_byte(PCI_CFG_STAT);
	if (!(byte_val & PCI_CAPPTR_PRESENT)) {
		DHD_ERROR(("%s : PCI CAP pointer not present.\n", __FUNCTION__));
		goto end;
	}

	cap_ptr = read_pci_cfg_byte(PCI_CFG_CAPPTR);
	
	if (cap_ptr == 0x00) {
		DHD_ERROR(("%s : PCI CAP pointer is 0x00.\n", __FUNCTION__));
		goto end;
	}

	

	cap_id = read_pci_cfg_byte(cap_ptr);

	while (cap_id != req_cap_id) {
		cap_ptr = read_pci_cfg_byte((cap_ptr + 1));
		if (cap_ptr == 0x00) break;
		cap_id = read_pci_cfg_byte(cap_ptr);
	}

end:
	return cap_ptr;
}

void
dhdpcie_pme_active(osl_t *osh, bool enable)
{
	uint8 cap_ptr;
	uint32 pme_csr;

	cap_ptr = dhdpcie_find_pci_capability(osh, PCI_CAP_POWERMGMTCAP_ID);

	if (!cap_ptr) {
		DHD_ERROR(("%s : Power Management Capability not present\n", __FUNCTION__));
		return;
	}

	pme_csr = OSL_PCI_READ_CONFIG(osh, cap_ptr + PME_CSR_OFFSET, sizeof(uint32));
	DHD_ERROR(("%s : pme_sts_ctrl 0x%x\n", __FUNCTION__, pme_csr));

	pme_csr |= PME_CSR_PME_STAT;
	if (enable) {
		pme_csr |= PME_CSR_PME_EN;
	} else {
		pme_csr &= ~PME_CSR_PME_EN;
	}

	OSL_PCI_WRITE_CONFIG(osh, cap_ptr + PME_CSR_OFFSET, sizeof(uint32), pme_csr);
}

bool
dhdpcie_pme_cap(osl_t *osh)
{
	uint8 cap_ptr;
	uint32 pme_cap;

	cap_ptr = dhdpcie_find_pci_capability(osh, PCI_CAP_POWERMGMTCAP_ID);

	if (!cap_ptr) {
		DHD_ERROR(("%s : Power Management Capability not present\n", __FUNCTION__));
		return FALSE;
	}

	pme_cap = OSL_PCI_READ_CONFIG(osh, cap_ptr, sizeof(uint32));

	DHD_ERROR(("%s : pme_cap 0x%x\n", __FUNCTION__, pme_cap));

	return ((pme_cap & PME_CAP_PM_STATES) != 0);
}
#endif 

#if defined(DHD_PCIE_RUNTIMEPM) && defined(CUSTOMER_HW_ONE)
void dhd_mfg_setidletime(dhd_pub_t *dhdp, int idle_time)
{
	if (!dhdp || !dhdp->bus) {
		DHD_ERROR(("%s Bus is invalid\n", __FUNCTION__));
		return;
	}
	dhdp->bus->idletime = idle_time;
}
#endif 

void dhd_bus_dump(dhd_pub_t *dhdp, struct bcmstrbuf *strbuf)
{
	uint16 flowid;
	int ix = 0;
	flow_ring_node_t *flow_ring_node;
	flow_info_t *flow_info;
	char eabuf[ETHER_ADDR_STR_LEN];

	if (dhdp->busstate != DHD_BUS_DATA)
		return;

	dhd_prot_print_info(dhdp, strbuf);
	bcm_bprintf(strbuf, "dhd cumm_ctr %d\n", DHD_CUMM_CTR_READ(&dhdp->cumm_ctr));
	bcm_bprintf(strbuf,
		"%s %4s %2s %4s %17s %4s %4s %6s %10s %4s %4s ",
		"Num:", "Flow", "If", "Prio", ":Dest_MacAddress:", "Qlen", "CLen", "L2CLen",
		"Overflows", "RD", "WR");
	bcm_bprintf(strbuf, "%5s %6s %5s \n", "Acked", "tossed", "noack");

	for (flowid = 0; flowid < dhdp->num_flow_rings; flowid++) {
		flow_ring_node = DHD_FLOW_RING(dhdp, flowid);
		if (!flow_ring_node->active)
			continue;

		flow_info = &flow_ring_node->flow_info;
		bcm_bprintf(strbuf,
			"%3d. %4d %2d %4d %17s %4d %4d %6d %10u ", ix++,
			flow_ring_node->flowid, flow_info->ifindex, flow_info->tid,
			bcm_ether_ntoa((struct ether_addr *)&flow_info->da, eabuf),
			DHD_FLOW_QUEUE_LEN(&flow_ring_node->queue),
			DHD_CUMM_CTR_READ(DHD_FLOW_QUEUE_CLEN_PTR(&flow_ring_node->queue)),
			DHD_CUMM_CTR_READ(DHD_FLOW_QUEUE_L2CLEN_PTR(&flow_ring_node->queue)),
			DHD_FLOW_QUEUE_FAILURES(&flow_ring_node->queue));
		dhd_prot_print_flow_ring(dhdp, flow_ring_node->prot_info, strbuf,
			"%4d %4d ");
		bcm_bprintf(strbuf,
			"%5s %6s %5s\n", "NA", "NA", "NA");
	}
	bcm_bprintf(strbuf, "D3 inform cnt %d\n", dhdp->bus->d3_inform_cnt);
	bcm_bprintf(strbuf, "D0 inform cnt %d\n", dhdp->bus->d0_inform_cnt);
	bcm_bprintf(strbuf, "D0 inform in use cnt %d\n", dhdp->bus->d0_inform_in_use_cnt);
	bcm_bprintf(strbuf, "D3 Ack WAR cnt %d\n", dhdp->bus->d3_ack_war_cnt);
}

static void
dhd_update_txflowrings(dhd_pub_t *dhd)
{
	unsigned long flags;
	dll_t *item, *next;
	flow_ring_node_t *flow_ring_node;
	struct dhd_bus *bus = dhd->bus;

	DHD_FLOWRING_LIST_LOCK(bus->dhd->flowring_list_lock, flags);
	for (item = dll_head_p(&bus->const_flowring);
		(!dhd_is_device_removed(dhd) && !dll_end(&bus->const_flowring, item));
		item = next) {
		if (dhd->hang_was_sent) {
			break;
		}

		next = dll_next_p(item);
		flow_ring_node = dhd_constlist_to_flowring(item);
		dhd_prot_update_txflowring(dhd, flow_ring_node->flowid, flow_ring_node->prot_info);
	}
	DHD_FLOWRING_LIST_UNLOCK(bus->dhd->flowring_list_lock, flags);
}

static void
dhd_bus_gen_devmb_intr(struct dhd_bus *bus)
{
	if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
		(bus->sih->buscorerev == 4)) {
		DHD_ERROR(("mailbox communication not supported\n"));
		return;
	}
	if (bus->db1_for_mb)  {
		
		DHD_INFO(("writing a mail box interrupt to the device, through doorbell 1\n"));
		si_corereg(bus->sih, bus->sih->buscoreidx, PCIH2D_DB1, ~0, 0x12345678);
	} else {
		DHD_INFO(("writing a mail box interrupt to the device, through config space\n"));
		dhdpcie_bus_cfg_write_dword(bus, PCISBMbx, 4, (1 << 0));
		dhdpcie_bus_cfg_write_dword(bus, PCISBMbx, 4, (1 << 0));
	}
}

static void
dhd_bus_set_device_wake(struct dhd_bus *bus, bool val)
{
	if (bus->device_wake_state != val)
	{
		DHD_INFO(("Set Device_Wake to %d\n", val));
#ifdef PCIE_OOB
		if (bus->oob_enabled)
		{
			if (val)
			{
				gpio_port = gpio_port | (1 << DEVICE_WAKE);
				gpio_write_port_non_block(gpio_handle_val, gpio_port);
			} else {
				gpio_port = gpio_port & (0xff ^ (1 << DEVICE_WAKE));
				gpio_write_port_non_block(gpio_handle_val, gpio_port);
			}
		}
#endif 
		bus->device_wake_state = val;
	}
}

#ifdef PCIE_OOB
void
dhd_oob_set_bt_reg_on(struct dhd_bus *bus, bool val)
{
	DHD_INFO(("Set Device_Wake to %d\n", val));
	if (val)
	{
		gpio_port = gpio_port | (1 << BIT_BT_REG_ON);
		gpio_write_port(gpio_handle_val, gpio_port);
	} else {
		gpio_port = gpio_port & (0xff ^ (1 << BIT_BT_REG_ON));
		gpio_write_port(gpio_handle_val, gpio_port);
	}
}

int
dhd_oob_get_bt_reg_on(struct dhd_bus *bus)
{
	int ret;
	uint8 val;
	ret = gpio_read_port(gpio_handle_val, &val);

	if (ret < 0) {
		DHD_ERROR(("gpio_read_port returns %d\n", ret));
		return ret;
	}

	if (val & (1 << BIT_BT_REG_ON))
	{
		ret = 1;
	} else {
		ret = 0;
	}

	return ret;
}

static void
dhd_bus_doorbell_timeout_reset(struct dhd_bus *bus)
{
	if (dhd_doorbell_timeout)
		dhd_timeout_start(&bus->doorbell_timer,
			(dhd_doorbell_timeout * 1000) / dhd_watchdog_ms);
	else if (!(bus->dhd->busstate == DHD_BUS_SUSPEND))
		dhd_bus_set_device_wake(bus, FALSE);
}
#endif 

void
dhd_bus_ringbell(struct dhd_bus *bus, uint32 value)
{
	
	if ((bus->dhd->busstate == DHD_BUS_SUSPEND || bus->d3_suspend_pending) && bus->wait_for_d3_ack) {
		DHD_ERROR(("%s: trying to ring the doorbell when in suspend state\n",
			__FUNCTION__));
		return;
	}
	if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
		(bus->sih->buscorerev == 4)) {
		si_corereg(bus->sih, bus->sih->buscoreidx, PCIMailBoxInt, PCIE_INTB, PCIE_INTB);
	} else {
		
		DHD_INFO(("writing a door bell to the device\n"));
		si_corereg(bus->sih, bus->sih->buscoreidx, PCIH2D_MailBox, ~0, 0x12345678);
	}
}

void
dhdpcie_bus_ringbell_fast(struct dhd_bus *bus, uint32 value)
{
	if ((bus->dhd->busstate == DHD_BUS_SUSPEND || bus->d3_suspend_pending) && bus->wait_for_d3_ack) {
		DHD_ERROR(("%s: trying to ring the doorbell when in suspend state\n",
			__FUNCTION__));
		return;
	}
#ifdef PCIE_OOB
	dhd_bus_set_device_wake(bus, TRUE);
	dhd_bus_doorbell_timeout_reset(bus);
#endif
	W_REG(bus->pcie_mb_intr_osh, bus->pcie_mb_intr_addr, value);
}

static void
dhd_bus_ringbell_oldpcie(struct dhd_bus *bus, uint32 value)
{
	uint32 w;
	
	if ((bus->dhd->busstate == DHD_BUS_SUSPEND || bus->d3_suspend_pending ) && bus->wait_for_d3_ack) {
		DHD_ERROR(("%s: trying to ring the doorbell when in suspend state\n",
			__FUNCTION__));
		return;
	}
	w = (R_REG(bus->pcie_mb_intr_osh, bus->pcie_mb_intr_addr) & ~PCIE_INTB) | PCIE_INTB;
	W_REG(bus->pcie_mb_intr_osh, bus->pcie_mb_intr_addr, w);
}

dhd_mb_ring_t
dhd_bus_get_mbintr_fn(struct dhd_bus *bus)
{
	if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
		(bus->sih->buscorerev == 4)) {
		bus->pcie_mb_intr_addr = si_corereg_addr(bus->sih, bus->sih->buscoreidx,
			PCIMailBoxInt);
		if (bus->pcie_mb_intr_addr) {
			bus->pcie_mb_intr_osh = si_osh(bus->sih);
			return dhd_bus_ringbell_oldpcie;
		}
	} else {
		bus->pcie_mb_intr_addr = si_corereg_addr(bus->sih, bus->sih->buscoreidx,
			PCIH2D_MailBox);
		if (bus->pcie_mb_intr_addr) {
			bus->pcie_mb_intr_osh = si_osh(bus->sih);
			return dhdpcie_bus_ringbell_fast;
		}
	}
	return dhd_bus_ringbell;
}

bool BCMFASTPATH
dhd_bus_dpc(struct dhd_bus *bus)
{
	bool resched = FALSE;	  
	unsigned long flags;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	DHD_GENERAL_LOCK(bus->dhd, flags);
	if (bus->dhd->busstate == DHD_BUS_DOWN) {
		DHD_ERROR(("%s: Bus down, ret\n", __FUNCTION__));
		bus->intstatus = 0;
		DHD_GENERAL_UNLOCK(bus->dhd, flags);
		return 0;
	}
	bus->dhd->dhd_bus_busy_state |= DHD_BUS_BUSY_IN_DPC;
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	bus->ipend = FALSE;
	resched = dhdpcie_bus_process_mailbox_intr(bus, bus->intstatus);
	if (!resched) {
		bus->intstatus = 0;
		if (!bus->pci_d3hot_done && !bus->dhd->hang_was_sent) {
			dhdpcie_bus_intr_enable(bus);
		} else {
			DHD_ERROR(("%s: dhdpcie_bus_intr_enable skip in pci D3hot state \n",
					__FUNCTION__));
		}
	}

	DHD_GENERAL_LOCK(bus->dhd, flags);
	bus->dhd->dhd_bus_busy_state &= ~DHD_BUS_BUSY_IN_DPC;
	dhd_os_busbusy_wake(bus->dhd);
	DHD_GENERAL_UNLOCK(bus->dhd, flags);

	return resched;

}


static void
dhdpcie_send_mb_data(dhd_bus_t *bus, uint32 h2d_mb_data)
{
	uint32 cur_h2d_mb_data = 0;

	DHD_INFO_HW4(("%s: H2D_MB_DATA: 0x%08X\n", __FUNCTION__, h2d_mb_data));
	if(bus->islinkdown) {
		DHD_ERROR(("%s: PCIe link was down\n", __FUNCTION__));
		return;
	}

	dhd_bus_cmn_readshared(bus, &cur_h2d_mb_data, H2D_MB_DATA, 0);

	if (cur_h2d_mb_data != 0) {
		uint32 i = 0;
		DHD_INFO(("GRRRRRRR: MB transaction is already pending 0x%04x\n", cur_h2d_mb_data));
		while ((i++ < 100) && cur_h2d_mb_data) {
			OSL_DELAY(10);
			dhd_bus_cmn_readshared(bus, &cur_h2d_mb_data, H2D_MB_DATA, 0);
		}
		if (i >= 100) {
			DHD_ERROR(("%s : waited 1ms for the dngl "
				"to ack the previous mb transaction\n", __FUNCTION__));
			DHD_ERROR(("%s : MB transaction is still pending 0x%04x\n",
				__FUNCTION__, cur_h2d_mb_data));
		}
	}

	dhd_bus_cmn_writeshared(bus, &h2d_mb_data, sizeof(uint32), H2D_MB_DATA, 0);
	dhd_bus_gen_devmb_intr(bus);

	if (h2d_mb_data == H2D_HOST_D3_INFORM) {
		DHD_INFO_HW4(("%s: send H2D_HOST_D3_INFORM to dongle\n", __FUNCTION__));
		bus->d3_inform_cnt++;
	}
	if (h2d_mb_data == H2D_HOST_D0_INFORM_IN_USE) {
		DHD_INFO_HW4(("%s: send H2D_HOST_D0_INFORM_IN_USE to dongle\n", __FUNCTION__));
		bus->d0_inform_in_use_cnt++;
	}
	if (h2d_mb_data == H2D_HOST_D0_INFORM) {
		DHD_INFO_HW4(("%s: send H2D_HOST_D0_INFORM to dongle\n", __FUNCTION__));
		bus->d0_inform_cnt++;
	}
}

static void
dhdpcie_handle_mb_data(dhd_bus_t *bus)
{
	uint32 d2h_mb_data = 0;
	uint32 zero = 0;

#ifdef CUSTOMER_HW_ONE
	if (bus->dhd->os_stopped) {
		DHD_ERROR(("%s: interface stopped.\n", __FUNCTION__));
		return;
	}
#endif

	dhd_bus_cmn_readshared(bus, &d2h_mb_data, D2H_MB_DATA, 0);
	if (!d2h_mb_data) {
		DHD_INFO_HW4(("%s: Invalid D2H_MB_DATA: 0x%08x\n",
			__FUNCTION__, d2h_mb_data));
		return;
	}

	dhd_bus_cmn_writeshared(bus, &zero, sizeof(uint32), D2H_MB_DATA, 0);

	DHD_INFO_HW4(("D2H_MB_DATA: 0x%08x\n", d2h_mb_data));
	if (d2h_mb_data & D2H_DEV_FWHALT)  {
		DHD_ERROR(("FW trap has happened\n"));
		DHD_OS_WAKE_LOCK(bus->dhd);
		dhdpcie_checkdied(bus, NULL, 0);
		
		bus->dhd->busstate = DHD_BUS_DOWN;
		DHD_OS_WAKE_UNLOCK(bus->dhd);
		return;
	}
	if (d2h_mb_data & D2H_DEV_DS_ENTER_REQ)  {
		if ((bus->dhd->busstate == DHD_BUS_SUSPEND || bus->d3_suspend_pending) && bus->wait_for_d3_ack) {
			DHD_ERROR(("DS-ENTRY AFTER D3-ACK!!!!! QUITING\n"));
			bus->dhd->busstate = DHD_BUS_DOWN;
			return;
		}
		
		DHD_INFO(("D2H_MB_DATA: DEEP SLEEP REQ\n"));
		dhdpcie_send_mb_data(bus, H2D_HOST_DS_ACK);
		DHD_INFO(("D2H_MB_DATA: sent DEEP SLEEP ACK\n"));
	}
	if (d2h_mb_data & D2H_DEV_DS_EXIT_NOTE)  {
		
		DHD_INFO(("D2H_MB_DATA: DEEP SLEEP EXIT\n"));
	}
	if (d2h_mb_data & D2H_DEV_D3_ACK)  {
		
		DHD_INFO_HW4(("D2H_MB_DATA: D3 ACK\n"));
		if (!bus->wait_for_d3_ack) {
			bus->wait_for_d3_ack = 1;
			dhd_os_d3ack_wake(bus->dhd);
		}
	}
}

static bool
dhdpcie_bus_process_mailbox_intr(dhd_bus_t *bus, uint32 intstatus)
{
	bool resched = FALSE;

	if ((bus->sih->buscorerev == 2) || (bus->sih->buscorerev == 6) ||
		(bus->sih->buscorerev == 4)) {
		
		if (intstatus & I_BIT1) {
			resched = dhdpci_bus_read_frames(bus);
		} else if (intstatus & I_BIT0) {
			
		}
	} else {
		if (intstatus & (PCIE_MB_TOPCIE_FN0_0 | PCIE_MB_TOPCIE_FN0_1))
			dhdpcie_handle_mb_data(bus);

		if (bus->dhd->busstate == DHD_BUS_SUSPEND) {
			goto exit;
		}

		if (intstatus & PCIE_MB_D2H_MB_MASK) {
			resched = dhdpci_bus_read_frames(bus);
		}
	}

exit:
	return resched;
}

static bool
dhdpci_bus_read_frames(dhd_bus_t *bus)
{
	bool more = FALSE;

	
	DHD_PERIM_LOCK_ALL((bus->dhd->fwder_unit % FWDER_MAX_UNIT));
	dhd_prot_process_ctrlbuf(bus->dhd);
	
	DHD_PERIM_UNLOCK_ALL((bus->dhd->fwder_unit % FWDER_MAX_UNIT));

	DHD_PERIM_LOCK_ALL((bus->dhd->fwder_unit % FWDER_MAX_UNIT));
	
	dhd_update_txflowrings(bus->dhd);

	more |= dhd_prot_process_msgbuf_txcpl(bus->dhd, dhd_txbound);

	more |= dhd_prot_process_msgbuf_rxcpl(bus->dhd, dhd_rxbound);

	
	if (bus->dhd->hang_was_sent) {
		more = FALSE;
	}
	DHD_PERIM_UNLOCK_ALL((bus->dhd->fwder_unit % FWDER_MAX_UNIT));

	return more;
}

bool
dhdpcie_tcm_valid(dhd_bus_t *bus)
{
	uint32 addr = 0;
	int rv;
	uint32 shaddr = 0;
	pciedev_shared_t sh;

	shaddr = bus->dongle_ram_base + bus->ramsize - 4;

	
	addr = LTOH32(dhdpcie_bus_rtcm32(bus, shaddr));

	if ((addr == 0) || (addr == bus->nvram_csm) || (addr < bus->dongle_ram_base) ||
		(addr > shaddr)) {
		DHD_ERROR(("%s: address (0x%08x) of pciedev_shared invalid addr\n",
			__FUNCTION__, addr));
		return FALSE;
	}

	
	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr, (uint8 *)&sh,
		sizeof(pciedev_shared_t))) < 0) {
		DHD_ERROR(("Failed to read PCIe shared struct with %d\n", rv));
		return FALSE;
	}

	
	if (sh.console_addr != bus->pcie_sh->console_addr) {
		DHD_ERROR(("Contents of pciedev_shared_t structure are not matching.\n"));
		return FALSE;
	}

	return TRUE;
}

static bool
dhdpcie_check_firmware_compatible(uint32 firmware_api_version, uint32 host_api_version)
{
	DHD_INFO(("firmware api revision %d, host api revision %d\n",
		firmware_api_version, host_api_version));
	if (firmware_api_version <= host_api_version)
		return TRUE;
	if ((firmware_api_version == 6) && (host_api_version == 5))
		return TRUE;
	if ((firmware_api_version == 5) && (host_api_version == 6))
		return TRUE;
	return FALSE;
}

static int
dhdpcie_readshared(dhd_bus_t *bus)
{
	uint32 addr = 0;
	int rv, dma_indx_wr_buf, dma_indx_rd_buf;
	uint32 shaddr = 0;
	pciedev_shared_t *sh = bus->pcie_sh;
	dhd_timeout_t tmo;

	shaddr = bus->dongle_ram_base + bus->ramsize - 4;
	DHD_ERROR(("%s: dongle_ram_base 0x%x ramsize 0x%x tcm %p shaddr 0x%x nvram_csm 0x%x\n",
		__FUNCTION__, bus->dongle_ram_base, bus->ramsize, bus->tcm, shaddr, bus->nvram_csm));
	
	dhd_timeout_start(&tmo, MAX_READ_TIMEOUT);

	while (((addr == 0) || (addr == bus->nvram_csm)) && !dhd_timeout_expired(&tmo)) {
		
		addr = LTOH32(dhdpcie_bus_rtcm32(bus, shaddr));
		OSL_CACHE_INV((void *)(bus->tcm + shaddr), sizeof(addr)); 
		OSL_CPU_RELAX(); 
	}

	if ((addr == 0) || (addr == bus->nvram_csm) || (addr < bus->dongle_ram_base) ||
		(addr > shaddr)) {
		DHD_ERROR(("%s: address (0x%08x) of pciedev_shared invalid at shaddr (0x%08x)\n",
			__FUNCTION__, addr, shaddr));
		DHD_ERROR(("Waited %u usec, dongle is not ready\n", tmo.elapsed));
#ifdef DHD_FW_COREDUMP
		
		if (addr != 0xffffffff) {
			
#if 0
			bus->dhd->memdump_enabled = DUMP_MEMFILE_BUGON;
#else
			bus->dhd->memdump_enabled = DUMP_MEMFILE;
#endif
			
			bus->dhd->memdump_type = DUMP_TYPE_READ_SHM_FAILED;
			dhd_bus_mem_dump(bus->dhd);
		}
#endif 
		return BCME_ERROR;
	} else {
		bus->shared_addr = (ulong)addr;
		DHD_ERROR(("PCIe shared addr (0x%08x) read took %u usec "
			"before dongle is ready\n", addr, tmo.elapsed));
	}

	
	if ((rv = dhdpcie_bus_membytes(bus, FALSE, addr, (uint8 *)sh,
		sizeof(pciedev_shared_t))) < 0) {
		DHD_ERROR(("Failed to read PCIe shared struct with %d\n", rv));
		return rv;
	}

	
	sh->flags = ltoh32(sh->flags);
	sh->trap_addr = ltoh32(sh->trap_addr);
	sh->assert_exp_addr = ltoh32(sh->assert_exp_addr);
	sh->assert_file_addr = ltoh32(sh->assert_file_addr);
	sh->assert_line = ltoh32(sh->assert_line);
	sh->console_addr = ltoh32(sh->console_addr);
	sh->msgtrace_addr = ltoh32(sh->msgtrace_addr);
	sh->dma_rxoffset = ltoh32(sh->dma_rxoffset);
	sh->rings_info_ptr = ltoh32(sh->rings_info_ptr);

#ifdef DHD_DEBUG
	
	bus->console_addr = sh->console_addr;
#endif

	
	bus->dma_rxoffset = bus->pcie_sh->dma_rxoffset;
	dhd_prot_rx_dataoffset(bus->dhd, bus->dma_rxoffset);

	DHD_ERROR(("DMA RX offset from shared Area %d\n", bus->dma_rxoffset));

	if (!(dhdpcie_check_firmware_compatible(sh->flags & PCIE_SHARED_VERSION_MASK,
		PCIE_SHARED_VERSION)))
	{
		DHD_ERROR(("%s: pcie_shared version %d in dhd "
		           "is older than pciedev_shared version %d in dongle\n",
		           __FUNCTION__, PCIE_SHARED_VERSION,
		           sh->flags & PCIE_SHARED_VERSION_MASK));
		return BCME_ERROR;
	}

	bus->rw_index_sz = (sh->flags & PCIE_SHARED_2BYTE_INDICES) ?
		sizeof(uint16) : sizeof(uint32);
	DHD_ERROR(("%s: Dongle advertizes %d size indices\n",
		__FUNCTION__, bus->rw_index_sz));

	
	if (sh->flags & PCIE_SHARED_DMA_INDEX) {


		DHD_ERROR(("%s: Host support DMAing indices: H2D:%d - D2H:%d. FW supports it\n",
			__FUNCTION__,
			(DMA_INDX_ENAB(bus->dhd->dma_h2d_ring_upd_support) ? 1 : 0),
			(DMA_INDX_ENAB(bus->dhd->dma_d2h_ring_upd_support) ? 1 : 0)));

	} else if (DMA_INDX_ENAB(bus->dhd->dma_d2h_ring_upd_support) ||
	           DMA_INDX_ENAB(bus->dhd->dma_h2d_ring_upd_support)) {

#ifdef BCM_INDX_DMA
		DHD_ERROR(("%s: Incompatible FW. FW does not support DMAing indices\n",
			__FUNCTION__));
		return BCME_ERROR;
#endif
		DHD_ERROR(("%s: Host supports DMAing indices but FW does not\n",
			__FUNCTION__));
		bus->dhd->dma_d2h_ring_upd_support = FALSE;
		bus->dhd->dma_h2d_ring_upd_support = FALSE;
	}


	
	{
		ring_info_t  ring_info;

		if ((rv = dhdpcie_bus_membytes(bus, FALSE, sh->rings_info_ptr,
			(uint8 *)&ring_info, sizeof(ring_info_t))) < 0)
			return rv;

		bus->h2d_mb_data_ptr_addr = ltoh32(sh->h2d_mb_data_ptr);
		bus->d2h_mb_data_ptr_addr = ltoh32(sh->d2h_mb_data_ptr);


		bus->max_sub_queues = ltoh16(ring_info.max_sub_queues);

		if (DMA_INDX_ENAB(bus->dhd->dma_h2d_ring_upd_support)) {
			dma_indx_wr_buf = dhd_prot_dma_indx_init(bus->dhd, bus->rw_index_sz,
				H2D_DMA_INDX_WR_BUF, bus->max_sub_queues);
			dma_indx_rd_buf = dhd_prot_dma_indx_init(bus->dhd, bus->rw_index_sz,
				D2H_DMA_INDX_RD_BUF, BCMPCIE_D2H_COMMON_MSGRINGS);

			if ((dma_indx_wr_buf != BCME_OK) || (dma_indx_rd_buf != BCME_OK)) {
				DHD_ERROR(("%s: Failed to allocate memory for dma'ing h2d indices"
					"Host will use w/r indices in TCM\n",
					__FUNCTION__));
				bus->dhd->dma_h2d_ring_upd_support = FALSE;
			}
		}

		if (DMA_INDX_ENAB(bus->dhd->dma_d2h_ring_upd_support)) {
			dma_indx_wr_buf = dhd_prot_dma_indx_init(bus->dhd, bus->rw_index_sz,
				D2H_DMA_INDX_WR_BUF, BCMPCIE_D2H_COMMON_MSGRINGS);
			dma_indx_rd_buf = dhd_prot_dma_indx_init(bus->dhd, bus->rw_index_sz,
				H2D_DMA_INDX_RD_BUF, bus->max_sub_queues);

			if ((dma_indx_wr_buf != BCME_OK) || (dma_indx_rd_buf != BCME_OK)) {
				DHD_ERROR(("%s: Failed to allocate memory for dma'ing d2h indices"
					"Host will use w/r indices in TCM\n",
					__FUNCTION__));
				bus->dhd->dma_d2h_ring_upd_support = FALSE;
			}
		}

		
		dhd_fillup_ring_sharedptr_info(bus, &ring_info);

		bcm_print_bytes("ring_info_raw", (uchar *)&ring_info, sizeof(ring_info_t));
		DHD_INFO(("ring_info\n"));

		DHD_ERROR(("%s: max H2D queues %d\n",
			__FUNCTION__, ltoh16(ring_info.max_sub_queues)));

		DHD_INFO(("mail box address\n"));
		DHD_INFO(("%s: h2d_mb_data_ptr_addr 0x%04x\n",
			__FUNCTION__, bus->h2d_mb_data_ptr_addr));
		DHD_INFO(("%s: d2h_mb_data_ptr_addr 0x%04x\n",
			__FUNCTION__, bus->d2h_mb_data_ptr_addr));
	}

	bus->dhd->d2h_sync_mode = sh->flags & PCIE_SHARED_D2H_SYNC_MODE_MASK;
	DHD_INFO(("%s: d2h_sync_mode 0x%08x\n",
		__FUNCTION__, bus->dhd->d2h_sync_mode));

	return BCME_OK;
} 

static void
dhd_fillup_ring_sharedptr_info(dhd_bus_t *bus, ring_info_t *ring_info)
{
	uint16 i = 0;
	uint16 j = 0;
	uint32 tcm_memloc;
	uint32	d2h_w_idx_ptr, d2h_r_idx_ptr, h2d_w_idx_ptr, h2d_r_idx_ptr;

	

	{
		
		tcm_memloc = ltoh32(ring_info->ringmem_ptr);

		
		for (i = 0; i <= BCMPCIE_COMMON_MSGRING_MAX_ID; i++) {
			bus->ring_sh[i].ring_mem_addr = tcm_memloc;
			
			tcm_memloc = tcm_memloc + sizeof(ring_mem_t);
			DHD_INFO(("ring id %d ring mem addr 0x%04x \n",
				i, bus->ring_sh[i].ring_mem_addr));
		}
	}

	
	{
		d2h_w_idx_ptr = ltoh32(ring_info->d2h_w_idx_ptr);
		d2h_r_idx_ptr = ltoh32(ring_info->d2h_r_idx_ptr);
		h2d_w_idx_ptr = ltoh32(ring_info->h2d_w_idx_ptr);
		h2d_r_idx_ptr = ltoh32(ring_info->h2d_r_idx_ptr);

		
		for (i = 0; i < BCMPCIE_H2D_COMMON_MSGRINGS; i++) {
			bus->ring_sh[i].ring_state_w = h2d_w_idx_ptr;
			bus->ring_sh[i].ring_state_r = h2d_r_idx_ptr;

			
			h2d_w_idx_ptr = h2d_w_idx_ptr + bus->rw_index_sz;
			h2d_r_idx_ptr = h2d_r_idx_ptr + bus->rw_index_sz;

			DHD_INFO(("h2d w/r : idx %d write %x read %x \n", i,
				bus->ring_sh[i].ring_state_w, bus->ring_sh[i].ring_state_r));
		}

		
		for (j = 0; j < BCMPCIE_D2H_COMMON_MSGRINGS; j++, i++) {
			bus->ring_sh[i].ring_state_w = d2h_w_idx_ptr;
			bus->ring_sh[i].ring_state_r = d2h_r_idx_ptr;

			
			d2h_w_idx_ptr = d2h_w_idx_ptr + bus->rw_index_sz;
			d2h_r_idx_ptr = d2h_r_idx_ptr + bus->rw_index_sz;

			DHD_INFO(("d2h w/r : idx %d write %x read %x \n", i,
				bus->ring_sh[i].ring_state_w, bus->ring_sh[i].ring_state_r));
		}

		
		for (j = 0; j < (bus->max_sub_queues - BCMPCIE_H2D_COMMON_MSGRINGS);
			i++, j++)
		{
			bus->ring_sh[i].ring_state_w = h2d_w_idx_ptr;
			bus->ring_sh[i].ring_state_r = h2d_r_idx_ptr;

			
			h2d_w_idx_ptr = h2d_w_idx_ptr + bus->rw_index_sz;
			h2d_r_idx_ptr = h2d_r_idx_ptr + bus->rw_index_sz;

			DHD_INFO(("FLOW Rings h2d w/r : idx %d write %x read %x \n", i,
				bus->ring_sh[i].ring_state_w,
				bus->ring_sh[i].ring_state_r));
		}
	}
} 

int dhd_bus_init(dhd_pub_t *dhdp, bool enforce_mutex)
{
	dhd_bus_t *bus = dhdp->bus;
	int  ret = 0;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	ASSERT(bus->dhd);
	if (!bus->dhd)
		return 0;

	
	bus->reg = si_setcore(bus->sih, PCIE2_CORE_ID, 0);
	ASSERT(bus->reg != NULL);

	
	ret = dhdpcie_readshared(bus);
	if (ret < 0) {
		DHD_ERROR(("%s :Shared area read failed \n", __FUNCTION__));
		return ret;
	}

	
	bus->reg = si_setcore(bus->sih, PCIE2_CORE_ID, 0);
	ASSERT(bus->reg != NULL);

	
	dhdp->busstate = DHD_BUS_DATA;

	if (!dhd_download_fw_on_driverload)
		dhd_dpc_enable(bus->dhd);

	
	dhdpcie_bus_intr_enable(bus);

	

#ifdef DHD_PCIE_RUNTIMEPM
	bus->idlecount = 0;
	bus->idletime = (int32)MAX_IDLE_COUNT;
	init_waitqueue_head(&bus->rpm_queue);
	mutex_init(&bus->pm_lock);
#endif 
	bus->d3_ack_war_cnt=0;
	return ret;
}

static void
dhdpcie_init_shared_addr(dhd_bus_t *bus)
{
	uint32 addr = 0;
	uint32 val = 0;
	addr = bus->dongle_ram_base + bus->ramsize - 4;
#ifdef DHD_PCIE_RUNTIMEPM
	dhdpcie_runtime_bus_wake(bus->dhd, TRUE, __builtin_return_address(0));
#endif 
	dhdpcie_bus_membytes(bus, TRUE, addr, (uint8 *)&val, sizeof(val));
}


bool
dhdpcie_chipmatch(uint16 vendor, uint16 device)
{
	if (vendor != PCI_VENDOR_ID_BROADCOM) {
		DHD_ERROR(("%s: Unsupported vendor %x device %x\n", __FUNCTION__,
			vendor, device));
		return (-ENODEV);
	}

#ifdef CUSTOMER_HW_ONE
	if (device == BCM4359_D11AC_ID || device == BCM43596_D11AC_ID) {
		DHD_ERROR(("%s: WIFI_OTP: OTP is NOT empty\n", __FUNCTION__));
		
		
		otp_write = 1;
		
	} else {
		DHD_ERROR(("%s: WIFI_OTP: OTP is empty\n", __FUNCTION__));
		
		
		otp_write = 0;
		
	}
#endif 

	if ((device == BCM4350_D11AC_ID) || (device == BCM4350_D11AC2G_ID) ||
		(device == BCM4350_D11AC5G_ID) || (device == BCM4350_CHIP_ID) ||
		(device == BCM43569_CHIP_ID))
		return 0;

	if ((device == BCM4354_D11AC_ID) || (device == BCM4354_D11AC2G_ID) ||
		(device == BCM4354_D11AC5G_ID) || (device == BCM4354_CHIP_ID))
		return 0;

	if ((device == BCM4356_D11AC_ID) || (device == BCM4356_D11AC2G_ID) ||
		(device == BCM4356_D11AC5G_ID) || (device == BCM4356_CHIP_ID))
		return 0;

	if ((device == BCM4345_D11AC_ID) || (device == BCM4345_D11AC2G_ID) ||
		(device == BCM4345_D11AC5G_ID) || BCM4345_CHIP(device))
		return 0;

	if ((device == BCM4335_D11AC_ID) || (device == BCM4335_D11AC2G_ID) ||
		(device == BCM4335_D11AC5G_ID) || (device == BCM4335_CHIP_ID))
		return 0;

	if ((device == BCM43602_D11AC_ID) || (device == BCM43602_D11AC2G_ID) ||
		(device == BCM43602_D11AC5G_ID) || (device == BCM43602_CHIP_ID))
		return 0;

	if ((device == BCM43569_D11AC_ID) || (device == BCM43569_D11AC2G_ID) ||
		(device == BCM43569_D11AC5G_ID) || (device == BCM43569_CHIP_ID))
		return 0;

	if ((device == BCM4358_D11AC_ID) || (device == BCM4358_D11AC2G_ID) ||
		(device == BCM4358_D11AC5G_ID))
		return 0;

	if ((device == BCM4349_D11AC_ID) || (device == BCM4349_D11AC2G_ID) ||
		(device == BCM4349_D11AC5G_ID) || (device == BCM4349_CHIP_ID))
		return 0;

	if ((device == BCM4355_D11AC_ID) || (device == BCM4355_D11AC2G_ID) ||
		(device == BCM4355_D11AC5G_ID) || (device == BCM4355_CHIP_ID))
		return 0;

	if ((device == BCM4359_D11AC_ID) || (device == BCM4359_D11AC2G_ID) ||
		(device == BCM4359_D11AC5G_ID))
		return 0;

	if ((device == BCM43596_D11AC_ID) || (device == BCM43596_D11AC2G_ID) ||
		(device == BCM43596_D11AC5G_ID))
		return 0;


	if ((device == BCM4365_D11AC_ID) || (device == BCM4365_D11AC2G_ID) ||
		(device == BCM4365_D11AC5G_ID) || (device == BCM4365_CHIP_ID))
		return 0;

	if ((device == BCM4366_D11AC_ID) || (device == BCM4366_D11AC2G_ID) ||
		(device == BCM4366_D11AC5G_ID) || (device == BCM4366_CHIP_ID))
		return 0;

	DHD_ERROR(("%s: Unsupported vendor %x device %x\n", __FUNCTION__, vendor, device));
	return (-ENODEV);
} 

static int
dhdpcie_cc_nvmshadow(dhd_bus_t *bus, struct bcmstrbuf *b)
{
	uint16 dump_offset = 0;
	uint32 dump_size = 0, otp_size = 0, sprom_size = 0;

	
	int  otp_size_65nm[8] = {0, 2048, 4096, 8192, 4096, 6144, 512, 1024};

	volatile uint16 *nvm_shadow;

	uint cur_coreid;
	uint chipc_corerev;
	chipcregs_t *chipcregs;

	
	cur_coreid = si_coreid(bus->sih);
	
	chipcregs = (chipcregs_t *)si_setcore(bus->sih, CC_CORE_ID, 0);
	ASSERT(chipcregs != NULL);

	chipc_corerev = si_corerev(bus->sih);

	
	if (chipc_corerev < 44) {
		DHD_ERROR(("%s: ChipcommonCore Rev %d < 44\n", __FUNCTION__, chipc_corerev));
		return BCME_UNSUPPORTED;
	}

	
	if (((uint16)bus->sih->chip != BCM4350_CHIP_ID) && !BCM4345_CHIP((uint16)bus->sih->chip)) {
		DHD_ERROR(("%s: cc_nvmdump cmd. supported for 4350/4345 only\n",
			__FUNCTION__));
		return BCME_UNSUPPORTED;
	}

	
	if (chipcregs->sromcontrol & SRC_PRESENT) {
		
		sprom_size = (1 << (2 * ((chipcregs->sromcontrol & SRC_SIZE_MASK)
					>> SRC_SIZE_SHIFT))) * 1024;
		bcm_bprintf(b, "\nSPROM Present (Size %d bits)\n", sprom_size);
	}

	if (chipcregs->sromcontrol & SRC_OTPPRESENT) {
		bcm_bprintf(b, "\nOTP Present");

		if (((chipcregs->otplayout & OTPL_WRAP_TYPE_MASK) >> OTPL_WRAP_TYPE_SHIFT)
			== OTPL_WRAP_TYPE_40NM) {
			
			otp_size =  (((chipcregs->capabilities & CC_CAP_OTPSIZE)
				        >> CC_CAP_OTPSIZE_SHIFT) + 1) * 1024;
			bcm_bprintf(b, "(Size %d bits)\n", otp_size);
		} else {
			
			otp_size = otp_size_65nm[(chipcregs->capabilities & CC_CAP_OTPSIZE)
				        >> CC_CAP_OTPSIZE_SHIFT];
			bcm_bprintf(b, "(Size %d bits)\n", otp_size);
			DHD_INFO(("%s: 65nm/130nm OTP Size not tested. \n",
				__FUNCTION__));
		}
	}

	if (((chipcregs->sromcontrol & SRC_PRESENT) == 0) &&
		((chipcregs->capabilities & CC_CAP_OTPSIZE) == 0)) {
		DHD_ERROR(("%s: SPROM and OTP could not be found \n",
			__FUNCTION__));
		return BCME_NOTFOUND;
	}

	
	if ((chipcregs->sromcontrol & SRC_OTPSEL) &&
		(chipcregs->sromcontrol & SRC_OTPPRESENT)) {

		bcm_bprintf(b, "OTP Strap selected.\n"
		               "\nOTP Shadow in ChipCommon:\n");

		dump_size = otp_size / 16 ; 

	} else if (((chipcregs->sromcontrol & SRC_OTPSEL) == 0) &&
		(chipcregs->sromcontrol & SRC_PRESENT)) {

		bcm_bprintf(b, "SPROM Strap selected\n"
				"\nSPROM Shadow in ChipCommon:\n");

		
		
		dump_size = sprom_size > 8 ? (8 * 1024) / 16 : sprom_size / 16;
	} else {
		DHD_ERROR(("%s: NVM Shadow does not exist in ChipCommon\n",
			__FUNCTION__));
		return BCME_NOTFOUND;
	}

	if (bus->regs == NULL) {
		DHD_ERROR(("ChipCommon Regs. not initialized\n"));
		return BCME_NOTREADY;
	} else {
	    bcm_bprintf(b, "\n OffSet:");

	    
	    nvm_shadow = chipcregs->sromotp;

	    while (dump_offset < dump_size) {
		if (dump_offset % 2 == 0)
			
			bcm_bprintf(b, "\n 0x%04x", dump_offset * 2);

		bcm_bprintf(b, "\t0x%04x", *(nvm_shadow + dump_offset));
		dump_offset += 0x1;
	    }
	}

	
	si_setcore(bus->sih, cur_coreid, 0);

	return BCME_OK;
} 

void dhd_bus_clean_flow_ring(dhd_bus_t *bus, void *node)
{
	void *pkt;
	flow_queue_t *queue;
	flow_ring_node_t *flow_ring_node = (flow_ring_node_t *)node;
	unsigned long flags;

	queue = &flow_ring_node->queue;

#ifdef DHDTCPACK_SUPPRESS
	dhd_tcpack_info_tbl_clean(bus->dhd);
#endif 

	
	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);

	
	while ((pkt = dhd_flow_queue_dequeue(bus->dhd, queue)) != NULL) {
		PKTFREE(bus->dhd->osh, pkt, TRUE);
	}
	ASSERT(DHD_FLOW_QUEUE_EMPTY(queue));

	flow_ring_node->status = FLOW_RING_STATUS_CLOSED;
	flow_ring_node->active = FALSE;

	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	DHD_FLOWRING_LIST_LOCK(bus->dhd->flowring_list_lock, flags);
	dll_delete(&flow_ring_node->list);
	DHD_FLOWRING_LIST_UNLOCK(bus->dhd->flowring_list_lock, flags);

	
	dhd_prot_flowrings_pool_release(bus->dhd,
		flow_ring_node->flowid, flow_ring_node->prot_info);

	
	dhd_flowid_free(bus->dhd, flow_ring_node->flow_info.ifindex,
		flow_ring_node->flowid);
}

int
dhd_bus_flow_ring_create_request(dhd_bus_t *bus, void *arg)
{
	flow_ring_node_t *flow_ring_node = (flow_ring_node_t *)arg;

	DHD_INFO(("%s :Flow create\n", __FUNCTION__));

	
	if (dhd_prot_flow_ring_create(bus->dhd, flow_ring_node) != BCME_OK)
		return BCME_NOMEM;

	return BCME_OK;
}

void
dhd_bus_flow_ring_create_response(dhd_bus_t *bus, uint16 flowid, int32 status)
{
	flow_ring_node_t *flow_ring_node;
	unsigned long flags;

	DHD_INFO(("%s :Flow Response %d \n", __FUNCTION__, flowid));

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flowid);
	ASSERT(flow_ring_node->flowid == flowid);

	if (status != BCME_OK) {
		DHD_ERROR(("%s Flow create Response failure error status = %d \n",
		     __FUNCTION__, status));
		
		dhd_bus_clean_flow_ring(bus, flow_ring_node);
		return;
	}

	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);
	flow_ring_node->status = FLOW_RING_STATUS_OPEN;
	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	dhd_bus_schedule_queue(bus, flowid, FALSE); 

	return;
}

int
dhd_bus_flow_ring_delete_request(dhd_bus_t *bus, void *arg)
{
	void * pkt;
	flow_queue_t *queue;
	flow_ring_node_t *flow_ring_node;
	unsigned long flags;

	DHD_INFO(("%s :Flow Delete\n", __FUNCTION__));

	flow_ring_node = (flow_ring_node_t *)arg;

	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);
	if (flow_ring_node->status == FLOW_RING_STATUS_DELETE_PENDING) {
		DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);
		DHD_ERROR(("%s :Delete Pending\n", __FUNCTION__));
		return BCME_ERROR;
	}
	flow_ring_node->status = FLOW_RING_STATUS_DELETE_PENDING;

	queue = &flow_ring_node->queue; 

#ifdef DHDTCPACK_SUPPRESS
	dhd_tcpack_info_tbl_clean(bus->dhd);
#endif 
	
	while ((pkt = dhd_flow_queue_dequeue(bus->dhd, queue)) != NULL) {
		PKTFREE(bus->dhd->osh, pkt, TRUE);
	}
	ASSERT(DHD_FLOW_QUEUE_EMPTY(queue));

	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	
	dhd_prot_flow_ring_delete(bus->dhd, flow_ring_node);

	return BCME_OK;
}

void
dhd_bus_flow_ring_delete_response(dhd_bus_t *bus, uint16 flowid, uint32 status)
{
	flow_ring_node_t *flow_ring_node;

	DHD_INFO(("%s :Flow Delete Response %d \n", __FUNCTION__, flowid));

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flowid);
	ASSERT(flow_ring_node->flowid == flowid);

	if (status != BCME_OK) {
		DHD_ERROR(("%s Flow Delete Response failure error status = %d \n",
		    __FUNCTION__, status));
		return;
	}
	
	dhd_bus_clean_flow_ring(bus, flow_ring_node);

	return;

}

int dhd_bus_flow_ring_flush_request(dhd_bus_t *bus, void *arg)
{
	void *pkt;
	flow_queue_t *queue;
	flow_ring_node_t *flow_ring_node;
	unsigned long flags;

	DHD_INFO(("%s :Flow Delete\n", __FUNCTION__));

	flow_ring_node = (flow_ring_node_t *)arg;
	queue = &flow_ring_node->queue; 

	DHD_FLOWRING_LOCK(flow_ring_node->lock, flags);

#ifdef DHDTCPACK_SUPPRESS
	dhd_tcpack_info_tbl_clean(bus->dhd);
#endif 

	
	while ((pkt = dhd_flow_queue_dequeue(bus->dhd, queue)) != NULL) {
		PKTFREE(bus->dhd->osh, pkt, TRUE);
	}
	ASSERT(DHD_FLOW_QUEUE_EMPTY(queue));

	DHD_FLOWRING_UNLOCK(flow_ring_node->lock, flags);

	
	dhd_prot_flow_ring_flush(bus->dhd, flow_ring_node);

	flow_ring_node->status = FLOW_RING_STATUS_FLUSH_PENDING;
	return BCME_OK;
}

void
dhd_bus_flow_ring_flush_response(dhd_bus_t *bus, uint16 flowid, uint32 status)
{
	flow_ring_node_t *flow_ring_node;

	if (status != BCME_OK) {
		DHD_ERROR(("%s Flow flush Response failure error status = %d \n",
		    __FUNCTION__, status));
		return;
	}

	flow_ring_node = DHD_FLOW_RING(bus->dhd, flowid);
	ASSERT(flow_ring_node->flowid == flowid);

	flow_ring_node->status = FLOW_RING_STATUS_OPEN;
	return;
}

uint32
dhd_bus_max_h2d_queues(struct dhd_bus *bus)
{
	return bus->max_sub_queues;
}

void
dhd_bus_pktq_flush(dhd_pub_t *dhdp)
{
	return;
}

int
dhdpcie_bus_clock_start(struct dhd_bus *bus)
{
	return dhdpcie_start_host_pcieclock(bus);
}

int
dhdpcie_bus_clock_stop(struct dhd_bus *bus)
{
	return dhdpcie_stop_host_pcieclock(bus);
}

int
dhdpcie_bus_disable_device(struct dhd_bus *bus)
{
	return dhdpcie_disable_device(bus);
}

int
dhdpcie_bus_enable_device(struct dhd_bus *bus)
{
	return dhdpcie_enable_device(bus);
}

int
dhdpcie_bus_alloc_resource(struct dhd_bus *bus)
{
	return dhdpcie_alloc_resource(bus);
}

void
dhdpcie_bus_free_resource(struct dhd_bus *bus)
{
	dhdpcie_free_resource(bus);
}

int
dhd_bus_request_irq(struct dhd_bus *bus)
{
	return dhdpcie_bus_request_irq(bus);
}

bool
dhdpcie_bus_dongle_attach(struct dhd_bus *bus)
{
	return dhdpcie_dongle_attach(bus);
}

int
dhd_bus_release_dongle(struct dhd_bus *bus)
{
	bool dongle_isolation;
	osl_t *osh;

	DHD_TRACE(("%s: Enter\n", __FUNCTION__));

	if (bus) {
		osh = bus->osh;
		ASSERT(osh);

		if (bus->dhd) {
			dongle_isolation = bus->dhd->dongle_isolation;
			dhdpcie_bus_release_dongle(bus, osh, dongle_isolation, TRUE);
		}
	}

	return 0;
}

#ifdef BCMPCIE_OOB_HOST_WAKE
int
dhd_bus_oob_intr_register(dhd_pub_t *dhdp)
{
	return dhdpcie_oob_intr_register(dhdp->bus);
}

void
dhd_bus_oob_intr_unregister(dhd_pub_t *dhdp)
{
	dhdpcie_oob_intr_unregister(dhdp->bus);
}

void
dhd_bus_oob_intr_set(dhd_pub_t *dhdp, bool enable)
{
	dhdpcie_oob_intr_set(dhdp->bus, enable);
}
#endif 
