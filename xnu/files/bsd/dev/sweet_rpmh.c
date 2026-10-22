/*
 * SWEET PM6150L L11C bring-up; see report.md for DT and source evidence.
 * Sole XNU apps-DRV owner, called from thread context with deep idle disabled.
 * No sleep/wake programming, SPMI writes, GIC handler, or timeout recovery.
 */
#if 1 /* HackiOS Sweet board */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
int sweet_rpmh_vreg_enable(const char *res, unsigned mv);
int sweet_rpmh_arc_probe(const char *res);
int sweet_rpmh_arc_read(const char *res);
int sweet_rpmh_arc_require(const char *res, unsigned minimum);
int sweet_rpmh_arc_raise(const char *res, unsigned target_vlvl);

/* Resource descriptor resolved from the bootloader command DB. */
struct rh_res {
	uint32_t addr;
	uint32_t aux;
	uint16_t aux_len;
	unsigned slave;
};

/* Live downstream DT has two positional reg entries, no reg-names. */
#define RH_RSC_PA       0x18220000u
#define RH_TCS_PA       0x18220d00u
#define RH_TCS_SIZE     0x3000u
#define RH_DICT_PA      0x0c3f000cu
#define RH_DRV          2u
#define RH_ACTIVE_N     2u       /* DT order: ACTIVE 2, SLEEP 3, WAKE 3 */
#define RH_NORMAL_N     8u       /* hidden CONTROL TCS is not in this bank */
#define RH_TCS_STRIDE   0x2a0u
#define RH_CMD_STRIDE   0x14u
#define RH_IRQ_EN       0x00u
#define RH_IRQ_STATUS   0x04u
#define RH_IRQ_CLEAR    0x08u
/* Saved sweet live FDT: apps_rsc interrupts=<0 5 0>, GICv3 at
 * 0x17a00000. SPI5 is GIC INTID37. RSC completion latching and GIC
 * delivery are separate gates: keep delivery disabled while polling. */
#define RH_GICD_PA      0x17a00000u
#define RH_GIC_INTID    37u
#define RH_GIC_ISENABLER 0x100u
#define RH_GIC_ISACTIVER 0x300u
#define RH_WAIT         0x10u
#define RH_CONTROL      0x14u
#define RH_STATUS       0x18u
#define RH_ENABLE       0x1cu
#define RH_MSGID        0x30u
#define RH_ADDR         0x34u
#define RH_DATA         0x38u
#define RH_CMD_STATUS   0x3cu
#define RH_RESP_DATA    0x40u
#define RH_AMC_EN       (1u << 16)
#define RH_TRIGGER      (1u << 24)
#define RH_ISSUED       (1u << 8)
#define RH_COMPLETE     (1u << 16)
#define RH_READ_ACK     (8u | (1u << 8))
#define RH_WRITE_ACK    (RH_READ_ACK | (1u << 16))
#define RH_DB_MAGIC     0x0c0330dbu
#define RH_DB_HEADER    144u
#define RH_DB_ENTRY     24u
#define RH_DB_MAX       0x100000u /* policy bound, not a DT size */
#define RH_ARC          3u
#define RH_VRM          4u
#define RH_ARC_LEVEL_SIZE   2u
#define RH_ARC_MAX_LEVELS   16u
#define RH_TIMEOUT_US   100000u
#define RH_ARC_TIMEOUT_US 200000u

static vm_offset_t rh_rsc, rh_tcs, rh_dict, rh_db;
static uint32_t rh_db_size;
static unsigned rh_ncpt;
static unsigned rh_lock;
static bool rh_poisoned;
static bool rh_touch_ready; /* protected by rh_lock; ARC reads run after L11C */

