/*
 * SWEET Adreno 618 bring-up, stages 1 to 4 of the original port (DC28088/ios-sweet
 * fe987bf05e0d, patches 0081-0088; docs/GPU_PROGRESS.md there). Stage 4 (CX power)
 * passed on the phone on 2026-10-19; nothing beyond it is carried over yet.
 *
 * Boot-arg sweetgpu=N, together with sweet=1 (the HakiOS Adreno driver, gpu=1, is the
 * A642L's and is not used here):
 *   1  reads GPUCC and the RPMh command DB tables only
 *   2  adds RPMh ARC READ transactions (mx.lvl, cx.lvl, gfx.lvl), 60 s after boot
 *   3  additionally raises only gfx.lvl to LOW_SVS and verifies the readback
 *   4  requires mx.lvl and cx.lvl at LOW_SVS or more, then powers the CX GDSC
 *   5  after 4: enables the GMU and SMMU clocks (GCC GPLL0 vote, bus branches, GPUCC root at
 *      200 MHz), then reads GMU status and the SMMU ID registers
 *   6  after 5: stages a618_gmu.bin (fw/ of the small image) into the GMU's instruction TCM and
 *      reads it back; the GMU's processor stays in reset
 *   7  after 6: wired uncached memory and an SMMU context for the GMU, verified by hardware
 *      translation probes (sweet_gpu_boot.c); the GMU's processor stays in reset
 *   8  after 7: releases the GMU's processor and runs its cold boot and HFI handshake
 * No GX power by the host, GPU command processor or command submission at any stage (stages 5-8
 * are ios-sweet patches 0089-0098). Shared rails are never lowered.
 */
#if defined(__arm64__)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

/* Mirror board diagnostics into the resident loader live log. */
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
#define printf hakios_tl

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int sweet_rpmh_arc_probe(const char *res);
extern int sweet_rpmh_arc_read(const char *res);
extern int sweet_rpmh_arc_require(const char *res, unsigned minimum);
extern int sweet_rpmh_arc_raise(const char *res, unsigned target_vlvl);
extern kern_return_t thread_terminate(thread_t);
extern int sweet_gpu_boot(unsigned stage, vm_offset_t gpucc, vm_offset_t gmu_base, vm_offset_t smmu_base);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
void sweet_gpu_probe(void);

/* LOW_SVS is the first safe GPU corner; the write only raises gfx.lvl. */
#define SWEET_GPU_GFX_TARGET 65u

#define SWEET_GPUCC_PA       0x05090000u
#define SWEET_GPUCC_SIZE     0x00009000u
#define SWEET_GCC_PA         0x00100000u
#define SWEET_GCC_SIZE       0x00072000u
#define SWEET_GMU_PA         0x0506a000u
#define SWEET_GMU_SIZE       0x00031000u
#define SWEET_SMMU_PA        0x05040000u
#define SWEET_SMMU_SIZE      0x00010000u
#define A618_GMU_SYSRESET    0x14000u
#define A618_GMU_POWER_STATUS 0x14340u
#define A618_GMU_AHB_FENCE_STATUS 0x24c4cu
#define A618_GMU_ITCM        0x3000u
/* CRC32 of the phone's a618_gmu.bin (SHA256 9ef10b2b...bf2e), as the original verified it. */
#define A618_GMU_FW_CRC32    0x574c0d4bu
#define GPU_CC_GX_BCR        0x1008u
#define GPU_CC_GX_GDSCR      0x100cu
#define GPU_CC_CX_GDSCR      0x106cu
#define GPU_CC_GX_DOMAIN     0x1508u
#define GPU_CC_CX_HW_CTRL    0x1540u
#define GDSC_PWR_ON          (1u << 31)
#define GDSC_SW_COLLAPSE     (1u << 0)
#define GDSC_HW_CONTROL      (1u << 1)
#define BRANCH_CLK_OFF       (1u << 31)
#define BRANCH_CLK_ENABLE    (1u << 0)

static vm_offset_t gpucc;
static int gpu_probe_attempted;
static int gpu_stage;
static int gpu_sleep_chan;

