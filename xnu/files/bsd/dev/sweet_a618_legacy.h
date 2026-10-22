/* SPDX-License-Identifier: MIT */
#ifndef SWEET_A618_LEGACY_H
#define SWEET_A618_LEGACY_H
#include <sys/errno.h>
#define ARM64_BOARD_CONFIG_SWEET 1
struct a618_io {void *cookie;uint32_t (*read)(void *,uint32_t);void (*write)(void *,uint32_t,uint32_t);void (*delay_us)(void *,unsigned);};
struct a618_clock_result {unsigned block;uint32_t offset,value;unsigned completed;};
#define A618_GMU_SYSRESET 0x14000u
#define A618_GMU_FW_INIT_RESULT 0x14070u
#define A618_HFI_IOVA 0x60004000u
#define A618_DEBUG_IOVA 0x60008000u
#define A618_LOG_IOVA 0x6000c000u
struct a618_dma_region {unsigned char *cpu;uint64_t physical;unsigned bytes,uncached;};
struct a618_hfi {volatile uint32_t *table;void (*barrier)(void *);void *cookie;unsigned sequence,poisoned,responses;uint32_t last_request,last_response,firmware_error;};
struct a618_gmu_boot_result {unsigned phase;uint32_t offset,value,firmware_version;};
#endif