static uint32_t
rh_read(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
rh_write(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
rh_sync(uint32_t off, uint32_t value)
{
	rh_write(rh_tcs, off, value);
	for (unsigned i = 0; i < 1000; ++i) {
		if (rh_read(rh_tcs, off) == value) {
			return true;
		}
		delay(1);
	}
	printf("SWEET RPMH: readback timeout off=%#x want=%#x got=%#x\n",
	    off, value, rh_read(rh_tcs, off));
	return false;
}

/* DB is little endian; avoid packed structs and unaligned Device loads. */
static uint16_t
rh_db16(uint32_t off)
{
	uint32_t v = rh_read(rh_db, off & ~3u);
	return (uint16_t)(v >> (8u * (off & 3u)));
}

static bool
rh_range(uint32_t off, uint32_t size)
{
	return off <= rh_db_size && size <= rh_db_size - off;
}

static int
rh_map(void)
{
	if (!rh_rsc) {
		rh_rsc = ml_io_map(RH_RSC_PA, 0x100);
	}
	if (!rh_tcs) {
		rh_tcs = ml_io_map(RH_TCS_PA, RH_TCS_SIZE);
	}
	if (!rh_dict) {
		rh_dict = ml_io_map(RH_DICT_PA, 8);
	}
	if (!rh_rsc || !rh_tcs || !rh_dict) {
		printf("SWEET RPMH: RSC/locator mapping failed\n");
		return ENOMEM;
	}
	uint32_t id = rh_read(rh_rsc, 0);
	uint32_t solver = rh_read(rh_rsc, 4);
	uint32_t config = rh_read(rh_rsc, 0x0c);
	unsigned major = (id >> 16) & 0xffu;
	unsigned count = (config >> (6u * RH_DRV)) & 0x3fu;
	rh_ncpt = (config >> 27) & 0x1fu;
	printf("SWEET RPMH: rsc=%#x tcs=%#x DRV=%u id=%#x solver=%#x config=%#x TCS=%u ncpt=%u\n",
	    RH_RSC_PA, RH_TCS_PA, RH_DRV, id, solver, config, count, rh_ncpt);
	/* This client implements the pre-v3 layout only; do not guess v3 offsets. */
	if ((major != 1 && major != 2) || config == 0xffffffffu ||
	    solver == 0xffffffffu || count < RH_NORMAL_N ||
	    count > RH_TCS_SIZE / RH_TCS_STRIDE || rh_ncpt < 3 || rh_ncpt > 16) {
		printf("SWEET RPMH: unsupported RSC geometry/revision\n");
		return ENODEV;
	}
	if (!rh_db) {
		uint32_t pa = rh_read(rh_dict, 0);
		uint32_t size = rh_read(rh_dict, 4);
		printf("SWEET RPMH: cmd-db locator=%#x pa=%#x size=%#x\n", RH_DICT_PA, pa, size);
		/* Firmware supplies the reserved RAM address; never scan physical RAM. */
		if (pa < 0x80000000u || (pa & 3u) || (size & 3u) ||
		    size < RH_DB_HEADER || size > RH_DB_MAX ||
		    (uint64_t)pa + size > 0x100000000ULL) {
			printf("SWEET RPMH: invalid cmd-db locator\n");
			return EINVAL;
		}
		rh_db = ml_io_map(pa, size);
		if (!rh_db) {
			return ENOMEM;
		}
		rh_db_size = size;
	}
	uint32_t db_version = rh_read(rh_db, 0);
	uint32_t magic = rh_read(rh_db, 4);
	uint32_t flags = rh_read(rh_db, 140);
	printf("SWEET RPMH: cmd-db version=%#x magic=%#x checksum=%#x flags=%#x\n",
	    db_version, magic, rh_read(rh_db, 136), flags);
	/* Linux only checks magic; version 1 is an intentionally stricter policy. */
	if (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
		printf("SWEET RPMH: unsupported/standalone cmd-db\n");
		return EINVAL;
	}
	return 0;
}

static int
rh_lookup(const char *res, unsigned expected_slave, struct rh_res *out)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; ++i) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; ++i) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		uint32_t data = RH_DB_HEADER + rh_db16(h + 4);
		unsigned count = rh_db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3u) ||
		    !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(data, 0)) {
			printf("SWEET RPMH: malformed cmd-db slave header %u\n", i);
			return EINVAL;
		}
		for (unsigned j = 0; j < count; ++j) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rh_read(rh_db, e) != key[0] || rh_read(rh_db, e + 4) != key[1]) {
				continue;
			}
			uint32_t addr = rh_read(rh_db, e + 16);
			uint32_t aux_len = rh_db16(e + 20);
			uint32_t aux_off = rh_db16(e + 22);
			uint32_t align = expected_slave == RH_ARC ? 3u : 15u;
			if (slave != expected_slave || (addr >> 16) != expected_slave ||
			    (addr & align) || aux_off > rh_db_size - data ||
			    !rh_range(data + aux_off, aux_len)) {
				printf("SWEET RPMH: invalid slave %u entry for %s\n",
				    expected_slave, res);
				return EINVAL;
			}
			if (expected_slave == RH_ARC &&
			    ((data + aux_off) & 1u || rh_db16(h + 8) != 0x1000u ||
			    aux_len < RH_ARC_LEVEL_SIZE ||
			    aux_len > RH_ARC_MAX_LEVELS * RH_ARC_LEVEL_SIZE ||
			    (aux_len & (RH_ARC_LEVEL_SIZE - 1u)))) {
				printf("SWEET RPMH: invalid ARC aux length %u for %s\n",
				    aux_len, res);
				return EINVAL;
			}
			out->addr = addr;
			out->aux = data + aux_off;
			out->aux_len = (uint16_t)aux_len;
			out->slave = slave;
			++matches;
			printf("SWEET RPMH: %s addr=%#x slave=%u aux=%u@%#x version=%#x priority=%#x:%#x\n",
			    res, addr, slave, aux_len, data + aux_off, rh_db16(h + 8),
			    rh_read(rh_db, e + 8), rh_read(rh_db, e + 12));
		}
	}
	if (matches != 1) {
		printf("SWEET RPMH: %s lookup matches=%u; refusing\n", res, matches);
		return ENODEV;
	}
	return 0;
}

