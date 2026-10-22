/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ios-sweet contributors
 *
 * SM7150 clock and firmware-staging implementation using public Qualcomm
 * interfaces. Source references: docs/adreno-port.md (postmarketOS section).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#if defined(KERNEL)
#include <sys/errno.h>
#else
#include <errno.h>
#endif


static int
legacy_clock_io_valid(const struct a618_io *io)
{
	return io && io->read && io->write && io->delay_us;
}

static int
legacy_clock_read_checked(const struct a618_io *io, unsigned block, uint32_t offset,
    struct a618_clock_result *result)
{
	result->block = block;
	result->offset = offset;
	result->value = io->read(io->cookie, offset);
	return result->value == 0xffffffffu ? EIO : 0;
}

static int
legacy_clock_update(const struct a618_io *io, unsigned block, uint32_t offset,
    uint32_t mask, uint32_t value, struct a618_clock_result *result)
{
	int error = legacy_clock_read_checked(io, block, offset, result);
	if (error)
		return error;
	io->write(io->cookie, offset, (result->value & ~mask) | value);
	error = legacy_clock_read_checked(io, block, offset, result);
	if (error)
		return error;
	return (result->value & mask) == value ? 0 : EIO;
}

static int
legacy_clock_poll(const struct a618_io *io, unsigned block, uint32_t offset,
    uint32_t mask, uint32_t expected, struct a618_clock_result *result)
{
	for (unsigned elapsed = 0; elapsed < 500; ++elapsed) {
		int error = legacy_clock_read_checked(io, block, offset, result);
		if (error)
			return error;
		if ((result->value & mask) == expected)
			return 0;
		io->delay_us(io->cookie, 1);
	}
	return ETIMEDOUT;
}

static int
legacy_clock_branch(const struct a618_io *io, unsigned block, uint32_t offset,
    struct a618_clock_result *result)
{
	int error = legacy_clock_update(io, block, offset, 1, 1, result);
	if (error)
		return error;
	/* A running NOC FSM may report bit31 set; match Linux branch2 semantics. */
	for (unsigned elapsed = 0; elapsed < 500; ++elapsed) {
		error = legacy_clock_read_checked(io, block, offset, result);
		if (error)
			return error;
		if (!(result->value & (1u << 31)) ||
		    ((result->value >> 28) & 7u) == 2) {
			++result->completed;
			return 0;
		}
		io->delay_us(io->cookie, 1);
	}
	return ETIMEDOUT;
}

int
a618_enable_gmu_clocks(const struct a618_io *gpucc,
    const struct a618_io *gcc, struct a618_clock_result *result)
{
	int error;
	if (!result || !legacy_clock_io_valid(gpucc) || !legacy_clock_io_valid(gcc))
		return EINVAL;
	result->block = result->completed = 0;
	result->offset = result->value = 0;
	/* CX power and software control must still be established. */
	error = legacy_clock_read_checked(gpucc, 0, 0x106c, result);
	if (error)
		return error;
	if (result->value & 3u)
		return EAGAIN;
	error = legacy_clock_read_checked(gpucc, 0, 0x1540, result);
	if (error)
		return error;
	if (!(result->value & (1u << 31)))
		return EAGAIN;

	/* GPLL0 is a shared, fixed FABIA PLL. Check 19.2 MHz * 31.25 = 600 MHz
	 * before voting its output. Never reprogram this system PLL.
	 */
	error = legacy_clock_read_checked(gcc, 1, 0x4, result);
	if (error)
		return error;
	if (result->value != 31)
		return ERANGE;
	error = legacy_clock_read_checked(gcc, 1, 0x38, result);
	if (error)
		return error;
	if (result->value != 0x4000)
		return ERANGE;
	error = legacy_clock_read_checked(gcc, 1, 0, result);
	if (error)
		return error;
	if (result->value & (1u << 20)) {
		/* FABIA in vote-FSM mode: Linux waits for ACTIVE, not LOCK_DET. */
		error = legacy_clock_update(gcc, 1, 0x52000, 1, 1, result);
		if (!error)
			error = legacy_clock_poll(gcc, 1, 0, 1u << 30, 1u << 30, result);
	} else {
		/* Preserve an already running direct-control PLL. Its rate was
		 * checked above; do not change MODE/OPMODE or pretend a vote
		 * can start a PLL which is not under vote-FSM control.
		 */
		if ((result->value & ((1u << 31) | 1u)) != ((1u << 31) | 1u))
			return EBUSY;
		error = legacy_clock_read_checked(gcc, 1, 0x2c, result);
		if (!error && result->value != 1)
			error = EBUSY;
	}
	if (error)
		return error;
	/* GCC_GPU_GPLL0_DIV_CLK_SRC: fixed divide by two, 300 MHz. */
	error = legacy_clock_update(gcc, 1, 0x52004, 1u << 16, 1u << 16, result);
	if (error)
		return error;
	gcc->delay_us(gcc->cookie, 10);
	/* Bus clocks named by the postmarketOS GMU and SMMU DT nodes. */
	static const uint32_t bus[] = { 0x4452c, 0x7100c, 0x71018 };
	for (unsigned i = 0; i < sizeof(bus) / sizeof(bus[0]); ++i) {
		error = legacy_clock_branch(gcc, 1, bus[i], result);
		if (error)
			return error;
	}
	/* CXO and AHB first; no GX clock or PLL0/PLL1 register is touched. */
	error = legacy_clock_branch(gpucc, 0, 0x1004, result);
	if (!error)
		error = legacy_clock_branch(gpucc, 0, 0x109c, result);
	if (!error)
		error = legacy_clock_branch(gpucc, 0, 0x1078, result);
	if (!error)
		error = legacy_clock_branch(gpucc, 0, 0x108c, result);
	if (error)
		return error;
	/* RCG2 force-enable while selecting/dividing the existing GCC parent. */
	error = legacy_clock_update(gpucc, 0, 0x1120, 2, 2, result);
	if (error)
		return error;
	error = legacy_clock_poll(gpucc, 0, 0x1120, 1u << 31, 0, result);
	if (error)
		return error;
	/* SRC_SEL=6, HID_DIV=2: 300 MHz * 2 / (2+1) = 200 MHz. */
	error = legacy_clock_update(gpucc, 0, 0x1124, 0x10371fu, 0x602, result);
	if (error)
		return error;
	/* UPDATE is self-clearing; do not require immediate readback of bit0. */
	error = legacy_clock_read_checked(gpucc, 0, 0x1120, result);
	if (error)
		return error;
	gpucc->write(gpucc->cookie, 0x1120, result->value | 1u);
	error = legacy_clock_poll(gpucc, 0, 0x1120, 1, 0, result);
	if (error)
		return error;
	error = legacy_clock_read_checked(gpucc, 0, 0x1124, result);
	if (error)
		return error;
	if ((result->value & 0x10371fu) != 0x602)
		return EIO;
	/* Source probe recommends all four WAKE/SLEEP bits before GMU enable. */
	error = legacy_clock_update(gpucc, 0, 0x1098, 0xff0, 0xff0, result);
	if (!error)
		error = legacy_clock_branch(gpucc, 0, 0x1098, result);
	/* Leave the root held on for staging; shutdown belongs to the runtime
	 * state machine once firmware and SMMU execution are implemented.
	 */
	return error;
}

#endif
