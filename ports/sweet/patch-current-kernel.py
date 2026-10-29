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
static uint32_t gic_vtimer_intid = 27;''').replace('GIC_INTID_VTIMER', 'gic_vtimer_intid')
old = '''	uint32_t mine = 1u << gic_vtimer_intid;

	if (gicd_base == 0) {'''
new = '''	uint32_t mine;

	if (gicd_base == 0) {
		uint32_t timer_intid = 27;
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
f.write_text(s)
print('Applied board-selectable timer PPI; Sweet must boot with gic_vtimer=19.')