/*
 * Decode an ARC level table.  Command-DB aux data is a little-endian u16
 * array of resource-specific levels; the consumer level adds one because
 * zero is an invalid voltage.  Zero padding after the first entry is
 * ignored, matching the downstream regulator driver.
 */
static int
rh_arc_levels(const struct rh_res *res, uint32_t *level, unsigned *count)
{
	unsigned n = res->aux_len / RH_ARC_LEVEL_SIZE;
	if (n == 0 || n > RH_ARC_MAX_LEVELS) {
		return EINVAL;
	}
	unsigned used = n;
	for (unsigned i = 0; i < n; ++i) {
		uint16_t value = rh_db16(res->aux + i * RH_ARC_LEVEL_SIZE);
		if (i > 0 && value == 0) {
			used = i;
			for (unsigned j = i + 1; j < n; ++j) {
				if (rh_db16(res->aux + j * RH_ARC_LEVEL_SIZE)) {
					return EINVAL; /* nonzero data after padding */
				}
			}
			break;
		}
		level[i] = value + 1;
		if (i > 0 && level[i] < level[i - 1]) {
			printf("SWEET RPMH: non-monotonic ARC table index=%u\n", i);
			return EINVAL;
		}
	}
	if (used == 0 || level[used - 1] == 0) {
		return EINVAL;
	}
	*count = used;
	return 0;
}

