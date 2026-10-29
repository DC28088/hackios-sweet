/*
 * SWEET Adreno 618: stages 7 and 8 of the original port (DC28088/ios-sweet fe987bf05e0d, patches
 * 0092-0098): the GMU's shared memory behind an SMMU context, then the GMU's cold boot over the
 * legacy HFI v1 queues. The SMMU, HFI and GMU code below is the original's, unchanged except for
 * names; the memory allocator and the orchestration are for this kernel. On the phone the original
 * reached phase 6 (firmware 0x10110000) with all five HFI responses on 2026-10-20 (patch 0098).
 *
 * Boot-arg sweetgpu=7: after stage 6, wired uncached memory, an SMMU context bank (CB1, stream 5)
 * for the GMU and the HFI tables, all checked with hardware translation probes; the GMU's
 * processor stays in reset.
 * Boot-arg sweetgpu=8: after 7, releases the GMU's processor and talks to it over HFI. Nothing
 * of it is stopped again: a later run in the same loader session finds the GMU running, and
 * stage 6 then refuses; reset the phone between runs.
 * Boot-arg sweetgpu=9: after 8, the GPU core: GX power through the GMU, the SQE, the CP's own SMMU
 * context (CB0), the signed zap shader, ME_INIT and one fence write by PM4 (patches 0096-0106).
 * No renderer, no userland interface: this establishes no acceleration.
 */
#if defined(__arm64__)
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
#include <machine/machine_routines.h>

/* Mirror board diagnostics into the resident loader live log. */
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
#define printf hakios_tl

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern int sweet_rpmh_arc_probe(const char *res);
extern int sweet_rpmh_adreno_votes(uint32_t *gpu, uint32_t *gmu);
int sweet_gpu_boot(unsigned stage, vm_offset_t gpucc, vm_offset_t gmu_base, vm_offset_t smmu_base);

#define A618_GMU_SYSRESET 0x14000u
#define A618_GMU_FW_INIT_RESULT 0x14070u
#define A618_DMA_BYTES 0x14000u
#define A618_TABLE_BYTES 0x4000u
#define A618_SHARED_IOVA 0x60000000u
#define A618_HFI_IOVA 0x60004000u
#define A618_DEBUG_IOVA 0x60008000u
#define A618_LOG_IOVA 0x6000c000u

struct a618_io {
	void *cookie;
	uint32_t (*read)(void *, uint32_t);
	void (*write)(void *, uint32_t, uint32_t);
	void (*delay_us)(void *, unsigned);
};
struct a618_dma_region {
	unsigned char *cpu;
	uint64_t physical;
	unsigned bytes;
	unsigned uncached;
};
struct a618_smmu_io {
	struct a618_io registers;
	uint64_t (*read64)(void *, uint32_t);
	void (*write64)(void *, uint32_t, uint64_t);
	void (*barrier)(void *);
};
struct a618_smmu_result {
	unsigned context, slot, translated;
	uint32_t control, fault, offset, value;
	uint64_t par;
};
struct a618_hfi {
	volatile uint32_t *table;
	void (*barrier)(void *);
	void *cookie;
	unsigned sequence, poisoned, responses;
	uint32_t last_request, last_response, firmware_error;
};
struct a618_gmu_boot_result {
	unsigned phase;
	uint32_t offset, value, firmware_version;
};

/* ---- wired, uncached memory (this kernel's allocator; the original used an IOKit descriptor) ---- */

/*
 * In one physical piece and 16 KiB aligned, so that it can be mapped a second time without this
 * processor's cache in between (as hakios.lx.c and hakios.adreno.c do): the cached view is written
 * out and never used again. Never freed: once the SMMU can reach it, hardware ownership may be
 * uncertain, and one allocation per boot is all this takes.
 */
