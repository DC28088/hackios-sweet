#!/usr/bin/env python3
"""Apply Sweet-only payload reservation and HackiOS handoff to pinned Mu source."""
from pathlib import Path
import sys
import subprocess
import hashlib
root=Path(sys.argv[1])
# The pinned device-binaries INF selects a USB-host-only patch. Its NOP at
# 0x3100 forces the platform-29 path: PortMode=8 and CurrentMode override=1
# (host), so even ConfigUsbEvent's peripheral request (mode 4) starts XHCI.
# Use the matching, unmodified Sweet binary: its default override is mode 4.
# Keep this local to the Sweet build and fail closed if the upstream pin moves.
usb=root/'Binaries/sweet/QcomPkg/Drivers/UsbConfigDxe'
for name, digest in {
    'UsbConfigDxe.efi': 'effec47a598ce2b277defa47c85f0842ac9a2f03d539934a2a20a2285600e2f7',
    'UsbConfigDxe.patched.efi': '80bfc029b36396a035278a6b174239af3e51aaf6129cd32068b2cbda82a24934',
}.items():
    if hashlib.sha256((usb/name).read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Unexpected pinned Sweet USB binary: {name}')
p=usb/'UsbConfigDxe.inf'
s=p.read_text();anchor='PE32|UsbConfigDxe.patched.efi|*'
assert s.count(anchor)==1
p.write_text(s.replace(anchor, 'PE32|UsbConfigDxe.efi|*'))
print('Selected original Sweet UsbConfigDxe for USB peripheral mode')
# Split RAM bank registration around SEC/DXE's existing GCD reservations.
# In particular, never re-add the fixed-address payload as conventional RAM.
subprocess.run(['git', '-C', str(root), 'apply', str(Path(__file__).resolve().parent / 'patches/dynamic-ram-reservations.patch')], check=True)
p=root/'Platforms/Xiaomi/sweetPkg/Library/DeviceMemoryMapLib/DeviceMemoryMapLib.c'
s=p.read_text();anchor='  // Other Memory Regions'
assert s.count(anchor)==1
s=s.replace(anchor,'  {"HackiOS Payload", 0xF0000000, 0x04000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},\n\n'+anchor);p.write_text(s)
# Run after the complete platform AfterConsole path. The pinned Mu version
# offers a priority-boot hook before disk BootOrder; use it so unrelated EFI
# disk options cannot intercept the resident payload. Keep upstream's
# UnableToBoot hook as a fallback. EndOfDxe already runs in BeforeConsole.
p=root/'Silicon/Silicium/SiliciumPkg/Library/PlatformBootManagerLib/MsPlatform.c'
s=p.read_text()
anchor='#include <Library/UefiBootServicesTableLib.h>'
assert s.count(anchor)==1
s=s.replace(anchor, anchor+'\n#include <Library/BaseMemoryLib.h>\n#include <Library/UefiLib.h>')
anchor='VOID\nEFIAPI\nPlatformBootManagerOnDemandConInConnect'
assert s.count(anchor)==1
code=(Path(__file__).resolve().parent/'HackiOSPayload.c.inc').read_text()
s=s.replace(anchor,code+'\n'+anchor)
anchor='  // Get Priority Boot'
assert s.count(anchor)==1
s=s.replace(anchor,'  HackiOSSweetStart ();\n\n'+anchor)
anchor='  DeviceBootManagerUnableToBoot ();'
assert s.count(anchor)==1
s=s.replace(anchor,'  HackiOSSweetStart ();\n'+anchor)
p.write_text(s)
print('Reserved Sweet payload and installed HackiOS BDS priority/fallback handoff')