static bool
rh_free(unsigned t)
{
	uint32_t b = t * RH_TCS_STRIDE;
	/* Idle alone is insufficient: a sleep/wake payload can be armed and idle. */
	return rh_read(rh_tcs, b + RH_STATUS) != 0 &&
	    rh_read(rh_tcs, b + RH_STATUS) != 0xffffffffu &&
	    rh_read(rh_tcs, b + RH_CONTROL) == 0 &&
	    rh_read(rh_tcs, b + RH_ENABLE) == 0 &&
	    rh_read(rh_tcs, b + RH_WAIT) == 0 &&
	    !(rh_read(rh_tcs, RH_IRQ_STATUS) & (1u << t));
}

/* No GIC writes: this polled controller has no registered IRQ handler.
 * Refuse ownership if another user has enabled or activated its SPI. */
static bool
rh_poll_delivery_disabled(void)
{
	static vm_offset_t gicd;
	if (!gicd) {
		gicd = ml_io_map(RH_GICD_PA, 0x1000);
	}
	if (!gicd) {
		return false;
	}
	const uint32_t reg = (RH_GIC_INTID / 32u) * 4u;
	const uint32_t bit = 1u << (RH_GIC_INTID % 32u);
	uint32_t enabled = rh_read(gicd, RH_GIC_ISENABLER + reg);
	uint32_t active = rh_read(gicd, RH_GIC_ISACTIVER + reg);
	printf("SWEET RPMH: poll completion v2 GIC INTID%u enable=%#x active=%#x\n",
	    RH_GIC_INTID, enabled, active);
	return enabled != 0xffffffffu && active != 0xffffffffu &&
	    !(enabled & bit) && !(active & bit);
}

static void
rh_quarantine_source(uint32_t bit)
{
	/* Never acknowledge, reset or retry an uncertain transaction. Mask only
	 * our source; retain the poisoned state and all command storage. */
	(void)rh_sync(RH_IRQ_EN, rh_read(rh_tcs, RH_IRQ_EN) & ~bit);
}

