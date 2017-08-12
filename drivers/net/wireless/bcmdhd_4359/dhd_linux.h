/*
 * DHD Linux header file (dhd_linux exports for cfg80211 and other components)
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
 * $Id: dhd_linux.h 399301 2013-04-29 21:41:52Z $
 */

#ifndef __DHD_LINUX_H__
#define __DHD_LINUX_H__

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <dngl_stats.h>
#include <dhd.h>
#ifdef DHD_WMF
#include <dhd_wmf_linux.h>
#endif

#if defined(CONFIG_HAS_EARLYSUSPEND) && defined(DHD_USE_EARLYSUSPEND)
#include <linux/earlysuspend.h>
#endif 

#if defined(CONFIG_WIFI_CONTROL_FUNC)
#include <linux/wlan_plat.h>
#endif

#if !defined(CONFIG_WIFI_CONTROL_FUNC)
#define WLAN_PLAT_NODFS_FLAG    0x01
struct wifi_platform_data {
	int (*set_power)(int val);
	int (*set_reset)(int val);
	int (*set_carddetect)(int val);
	void *(*mem_prealloc)(int section, unsigned long size);
	int (*get_mac_addr)(unsigned char *buf);
#if defined(CUSTOMER_HW_ONE)
	int (*get_wake_irq)(void);
#endif 
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 58)) || defined(CUSTOM_COUNTRY_CODE)
	void *(*get_country_code)(char *ccode, u32 flags);
#else 
	void *(*get_country_code)(char *ccode);
#endif 
#if defined(CUSTOMER_HW_ONE)
	int (*get_irq_number)(unsigned long *flags_ptr);
#endif 
 };
#endif 
#define DHD_REGISTRATION_TIMEOUT  12000  

typedef struct wifi_adapter_info {
	const char	*name;
	uint		irq_num;
	uint		intr_flags;
	const char	*fw_path;
	const char	*nv_path;
	void		*wifi_plat_data;	
	uint		bus_type;
	uint		bus_num;
	uint		slot_num;
} wifi_adapter_info_t;

typedef struct bcmdhd_wifi_platdata {
	uint				num_adapters;
	wifi_adapter_info_t	*adapters;
} bcmdhd_wifi_platdata_t;

typedef struct dhd_sta {
	cumm_ctr_t cumm_ctr;    
	uint16 flowid[NUMPRIO]; 
	void * ifp;             
	struct ether_addr ea;   
	struct list_head list;  
	int idx;                
	int ifidx;              
} dhd_sta_t;
typedef dhd_sta_t dhd_sta_pool_t;

int dhd_wifi_platform_register_drv(void);
void dhd_wifi_platform_unregister_drv(void);
wifi_adapter_info_t* dhd_wifi_platform_get_adapter(uint32 bus_type, uint32 bus_num,
	uint32 slot_num);
int wifi_platform_set_power(wifi_adapter_info_t *adapter, bool on, unsigned long msec);
int wifi_platform_bus_enumerate(wifi_adapter_info_t *adapter, bool device_present);
int wifi_platform_get_irq_number(wifi_adapter_info_t *adapter, unsigned long *irq_flags_ptr);
int wifi_platform_get_mac_addr(wifi_adapter_info_t *adapter, unsigned char *buf);
#ifdef CUSTOM_COUNTRY_CODE
void *wifi_platform_get_country_code(wifi_adapter_info_t *adapter, char *ccode,
   u32 flags);
#else
void *wifi_platform_get_country_code(wifi_adapter_info_t *adapter, char *ccode);
#endif 
void* wifi_platform_prealloc(wifi_adapter_info_t *adapter, int section, unsigned long size);
void* wifi_platform_get_prealloc_func_ptr(wifi_adapter_info_t *adapter);

int dhd_get_fw_mode(struct dhd_info *dhdinfo);
bool dhd_update_fw_nv_path(struct dhd_info *dhdinfo);

#ifdef DHD_WMF
dhd_wmf_t* dhd_wmf_conf(dhd_pub_t *dhdp, uint32 idx);
#endif 
#endif 
