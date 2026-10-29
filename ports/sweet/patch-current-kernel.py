#!/usr/bin/env python3
"""Apply Sweet timer routing to the reviewed HakiOS source before xnu/apply.sh."""
from pathlib import Path
import argparse
import subprocess

PIN = '88513176b1e96ecf0b764cd62c6f4e6ac71345e3'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('upstream', type=Path)
a = p.parse_args()
assert subprocess.check_output(['git', '-C', str(a.upstream), 'rev-parse', 'HEAD'], text=True).strip() == PIN
f = a.upstream / 'xnu/files/osfmk/arm64/gic_v3_irq.c'
s = f.read_text()
old = '#define GIC_INTID_VTIMER        27          /* EL1 virtual timer PPI */'
assert s.count(old) == 1
s = s.replace(old, '''/* SM7150 routes the virtual timer on INTID 19; QEMU/Redwood use 27.
 * Select the board's PPI before enabling interrupts on the boot processor. */
static uint32_t gic_vtimer_intid = 27;
static uint32_t gic_sweet;''').replace('GIC_INTID_VTIMER', 'gic_vtimer_intid')
old = '''	uint32_t mine = 1u << gic_vtimer_intid;

	if (gicd_base == 0) {'''
new = '''	uint32_t mine;

	if (gicd_base == 0) {
		PE_parse_boot_argn("sweet", &gic_sweet, sizeof(gic_sweet));
		uint32_t timer_intid = gic_sweet ? 19 : 27;
		PE_parse_boot_argn("gic_vtimer", &timer_intid, sizeof(timer_intid));
		if (timer_intid < 16 || timer_intid >= 32) {
			panic("gic: virtual timer INTID %u is not a PPI", timer_intid);
		}
		gic_vtimer_intid = timer_intid;'''
assert s.count(old) == 1
s = s.replace(old, new)
old = '\n\t/* System-register CPU interface */'
assert s.count(old) == 1
s = s.replace(old, '\n\tmine = 1u << gic_vtimer_intid;\n' + old)
# Match the original, hardware-qualified Sweet Group1NS setup. Its nonsecure
# distributor view needs both enable bits; its EOI path uses combined mode.
old = 'gic_read32(gicr + GICR_IGROUPR0) | mine'
assert s.count(old) == 1
s = s.replace(old, 'gic_sweet ? 0xffffffffu : (gic_read32(gicr + GICR_IGROUPR0) | mine)')
old = '\tuint32_t ctlr = gic_read32(gicd_base + GICD_CTLR);'
assert s.count(old) == 1
s = s.replace(old, old + '\n\tuint32_t group_enable = gic_sweet ? 3u : GICD_CTLR_ENABLE_G1NS;')
s = s.replace('ctlr & (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)', 'ctlr & (group_enable | GICD_CTLR_ARE_NS)')
s = s.replace('!= (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)', '!= (group_enable | GICD_CTLR_ARE_NS)')
s = s.replace('ctlr | GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS', 'ctlr | group_enable | GICD_CTLR_ARE_NS')
old = '\t__builtin_arm_wsr64("ICC_AP1R0_EL1", 0);'
assert s.count(old) == 1
s = s.replace(old, '\tif (gic_sweet) {\n\t\t__builtin_arm_wsr64("ICC_CTLR_EL1", __builtin_arm_rsr64("ICC_CTLR_EL1") & ~2ull);\n\t}\n' + old)
f.write_text(s)
print('Applied board-selectable timer PPI; Sweet must boot with gic_vtimer=19.')