static int
rh_send_cmds(const char *res, uint32_t address, uint32_t align_mask,
    const uint32_t *offsets, const uint32_t *values, unsigned n,
    unsigned timeout_us, uint32_t *response)
{
	if (n == 0 || n > rh_ncpt || n > RH_ARC_MAX_LEVELS ||
	    (response && n != 1)) {
		return EINVAL;
	}
	int chosen = -1;
	for (unsigned t = 0; t < RH_ACTIVE_N; ++t) {
		uint32_t b = t * RH_TCS_STRIDE;
		uint32_t enabled = rh_read(rh_tcs, b + RH_ENABLE);
		printf("SWEET RPMH: ACTIVE TCS%u idle=%#x control=%#x enable=%#x wait=%#x\n",
		    t, rh_read(rh_tcs, b + RH_STATUS), rh_read(rh_tcs, b + RH_CONTROL),
		    enabled, rh_read(rh_tcs, b + RH_WAIT));
		if (enabled & ~((1u << rh_ncpt) - 1u)) {
			return EIO;
		}
		for (unsigned c = 0; c < rh_ncpt; ++c) {
			if ((enabled & (1u << c)) &&
			    (rh_read(rh_tcs, b + RH_ADDR + c * RH_CMD_STRIDE) &
			    ~align_mask) == address) {
				printf("SWEET RPMH: conflicting inherited %s request in TCS%u\n",
				    res, t);
				return EBUSY;
			}
		}
		if (chosen < 0 && rh_free(t)) {
			chosen = (int)t;
		}
	}
	if (chosen < 0) {
		printf("SWEET RPMH: no empty idle ACTIVE TCS; leaving inherited state intact\n");
		return EBUSY;
	}
	unsigned t = (unsigned)chosen;
	uint32_t b = t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t enable = (1u << n) - 1u;
	uint32_t irq_en = rh_read(rh_tcs, RH_IRQ_EN);
	const bool poll_latch = strcmp(res, "ldoc11") != 0;
	if (irq_en == 0xffffffffu || !rh_free(t) || (poll_latch && !rh_poll_delivery_disabled())) {
		return EBUSY;
	}
	/* Only this TCS's source is masked. Other sources/CPU IRQs remain intact. */
	if (!rh_sync(RH_IRQ_EN, irq_en & ~bit)) {
		rh_poisoned = true;
		return EIO;
	}
	if (!rh_free(t)) {
		/* No payload was touched. Do not acknowledge somebody else's IRQ. */
		if (!rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
			rh_poisoned = true;
		}
		return EBUSY;
	}
	/* From here an uncertain write/timeout quarantines our slot for this boot. */
	rh_poisoned = true;
	printf("SWEET RPMH: TCS%u ACTIVE %s %s commands=%u irq-enable=%#x\n",
	    t, response ? "READ" : "WRITE", res, n, irq_en);
	for (unsigned c = 0; c < n; ++c) {
		uint32_t cmd = b + c * RH_CMD_STRIDE;
		if (!rh_sync(cmd + RH_MSGID, response ? RH_READ_ACK : RH_WRITE_ACK) ||
		    !rh_sync(cmd + RH_ADDR, address + offsets[c]) ||
		    !rh_sync(cmd + RH_DATA, values[c])) {
			return EIO;
		}
	}
	if (!rh_sync(b + RH_WAIT, enable) || !rh_sync(b + RH_ENABLE, enable) ||
	    !rh_sync(b + RH_CONTROL, 0) || !rh_sync(b + RH_CONTROL, RH_AMC_EN)) {
		return EIO;
	}
	/* Linux keeps ACTIVE TCS completion sources enabled. Masking this
	 * source made IRQ_STATUS stay zero and left only a transient busy edge
	 * as completion evidence. Clear only our known-free latch, enable it,
	 * and require it to start clear. GIC delivery stays disabled throughout. */
	if (poll_latch) {
		rh_write(rh_tcs, RH_IRQ_CLEAR, bit);
		if ((rh_read(rh_tcs, RH_IRQ_STATUS) & bit) ||
		    !rh_sync(RH_IRQ_EN, rh_read(rh_tcs, RH_IRQ_EN) | bit) ||
		    (rh_read(rh_tcs, RH_IRQ_STATUS) & bit)) {
			rh_quarantine_source(bit);
			return EIO;
		}
	}
	rh_write(rh_tcs, b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	uint32_t irq = 0, idle = 0, status[RH_ARC_MAX_LEVELS] = {0};
	bool done = false, progress = false;
	for (unsigned us = 0; us < timeout_us; ++us) {
		irq = rh_read(rh_tcs, RH_IRQ_STATUS);
		idle = rh_read(rh_tcs, b + RH_STATUS);
		bool complete = true;
		for (unsigned c = 0; c < n; ++c) {
			status[c] = rh_read(rh_tcs, b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
			complete &= status[c] != 0xffffffffu &&
			    (status[c] & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
		}
		progress |= idle == 0 || !complete;
		/* ARC requires the fresh latch. Preserve the existing touchscreen
		 * regulator path; it is not part of this GPU completion change.
		 * Old command status and an idle TCS alone are never accepted. */
		if (idle && idle != 0xffffffffu && irq != 0xffffffffu &&
		    ((irq & bit) || (!poll_latch && progress)) && complete) {
			done = true;
			break;
		}
		delay(1);
	}
	printf("SWEET RPMH: TCS%u done=%d irq=%#x idle=%#x control=%#x",
	    t, done, irq, idle, rh_read(rh_tcs, b + RH_CONTROL));
	for (unsigned c = 0; c < n; ++c) {
		printf(" status%u=%#x", c, status[c]);
	}
	printf("\n");
	if (!done) {
		rh_quarantine_source(bit);
		printf("SWEET RPMH: timeout; TCS quarantined, IRQ remains masked; no reset/retry\n");
		return ETIMEDOUT;
	}
	/* Read payload before releasing the slot; do not expose it on cleanup error. */
	uint32_t reply = response ? rh_read(rh_tcs, b + RH_RESP_DATA) : 0;
	/* Proven completion: clear trigger before mode, then free only our commands. */
	if (!rh_sync(RH_IRQ_EN, rh_read(rh_tcs, RH_IRQ_EN) & ~bit) ||
	    !rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0)) {
		return EIO;
	}
	rh_write(rh_tcs, RH_IRQ_CLEAR, bit); /* W1C: never clear other TCS bits */
	if ((rh_read(rh_tcs, RH_IRQ_STATUS) & bit) ||
	    !rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
		return EIO;
	}
	rh_poisoned = false;
	if (response) {
		*response = reply;
		printf("SWEET RPMH: %s ACTIVE read complete raw=%#x\n", res, reply);
	} else {
		printf("SWEET RPMH: %s ACTIVE vote complete\n", res);
	}
	return 0;
}

int
sweet_rpmh_vreg_enable(const char *res, unsigned mv)
{
	/* Board-specific helper: do not apply PMIC5-LDO policy to arbitrary resources. */
	if (!res || strcmp(res, "ldoc11") || mv < 2950 || mv > 3400) {
		printf("SWEET RPMH: expected ldoc11 and 2950..3400 mV\n");
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	struct rh_res found = {0};
	if (rh_poisoned) {
		printf("SWEET RPMH: previous uncertain transaction; refusing further access\n");
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, RH_VRM, &found);
	}
	if (!error) {
		/* HPM (raw 7) is supported by this PMIC5 LDO; enable comes last. */
		const uint32_t offsets[3] = {0, 8, 4};
		const uint32_t values[3] = {mv, 7, 1};
		error = rh_send_cmds(res, found.addr, 15u, offsets, values, 3,
		    RH_TIMEOUT_US, NULL);
		if (!error) {
			rh_touch_ready = true;
		}
	}
out:
	if (error) {
		printf("SWEET RPMH: enable failed errno=%d\n", error);
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

static bool
rh_arc_name(const char *res)
{
	return res && (!strcmp(res, "cx.lvl") || !strcmp(res, "mx.lvl") ||
	    !strcmp(res, "gfx.lvl"));
}

/*
 * Read-only command-DB inspection.  No TCS is claimed, no RPMH command is
 * written, and no rail is changed.  This is the evidence-gathering step before
 * the first ARC vote.
 */
int
sweet_rpmh_arc_probe(const char *res)
{
	/* XO inspection only: write APIs retain the original resource whitelist. */
	if (!rh_arc_name(res) && (!res || strcmp(res, "xo.lvl"))) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	struct rh_res found = {0};
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, RH_ARC, &found);
	}
	if (!error) {
		uint32_t level[RH_ARC_MAX_LEVELS] = {0};
		unsigned count = 0;
		error = rh_arc_levels(&found, level, &count);
		if (!error) {
			printf("SWEET RPMH: ARC %s read-only table:", res);
			for (unsigned i = 0; i < count; ++i) {
				printf(" %u:%u", i, level[i]);
			}
			printf("\n");
		}
	}
	if (error) {
		printf("SWEET RPMH: ARC probe %s failed errno=%d\n", res, error);
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/* Build wire votes from raw command-DB levels, not regulator levels (+1).
 * Uses only command-DB RAM: no new ACTIVE TCS write authority. */
int sweet_rpmh_adreno_votes(uint32_t *gpu, uint32_t *gmu);
int
sweet_rpmh_adreno_votes(uint32_t *gpu, uint32_t *gmu)
{
 if (!gpu || !gmu) return EINVAL;
 *gpu = *gmu = 0;
 if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) return EBUSY;
 const char *names[] = {"gfx.lvl", "cx.lvl", "mx.lvl"};
 const uint32_t address[] = {0x30050, 0x30000, 0x30010};
 uint32_t levels[3][RH_ARC_MAX_LEVELS] = {{0}};
 unsigned counts[3] = {0}, selected[3] = {0};
 int error = rh_map();
 for (unsigned r = 0; !error && r < 3; ++r) {
  struct rh_res resource = {0};
  error = rh_lookup(names[r], RH_ARC, &resource);
  if (!error && resource.addr != address[r]) error = ENODEV;
  if (!error) error = rh_arc_levels(&resource, levels[r], &counts[r]);
  if (error) break;
  /* MIN_SVS is raw 48 in the pinned RPMh bindings. The measured
  * GFX/CX tables round this up to 64; MX rounds up to 192. */
  unsigned i;
  for (i = 0; i < counts[r]; ++i) {
   --levels[r][i];
   if (!selected[r] && levels[r][i] >= 48) selected[r] = i;
  }
  if (!selected[r] || levels[r][0] != 0) error = ERANGE;
 }
 if (!error) {
  *gpu = (levels[0][selected[0]] << 16) | (selected[2] << 8) | selected[0];
  *gmu = (levels[1][selected[1]] << 16) | (selected[2] << 8) | selected[1];
 }
 __atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
 return error;
}

/*
 * Read one ARC resource while the caller holds rh_lock. The same serialized
 * ACTIVE TCS engine is used for reads and for the raise-only write below.
 */
static int
rh_arc_read_locked(const char *res, struct rh_res *found,
    uint32_t *level, unsigned *count, uint32_t *reply)
{
	int error = rh_map();
	if (!error) {
		error = rh_lookup(res, RH_ARC, found);
	}
	if (!error) {
		error = rh_arc_levels(found, level, count);
	}
	if (!error) {
		const uint32_t zero = 0;
		error = rh_send_cmds(res, found->addr, 3u, &zero, &zero, 1,
		    RH_ARC_TIMEOUT_US, reply);
	}
	return error;
}

/*
 * ARC READ uses the same serialized ACTIVE TCS engine as the touch VRM.
 * qcom-rpmh-mailbox.c defines READ by clearing MSGID.WRITE and retrieves
 * CMD_RESP_DATA at 0x40. This does not change a resource vote, although it
 * programs a TCS. Do not infer the retained APPS vote from the DT minimum.
 *
 * The meaning of an ARC reply must be established on this board before
 * introducing a vote floor. Until then this API logs raw replies only.
 */
int
sweet_rpmh_arc_read(const char *res)
{
	if (!rh_arc_name(res)) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	struct rh_res found = {0};
	uint32_t level[RH_ARC_MAX_LEVELS] = {0};
	unsigned count = 0;
	uint32_t reply = 0xffffffffu;
	if (rh_poisoned) {
		printf("SWEET RPMH: previous uncertain transaction; refusing ARC read\n");
		goto out;
	}
	if (!rh_touch_ready) {
		/* A failed experiment must not block the first touch-power vote. */
		error = EAGAIN;
		printf("SWEET RPMH: ARC read deferred: L11C has not completed\n");
		goto out;
	}
	error = rh_arc_read_locked(res, &found, level, &count, &reply);
	if (!error) {
		printf("SWEET RPMH: ARC %s reply=%#x table-count=%u\n",
		    res, reply, count);
		if (reply < count) {
			printf("SWEET RPMH: ARC reply as index: vlvl=%u (interpretation unverified)\n",
			    level[reply]);
		} else {
			printf("SWEET RPMH: ARC reply is not a plain table index\n");
		}
	}
out:
	if (error) {
		printf("SWEET RPMH: ARC read %s failed errno=%d\n", res, error);
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/* Check the current board's ARC selector interpretation without voting.
 * Unlike arc_read(), this returns an error for off/out-of-range replies.
 * This is a resource-setting check; it does not measure a rail voltage.
 */
int
sweet_rpmh_arc_require(const char *res, unsigned minimum)
{
	if (!rh_arc_name(res) || minimum <= 1)
		return EINVAL;
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE))
		return EBUSY;
	int error = EIO;
	struct rh_res found = {0};
	uint32_t levels[RH_ARC_MAX_LEVELS] = {0}, reply = 0xffffffffu;
	unsigned count = 0;
	if (rh_poisoned)
		goto out;
	if (!rh_touch_ready) {
		error = EAGAIN;
		goto out;
	}
	error = rh_arc_read_locked(res, &found, levels, &count, &reply);
	if (error)
		goto out;
	if (reply >= count) {
		error = EIO;
		goto out;
	}
	printf("SWEET RPMH: ARC require %s selector=%u interpreted-level=%u minimum=%u\n",
	    res, reply, levels[reply], minimum);
	if (levels[reply] < minimum)
		error = EAGAIN;
out:
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}

/*
 * Raise-only ARC vote. This is deliberately narrower than 0083's fixed
 * votes: it reads the resource first, refuses a non-index reply, and never
 * writes a selector below the current level. The caller uses it only for
 * gfx.lvl; shared mx/cx votes remain untouched.
 */
int
sweet_rpmh_arc_raise(const char *res, unsigned target_vlvl)
{
	if (!res || strcmp(res, "gfx.lvl") || target_vlvl == 0) {
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	struct rh_res found = {0};
	uint32_t level[RH_ARC_MAX_LEVELS] = {0};
	unsigned count = 0;
	uint32_t before = 0xffffffffu, after = 0xffffffffu;
	unsigned selector = 0;

	if (rh_poisoned) {
		printf("SWEET RPMH: previous uncertain transaction; refusing ARC raise\n");
		goto out;
	}
	if (!rh_touch_ready) {
		error = EAGAIN;
		printf("SWEET RPMH: ARC raise deferred: L11C has not completed\n");
		goto out;
	}
	error = rh_arc_read_locked(res, &found, level, &count, &before);
	if (error) {
		goto out;
	}
	printf("SWEET RPMH: ARC %s raise before=%#x table-count=%u target-vlvl=%u\n",
	    res, before, count, target_vlvl);
	if (before >= count) {
		printf("SWEET RPMH: ARC %s reply is not a table index; refusing write\n", res);
		error = EINVAL;
		goto out;
	}
	if (level[before] >= target_vlvl) {
		printf("SWEET RPMH: ARC %s already at/above vlvl=%u; no write\n",
		    res, level[before]);
		error = 0;
		goto out;
	}
	for (selector = 0; selector < count; ++selector) {
		if (level[selector] >= target_vlvl) {
			break;
		}
	}
	if (selector == 0 || selector >= count) {
		printf("SWEET RPMH: ARC %s no safe selector >= target; refusing write\n", res);
		error = ERANGE;
		goto out;
	}
	printf("SWEET RPMH: ARC %s raise selector=%u vlvl=%u from=%#x vlvl=%u\n",
	    res, selector, level[selector], before, level[before]);
	{
		const uint32_t offset = 0;
		const uint32_t value = selector;
		error = rh_send_cmds(res, found.addr, 3u, &offset, &value, 1,
		    RH_ARC_TIMEOUT_US, NULL);
	}
	if (error) {
		goto out;
	}
	error = rh_arc_read_locked(res, &found, level, &count, &after);
	if (error) {
		goto out;
	}
	printf("SWEET RPMH: ARC %s raise after=%#x expected=%u\n",
	    res, after, selector);
	if (after != selector) {
		printf("SWEET RPMH: ARC %s raise readback mismatch; no retry, no GPUCC access\n", res);
		error = EIO;
	} else if (after >= count) {
		printf("SWEET RPMH: ARC %s raise readback index outside table; no retry\n", res);
		error = EIO;
	} else {
		printf("SWEET RPMH: ARC %s raise verified selector=%u vlvl=%u\n",
		    res, selector, level[after]);
	}
out:
	if (error) {
		printf("SWEET RPMH: ARC raise %s failed errno=%d\n", res, error);
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
