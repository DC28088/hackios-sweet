#!/usr/bin/env python3
"""Apply Sweet timer routing to the reviewed HakiOS source before xnu/apply.sh."""
from pathlib import Path
import argparse
import subprocess
import shutil

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

# Preserve the known Sweet hardware protocol while using the current HakiOS
# queue/HID bridge. The Redwood drivers must never probe a Sweet board.
source = Path(__file__).resolve().parent / 'input'
target = a.upstream / 'xnu/files/bsd/dev'
names = ['sweet_touch.c', 'sweet_keys.c', 'sweet_rpmh.c', 'hakios.sweet_input.c']
for name in names:
    shutil.copy2(source / name, target / name)
f = a.upstream / 'xnu/hakios.patch'
s = f.read_text()
old = '+bsd/dev/hakios.input.c\t\tstandard\n'
assert s.count(old) == 1
s = s.replace(old, old + ''.join('+bsd/dev/' + name + '\t\tstandard\n' for name in names))
old = '@@ -109,6 +109,41 @@'
assert s.count(old) == 1
s = s.replace(old, '@@ -109,6 +109,45 @@')
f.write_text(s)
f = target / 'hakios.input.c'
s = f.read_text()
old = '\thakios_goodix_init();\n\thakios_buttons_init();'
assert s.count(old) == 1
s = s.replace(old, '''\tuint32_t sweet = 0;
\tPE_parse_boot_argn("sweet", &sweet, sizeof(sweet));
\tif (sweet) {
\t\textern void hakios_sweet_input_init(void);
\t\thakios_sweet_input_init();
\t} else {
\t\thakios_goodix_init();
\t\thakios_buttons_init();
\t}''')
f.write_text(s)
print('Integrated Sweet keys, GENI I2C touch and L11C RPMh with HakiOS input queue.')
