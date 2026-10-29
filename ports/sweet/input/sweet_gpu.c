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
 * No GMU, SMMU, GX or command submission at any stage. Shared rails are never lowered.
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
void sweet_gpu_probe(void);

/* LOW_SVS is the first safe GPU corner; the write only raises gfx.lvl. */
#define SWEET_GPU_GFX_TARGET 65u

#define SWEET_GPUCC_PA       0x05090000u
#define SWEET_GPUCC_SIZE     0x00009000u
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
	if (enabled < 1 || enabled > 4) {
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
		printf("SWEET GPU: read-only audit complete; sweetgpu=2 reads ARC resources, 3 raises gfx.lvl, 4 powers CX\n");
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
