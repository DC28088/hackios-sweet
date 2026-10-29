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
 * No GPU core, command processor, ring, zap shader or command at any stage.
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
static int dma_allocated;
static int
sweet_adreno_dma_allocate(struct a618_dma_region *region)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	if (!region) {
		return EINVAL;
	}
	if (dma_allocated) {
		return EALREADY;
	}
	if (kmem_alloc_contig(kernel_map, &cached, A618_DMA_BYTES, 0x3fff, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return ENOMEM;
	}
	physical = kvtophys(cached);
	if (!physical || (physical & 0x3fff) || physical > (1ull << 36) - A618_DMA_BYTES) {
		return ERANGE;
	}
	for (vm_offset_t line = cached; line < cached + A618_DMA_BYTES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	uncached = ml_io_map_unmappable(physical, A618_DMA_BYTES, VM_WIMG_WCOMB);
	if (uncached == 0) {
		return ENOMEM;
	}
	dma_allocated = 1;
	region->cpu = (unsigned char *)uncached;
	region->physical = physical;
	region->bytes = A618_DMA_BYTES;
	region->uncached = 1;
	/* Naturally aligned volatile word stores, as the original: no DC ZVA or vector stores. */
	volatile uint32_t *words = (volatile uint32_t *)uncached;
	for (unsigned i = 0; i < A618_DMA_BYTES / 4; ++i) {
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
	error = sweet_adreno_dma_allocate(&memory);
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
	return error;
}
#endif /* __arm64__ */