static int
sweet_adreno_dma_allocate(struct a618_dma_region *region, unsigned bytes)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	if (!region) {
		return EINVAL;
	}
	if (kmem_alloc_contig(kernel_map, &cached, bytes, 0x3fff, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return ENOMEM;
	}
	physical = kvtophys(cached);
	if (!physical || (physical & 0x3fff) || physical > (1ull << 36) - bytes) {
		return ERANGE;
	}
	for (vm_offset_t line = cached; line < cached + bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	uncached = ml_io_map_unmappable(physical, bytes, VM_WIMG_WCOMB);
	if (uncached == 0) {
		return ENOMEM;
	}
	region->cpu = (unsigned char *)uncached;
	region->physical = physical;
	region->bytes = bytes;
	region->uncached = 1;
	/* Naturally aligned volatile word stores, as the original: no DC ZVA or vector stores. */
	volatile uint32_t *words = (volatile uint32_t *)uncached;
	for (unsigned i = 0; i < bytes / 4; ++i) {
		words[i] = 0;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return 0;
}

/* ================================ original: sweet_adreno_smmu.c (0095) ================================ */
#define CB1 0x9000u
#define ASID 0x61u
/* FSR.Format[10:9] is read-only metadata, not a W1C fault. */
#define FSR_FAULT 0xc00001feu
static int
smmu_put(const struct a618_io *io, uint32_t offset, uint32_t value,
 uint32_t mask, struct a618_smmu_result *r)
{
 io->write(io->cookie, offset, value);
 r->offset = offset; r->value = io->read(io->cookie, offset);
 return r->value != 0xffffffffu && (r->value & mask) == (value & mask) ? 0 : EIO;
}
static int
smmu_wait_clear(const struct a618_io *io, uint32_t offset, unsigned mask,
 struct a618_smmu_result *r)
{
 for (unsigned i = 0; i < 500; ++i) {
  r->offset = offset; r->value = io->read(io->cookie, offset);
  if (r->value == 0xffffffffu) return EIO;
  if (!(r->value & mask)) return 0;
  io->delay_us(io->cookie, 1);
 }
 return ETIMEDOUT;
}
static int
a618_smmu_prepare(const struct a618_smmu_io *ops,
 const struct a618_dma_region *memory, struct a618_smmu_result *r)
{
 if (!ops || !memory || !r || !ops->registers.read ||
     !ops->registers.write || !ops->registers.delay_us ||
     !ops->read64 || !ops->write64 || !ops->barrier || !memory->cpu ||
     !memory->uncached || memory->bytes != A618_DMA_BYTES ||
     (memory->physical & 0x3fff) || !memory->physical ||
     memory->physical > (1ull << 36) - A618_DMA_BYTES ||
     ((uintptr_t)memory->cpu & 7)) return EINVAL;
 const struct a618_io *io = &ops->registers;
 *r = (struct a618_smmu_result){ .context = 1, .slot = 0xffffffffu };
 /* Measured 0091 profile: 4 KiB register pages, 0x8000 CB base, five
  * stream match slots and context banks, 36-bit PA. 16K PT granules are
  * intentionally not used (Qualcomm advertises broken 16K support).
  */
 const uint32_t identity[] = {0x48016605, 0x20008005, 0x7511};
 for (unsigned i = 0; i < 3; ++i) {
  r->offset = 0x20 + 4 * i; r->value = io->read(io->cookie, r->offset);
  if (r->value != identity[i]) return ENOTSUP;
 }
 /* Do not steal an active bank or matching stream, even if its firmware
  * owner appears idle. CB0 and all protected banks are left alone.
  */
 r->offset = CB1; r->value = io->read(io->cookie, CB1);
 if (r->value == 0xffffffffu) return EIO;
 if (r->value & 1) return EBUSY; /* M=0 is disabled; other reset bits may be UNKNOWN. */
 for (unsigned i = 0; i < 5; ++i) {
  r->offset = 0x800 + 4 * i;
  uint32_t smr = r->value = io->read(io->cookie, r->offset);
  if (smr == 0xffffffffu) return EIO;
  if (!(smr & 0x80000000u)) {
   if (r->slot == 0xffffffffu) r->slot = i;
  } else if (((smr ^ 5u) & ~((smr >> 16) & 0x7fffu) & 0xffffu) == 0) {
   r->slot = i; return EBUSY;
  }
 }
 if (r->slot == 0xffffffffu) return ENOSPC;
 r->control = io->read(io->cookie, 0);
 if (r->control == 0xffffffffu) return EIO;
 /* Three 4K tables inside a 16K host page; no mapping of the table arena.
  * L1[1] -> L2[256] -> L3[0..15]: shared 64K at 0x60000000.
  * All other entries fault. Both privileged/unprivileged data R/W, NX,
  * Normal NC and outer shareable match the uncached CPU mapping.
  */
 volatile uint64_t *l1 = (volatile uint64_t *)memory->cpu;
 volatile uint64_t *l2 = (volatile uint64_t *)(memory->cpu + 0x1000);
 volatile uint64_t *l3 = (volatile uint64_t *)(memory->cpu + 0x2000);
 for (unsigned i = 0; i < 512; ++i) l1[i] = l2[i] = l3[i] = 0;
 l1[1] = (memory->physical + 0x1000) | 3;
 l2[256] = (memory->physical + 0x2000) | 3;
 for (unsigned i = 0; i < 16; ++i)
  l3[i] = (memory->physical + A618_TABLE_BYTES + 0x1000 * i) |
      (3ull << 53) | (1u << 11) | (1u << 10) | (2u << 8) | (1u << 6) | 3;
 ops->barrier(io->cookie);
 int error;
 #define SET(o,v,m) do { error = smmu_put(io, (o), (v), (m), r); if (error) return error; } while (0)
 SET(0x1804, 1, 1); /* GR1 CBA2R(1): AArch64. */
 SET(0x1004, 0x1f300, 0x3f3ff); /* Stage1, stage2 bypass, weak upstream attributes. */
 SET(CB1 + 0x10, 0x38011, 0x38077); /* TCR2: SEP upstream, 36-bit output, AArch64 ASID. */
 SET(CB1 + 0x30, 0x00802020u, 0x00c0ffbfu); /* EPD1, OSH, NC walks, T0SZ=32; AArch64, no EAE. */
 uint64_t ttbr = memory->physical | ((uint64_t)ASID << 48);
 ops->write64(io->cookie, CB1 + 0x20, ttbr);
 r->offset = CB1 + 0x20;
 r->par = ops->read64(io->cookie, r->offset);
 if ((r->par & 0xffff000ffffff000ull) != ttbr) return EIO;
 ops->write64(io->cookie, CB1 + 0x28, 0);
 SET(CB1 + 0x38, 0x44, 0xff); /* MAIR[0]: Normal NC. */
 /* MAIR1 is unused: every data PTE references MAIR0[0]. */
 /* Fault reporting enabled, IRQ/stalling disabled until an IRQ handler
  * exists; HUPCF continues other transactions after an abort.
  */
 /* AFE/TRE are UNK/SBOP for long descriptors: write ones, do not
  * compare their readback. Check M, E, fault controls, WXN and ASIDPNE.
  */
 SET(CB1, 0x1127, 0x17f1);
 /* Block unmatched streams and enable this device. Preserve other controls;
  * do not enable interrupt sources without handlers. No reset-all/TLBI-all.
  */
 SET(0, (r->control | 0x1c12u) & ~0xe025u, 0xfc37);
 io->write(io->cookie, CB1 + 0x610, ASID);
 io->write(io->cookie, CB1 + 0x7f0, 0);
 error = smmu_wait_clear(io, CB1 + 0x7f4, 1, r);
 if (error) return error;
 /* Hardware translation probes, not a software table walk: every shared
  * page plus a guard fault must match before the stream is published.
  */
 for (unsigned i = 0; i <= 16; ++i) {
  uint64_t va = (uint64_t)A618_SHARED_IOVA + i * 0x1000u;
  ops->write64(io->cookie, CB1 + 0x800, va);
  error = smmu_wait_clear(io, CB1 + 0x8f0, 1, r);
  if (error) return error;
  r->par = ops->read64(io->cookie, CB1 + 0x50);
  if (r->par == 0xffffffffffffffffull) return EIO;
  if (i == 16) {
   if (!(r->par & 1)) return EFAULT;
  } else {
   uint64_t expected = memory->physical + A618_TABLE_BYTES + i * 0x1000u;
   if ((r->par & 1) || (r->par & 0x0000000ffffff000ull) != expected)
    return EFAULT;
   ++r->translated;
  }
 }
 r->fault = io->read(io->cookie, CB1 + 0x58);
 if (r->fault == 0xffffffffu) return EIO;
 if (r->fault & (1u << 30)) return EBUSY; /* Never resume an unknown stall. */
 if (r->fault & FSR_FAULT) {
  io->write(io->cookie, CB1 + 0x58, r->fault & (FSR_FAULT & ~(1u << 30)));
  uint32_t after = io->read(io->cookie, CB1 + 0x58);
  if (after == 0xffffffffu || (after & FSR_FAULT)) return EIO;
 }
 /* Only GMU kernel stream 5: route first, VALID last. Never program
  * GMU user stream 4, GPU stream 0, broad masks or a bypass entry.
  */
 SET(0xc00 + 4 * r->slot, 1, 0x030300ff);
 SET(0x800 + 4 * r->slot, 0x80000005u, 0xffffffffu);
 r->fault = io->read(io->cookie, CB1 + 0x58);
 #undef SET
 return r->fault == 0xffffffffu || (r->fault & FSR_FAULT) ? EIO : 0;
}

/* ================================ original: sweet_adreno_hfi.c (0097) ================================ */
#define QUEUE_WORDS 1024u
#define HEADER_WORDS 12u
static volatile uint32_t *
queue_header(struct a618_hfi *hfi, unsigned queue)
{
 return hfi->table + 6 + queue * HEADER_WORDS;
}
static int
indices(struct a618_hfi *hfi, unsigned queue, unsigned *read, unsigned *write)
{
 volatile uint32_t *header = queue_header(hfi, queue);
 hfi->barrier(hfi->cookie);
 if (header[0] != 1 || header[3] != QUEUE_WORDS) return EIO;
 *read = header[10]; *write = header[11];
 if (*read >= QUEUE_WORDS || *write >= QUEUE_WORDS) return EIO;
 hfi->barrier(hfi->cookie);
 return 0;
}
static int
a618_hfi_init(struct a618_hfi *hfi, const struct a618_dma_region *memory,
 void (*barrier)(void *), void *cookie)
{
 if (!hfi || !memory || !memory->cpu || !memory->uncached || !barrier ||
     memory->bytes != A618_DMA_BYTES || ((uintptr_t)memory->cpu & 3))
  return EINVAL;
 hfi->table = (volatile uint32_t *)(memory->cpu + A618_TABLE_BYTES +
     (A618_HFI_IOVA - A618_SHARED_IOVA));
 hfi->barrier = barrier; hfi->cookie = cookie;
 hfi->sequence = hfi->poisoned = hfi->responses = 0;
 hfi->last_request = hfi->last_response = hfi->firmware_error = 0;
 for (unsigned i = 0; i < 4096; ++i) hfi->table[i] = 0;
 hfi->table[0] = 0;
 hfi->table[1] = 24 + 3 * 48; /* size in bytes; offsets below in DWORDs */
 hfi->table[2] = 6; hfi->table[3] = 12;
 hfi->table[4] = hfi->table[5] = 3;
 const unsigned id[] = {0, 4, 5};
 for (unsigned q = 0; q < 3; ++q) {
  volatile uint32_t *header = queue_header(hfi, q);
  header[0] = 1;
  header[1] = A618_HFI_IOVA + (q + 1) * 4096;
  header[2] = (10u << 8) | id[q];
  header[3] = QUEUE_WORDS;
  header[6] = header[7] = header[8] = 1;
 }
 barrier(cookie);
 return 0;
}
static int
a618_hfi_write(struct a618_hfi *hfi, const uint32_t *words, unsigned count)
{
 if (!hfi || !hfi->table || !hfi->barrier || !words || !count || count > 255 ||
     ((words[0] >> 8) & 255) != count || (words[0] & 0xf0000u)) return EINVAL;
 unsigned read, write;
 int error = indices(hfi, 0, &read, &write);
 if (error) return error;
 unsigned free_words = (read + QUEUE_WORDS - write - 1) % QUEUE_WORDS;
 if (count > free_words) return ENOSPC;
 volatile uint32_t *data = hfi->table + QUEUE_WORDS;
 for (unsigned i = 0; i < count; ++i) data[(write + i) % QUEUE_WORDS] = words[i];
 hfi->barrier(hfi->cookie);
 queue_header(hfi, 0)[11] = (write + count) % QUEUE_WORDS;
 hfi->barrier(hfi->cookie);
 return 0;
}
static int
a618_hfi_read(struct a618_hfi *hfi, unsigned queue, uint32_t *words,
 unsigned capacity, unsigned *count)
{
 if (!count) return EINVAL;
 *count = 0;
 if (!hfi || !hfi->table || !hfi->barrier || !words || !capacity ||
     queue < 1 || queue > 2) return EINVAL;
 unsigned read, write;
 int error = indices(hfi, queue, &read, &write);
 if (error) return error;
 unsigned available = (write + QUEUE_WORDS - read) % QUEUE_WORDS;
 if (!available) {
  queue_header(hfi, queue)[8] = 1;
  hfi->barrier(hfi->cookie);
  return EAGAIN;
 }
 volatile uint32_t *data = hfi->table + (queue + 1) * QUEUE_WORDS;
 unsigned size = (data[read] >> 8) & 255;
 if (!size) return EIO;
 if (size > available) return EAGAIN;
 if (size > capacity) return EMSGSIZE;
 for (unsigned i = 0; i < size; ++i) words[i] = data[(read + i) % QUEUE_WORDS];
 hfi->barrier(hfi->cookie);
 queue_header(hfi, queue)[10] = (read + size) % QUEUE_WORDS;
 hfi->barrier(hfi->cookie);
 *count = size;
 return 0;
}

/* GMU-relative byte offsets from the A6xx register XML. Keep host IRQs
 * masked: this backend polls, and no XNU interrupt handler is installed. */
#define HFI_INTR_INFO 0x14648u
#define HFI_INTR_CLEAR 0x14644u
#define HFI_DOORBELL 0x14650u
#define HFI_CM3_FAULT (1u << 23)
static int
a618_hfi_call(struct a618_hfi *hfi, const struct a618_io *io, unsigned id,
 const uint32_t *body, unsigned body_words, uint32_t *payload,
 unsigned payload_words)
{
 if (!hfi || !hfi->table || !hfi->barrier || !io || !io->read ||
     !io->write || !io->delay_us || id > 255 || body_words > 254 ||
     (body_words && !body) || payload_words > 252 ||
     (payload_words && !payload)) return EINVAL;
 if (hfi->poisoned) return EIO;
 uint32_t message[255];
 hfi->sequence = (hfi->sequence + 1) % 0xfffu;
 uint32_t sent = (hfi->sequence << 20) | ((body_words + 1) << 8) | id;
 message[0] = sent;
 hfi->last_request = sent;
 for (unsigned i = 0; i < body_words; ++i) message[i + 1] = body[i];
 int error = a618_hfi_write(hfi, message, body_words + 1);
 if (error) return error; /* Nothing published on a rejected append. */
 io->write(io->cookie, HFI_DOORBELL, 1);
 for (unsigned elapsed = 0; elapsed < 1000000; elapsed += 100) {
  uint32_t interrupt = io->read(io->cookie, HFI_INTR_INFO);
  if (interrupt == 0xffffffffu || (interrupt & HFI_CM3_FAULT)) {
   error = EIO; goto failed;
  }
  /* W1C before reading: a concurrent firmware append remains visible via
   * the queue indices, including when no fresh interrupt edge is seen. */
  if (interrupt & 1) io->write(io->cookie, HFI_INTR_CLEAR, 1);
  unsigned words = 0;
  error = a618_hfi_read(hfi, 1, message, 255, &words);
  if (error != EAGAIN && error) goto failed;
  if (!error) {
   ++hfi->responses;
   unsigned received_id = message[0] & 255;
   unsigned type = (message[0] >> 16) & 15;
   if (received_id == 100) { error = EIO; goto failed; }
   if (received_id != 126 || type != 2 || words < 3) {
    error = EPROTO; goto failed;
   }
   /* Match command ID and transaction sequence as in the Linux transport;
    * request length bits are not used to size the ACK payload. The actual
    * response length/type were validated above. Stale ACKs cannot extend
    * the fixed one-second deadline. */
   hfi->last_response = message[1];
   if ((message[1] & 0xfff000ffu) == (sent & 0xfff000ffu)) {
    hfi->firmware_error = message[2];
    if (message[2]) { error = EIO; goto failed; }
    if (words < 3 + payload_words) { error = EPROTO; goto failed; }
    for (unsigned i = 0; i < payload_words; ++i) payload[i] = message[3 + i];
    return 0;
   }
  }
  /* Drain a single debug packet per iteration so logs cannot deadlock FW.
   * All queue indices and packet sizes pass the same validation. */
  error = a618_hfi_read(hfi, 2, message, 255, &words);
  if (error != EAGAIN && error) goto failed;
  if (!error && (message[0] & 255) == 100) { error = EIO; goto failed; }
  io->delay_us(io->cookie, 100);
 }
 error = ETIMEDOUT;
failed:
 hfi->poisoned = 1;
 return error;
}

static int
a618_hfi_bootstrap(struct a618_hfi *hfi, const struct a618_io *io,
 uint32_t gpu_vote, uint32_t gmu_vote, uint32_t *firmware_version)
{
 if (!firmware_version || !(gpu_vote >> 16) || !(gmu_vote >> 16)) return EINVAL;
 *firmware_version = 0;
 uint32_t init[] = {0, A618_DEBUG_IOVA, 0x4000, 1}; /* cold boot = 1 */
 int error = a618_hfi_call(hfi, io, 0, init, 4, 0, 0);
 if (error) return error;
 uint32_t supported = (1u << 28) | (1u << 19) | (1u << 17); /* HFI 1.10 */
 error = a618_hfi_call(hfi, io, 1, &supported, 1, firmware_version, 1);
 if (error) return error;
 /* Two levels include the mandatory off entry; units are kHz. Upper
  * corners stay absent until power, thermal control and recovery exist. */
 uint32_t perf[42] = {0}; /* body of the fixed 43-DWORD v1 message */
 perf[0] = perf[1] = 2;
 perf[4] = gpu_vote; perf[5] = 180000;
 perf[36] = gmu_vote; perf[37] = 200000;
 error = a618_hfi_call(hfi, io, 4, perf, 42, 0, 0);
 if (error) return error;
 /* A618 uses a single DDR off entry: GMU does not scale its bus. Exact
  * target-specific wire constants from Linux a618_build_bw_table().
  * Host interconnect bandwidth remains a separate prerequisite. */
 uint32_t bw[159] = {0}; /* body of the fixed 160-DWORD message */
 bw[0] = 1; bw[1] = 1; bw[2] = 3; bw[3] = bw[4] = 1;
 bw[5] = 0x5007c;
 bw[11] = 0x40000000; bw[17] = 0x60000001;
 bw[23] = 0x50000; bw[24] = 0x5003c; bw[25] = 0x5000c;
 bw[31] = bw[32] = bw[33] = 0x40000000;
 error = a618_hfi_call(hfi, io, 3, bw, 159, 0, 0);
 if (error) return error;
 /* Wire message 5 ends legacy initialization (named TEST in Linux). */
 return a618_hfi_call(hfi, io, 5, 0, 0, 0, 0);
}

/* ================================ original: sweet_adreno_gmu.c (0094) ================================ */
#define GMU_CM3_DTCM_START 0x7000u
#define GMU_GENERAL_7 0x14730u
#define GMU_CM3_BOOT_CONFIG 0x14004u
#define GMU_HFI_QTBL_ADDR 0x14614u
#define GMU_HFI_QTBL_INFO 0x14610u
#define GMU_AHB_FENCE_RANGE_0 0x24c44u
#define GMU_CM3_CFG 0x140b4u
#define GMU_HFI_SFR_ADDR 0x14608u
#define GPU_GMU_CX_GMU_PWR_COL_CP_MSG 0x14400u
#define GPU_GMU_CX_GMU_PWR_COL_CP_RESP 0x14404u
#define GMU_SYS_BUS_CONFIG 0x1303cu
#define GMU_ICACHE_CONFIG 0x13000u
#define GMU_DCACHE_CONFIG 0x13004u
#define GMU_PWR_COL_INTER_FRAME_CTRL 0x14300u
#define GMU_RPMH_CTRL 0x143a0u
#define GMU_BOOT_SLUMBER_OPTION 0x8fe0u
#define GMU_GX_VOTE_IDX 0x8fe4u
#define GMU_MX_VOTE_IDX 0x8fe8u
#define GMU_HOST2GMU_INTR_SET 0x14650u
#define GMU_GMU2HOST_INTR_INFO 0x14648u
#define GMU_GMU2HOST_INTR_CLR 0x14644u
#define GMU_GMU2HOST_INTR_MASK 0x1464cu
#define GMU_AO_HOST_INTERRUPT_MASK 0x24c18u
#define GMU_GX_SPTPRAC_POWER_CONTROL 0x204u
#define GMU_SPTPRAC_PWR_CLK_STATUS 0x14340u
#define GMU_HFI_CTRL_INIT 0x14618u
#define GMU_HFI_CTRL_STATUS 0x14600u

static int
gmu_valid_io(const struct a618_io *io)
{
 return io && io->read && io->write && io->delay_us;
}
static int
gmu_poll(const struct a618_io *io, uint32_t offset, uint32_t mask,
 uint32_t expected, unsigned timeout, struct a618_gmu_boot_result *result)
{
 result->offset = offset;
 for (unsigned us = 0; us <= timeout; us += 10) {
  result->value = io->read(io->cookie, offset);
  if (result->value == 0xffffffffu) return EIO;
  uint32_t faults = io->read(io->cookie, 0x24c14); /* AO status */
  uint32_t interrupts = io->read(io->cookie, GMU_GMU2HOST_INTR_INFO);
  if (faults == 0xffffffffu || interrupts == 0xffffffffu ||
      (faults & 0x29) || (interrupts & (1u << 23))) return EIO;
  if ((result->value & mask) == expected) return 0;
  if (us != timeout) io->delay_us(io->cookie, 10);
 }
 return ETIMEDOUT;
}
static void
rscc_write(const struct a618_io *gmu, unsigned reg, uint32_t value)
{
 gmu->write(gmu->cookie, 0x23000 + reg * 4, value);
}
static void
pdc_program(const struct a618_io *gmu, const struct a618_io *pdc,
 const struct a618_io *seq)
{
 /* SM7150 A618 constants, not the newer A650/A7xx sequence. */
 static const uint32_t rsc[] = {
  0xa7a506a0, 0xa1e6a6e7, 0xa2e081e1, 0xe9a982e2, 0x0020e8a8 };
 static const uint32_t program[] = {
  0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };
 rscc_write(gmu, 4, 1u << 24);
 rscc_write(gmu, 0xb, 1);
 for (unsigned i = 0; i < 3; ++i) {
  rscc_write(gmu, 0xe + i * 2, i == 2 ? 0x80000000 : 0);
  rscc_write(gmu, 0xd + i * 2, 0);
 }
 rscc_write(gmu, 0x100, 0);
 rscc_write(gmu, 8, 0x4520);
 rscc_write(gmu, 9, 0x4510);
 rscc_write(gmu, 0xa, 0x4514);
 for (unsigned i = 0; i < 5; ++i) {
  rscc_write(gmu, 0x180 + i, rsc[i]);
  seq->write(seq->cookie, i * 4, program[i]);
 }
 /* Third address is the target-specific A618 PDC resource from Linux.
  * Do not label it xo.lvl without command-DB identity evidence. */
 static const uint32_t address[] = {0x30010, 0x30000, 0x30090};
 static const uint32_t sleep[] = {1, 0, 0}, wake[] = {2, 2, 3};
 for (unsigned bank = 0; bank < 2; ++bank) {
  unsigned base = bank ? 0x15d6 : 0x1572; /* TCS3 / TCS1 DWORD offsets */
  pdc->write(pdc->cookie, (base + 1) * 4, 7);
  pdc->write(pdc->cookie, (base + 2) * 4, 0);
  pdc->write(pdc->cookie, base * 4, 0);
  for (unsigned i = 0; i < 3; ++i) {
   unsigned command = base + 3 + 4 * i;
   pdc->write(pdc->cookie, command * 4, 0x10108);
   pdc->write(pdc->cookie, (command + 1) * 4, address[i]);
   pdc->write(pdc->cookie, (command + 2) * 4, bank ? wake[i] : sleep[i]);
  }
 }
 pdc->write(pdc->cookie, 0x1148 * 4, 0);
 pdc->write(pdc->cookie, 0x1140 * 4, 0x80000001);
}
static int
a618_gmu_boot_legacy(const struct a618_io *gmu, const struct a618_io *pdc,
 const struct a618_io *seq, struct a618_hfi *hfi, uint32_t gpu_vote,
 uint32_t gmu_vote, struct a618_gmu_boot_result *result)
{
 if (!gmu_valid_io(gmu) || !gmu_valid_io(pdc) || !gmu_valid_io(seq) || !hfi ||
     !hfi->table || !hfi->barrier || hfi->poisoned || !result ||
     !(gpu_vote >> 16) || !(gmu_vote >> 16)) return EINVAL;
 result->phase = result->offset = result->value = result->firmware_version = 0;
 /* Never reload a running M3 or pretend a warm boot is cold. The caller
  * has already validated every SMMU page and the unmapped guard. */
 if (gmu->read(gmu->cookie, A618_GMU_SYSRESET) != 1) return EBUSY;
 uint32_t version = gmu->read(gmu->cookie, GMU_CM3_DTCM_START + 0xff8 * 4);
 if (version == 0xffffffffu) return EIO;
 /* Polled faults only; no unhandled GMU interrupts reach the host GIC. */
 gmu->write(gmu->cookie, GMU_GMU2HOST_INTR_MASK, 0xffffffffu);
 gmu->write(gmu->cookie, GMU_AO_HOST_INTERRUPT_MASK, 0xffffffffu);
 gmu->write(gmu->cookie, GMU_GMU2HOST_INTR_CLR, 0xffffffffu);
 gmu->write(gmu->cookie, 0x24c10, 0xffffffffu); /* AO status W1C */
 /* A reset M3 does not prove the independent RSC sequencer is idle. */
 if (gmu->read(gmu->cookie, 0x23000 + 0x101 * 4) != 0) return EBUSY;
 pdc_program(gmu, pdc, seq);
 gmu->write(gmu->cookie, GMU_GENERAL_7, 1);
 gmu->write(gmu->cookie, A618_GMU_FW_INIT_RESULT, 0);
 gmu->write(gmu->cookie, GMU_CM3_BOOT_CONFIG, 2);
 gmu->write(gmu->cookie, GMU_HFI_QTBL_ADDR, A618_HFI_IOVA);
 gmu->write(gmu->cookie, GMU_HFI_QTBL_INFO, 1);
 gmu->write(gmu->cookie, GMU_AHB_FENCE_RANGE_0, (1u << 31) | (0xau << 18) | 0xa0);
 gmu->write(gmu->cookie, GMU_CM3_CFG, 0x4052);
 /* 0x06010800 chipid in the GMU's packed minor/patch layout. */
 gmu->write(gmu->cookie, GMU_HFI_SFR_ADDR, 0x06018000);
 gmu->write(gmu->cookie, GPU_GMU_CX_GMU_PWR_COL_CP_MSG, A618_LOG_IOVA | 3);
 gmu->write(gmu->cookie, GMU_SYS_BUS_CONFIG, 1);
 gmu->write(gmu->cookie, GMU_ICACHE_CONFIG, 1);
 gmu->write(gmu->cookie, GMU_DCACHE_CONFIG, 1);
 /* ACTIVE idle policy: no automatic GX collapse until recovery exists. */
 gmu->write(gmu->cookie, GMU_PWR_COL_INTER_FRAME_CTRL, 0x09c40400);
 uint32_t rpmh = gmu->read(gmu->cookie, GMU_RPMH_CTRL);
 if (rpmh == 0xffffffffu) return EIO;
 gmu->write(gmu->cookie, GMU_RPMH_CTRL, rpmh | 0xf11);
 gmu->write(gmu->cookie, GPU_GMU_CX_GMU_PWR_COL_CP_RESP, 0);
 hfi->barrier(hfi->cookie);
 result->phase = 1;
 gmu->write(gmu->cookie, A618_GMU_SYSRESET, 0);
 int error = gmu_poll(gmu, A618_GMU_FW_INIT_RESULT,
     version <= 0x20010004 ? 0xffffffffu : 0x1ff,
     version <= 0x20010004 ? 0xbabeface : 0x100, 10000, result);
 if (error) goto failed;
 result->phase = 2;
 gmu->write(gmu->cookie, GMU_BOOT_SLUMBER_OPTION, 0);
 gmu->write(gmu->cookie, GMU_GX_VOTE_IDX, gpu_vote & 255);
 gmu->write(gmu->cookie, GMU_MX_VOTE_IDX, (gpu_vote >> 8) & 255);
 gmu->write(gmu->cookie, GMU_HOST2GMU_INTR_SET, 1u << 22);
 error = gmu_poll(gmu, GMU_GMU2HOST_INTR_INFO, 1u << 30, 1u << 30, 10000, result);
 gmu->write(gmu->cookie, GMU_GMU2HOST_INTR_CLR, 1u << 30);
 if (error) goto failed;
 result->phase = 3;
 gmu->write(gmu->cookie, GMU_GX_SPTPRAC_POWER_CONTROL, 0x778000);
 error = gmu_poll(gmu, GMU_SPTPRAC_PWR_CLK_STATUS, 0x38, 0x28, 100, result);
 if (error) goto failed;
 result->phase = 4;
 gmu->write(gmu->cookie, GMU_HFI_CTRL_INIT, 1);
 error = gmu_poll(gmu, GMU_HFI_CTRL_STATUS, 1, 1, 10000, result);
 if (error) goto failed;
 result->phase = 5;
 error = a618_hfi_bootstrap(hfi, gmu, gpu_vote, gmu_vote, &result->firmware_version);
 if (error) goto failed;
 result->phase = 6; /* GMU/HFI ready; this does not imply a working GPU CP. */
 return 0;
failed:
 /* Do not reset/power down behind an in-flight RPMh transaction or free
  * its DMA storage. Preserve the failure registers and poison the session.
  * Controlled shutdown/recovery is still required before any retry. */
 hfi->poisoned = 1;
 return error;
}


/* ================================ stage 9: command processor (original patches 0096-0106) ================================ */

#define A618_CP_DMA_BYTES 0x18000u
#define A618_CP_SQE_OFFSET 0x4000u
#define A618_CP_RING_OFFSET 0xc000u
#define A618_CP_FENCE_OFFSET 0x14000u
#define A618_CP_SHADOW_OFFSET (A618_CP_FENCE_OFFSET + 0x100u)
#define A618_CP_IOVA 0x70000000u
#define A618_CP_RING_IOVA (A618_CP_IOVA + 0x8000u)
#define A618_CP_FENCE_IOVA (A618_CP_IOVA + 0x10000u)
#define A618_CP_SHADOW_IOVA (A618_CP_FENCE_IOVA + 0x100u)
#define A618_CP_SNAPSHOT_COUNT 16
#define A618_GMU_AHB_FENCE_STATUS 0x24c4cu
struct a618_cp_snapshot {
 const char *tag;
 uint32_t irq, sqectl, instr_lo, instr_hi, rb_cntl, cp2gmu;
};
struct a618_cp_result {
 unsigned phase, submitted, completed;
 uint32_t rptr, wptr, status, interrupt, hw_fault, fence, value;
 uint32_t cp_interrupt;
 uint32_t irq_after_power, irq_before_auth, irq_after_auth;
 unsigned bootstrap_wait_us;
 uint32_t initial_sqectl, initial_instr_lo, initial_instr_hi, initial_rb_cntl;
 struct a618_cp_snapshot snapshot[A618_CP_SNAPSHOT_COUNT];
 unsigned snapshots;
 const char *first_fault;
 unsigned sqe_started, secure_prepare_attempted;
 int secure_prepare_error;
 uint64_t initial_tsb_base;
 uint32_t initial_tsb_size, initial_tsb_control;
 unsigned aperture_phase;
 int aperture_error;
 int64_t aperture_status;
 uint64_t aperture_response;
 int poisoned;
};
struct a618_pas_result { unsigned phase; int64_t status; uint64_t response; };
struct a618_commands {
 uint32_t *words;
 unsigned capacity;
 unsigned used;
};

/* ---- PM4 builder (sweet_adreno.c of 0089) ---- */
/* A6xx uses odd parity for the payload length and opcode/register fields. */
static uint32_t
emit_odd_parity(uint32_t value)
{
  unsigned parity = 1;
  while (value) {
    parity ^= value & 1u;
    value >>= 1;
  }
  return parity;
}

static int
emit_reserve(const struct a618_commands *commands, unsigned count)
{
  if (!commands || !commands->words || commands->used > commands->capacity)
    return EINVAL;
  if (count > commands->capacity - commands->used)
    return ENOSPC;
  return 0;
}

static uint32_t
emit_packet7(unsigned opcode, unsigned count)
{
  return 0x70000000u | count | (emit_odd_parity(count) << 15) |
      (opcode << 16) | (emit_odd_parity(opcode) << 23);
}

static __unused int
a618_emit_reg_write(struct a618_commands *commands, uint32_t reg, uint32_t value)
{
  int error;
  /* Register indices are DWORD indices, not byte offsets. */
  if (reg > 0x3ffffu)
    return EINVAL;
  error = emit_reserve(commands, 2);
  if (error)
    return error;
  commands->words[commands->used++] = 0x40000001u | (reg << 8) |
      (emit_odd_parity(reg) << 27); /* count=1 has odd parity already */
  commands->words[commands->used++] = value;
  return 0;
}

static int
a618_emit_memory_write(struct a618_commands *commands, uint64_t iova, uint32_t value)
{
  int error;
  if ((iova & 3u) || iova >= (1ull << 49))
    return EINVAL;
  error = emit_reserve(commands, 4);
  if (error)
    return error;
  commands->words[commands->used++] = emit_packet7(0x3d, 3); /* CP_MEM_WRITE */
  commands->words[commands->used++] = (uint32_t)iova;
  commands->words[commands->used++] = (uint32_t)(iova >> 32);
  commands->words[commands->used++] = value;
  return 0;
}

static int
a618_emit_fence(struct a618_commands *commands, uint64_t iova, uint32_t sequence)
{
  int error;
  if ((iova & 3u) || iova >= (1ull << 49))
    return EINVAL;
  error = emit_reserve(commands, 5);
  if (error)
    return error;
  commands->words[commands->used++] = emit_packet7(0x46, 4); /* CP_EVENT_WRITE */
  /* CACHE_FLUSH_TS + IRQ, same event used by MSM a6xx_submit(). */
  commands->words[commands->used++] = 4u | (1u << 31);
  commands->words[commands->used++] = (uint32_t)iova;
  commands->words[commands->used++] = (uint32_t)(iova >> 32);
  commands->words[commands->used++] = sequence;
  return 0;
}


/* ---- signed ZAP / aperture SCM (sweet_adreno_pas.c of 0103) ---- */
/* SPDX-License-Identifier: MIT
 * Restricted SM7150 signed-ZAP PAS transport (public Qualcomm SCM protocol).
 * No generic SCM entry point, memory-assignment or security-register bypass.
 */
static uint32_t
le32(const unsigned char *p)
{
 return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int
scm(unsigned service, unsigned command, uint64_t arguments,
 uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
 struct a618_pas_result *result)
{
 uint64_t function = 0x42000000u | (service << 8) | command;
 for (unsigned busy = 0; busy < 5; ++busy) {
  uint64_t resume = function, quirk_a6 = 0;
  for (unsigned interrupted = 0; interrupted < 32; ++interrupted) {
   register uint64_t x0 __asm__("x0") = resume;
   register uint64_t x1 __asm__("x1") = arguments;
   register uint64_t x2 __asm__("x2") = arg0;
   register uint64_t x3 __asm__("x3") = arg1;
   register uint64_t x4 __asm__("x4") = arg2;
   register uint64_t x5 __asm__("x5") = arg3;
   register uint64_t x6 __asm__("x6") = quirk_a6;
   register uint64_t x7 __asm__("x7") = 0;
   __asm__ volatile("dsb sy\n\tsmc #0\n\tdsb sy" :
       "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
       "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7) : :
       "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
       "x16", "x17", "memory", "cc");
   result->status = (int64_t)x0;
   result->response = x1;
   if (x0 == 1) { resume = 1; quirk_a6 = x6; continue; }
   if ((int64_t)x0 == -12) break; /* bounded busy retry */
   if (x0) return x0 == (uint64_t)-1 ? ENOTSUP : EIO;
   return 0;
  }
  if (result->status != -12) return ETIMEDOUT;
  if (busy != 4) delay(30000);
 }
 return EBUSY;
}
/* Public SCM aperture protocol used by the pinned Linux Adreno driver.
 * Availability is optional; an advertised call that fails is fatal. The four
 * arguments fit x2..x5 directly, so no shared extended-argument buffer exists.
 * This entry cannot select another context or issue arbitrary SCM commands. */
static int
a618_cp_configure_aperture(struct a618_pas_result *result)
{
 if (!result) return EINVAL;
 *result = (struct a618_pas_result){ .phase = 1 };
 int error = scm(6, 1, 1, 0x02000c1b, 0, 0, 0, result);
 /* Linux treats a failed availability query as unavailable. It has no
  * configuration side effects; retain raw status for the boot log. */
 if (error) return 0;
 if (!result->response) return 0; /* optional MP command unavailable */
 result->phase = 2;
 error = scm(0x0c, 0x1b, 4, 0xffff0000, 0xffffffffu,
     0xffffffffu, 0xffffffffu, result);
 if (error) return error;
 result->phase = 3;
 return 0;
}
static int
a618_zap_authenticate(volatile uint32_t *reserved, uint64_t physical,
 const struct a618_dma_region *metadata, const unsigned char *mdt,
 unsigned mdt_bytes, const unsigned char *code, unsigned code_bytes,
 struct a618_pas_result *result)
{
 if (!result) return EINVAL;
 *result = (struct a618_pas_result){0};
 if (!reserved || physical != 0x97f15000ull || !metadata || !metadata->cpu ||
     metadata->bytes != 0x4000 || !metadata->uncached ||
     !metadata->physical || (metadata->physical & 0x3fff) ||
     metadata->physical >= (1ull << 36) || !mdt || !code ||
     mdt_bytes != 0x1acc || code_bytes != 0x7c0 ||
     le32(mdt) != 0x464c457f || mdt[4] != 1 || mdt[5] != 1 ||
     le32(mdt + 28) != 52 || (le32(mdt + 44) & 0xffff) != 3 || le32(mdt + 40) != 0x00200034 ||
     le32(mdt + 52 + 2 * 32) != 1 ||
     le32(mdt + 52 + 2 * 32 + 12) != 0x5000 ||
     le32(mdt + 52 + 2 * 32 + 16) != code_bytes ||
     le32(mdt + 52 + 2 * 32 + 20) != code_bytes ||
     le32(mdt + 52 + 2 * 32 + 24) != 0x08000007) return EINVAL;
 result->phase = 1;
 const unsigned commands[] = {1, 2, 5};
 for (unsigned i = 0; i < 3; ++i) {
  int error = scm(6, 1, 1, 0x02000200u | commands[i], 0, 0, 0, result);
  if (error) return error;
  if (!result->response) return ENOTSUP;
 }
 volatile uint32_t *words = (volatile uint32_t *)metadata->cpu;
 for (unsigned i = 0; i < mdt_bytes / 4; ++i) words[i] = le32(mdt + i * 4);
 __asm__ volatile("dsb sy" ::: "memory");
 result->phase = 2;
 int error = scm(2, 1, 0x82, 13, metadata->physical, 0, 0, result);
 if (error || result->response) return error ? error : EACCES;
 /* TZ may now protect metadata. Retain its separate page and do not read,
  * modify, free or reuse it, including on errors below. */
 result->phase = 3;
 error = scm(2, 2, 3, 13, physical, 0x1000, 0, result);
 if (error || result->response) return error ? error : EACCES;
 for (unsigned i = 0; i < 0x1000 / 4; ++i) reserved[i] = 0;
 for (unsigned i = 0; i < code_bytes / 4; ++i) reserved[i] = le32(code + i * 4);
 __asm__ volatile("dsb sy" ::: "memory");
 result->phase = 4;
 error = scm(2, 5, 1, 13, 0, 0, 0, result);
 if (error || result->response) return error ? error : EACCES;
 result->phase = 5;
 return 0;
}

/* ---- CB0 context for the command processor (sweet_adreno_cp_smmu.c of 0096) ---- */
/* SPDX-License-Identifier: MIT
 * Original bounded SMMUv2 backend using the ARM architecture and public
 * Linux/Qualcomm register protocols. See docs/adreno-port.md for source pins.
 */
#define CP_ASID 0x62u
static int
cp_put(const struct a618_io *io, uint32_t offset, uint32_t value,
 uint32_t mask, struct a618_smmu_result *r)
{
 io->write(io->cookie, offset, value);
 r->offset = offset; r->value = io->read(io->cookie, offset);
 return r->value != 0xffffffffu && (r->value & mask) == (value & mask) ? 0 : EIO;
}
static int
cp_wait_clear(const struct a618_io *io, uint32_t offset, unsigned mask,
 struct a618_smmu_result *r)
{
 for (unsigned i = 0; i < 500; ++i) {
  r->offset = offset; r->value = io->read(io->cookie, offset);
  if (r->value == 0xffffffffu) return EIO;
  if (!(r->value & mask)) return 0;
  io->delay_us(io->cookie, 1);
 }
 return ETIMEDOUT;
}
static int
a618_smmu_prepare_cp(const struct a618_smmu_io *ops,
 const struct a618_dma_region *memory, struct a618_smmu_result *r)
{
 if (!ops || !memory || !r || !ops->registers.read ||
     !ops->registers.write || !ops->registers.delay_us ||
     !ops->read64 || !ops->write64 || !ops->barrier || !memory->cpu ||
     !memory->uncached || memory->bytes != A618_CP_DMA_BYTES ||
     (memory->physical & 0x3fff) || !memory->physical ||
     memory->physical > (1ull << 36) - A618_CP_DMA_BYTES ||
     ((uintptr_t)memory->cpu & 7)) return EINVAL;
 const struct a618_io *io = &ops->registers;
 *r = (struct a618_smmu_result){ .context = 0, .slot = 0xffffffffu };
 /* Measured 0091 profile: 4 KiB register pages, 0x8000 CB base, five
  * stream match slots and context banks, 36-bit PA. 16K PT granules are
  * intentionally not used (Qualcomm advertises broken 16K support).
  */
 const uint32_t identity[] = {0x48016605, 0x20008005, 0x7511};
 for (unsigned i = 0; i < 3; ++i) {
  r->offset = 0x20 + 4 * i; r->value = io->read(io->cookie, r->offset);
  if (r->value != identity[i]) return ENOTSUP;
 }
 /* Do not steal an active bank or matching stream, even if its firmware
  * owner appears idle. CB1 and protected banks are left alone.
  */
 r->offset = CP_CB; r->value = io->read(io->cookie, CP_CB);
 if (r->value == 0xffffffffu) return EIO;
 if (r->value & 1) return EBUSY; /* M=0 is disabled; other reset bits may be UNKNOWN. */
 for (unsigned i = 0; i < 5; ++i) {
  r->offset = 0x800 + 4 * i;
  uint32_t smr = r->value = io->read(io->cookie, r->offset);
  if (smr == 0xffffffffu) return EIO;
  if (!(smr & 0x80000000u)) {
   if (r->slot == 0xffffffffu) r->slot = i;
  } else if (((smr ^ 0u) & ~((smr >> 16) & 0x7fffu) & 0xffffu) == 0) {
   r->slot = i; return EBUSY;
  }
 }
 if (r->slot == 0xffffffffu) return ENOSPC;
 r->control = io->read(io->cookie, 0);
 if (r->control == 0xffffffffu) return EIO;
 /* Three 4K tables inside one host page: L1[1] -> L2[384] ->
  * L3[0..19] at 0x70000000. SQE pages are RO/executable; ring/fence
  * pages RW/NX. Tables are inaccessible to the GPU. Normal NC/OSH.
  */
 volatile uint64_t *l1 = (volatile uint64_t *)memory->cpu;
 volatile uint64_t *l2 = (volatile uint64_t *)(memory->cpu + 0x1000);
 volatile uint64_t *l3 = (volatile uint64_t *)(memory->cpu + 0x2000);
 for (unsigned i = 0; i < 512; ++i) l1[i] = l2[i] = l3[i] = 0;
 l1[1] = (memory->physical + 0x1000) | 3;
 l2[384] = (memory->physical + 0x2000) | 3;
 for (unsigned i = 0; i < 20; ++i)
  l3[i] = (memory->physical + A618_TABLE_BYTES + 0x1000 * i) |
      (i < 8 ? 0 : (3ull << 53)) | (1u << 11) | (1u << 10) |
      (2u << 8) | ((i < 8 ? 3u : 1u) << 6) | 3;
 ops->barrier(io->cookie);
 int error;
 #define SET(o,v,m) do { error = cp_put(io, (o), (v), (m), r); if (error) return error; } while (0)
 SET(0x1800, 1, 1); /* GR1 CBA2R(1): AArch64. */
 SET(0x1000, 0x1f300, 0x3f3ff); /* Stage1, stage2 bypass, weak upstream attributes. */
 SET(CP_CB + 0x10, 0x38011, 0x38077); /* TCR2: SEP upstream, 36-bit output, AArch64 ASID. */
 SET(CP_CB + 0x30, 0x00802020u, 0x00c0ffbfu); /* EPD1, OSH, NC walks, T0SZ=32; AArch64, no EAE. */
 uint64_t ttbr = memory->physical | ((uint64_t)CP_ASID << 48);
 ops->write64(io->cookie, CP_CB + 0x20, ttbr);
 r->offset = CP_CB + 0x20;
 r->par = ops->read64(io->cookie, r->offset);
 if ((r->par & 0xffff000ffffff000ull) != ttbr) return EIO;
 ops->write64(io->cookie, CP_CB + 0x28, 0);
 SET(CP_CB + 0x38, 0x44, 0xff); /* MAIR[0]: Normal NC. */
 /* MAIR1 is unused: every data PTE references MAIR0[0]. */
 /* Fault reporting enabled, IRQ/stalling disabled until an IRQ handler
  * exists; HUPCF continues other transactions after an abort.
  */
 /* AFE/TRE are UNK/SBOP for long descriptors: write ones, do not
  * compare their readback. Check M, E, fault controls, WXN and ASIDPNE.
  */
 SET(CP_CB, 0x1127, 0x17f1);
 /* Block unmatched streams and enable this device. Preserve other controls;
  * do not enable interrupt sources without handlers. No reset-all/TLBI-all.
  */
 SET(0, (r->control | 0x1c12u) & ~0xe025u, 0xfc37);
 io->write(io->cookie, CP_CB + 0x610, CP_ASID);
 io->write(io->cookie, CP_CB + 0x7f0, 0);
 error = cp_wait_clear(io, CP_CB + 0x7f4, 1, r);
 if (error) return error;
 /* Hardware translation probes, not a software table walk: all twenty pages
  * plus a guard fault must match before the stream is published.
  */
 for (unsigned i = 0; i <= 20; ++i) {
  uint64_t va = (uint64_t)A618_CP_IOVA + i * 0x1000u;
  ops->write64(io->cookie, CP_CB + 0x800, va);
  error = cp_wait_clear(io, CP_CB + 0x8f0, 1, r);
  if (error) return error;
  r->par = ops->read64(io->cookie, CP_CB + 0x50);
  if (r->par == 0xffffffffffffffffull) return EIO;
  if (i == 20) {
   if (!(r->par & 1)) return EFAULT;
  } else {
   uint64_t expected = memory->physical + A618_TABLE_BYTES + i * 0x1000u;
   if ((r->par & 1) || (r->par & 0x0000000ffffff000ull) != expected)
    return EFAULT;
   ++r->translated;
  }
 }
 r->fault = io->read(io->cookie, CP_CB + 0x58);
 if (r->fault == 0xffffffffu) return EIO;
 if (r->fault & (1u << 30)) return EBUSY; /* Never resume an unknown stall. */
 if (r->fault & FSR_FAULT) {
  io->write(io->cookie, CP_CB + 0x58, r->fault & (FSR_FAULT & ~(1u << 30)));
  uint32_t after = io->read(io->cookie, CP_CB + 0x58);
  if (after == 0xffffffffu || (after & FSR_FAULT)) return EIO;
 }
 /* Only exact GPU stream 0, route first and VALID last. CP_CB/stream5
  * remain owned by the running GMU. No user stream or bypass entry. */
 SET(0xc00 + 4 * r->slot, 0, 0x030300ff);
 SET(0x800 + 4 * r->slot, 0x80000000u, 0xffffffffu);
 r->fault = io->read(io->cookie, CP_CB + 0x58);
 #undef SET
 return r->fault == 0xffffffffu || (r->fault & FSR_FAULT) ? EIO : 0;
}

/* ---- SQE start, ring, PM4 (sweet_adreno_cp.c of 0106) ---- */
/* SPDX-License-Identifier: MIT
 * Bounded kernel-only A618 SQE/CP bring-up from pinned public PM4/register
 * protocols. No userspace PM4, scheduling, 3D context or graphics advertising.
 */
#define CP_RB_CNTL 0x2008u
#define CP_RB_RPTR_ADDR 0x2010u
#define CP_RB_RPTR 0x2018u
#define CP_RB_WPTR 0x201cu
#define CP_SQE_CNTL 0x2020u
#define CP_SQE_INSTR_BASE 0x20c0u
#define CP_ADDR_MODE_CNTL 0x2108u
#define CP_ROQ_THRESHOLDS_1 0x2304u
#define CP_ROQ_THRESHOLDS_2 0x2308u
#define CP_MEM_POOL_SIZE 0x230cu
#define CP_AHB_CNTL 0x2634u
#define CP_PROTECT_CNTL 0x213cu
#define RBBM_STATUS 0x840u
#define RBBM_INT_0_STATUS 0x804u
#define RBBM_INT_0_MASK 0xe0u
#define CP_HW_FAULT 0x2084u
#define CP_INTERRUPT_STATUS 0x208cu
#define CP_LOCAL_ERRORS 0xf7u
#define UCHE_FILTER_CNTL 0x3860u
#define UCHE_CACHE_WAYS 0x385cu
#define UCHE_WRITE_RANGE_MAX 0x3814u
#define UCHE_TRAP_BASE 0x3824u
#define UCHE_WRITE_THRU_BASE 0x381cu
#define GMU_HOST_SET 0x14650u
#define GMU_HOST_INFO 0x14648u
#define GMU_HOST_CLEAR 0x14644u
#define CP_IRQ_FAULT ((1u << 1) | (1u << 9) | (1u << 23))
static int
valid_io(const struct a618_io *io)
{
 return io && io->read && io->write && io->delay_us;
}
static void
write_address(const struct a618_io *io, unsigned offset, uint64_t address)
{
 io->write(io->cookie, offset, (uint32_t)address);
 io->write(io->cookie, offset + 4, (uint32_t)(address >> 32));
}
static unsigned
cp_odd_parity(uint32_t value)
{
 value ^= value >> 16; value ^= value >> 8; value ^= value >> 4;
 value &= 15;
 return 1 ^ ((0x6996 >> value) & 1);
}
static uint32_t
cp_packet7(unsigned opcode, unsigned count)
{
 return 0x70000000u | count | (cp_odd_parity(count) << 15) |
     (opcode << 16) | (cp_odd_parity(opcode) << 23);
}
static int
cp_fault(const struct a618_io *gpu, const struct a618_io *gmu,
 struct a618_cp_result *result)
{
   result->interrupt = gpu->read(gpu->cookie, RBBM_INT_0_STATUS);
 result->hw_fault = gpu->read(gpu->cookie, CP_HW_FAULT);
 result->cp_interrupt = gpu->read(gpu->cookie, CP_INTERRUPT_STATUS);
 uint32_t info = gmu->read(gmu->cookie, GMU_HOST_INFO);
 if ((result->cp_interrupt == 0xffffffffu ||
      (result->cp_interrupt & CP_LOCAL_ERRORS)) && !result->first_fault)
  result->first_fault = "runtime";
 return result->interrupt == 0xffffffffu || result->hw_fault == 0xffffffffu ||
     result->cp_interrupt == 0xffffffffu || (result->cp_interrupt & CP_LOCAL_ERRORS) ||
     (result->interrupt & CP_IRQ_FAULT) || result->hw_fault ||
     info == 0xffffffffu || (info & (1u << 23));
}
/* Bounded startup trace: identify the first setup write that latches a CP
 * error instead of attributing an inherited fault to the later release. */
static void
cp_snapshot(const struct a618_io *gpu, struct a618_cp_result *result,
 const char *tag)
{
 if (result->snapshots >= A618_CP_SNAPSHOT_COUNT) return;
 struct a618_cp_snapshot *snapshot = &result->snapshot[result->snapshots++];
 snapshot->tag = tag;
 snapshot->irq = gpu->read(gpu->cookie, CP_INTERRUPT_STATUS);
 snapshot->sqectl = gpu->read(gpu->cookie, CP_SQE_CNTL);
 snapshot->instr_lo = gpu->read(gpu->cookie, CP_SQE_INSTR_BASE);
 snapshot->instr_hi = gpu->read(gpu->cookie, CP_SQE_INSTR_BASE + 4);
 snapshot->rb_cntl = gpu->read(gpu->cookie, CP_RB_CNTL);
 snapshot->cp2gmu = gpu->read(gpu->cookie, 0x2048);
 if ((snapshot->irq == 0xffffffffu ||
      (snapshot->irq & CP_LOCAL_ERRORS)) && !result->first_fault)
  result->first_fault = tag;
}
static int
publish(const struct a618_io *gpu, const struct a618_io *gmu,
 const struct a618_dma_region *memory, const uint32_t *words, unsigned count,
 void (*barrier)(void *), void *cookie, struct a618_cp_result *result)
{
 if (result->poisoned || !count || count > 8188) return EINVAL;
 if (cp_fault(gpu, gmu, result)) { result->poisoned = 1; return EIO; }
 /* This SQE version requires WHERE_AM_I. Its memory shadow is the consumer
  * position; CP_RB_RPTR is not used as completion evidence. */
 volatile uint32_t *shadow = (volatile uint32_t *)(memory->cpu + A618_CP_SHADOW_OFFSET);
 barrier(cookie);
 result->rptr = *shadow;
 if (result->rptr >= 8192 || result->wptr >= 8192) return EIO;
 unsigned available = (result->rptr + 8192 - result->wptr - 1) % 8192;
 unsigned total = count + 3;
 if (total > available) return ENOSPC;
 volatile uint32_t *ring = (volatile uint32_t *)(memory->cpu + A618_CP_RING_OFFSET);
 for (unsigned i = 0; i < count; ++i) ring[(result->wptr + i) % 8192] = words[i];
 const uint32_t update[] = {cp_packet7(0x62, 2), A618_CP_SHADOW_IOVA, 0};
 for (unsigned i = 0; i < 3; ++i) ring[(result->wptr + count + i) % 8192] = update[i];
 barrier(cookie);
 result->wptr = (result->wptr + total) % 8192;
 gpu->write(gpu->cookie, CP_RB_WPTR, result->wptr);
 result->submitted += total;
 /* GMU may drop a GX write when its fence is closed: bounded retry, not a
  * read of CP_WPTR as an assumed proof of command execution. */
 for (unsigned us = 0; us < 1000; us += 10) {
  uint32_t fence = gmu->read(gmu->cookie, A618_GMU_AHB_FENCE_STATUS);
  if (fence == 0xffffffffu) goto failed;
  if (!(fence & 1)) break;
  if (us == 990) goto failed;
  gmu->delay_us(gmu->cookie, 10);
  gpu->write(gpu->cookie, CP_RB_WPTR, result->wptr);
 }
 for (unsigned us = 0; us < 10000; us += 10) {
  if (cp_fault(gpu, gmu, result)) goto failed;
  barrier(cookie);
  result->rptr = *shadow;
  if (result->rptr >= 8192) goto failed;
  result->status = gpu->read(gpu->cookie, RBBM_STATUS);
  if (result->rptr == result->wptr && !(result->status & ~1u)) {
   result->completed += total; return 0;
  }
  gpu->delay_us(gpu->cookie, 10);
 }
 result->poisoned = 1;
 return ETIMEDOUT;
failed:
 result->poisoned = 1;
 return EIO;
}
/* Stage independently of GX startup so the adapter can verify the physical
 * backing before publishing a command. No MMIO or hardware execution here. */
static int
a618_cp_stage(const struct a618_dma_region *memory, const unsigned char *firmware,
 unsigned bytes, void (*barrier)(void *), void *cookie)
{
 if (!memory || !memory->cpu || !memory->uncached ||
     memory->bytes != A618_CP_DMA_BYTES || ((uintptr_t)memory->cpu & 3) ||
     !firmware || bytes != 32136 || !barrier) return EINVAL;
 volatile uint32_t *sqe = (volatile uint32_t *)(memory->cpu + A618_CP_SQE_OFFSET);
 for (unsigned i = 0; i < (bytes - 4) / 4; ++i) {
  uint32_t word = 0;
  for (unsigned b = 0; b < 4; ++b)
   word |= (uint32_t)firmware[4 + i * 4 + b] << (8 * b);
  sqe[i] = word;
 }
 barrier(cookie);
 for (unsigned i = 0; i < (bytes - 4) / 4; ++i) {
  uint32_t word = 0;
  for (unsigned b = 0; b < 4; ++b)
   word |= (uint32_t)firmware[4 + i * 4 + b] << (8 * b);
  if (sqe[i] != word) return EIO;
 }
 barrier(cookie);
 return 0;
}
static int
a618_cp_start(const struct a618_io *gpu, const struct a618_io *gmu,
 const struct a618_dma_region *memory, const unsigned char *firmware,
 unsigned bytes, void (*barrier)(void *), void *cookie,
 int (*secure_prepare)(void *), void *secure_cookie, struct a618_cp_result *result)
{
 if (!valid_io(gpu) || !valid_io(gmu) || !memory || !memory->cpu ||
     memory->bytes != A618_CP_DMA_BYTES || !memory->uncached || !firmware ||
     bytes != 32136 || !barrier || !secure_prepare || !result) return EINVAL;
 *result = (struct a618_cp_result){0};
 /* Firmware version excludes the container DWORD. Require patched A630 SQE
  * at least as new as the pinned Linux driver requires for WHERE_AM_I. */
 uint32_t version = 0, patch = 0;
 for (unsigned i = 0; i < 4; ++i) {
  version |= (uint32_t)firmware[4 + i] << (i * 8);
  patch |= (uint32_t)firmware[12 + i] << (i * 8);
 }
 if (!(((version & 15) == 10 && (patch & 15) >= 1) ||
     (version & 0xfff) >= 0x190)) return ENOTSUP;
 volatile uint32_t *sqe = (volatile uint32_t *)(memory->cpu + A618_CP_SQE_OFFSET);
 for (unsigned i = 0; i < (bytes - 4) / 4; ++i) {
  uint32_t word = 0;
  for (unsigned b = 0; b < 4; ++b) word |= (uint32_t)firmware[4 + i * 4 + b] << (8 * b);
  if (sqe[i] != word) return EIO;
 }
 barrier(cookie);
 result->phase = 1;
 /* Legacy DCVS selects the one nonzero performance-table entry (180MHz).
  * An ARC voltage vote alone does not set the GX clock. */
 gmu->write(gmu->cookie, 0x8ff0, 0); /* DCVS_ACK_OPTION */
 gmu->write(gmu->cookie, 0x8ff4, 0x30000001); /* PERF_SETTING, index1 */
 gmu->write(gmu->cookie, 0x8ff8, 0xff); /* FW chooses bandwidth vote */
 gmu->write(gmu->cookie, GMU_HOST_SET, 1u << 23);
 unsigned elapsed;
 for (elapsed = 0; elapsed < 10000; elapsed += 10) {
  uint32_t info = gmu->read(gmu->cookie, GMU_HOST_INFO);
  if (info == 0xffffffffu || (info & (1u << 23))) goto failed;
  if (info & (1u << 31)) break;
  gmu->delay_us(gmu->cookie, 10);
 }
 gmu->write(gmu->cookie, GMU_HOST_CLEAR, 1u << 31);
 gmu->write(gmu->cookie, GMU_HOST_SET, 1u << 31);
 if (elapsed == 10000) { result->poisoned = 1; return ETIMEDOUT; }
 if (gmu->read(gmu->cookie, 0x8ffc)) goto failed; /* DCVS_RETURN */
 /* GX access is permitted only after the firmware acknowledges GPU_SET. */
 gmu->write(gmu->cookie, GMU_HOST_SET, 1u << 16);
 unsigned us;
 for (us = 0; us < 10000; us += 10) {
  uint32_t info = gmu->read(gmu->cookie, GMU_HOST_INFO);
  if (info == 0xffffffffu || (info & (1u << 23))) goto failed;
  if (info & (1u << 24)) break;
  gmu->delay_us(gmu->cookie, 10);
 }
 if (us == 10000) { result->poisoned = 1; return ETIMEDOUT; }
 gmu->write(gmu->cookie, GMU_HOST_CLEAR, 1u << 24);
 result->phase = 2;
 result->irq_after_power = gpu->read(gpu->cookie, CP_INTERRUPT_STATUS);
 result->initial_sqectl = gpu->read(gpu->cookie, CP_SQE_CNTL);
 result->initial_instr_lo = gpu->read(gpu->cookie, CP_SQE_INSTR_BASE);
 result->initial_instr_hi = gpu->read(gpu->cookie, CP_SQE_INSTR_BASE + 4);
 result->initial_rb_cntl = gpu->read(gpu->cookie, CP_RB_CNTL);
 cp_snapshot(gpu, result, "phase2");
 if (result->irq_after_power == 0xffffffffu ||
     result->initial_sqectl == 0xffffffffu ||
     result->initial_instr_lo == 0xffffffffu ||
     result->initial_instr_hi == 0xffffffffu ||
     result->initial_rb_cntl == 0xffffffffu) goto failed;
 /* A cold SQE is already halted. Halt only an engine that ABL left enabled,
  * and do it before the bus/clock release below so it cannot fetch from a
  * stale instruction base. Do not write 3 to a halted engine: that write has
  * not been proven inert here and currently precedes the observed fault. */
 if (result->initial_sqectl & 1u) {
  gpu->write(gpu->cookie, CP_SQE_CNTL, 3);
  cp_snapshot(gpu, result, "early_halt");
 }
 /* Fresh engine only. Never overwrite an already running command processor. */
 if (gpu->read(gpu->cookie, CP_RB_RPTR) || gpu->read(gpu->cookie, CP_RB_WPTR))
  goto failed;
 gpu->write(gpu->cookie, 0xf114, 0);
 (void)gpu->read(gpu->cookie, 0xf114);
 gpu->write(gpu->cookie, 0x58, 0);
 (void)gpu->read(gpu->cookie, 0x58);
 cp_snapshot(gpu, result, "gbif");
 /* Normal non-secure range setup from the A6xx startup protocol. Secure
  * mode itself is still left only through the authenticated ZAP path. */
 result->initial_tsb_control = gpu->read(gpu->cookie, 0x3e00c);
 result->initial_tsb_base = (uint64_t)gpu->read(gpu->cookie, 0x3e000) |
     ((uint64_t)gpu->read(gpu->cookie, 0x3e004) << 32);
 result->initial_tsb_size = gpu->read(gpu->cookie, 0x3e008);
 if (result->initial_tsb_control == 0xffffffffu ||
     result->initial_tsb_base == 0xffffffffffffffffull ||
     result->initial_tsb_size == 0xffffffffu) goto failed;
 gpu->write(gpu->cookie, 0x3e00c, 0); /* TSB_CNTL */
 write_address(gpu, 0x3e000, 0); /* TSB_TRUSTED_BASE */
 gpu->write(gpu->cookie, 0x3e008, 0); /* TSB_TRUSTED_SIZE */
 gpu->write(gpu->cookie, 0x3e040, 1); /* TSB_ADDR_MODE_CNTL */
 if (gpu->read(gpu->cookie, 0x3e00c) ||
     gpu->read(gpu->cookie, 0x3e000) || gpu->read(gpu->cookie, 0x3e004) ||
     gpu->read(gpu->cookie, 0x3e008) || gpu->read(gpu->cookie, 0x3e040) != 1)
  goto failed;
 cp_snapshot(gpu, result, "tsb");
 gpu->write(gpu->cookie, 0x3800, 1);
 gpu->write(gpu->cookie, RBBM_INT_0_MASK, 0); /* no unhandled host IRQ */
 cp_snapshot(gpu, result, "uche");
 gpu->write(gpu->cookie, CP_ADDR_MODE_CNTL, 1);
 gpu->write(gpu->cookie, CP_ROQ_THRESHOLDS_2, 0x010000c0);
 gpu->write(gpu->cookie, CP_ROQ_THRESHOLDS_1, 0x8040362c);
 gpu->write(gpu->cookie, CP_MEM_POOL_SIZE, 128);
 gpu->write(gpu->cookie, CP_AHB_CNTL, 1);
 gpu->write(gpu->cookie, UCHE_FILTER_CNTL, 0x804);
 gpu->write(gpu->cookie, UCHE_CACHE_WAYS, 4);
 write_address(gpu, UCHE_WRITE_RANGE_MAX, 0x1ffffffffffc0ull);
 write_address(gpu, UCHE_TRAP_BASE, 0x1fffffffff000ull);
 write_address(gpu, UCHE_WRITE_THRU_BASE, 0x1fffffffff000ull);
 cp_snapshot(gpu, result, "config");
 /* Hardware protections remain enabled; only kernel-built PM4 is submitted. */
 static const uint32_t protect[32] = {
  0x13fc0000,
  0x00140501,
  0x0bd0050b,
  0x8000050e,
  0x80000510,
  0x80000534,
  0x82080800,
  0x802008a0,
  0x809008ab,
  0x02b808de,
  0x81340900,
  0x89c8098d,
  0x80040e00,
  0x80300e03,
  0x830c3c00,
  0x7ffc3cc4,
  0x873c8630,
  0x80008e00,
  0x80008e08,
  0x807c8e50,
  0x876c9624,
  0x80049e70,
  0x861c9e78,
  0x873ca630,
  0x8000ae02,
  0x8cbcae50,
  0x8000b604,
  0x8004be02,
  0xdf7cbe20,
  0xaffcf000,
  0x7ffcfc00,
  0x80011c00,
 };
 for (unsigned i = 0; i < 32; ++i) gpu->write(gpu->cookie, (0x850 + i) * 4, protect[i]);
 gpu->write(gpu->cookie, CP_PROTECT_CNTL, 0xb);
 cp_snapshot(gpu, result, "protect");
 /* Match the optional firmware-owned CP SMMU aperture setup before SQE
  * can access context-bank state. GX is held by GPU_SET at this point. */
 struct a618_pas_result aperture;
 int aperture_error = a618_cp_configure_aperture(&aperture);
 result->aperture_phase = aperture.phase;
 result->aperture_error = aperture_error;
 result->aperture_status = aperture.status;
 result->aperture_response = aperture.response;
 if (aperture_error) { result->poisoned = 1; return aperture_error; }
 cp_snapshot(gpu, result, "aperture");
 write_address(gpu, CP_SQE_INSTR_BASE, A618_CP_IOVA);
 cp_snapshot(gpu, result, "instr_base");
 write_address(gpu, CP_RB_BASE, A618_CP_RING_IOVA);
 volatile uint32_t *shadow = (volatile uint32_t *)(memory->cpu + A618_CP_SHADOW_OFFSET);
 *shadow = 0;
 barrier(cookie);
 write_address(gpu, CP_RB_RPTR_ADDR, A618_CP_SHADOW_IOVA);
 /* Reject a truncated or dropped shadow-base write before SQE starts. */
 if (gpu->read(gpu->cookie, CP_RB_RPTR_ADDR) != A618_CP_SHADOW_IOVA ||
     gpu->read(gpu->cookie, CP_RB_RPTR_ADDR + 4)) goto failed;
 cp_snapshot(gpu, result, "rb_base");
 gpu->write(gpu->cookie, CP_RB_CNTL, (1u << 27) | (2u << 8) | 12u);
 cp_snapshot(gpu, result, "rb_cntl");
 /* Device vendor startup authenticates its signed ZAP before SQE release.
  * The adapter callback is synchronous and cannot submit PM4. It runs only
  * after GPU_SET, DCVS and base programming. Authorization failure retains
  * resources and leaves SQE halted. */
 result->irq_before_auth = gpu->read(gpu->cookie, CP_INTERRUPT_STATUS);
 cp_snapshot(gpu, result, "preauth");
 result->secure_prepare_attempted = 1;
 result->secure_prepare_error = secure_prepare(secure_cookie);
 if (result->secure_prepare_error) {
  result->poisoned = 1;
  return result->secure_prepare_error;
 }
 result->irq_after_auth = gpu->read(gpu->cookie, CP_INTERRUPT_STATUS);
 cp_snapshot(gpu, result, "auth");
 /* Attribute a pre-existing error to setup/authentication, rather than
  * describing it as an SQE execution failure. Never clear unknown faults. */
 if (cp_fault(gpu, gmu, result)) goto failed;
 gpu->write(gpu->cookie, CP_SQE_CNTL, 1);
 result->sqe_started = 1;
 cp_snapshot(gpu, result, "release");
 /* SQE bootstrap signals CP_CP2GMU_STATUS bit2 before entering waitin.
  * Require that firmware handshake before publishing any ring commands;
  * a timeout/fault here is distinct from a ME_INIT execution failure. */
 for (unsigned waited = 0; waited < 10000; waited += 10) {
  result->bootstrap_wait_us = waited;
  if (cp_fault(gpu, gmu, result)) goto failed;
  if (gpu->read(gpu->cookie, 0x2048) & 4u) break;
  gpu->delay_us(gpu->cookie, 10);
  if (waited == 9990) { result->poisoned = 1; return ETIMEDOUT; }
 }
 cp_snapshot(gpu, result, "bootstrap");
 uint32_t init[] = {cp_packet7(0x48, 8), 0x2f, 3, 0x20000000, 0, 0, 0, 0, 0};
 int error = publish(gpu, gmu, memory, init, 9, barrier, cookie, result);
 if (error) return error;
 result->phase = 3; /* SQE consumed ME_INIT; secure transition still required. */
 return 0;
failed:
 result->poisoned = 1;
 return EIO;
}
static int
a618_cp_leave_secure(const struct a618_io *gpu, const struct a618_io *gmu,
 const struct a618_dma_region *memory, void (*barrier)(void *), void *cookie,
 struct a618_cp_result *result)
{
 if (!valid_io(gpu) || !valid_io(gmu) || !memory || !memory->cpu || !barrier ||
     !result || result->phase != 3 || result->poisoned) return EINVAL;
 uint32_t secure[] = {cp_packet7(0x66, 1), 0};
 result->phase = 4;
 int error = publish(gpu, gmu, memory, secure, 2, barrier, cookie, result);
 if (error) return error;
 volatile uint32_t *fence = (volatile uint32_t *)(memory->cpu + A618_CP_FENCE_OFFSET);
 fence[0] = fence[1] = 0;
 barrier(cookie);
 uint32_t words[9];
 struct a618_commands commands = {words, 9, 0};
 error = a618_emit_memory_write(&commands, A618_CP_FENCE_IOVA + 4, 0x0618c0de);
 if (!error) error = a618_emit_fence(&commands, A618_CP_FENCE_IOVA, 1);
 if (error) return error;
 result->phase = 5;
 error = publish(gpu, gmu, memory, words, commands.used, barrier, cookie, result);
 if (error) return error;
 /* A drained ring need not imply an AXI write is visible to the CPU yet. */
 for (unsigned us = 0; us < 10000; us += 10) {
  barrier(cookie);
  result->fence = fence[0]; result->value = fence[1];
  if (result->fence == 1 && result->value == 0x0618c0de) break;
  if (cp_fault(gpu, gmu, result)) { result->poisoned = 1; return EIO; }
  gpu->delay_us(gpu->cookie, 10);
 }
 if (result->fence != 1 || result->value != 0x0618c0de) {
  result->poisoned = 1; return ETIMEDOUT;
 }
 result->phase = 6;
 /* Keep GPU_SET held: fault recovery and runtime power management are not
  * implemented. No iOS accelerator is registered by a self-test completion. */
 return 0;
}

/* ================================ orchestration (stages 7 and 8, as sweet_gpu.c of 0098) ================================ */

static uint32_t
io32_read(void *cookie, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)((vm_offset_t)cookie + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
io32_write(void *cookie, uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)((vm_offset_t)cookie + offset) = value;
	__asm__ volatile("dsb oshst" ::: "memory");
	(void)io32_read(cookie, offset);
}

static uint64_t
io64_read(void *cookie, uint32_t offset)
{
	uint64_t value = *(volatile uint64_t *)((vm_offset_t)cookie + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
io64_write(void *cookie, uint32_t offset, uint64_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint64_t *)((vm_offset_t)cookie + offset) = value;
	__asm__ volatile("dsb oshst" ::: "memory");
}

static void
io_barrier(void *cookie)
{
	(void)cookie;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
io_delay(void *cookie, unsigned usec)
{
	(void)cookie;
	delay(usec);
}

static uint32_t
cp_crc32(const uint8_t *p, unsigned n)
{
	uint32_t crc = 0xffffffffu;

	for (unsigned i = 0; i < n; ++i) {
		crc ^= p[i];
		for (unsigned b = 0; b < 8; ++b) {
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
		}
	}
	return ~crc;
}

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static int
cp_fw(const char *name, unsigned expect, const uint8_t **out)
{
	uint64_t size = 0;
	int disk = -1;

	*out = hakios_image_file("fw", name, &size, &disk);
	if (*out == NULL || size != expect) {
		printf("SWEET ADRENO: fw/%s: missing or wrong size (%llu, want %u)\n", name, size, expect);
		return ENOENT;
	}
	printf("SWEET ADRENO: fw/%s: %llu bytes CRC32=%#x\n", name, size, cp_crc32(*out, (unsigned)size));
	return 0;
}

struct cp_zap_request {
	volatile uint32_t *reserved;
	uint64_t physical;
	const struct a618_dma_region *metadata;
	const unsigned char *mdt, *code;
	unsigned mdt_bytes, code_bytes;
	struct a618_pas_result result;
};

static int
cp_zap_prepare(void *cookie)
{
	struct cp_zap_request *request = cookie;

	return a618_zap_authenticate(request->reserved, request->physical, request->metadata, request->mdt,
	    request->mdt_bytes, request->code, request->code_bytes, &request->result);
}

/* Stage 9, after the GMU is up: the SQE, the CP's own SMMU context, the signed zap shader, ME_INIT and
 * one fence write (sweet_gpu_cp_start of patch 0106, which reached phase 6 on 2026-10-20). */
static int
sweet_gpu_cp_start(vm_offset_t gmu, vm_offset_t smmu)
{
	static struct a618_dma_region memory, metadata;
	const uint8_t *sqe, *mdt, *code;
	const uint64_t zap_base = 0x97f15000ull;
	const uint32_t zap_size = 0x2000;
	int error;

	if ((error = cp_fw("a630_sqe.fw", 32136, &sqe)) || (error = cp_fw("a615_zap.mdt", 0x1acc, &mdt)) ||
	    (error = cp_fw("a615_zap.b02", 0x7c0, &code))) {
		return error;
	}
	error = sweet_adreno_dma_allocate(&memory, A618_CP_DMA_BYTES);
	printf("SWEET ADRENO: CP DMA errno=%d PA=%#llx bytes=%u\n", error, memory.physical, memory.bytes);
	if (error) {
		return error;
	}
	error = a618_cp_stage(&memory, sqe, 32136, io_barrier, NULL);
	if (error) {
		printf("SWEET ADRENO: SQE CPU staging/readback errno=%d\n", error);
		return error;
	}
	/* Read the wired allocation back through an independent physical mapping. */
	vm_offset_t physical_image = ml_io_map(memory.physical + A618_CP_SQE_OFFSET, 0x8000);
	if (!physical_image) {
		return ENOMEM;
	}
	volatile uint32_t *staged = (volatile uint32_t *)(memory.cpu + A618_CP_SQE_OFFSET);
	unsigned verified = 0;
	for (; verified < (32136 - 4) / 4; ++verified) {
		if (io32_read((void *)physical_image, verified * 4) != staged[verified]) {
			error = EIO;
			break;
		}
	}
	printf("SWEET ADRENO: SQE physical readback errno=%d words=%u first=%#x,%#x,%#x,%#x\n", error, verified,
	    io32_read((void *)physical_image, 0), io32_read((void *)physical_image, 4),
	    io32_read((void *)physical_image, 8), io32_read((void *)physical_image, 12));
	if (error) {
		return error;
	}
	const struct a618_smmu_io smmu_io = {
		{ (void *)smmu, io32_read, io32_write, io_delay }, io64_read, io64_write, io_barrier };
	struct a618_smmu_result map;
	bzero(&map, sizeof(map));
	error = a618_smmu_prepare_cp(&smmu_io, &memory, &map);
	printf("SWEET ADRENO: CP SMMU errno=%d CB=%u slot=%u pages=%u offset=%#x value=%#x PAR=%#llx FSR=%#x\n",
	    error, map.context, map.slot, map.translated, map.offset, map.value, map.par, map.fault);
	if (error) {
		return error;
	}
	vm_offset_t core = ml_io_map(0x05000000, 0x40000);
	if (!core) {
		return ENOMEM;
	}
	const struct a618_io gpu_io = { (void *)core, io32_read, io32_write, io_delay };
	const struct a618_io gmu_io = { (void *)gmu, io32_read, io32_write, io_delay };
	error = sweet_adreno_dma_allocate(&metadata, 0x4000);
	if (error) {
		return error;
	}
	vm_offset_t zap = ml_io_map(zap_base, zap_size);
	if (!zap) {
		return ENOMEM;
	}
	struct cp_zap_request request = {
		.reserved = (volatile uint32_t *)zap, .physical = zap_base, .metadata = &metadata,
		.mdt = mdt, .code = code, .mdt_bytes = 0x1acc, .code_bytes = 0x7c0,
	};
	struct a618_cp_result cp;
	bzero(&cp, sizeof(cp));
	printf("SWEET ADRENO: starting the command processor now\n");
	error = a618_cp_start(&gpu_io, &gmu_io, &memory, sqe, 32136, io_barrier, NULL, cp_zap_prepare, &request, &cp);
	printf("SWEET ADRENO: pre-SQE ZAP attempted=%u errno=%d phase=%u status=%lld response=%#llx\n",
	    cp.secure_prepare_attempted, cp.secure_prepare_error, request.result.phase, request.result.status,
	    request.result.response);
	printf("SWEET ADRENO: CP initial TSB control=%#x base=%#llx size=%#x\n", cp.initial_tsb_control,
	    cp.initial_tsb_base, cp.initial_tsb_size);
	printf("SWEET ADRENO: CP aperture errno=%d phase=%u status=%lld response=%#llx\n", cp.aperture_error,
	    cp.aperture_phase, cp.aperture_status, cp.aperture_response);
	printf("SWEET ADRENO: SQE ME_INIT errno=%d phase=%u submitted=%u completed=%u RPTR=%#x WPTR=%#x status=%#x IRQ=%#x fault=%#x\n",
	    error, cp.phase, cp.submitted, cp.completed, cp.rptr, cp.wptr, cp.status, cp.interrupt, cp.hw_fault);
	printf("SWEET ADRENO: CP startup IRQ power=%#x before_auth=%#x after_auth=%#x bootstrap_us=%u CP2GMU=%#x\n",
	    cp.irq_after_power, cp.irq_before_auth, cp.irq_after_auth, cp.bootstrap_wait_us,
	    io32_read((void *)core, 0x2048));
	printf("SWEET ADRENO: CP startup initial SQECTL=%#x INSTR=%#x%08x RB_CNTL=%#x snapshots=%u first_fault=%s\n",
	    cp.initial_sqectl, cp.initial_instr_hi, cp.initial_instr_lo, cp.initial_rb_cntl, cp.snapshots,
	    cp.first_fault ? cp.first_fault : "none");
	for (unsigned i = 0; i < cp.snapshots; ++i) {
		printf("SWEET ADRENO: CP SNAP[%s] IRQ=%#x SQECTL=%#x INSTR=%#x%08x CNTL=%#x CP2GMU=%#x\n",
		    cp.snapshot[i].tag, cp.snapshot[i].irq, cp.snapshot[i].sqectl, cp.snapshot[i].instr_hi,
		    cp.snapshot[i].instr_lo, cp.snapshot[i].rb_cntl, cp.snapshot[i].cp2gmu);
	}
	printf("SWEET ADRENO: CP execution status local_irq=%#x sqe_started=%u\n", cp.cp_interrupt, cp.sqe_started);
	printf("SWEET ADRENO: CP MMU CB0FSR=%#x CB1FSR=%#x\n", io32_read((void *)smmu, 0x8058),
	    io32_read((void *)smmu, 0x9058));
	printf("SWEET ADRENO: CP fault FAR=%#llx FSYNR0=%#x FSYNR1=%#x SID=%#x TTBR0=%#llx TCR=%#x SCTLR=%#x\n",
	    io64_read((void *)smmu, 0x8060), io32_read((void *)smmu, 0x8068), io32_read((void *)smmu, 0x806c),
	    io32_read((void *)smmu, 0x1400), io64_read((void *)smmu, 0x8020), io32_read((void *)smmu, 0x8030),
	    io32_read((void *)smmu, 0x8000));
	if (error && (cp.sqe_started || cp.first_fault)) {
		uint32_t state[51];

		io32_write((void *)core, 0x2420, 0);
		for (unsigned i = 0; i < 51; ++i) {
			state[i] = io32_read((void *)core, 0x2424);
		}
		for (unsigned i = 0; i < 51; i += 3) {
			printf("SWEET ADRENO: SQE_STAT[%u]=%#x,%#x,%#x\n", i, state[i], state[i + 1], state[i + 2]);
		}
		printf("SWEET ADRENO: CP hardware RPTR=%#x STATUS1=%#x INTERRUPT=%#x\n", io32_read((void *)core, 0x2018),
		    io32_read((void *)core, 0x2094), io32_read((void *)core, 0x208c));
	}
	if (error) {
		return error;
	}
	error = a618_cp_leave_secure(&gpu_io, &gmu_io, &memory, io_barrier, NULL, &cp);
	printf("SWEET ADRENO: CP fence errno=%d phase=%u submitted=%u completed=%u RPTR=%#x WPTR=%#x IRQ=%#x fault=%#x fence=%#x value=%#x\n",
	    error, cp.phase, cp.submitted, cp.completed, cp.rptr, cp.wptr, cp.interrupt, cp.hw_fault, cp.fence,
	    cp.value);
	printf("SWEET ADRENO: CP bootstrap complete errno=%d; no renderer, GPU acceleration not established\n", error);
	return error;
}

int
sweet_gpu_boot(unsigned stage, vm_offset_t gpucc, vm_offset_t gmu_base, vm_offset_t smmu_base)
{
	static struct a618_dma_region memory;
	static struct a618_hfi hfi;
	const struct a618_io gmu_io = { (void *)gmu_base, io32_read, io32_write, io_delay };
	struct a618_smmu_result result;
	int error;

	(void)sweet_rpmh_arc_probe("xo.lvl");
	/* Still no M3 release: verify the actual hardware translations first. */
	if (io32_read((void *)gmu_base, A618_GMU_SYSRESET) != 1 ||
	    !(io32_read((void *)gpucc, 0x100c) & 1u) || !(io32_read((void *)gpucc, 0x1508) & 1u)) {
		printf("SWEET ADRENO: DMA precondition failed: require M3 reset, GX collapse/clamp\n");
		return EBUSY;
	}
	error = sweet_adreno_dma_allocate(&memory, A618_DMA_BYTES);
	printf("SWEET ADRENO: DMA allocate errno=%d bytes=%u PA=%#llx uncached=%u\n",
	    error, memory.bytes, memory.physical, memory.uncached);
	if (error) {
		return error;
	}
	error = a618_hfi_init(&hfi, &memory, io_barrier, NULL);
	printf("SWEET ADRENO: legacy HFI table errno=%d IOVA=%#x queues=3; firmware not executing\n",
	    error, A618_HFI_IOVA);
	if (error) {
		return error;
	}
	const struct a618_smmu_io ops = {
		{ (void *)smmu_base, io32_read, io32_write, io_delay }, io64_read, io64_write, io_barrier };
	bzero(&result, sizeof(result));
	error = a618_smmu_prepare(&ops, &memory, &result);
	printf("SWEET ADRENO: SMMU map errno=%d CB=%u slot=%u translated=%u offset=%#x value=%#x PAR=%#llx FSR=%#x\n",
	    error, result.context, result.slot, result.translated, result.offset, result.value, result.par, result.fault);
	printf("SWEET ADRENO: SMMU mapping check complete; SYSRESET=%#x; no firmware execution\n",
	    io32_read((void *)gmu_base, A618_GMU_SYSRESET));
	if (error) {
		return error;
	}
	if (stage < 8) {
		return 0;
	}
	printf("SWEET ADRENO: GPU context baseline SCTLR0=%#x\n", io32_read((void *)smmu_base, 0x8000));
	for (unsigned slot = 0; slot < 5; ++slot) {
		printf("SWEET ADRENO: stream slot=%u SMR=%#x S2CR=%#x\n", slot,
		    io32_read((void *)smmu_base, 0x800 + slot * 4), io32_read((void *)smmu_base, 0xc00 + slot * 4));
	}
	uint32_t gpu_vote = 0, gmu_vote = 0;
	error = sweet_rpmh_adreno_votes(&gpu_vote, &gmu_vote);
	printf("SWEET ADRENO: MIN_SVS votes errno=%d GPU=%#x GMU=%#x\n", error, gpu_vote, gmu_vote);
	if (error) {
		return error;
	}
	/* PDC apertures from the pinned SM7150 DT, shared by the live A618. */
	vm_offset_t pdc = ml_io_map(0x0b290000, 0x10000);
	vm_offset_t seq = ml_io_map(0x0b490000, 0x10000);
	if (!pdc || !seq) {
		return ENOMEM;
	}
	const struct a618_io pdc_io = { (void *)pdc, io32_read, io32_write, io_delay };
	const struct a618_io seq_io = { (void *)seq, io32_read, io32_write, io_delay };
	struct a618_gmu_boot_result boot;
	bzero(&boot, sizeof(boot));
	printf("SWEET ADRENO: releasing the GMU processor now\n");
	error = a618_gmu_boot_legacy(&gmu_io, &pdc_io, &seq_io, &hfi, gpu_vote, gmu_vote, &boot);
	printf("SWEET ADRENO: legacy GMU boot errno=%d phase=%u offset=%#x value=%#x FW=%#x; CP unavailable\n",
	    error, boot.phase, boot.offset, boot.value, boot.firmware_version);
	printf("SWEET ADRENO: HFI request=%#x response=%#x fw_error=%#x packets=%u INFO=%#x HOST_RAW=%#x CB1FSR=%#x\n",
	    hfi.last_request, hfi.last_response, hfi.firmware_error, hfi.responses,
	    io32_read((void *)gmu_base, 0x14648), io32_read((void *)gmu_base, 0x14658),
	    io32_read((void *)smmu_base, 0x9058));
	io_barrier(NULL);
	for (unsigned q = 0; q < 3; ++q) {
		volatile uint32_t *qh = hfi.table + 6 + q * 12;

		printf("SWEET ADRENO: HFI queue=%u status=%#x dropped=%u RX=%u TX=%u RPTR=%u WPTR=%u\n",
		    q, qh[0], qh[5], qh[8], qh[9], qh[10], qh[11]);
	}
	if (!error && stage >= 9) {
		error = sweet_gpu_cp_start(gmu_base, smmu_base);
	}
	return error;
}
#endif /* __arm64__ */