/* All registers below are in GPUCC; none is in the Adreno/GMU aperture. */
static const struct {
	const char *name;
	uint32_t offset;
} gpu_branches[] = {
	{ "AHB",          0x1078u },
	{ "CX_APB",       0x1088u },
	{ "CX_SNOC_DVM",  0x108cu },
	{ "CX_GMU",       0x1098u },
	{ "CXO",          0x109cu },
	{ "CX_GFX3D",     0x10a4u },
	{ "CX_GFX3D_SLV", 0x10a8u },
	{ "GX_GFX3D",     0x1054u },
	{ "GX_CXO",       0x1060u },
	{ "GX_GMU",       0x1064u },
};

static uint32_t
gpucc_read(uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(gpucc + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
gpucc_write(uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(gpucc + offset) = value;
	__asm__ volatile("dsb oshst" ::: "memory");
	(void)gpucc_read(offset);
}

/*
 * The CX GDSC, as downstream gdsc_enable() does it. Does not reset an already powered
 * domain; never powers one down after an uncertain completion.
 */
static int
gpu_enable_cx(uint32_t *control_out, uint32_t *status_out, unsigned *waited_out)
{
	uint32_t control = gpucc_read(GPU_CC_CX_GDSCR);
	uint32_t status = gpucc_read(GPU_CC_CX_HW_CTRL);

	*control_out = control;
	*status_out = status;
	*waited_out = 0;
	if (control == 0xffffffffu || status == 0xffffffffu) {
		return EIO;
	}
	if (control & GDSC_HW_CONTROL) {
		return EBUSY;
	}
	if (!(control & GDSC_SW_COLLAPSE) && (status & GDSC_PWR_ON)) {
		return 0;
	}
	/* Live DT: qcom,clk-dis-wait-val = 8; preserve all other fields. */
	control = (control & ~(0xfu << 12)) | (8u << 12);
	gpucc_write(GPU_CC_CX_GDSCR, control & ~GDSC_SW_COLLAPSE);
	delay(1); /* gdsc_enable: at least eight XO cycles before polling */
	for (unsigned elapsed = 0; elapsed < 500; ++elapsed) {
		*waited_out = elapsed + 1;
		status = *status_out = gpucc_read(GPU_CC_CX_HW_CTRL);
		if (status == 0xffffffffu) {
			return EIO;
		}
		if (status & GDSC_PWR_ON) {
			delay(2); /* memory power-up settling */
			*control_out = gpucc_read(GPU_CC_CX_GDSCR);
			if (*control_out == 0xffffffffu ||
			    (*control_out & (GDSC_SW_COLLAPSE | GDSC_HW_CONTROL))) {
				return EIO;
			}
			return 0;
		}
		delay(1);
	}
	*control_out = gpucc_read(GPU_CC_CX_GDSCR);
	return ETIMEDOUT;
}

/* An aperture and what the stage 5/6 helpers do to it (ios-sweet sweet_adreno_clock.c, 0091). */
struct a618_io {
	vm_offset_t base;
};
struct a618_clock_result {
	unsigned block;
	uint32_t offset, value;
	unsigned completed;
};

static uint32_t
io_read(const struct a618_io *io, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(io->base + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
io_write(const struct a618_io *io, uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(io->base + offset) = value;
	__asm__ volatile("dsb oshst" ::: "memory");
	(void)io_read(io, offset);
}

static int
clk_read(const struct a618_io *io, unsigned block, uint32_t offset, struct a618_clock_result *result)
{
	result->block = block;
	result->offset = offset;
	result->value = io_read(io, offset);
	return result->value == 0xffffffffu ? EIO : 0;
}

static int
clk_update(const struct a618_io *io, unsigned block, uint32_t offset, uint32_t mask, uint32_t value,
    struct a618_clock_result *result)
{
	int error = clk_read(io, block, offset, result);
	if (error) {
		return error;
	}
	io_write(io, offset, (result->value & ~mask) | value);
	error = clk_read(io, block, offset, result);
	if (error) {
		return error;
	}
	return (result->value & mask) == value ? 0 : EIO;
}

static int
clk_poll(const struct a618_io *io, unsigned block, uint32_t offset, uint32_t mask, uint32_t expected,
    struct a618_clock_result *result)
{
	for (unsigned elapsed = 0; elapsed < 500; ++elapsed) {
		int error = clk_read(io, block, offset, result);
		if (error) {
			return error;
		}
		if ((result->value & mask) == expected) {
			return 0;
		}
		delay(1);
	}
	return ETIMEDOUT;
}

static int
clk_branch(const struct a618_io *io, unsigned block, uint32_t offset, struct a618_clock_result *result)
{
	int error = clk_update(io, block, offset, 1, 1, result);
	if (error) {
		return error;
	}
	/* A running NOC FSM may report bit31 set; match Linux branch2 semantics. */
	for (unsigned elapsed = 0; elapsed < 500; ++elapsed) {
		error = clk_read(io, block, offset, result);
		if (error) {
			return error;
		}
		if (!(result->value & (1u << 31)) || ((result->value >> 28) & 7u) == 2) {
			++result->completed;
			return 0;
		}
		delay(1);
	}
	return ETIMEDOUT;
}

static int
gpu_enable_gmu_clocks(const struct a618_io *gpucc_io, const struct a618_io *gcc, struct a618_clock_result *result)
{
	int error;
	static const uint32_t bus[] = { 0x4452c, 0x7100c, 0x71018 };

	result->block = result->completed = 0;
	result->offset = result->value = 0;
	/* CX power and software control must still be established. */
	error = clk_read(gpucc_io, 0, 0x106c, result);
	if (error) {
		return error;
	}
	if (result->value & 3u) {
		return EAGAIN;
	}
	error = clk_read(gpucc_io, 0, 0x1540, result);
	if (error) {
		return error;
	}
	if (!(result->value & (1u << 31))) {
		return EAGAIN;
	}
	/* GPLL0 is a shared, fixed FABIA PLL. Check 19.2 MHz * 31.25 = 600 MHz before voting its
	 * output. Never reprogram this system PLL. */
	error = clk_read(gcc, 1, 0x4, result);
	if (error) {
		return error;
	}
	if (result->value != 31) {
		return ERANGE;
	}
	error = clk_read(gcc, 1, 0x38, result);
	if (error) {
		return error;
	}
	if (result->value != 0x4000) {
		return ERANGE;
	}
	error = clk_read(gcc, 1, 0, result);
	if (error) {
		return error;
	}
	if (result->value & (1u << 20)) {
		/* FABIA in vote-FSM mode: Linux waits for ACTIVE, not LOCK_DET. */
		error = clk_update(gcc, 1, 0x52000, 1, 1, result);
		if (!error) {
			error = clk_poll(gcc, 1, 0, 1u << 30, 1u << 30, result);
		}
	} else {
		/* Preserve an already running direct-control PLL; its rate was checked above. */
		if ((result->value & ((1u << 31) | 1u)) != ((1u << 31) | 1u)) {
			return EBUSY;
		}
		error = clk_read(gcc, 1, 0x2c, result);
		if (!error && result->value != 1) {
			error = EBUSY;
		}
	}
	if (error) {
		return error;
	}
	/* GCC_GPU_GPLL0_DIV_CLK_SRC: fixed divide by two, 300 MHz. */
	error = clk_update(gcc, 1, 0x52004, 1u << 16, 1u << 16, result);
	if (error) {
		return error;
	}
	delay(10);
	/* Bus clocks named by the GMU and SMMU DT nodes. */
	for (unsigned i = 0; i < sizeof(bus) / sizeof(bus[0]); ++i) {
		error = clk_branch(gcc, 1, bus[i], result);
		if (error) {
			return error;
		}
	}
	/* The GMU requests CXO; its SMMU requests GPUCC AHB and GCC GPU_SNOC_DVM_GFX. No GX clock or
	 * GPUCC PLL register is touched. */
	error = clk_branch(gpucc_io, 0, 0x109c, result);
	if (!error) {
		error = clk_branch(gpucc_io, 0, 0x1078, result);
	}
	if (error) {
		return error;
	}
	/* RCG2 force-enable while selecting/dividing the existing GCC parent. */
	error = clk_update(gpucc_io, 0, 0x1120, 2, 2, result);
	if (error) {
		return error;
	}
	error = clk_poll(gpucc_io, 0, 0x1120, 1u << 31, 0, result);
	if (error) {
		return error;
	}
	/* SRC_SEL=6, HID_DIV=2: 300 MHz * 2 / (2+1) = 200 MHz. */
	error = clk_update(gpucc_io, 0, 0x1124, 0x10371fu, 0x602, result);
	if (error) {
		return error;
	}
	/* UPDATE is self-clearing; do not require immediate readback of bit0. */
	error = clk_read(gpucc_io, 0, 0x1120, result);
	if (error) {
		return error;
	}
	io_write(gpucc_io, 0x1120, result->value | 1u);
	error = clk_poll(gpucc_io, 0, 0x1120, 1, 0, result);
	if (error) {
		return error;
	}
	error = clk_read(gpucc_io, 0, 0x1124, result);
	if (error) {
		return error;
	}
	if ((result->value & 0x10371fu) != 0x602) {
		return EIO;
	}
	/* All four WAKE/SLEEP bits before the GMU clock is enabled. */
	error = clk_update(gpucc_io, 0, 0x1098, 0xff0, 0xff0, result);
	if (!error) {
		error = clk_branch(gpucc_io, 0, 0x1098, result);
	}
	/* The root stays on for staging; shutdown belongs to the runtime state machine. */
	return error;
}

static uint32_t
little_word(const unsigned char *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* The GMU's firmware into its instruction TCM, read back word by word; its processor stays in reset. */
static int
gpu_gmu_stage(const struct a618_io *gmu, const unsigned char *firmware, unsigned size, uint32_t expected_crc,
    unsigned *verified)
{
	uint32_t crc = 0xffffffffu, sp, pc, reset;

	*verified = 0;
	if (!firmware || size != 0x8000) {
		return EINVAL;
	}
	/* Validate all data before issuing any MMIO. */
	sp = little_word(firmware);
	pc = little_word(firmware + 4);
	if ((sp & 7) || sp < 0x40000 || sp > 0x44000 || !(pc & 1) || (pc & ~1u) >= size) {
		return EINVAL;
	}
	for (unsigned i = 0; i < size; ++i) {
		crc ^= firmware[i];
		for (unsigned bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
		}
	}
	if ((crc ^ 0xffffffffu) != expected_crc) {
		return EIO;
	}
	reset = io_read(gmu, A618_GMU_SYSRESET);
	if (reset == 0xffffffffu) {
		return EIO;
	}
	if (!(reset & 1)) {
		return EBUSY; /* Never overwrite a potentially running processor. */
	}
	for (unsigned offset = 0; offset < size; offset += 4) {
		uint32_t word = little_word(firmware + offset);

		io_write(gmu, A618_GMU_ITCM + offset, word);
		if (io_read(gmu, A618_GMU_ITCM + offset) != word) {
			return EIO;
		}
		*verified += 4;
	}
	reset = io_read(gmu, A618_GMU_SYSRESET);
	return reset != 0xffffffffu && (reset & 1) ? 0 : EIO;
}

/* Stage 5 (clocks, status) and 6 (firmware staging); matches the original: GMU firmware owns GX. */
static int
gpu_gmu_prepare(int stage)
{
	vm_offset_t gcc_base = ml_io_map(SWEET_GCC_PA, SWEET_GCC_SIZE);
	vm_offset_t gmu_base = ml_io_map(SWEET_GMU_PA, SWEET_GMU_SIZE);
	vm_offset_t smmu_base = ml_io_map(SWEET_SMMU_PA, SWEET_SMMU_SIZE);
	struct a618_io gpucc_io = { gpucc }, gcc_io = { gcc_base }, gmu_io = { gmu_base }, smmu_io = { smmu_base };
	struct a618_clock_result result;
	uint32_t reset, power, fence, id0, id1, id2;
	int error;

	if (!gcc_base || !gmu_base || !smmu_base) {
		return ENOMEM;
	}
	printf("SWEET ADRENO: GMU clocks starting\n");
	error = gpu_enable_gmu_clocks(&gpucc_io, &gcc_io, &result);
	printf("SWEET ADRENO: GMU clocks errno=%d block=%u offset=%#x value=%#x completed=%u\n",
	    error, result.block, result.offset, result.value, result.completed);
	if (error) {
		return error;
	}
	reset = io_read(&gmu_io, A618_GMU_SYSRESET);
	power = io_read(&gmu_io, A618_GMU_POWER_STATUS);
	fence = io_read(&gmu_io, A618_GMU_AHB_FENCE_STATUS);
	printf("SWEET ADRENO: GMU SYSRESET=%#x POWER_STATUS=%#x AHB_FENCE_STATUS=%#x\n", reset, power, fence);
	if (reset == 0xffffffffu || power == 0xffffffffu || fence == 0xffffffffu) {
		return EIO;
	}
	/* ID registers only: do not alter SMMU contexts or stream routing. */
	id0 = io_read(&smmu_io, 0x20);
	id1 = io_read(&smmu_io, 0x24);
	id2 = io_read(&smmu_io, 0x28);
	printf("SWEET ADRENO: SMMU IDR0=%#x IDR1=%#x IDR2=%#x\n", id0, id1, id2);
	if (id0 == 0xffffffffu || id1 == 0xffffffffu || id2 == 0xffffffffu || !id0) {
		return EIO;
	}
	if (stage >= 6) {
		uint64_t size = 0;
		int disk = -1;
		unsigned verified = 0;
		const uint8_t *firmware = hakios_image_file("fw", "a618_gmu.bin", &size, &disk);

		if (firmware == NULL) {
			printf("SWEET ADRENO: no fw/a618_gmu.bin in the image; no TCM write\n");
			return ENOENT;
		}
		printf("SWEET ADRENO: fw/a618_gmu.bin: %llu bytes in the image of /dev/md%d\n", size, disk);
		error = gpu_gmu_stage(&gmu_io, firmware, (unsigned)size, A618_GMU_FW_CRC32, &verified);
		printf("SWEET ADRENO: GMU TCM staging errno=%d verified_bytes=%u; M3 not released\n", error, verified);
		if (error) {
			return error;
		}
	}
	if (stage >= 7) {
		/* Stage 7 maps the GMU's shared memory; stage 8 releases the GMU (sweet_gpu_boot.c). */
		return sweet_gpu_boot((unsigned)stage, gpucc, gmu_base, smmu_base);
	}
	printf("SWEET ADRENO: GMU preparation complete; GX remains firmware controlled, no submission\n");
	return 0;
}

/* Pure metadata inspection; valid tables do not establish current votes. */
static int
gpu_arc_tables(void)
{
	static const char *const names[] = { "mx.lvl", "cx.lvl", "gfx.lvl" };
	for (unsigned i = 0; i < 3; ++i) {
		int error = sweet_rpmh_arc_probe(names[i]);
		if (error) {
			printf("SWEET GPU: ARC probe %s failed %d\n", names[i], error);
			return error;
		}
	}
	printf("SWEET GPU: ARC tables read-only complete\n");
	return 0;
}

static void
gpu_thread(void *arg, wait_result_t result)
{
	static const char *const names[] = { "mx.lvl", "cx.lvl", "gfx.lvl" };
	uint64_t deadline;
	int stage = gpu_stage;
	(void)arg;
	(void)result;

	/* Touch votes L11C at 45 s; RPMh is shared, so this waits longer. */
	clock_interval_to_deadline(60, NSEC_PER_SEC, &deadline);
	(void)assert_wait_deadline(&gpu_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
	if (stage >= 4) {
		uint32_t control, status;
		unsigned waited;
		int error = sweet_rpmh_arc_require("mx.lvl", SWEET_GPU_GFX_TARGET);
		if (!error) {
			error = sweet_rpmh_arc_require("cx.lvl", SWEET_GPU_GFX_TARGET);
		}
		if (error) {
			printf("SWEET ADRENO: CX preconditions failed errno=%d; no GPUCC write\n", error);
			goto out;
		}
		printf("SWEET ADRENO: CX power starting; no GPU core access\n");
		error = gpu_enable_cx(&control, &status, &waited);
		printf("SWEET ADRENO: CX result errno=%d control=%#x status=%#x waited_us=%u\n",
		    error, control, status, waited);
		if (!error && stage >= 5) {
			error = gpu_gmu_prepare(stage);
			printf("SWEET ADRENO: GMU stage=%d finished errno=%d; rendering unavailable\n", stage, error);
		}
		goto out;
	}
	if (stage == 3) {
		printf("SWEET GPU: ARC gfx raise starting; mx/cx read-only\n");
		for (unsigned i = 0; i < 2; ++i) {
			int error = sweet_rpmh_arc_read(names[i]);
			if (error) {
				printf("SWEET GPU: ARC gfx raise stopped at %s errno=%d; no retry\n",
				    names[i], error);
				goto out;
			}
		}
		int error = sweet_rpmh_arc_raise("gfx.lvl", SWEET_GPU_GFX_TARGET);
		if (error) {
			printf("SWEET GPU: ARC gfx raise stopped errno=%d; no retry\n", error);
			goto out;
		}
		printf("SWEET GPU: ARC gfx raise complete; GPUCC power/clock writes remain disabled\n");
		goto out;
	}
	printf("SWEET GPU: ARC readback starting; no resource votes\n");
	for (unsigned i = 0; i < 3; ++i) {
		int error = sweet_rpmh_arc_read(names[i]);
		if (error) {
			printf("SWEET GPU: ARC readback stopped at %s errno=%d; no retry\n",
			    names[i], error);
			goto out;
		}
	}
	printf("SWEET GPU: ARC readback complete; power/clock writes remain disabled\n");
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_gpu_probe(void)
{
	int enabled = 0;
	unsigned int i;

	if (gpu_probe_attempted) {
		return;
	}
	gpu_probe_attempted = 1;
	(void)PE_parse_boot_argn("sweetgpu", &enabled, sizeof(enabled));
	if (enabled < 1 || enabled > 8) {
		return;
	}
	printf("SWEET GPU: GPUCC audit at %#x; stage=%d, no GMU access\n", SWEET_GPUCC_PA, enabled);
	gpucc = ml_io_map(SWEET_GPUCC_PA, SWEET_GPUCC_SIZE);
	if (gpucc == 0) {
		printf("SWEET GPU: GPUCC mapping failed\n");
		return;
	}
	{
		uint32_t cx = gpucc_read(GPU_CC_CX_GDSCR);
		uint32_t cx_hw = gpucc_read(GPU_CC_CX_HW_CTRL);
		uint32_t gx = gpucc_read(GPU_CC_GX_GDSCR);
		uint32_t domain = gpucc_read(GPU_CC_GX_DOMAIN);
		uint32_t reset = gpucc_read(GPU_CC_GX_BCR);

		printf("SWEET GPU: CX_GDSCR=%#x CX_HW_CTRL=%#x GX_GDSCR=%#x GX_DOMAIN=%#x GX_BCR=%#x\n",
		    cx, cx_hw, gx, domain, reset);
		printf("SWEET GPU: CX pwr_on=%u collapse=%u hw_control=%u; GX bit31=%u collapse=%u hw_control=%u clamp=%u gmem_reset=%u block_reset=%u\n",
		    !!(cx_hw & GDSC_PWR_ON), !!(cx & GDSC_SW_COLLAPSE),
		    !!(cx & GDSC_HW_CONTROL), !!(gx & GDSC_PWR_ON),
		    !!(gx & GDSC_SW_COLLAPSE), !!(gx & GDSC_HW_CONTROL),
		    !!(domain & 1u), !!(domain & (1u << 4)), !!(reset & 1u));
	}
	for (i = 0; i < sizeof(gpu_branches) / sizeof(gpu_branches[0]); i++) {
		uint32_t value = gpucc_read(gpu_branches[i].offset);
		printf("SWEET GPU: CBCR %s @%#x=%#x enable=%u clk_off=%u fsm=%u\n",
		    gpu_branches[i].name, gpu_branches[i].offset, value,
		    !!(value & BRANCH_CLK_ENABLE), !!(value & BRANCH_CLK_OFF), (value >> 28) & 7u);
	}
	if (gpu_arc_tables()) {
		printf("SWEET GPU: ARC stage stopped; no GPUCC power/clock/reset writes\n");
		return;
	}
	if (enabled == 1) {
		printf("SWEET GPU: read-only audit complete; sweetgpu=2 reads ARC resources, 3 raises gfx.lvl, 4 powers CX, 5 GMU clocks, 6 firmware staging\n");
		return;
	}
	gpu_stage = enabled;
	thread_t thread;
	kern_return_t kr = kernel_thread_start(gpu_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
		printf("SWEET GPU: stage %d scheduled in 60 s\n", enabled);
	} else {
		printf("SWEET GPU: worker thread failed %d\n", kr);
	}
}
#endif /* __arm64__ */
