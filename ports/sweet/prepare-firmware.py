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
p=root/'Silicon/Silicium/SiliciumPkg/Library/PlatformBootManagerLib/BdsPlatform.c'
s=p.read_text();anchor='VOID\nExitPmAuth'
assert s.count(anchor)==1
code='''/* Sweet boot payload is copied by our own BootShim; no partition writes. */
STATIC VOID HackiOSSweetStart (VOID) {
  UINT8 *Base = (UINT8 *)(UINTN)0xF0000000;
  UINT32 *Header = (UINT32 *)Base;
  if (Header[0] != 0x50554E58 || Header[1] > 0x03FFF000) return;
  UINT8 *At = Base + 8, *End = At + Header[1];
  while (At + 24 <= End) {
    UINT32 Size = *(UINT32 *)(At + 16);
    if (Size > (UINTN)(End - At - 24)) return;
    if (CompareMem (At, "loader.efi", 11) == 0) {
      EFI_HANDLE Handle = NULL;
      if (!EFI_ERROR (gBS->LoadImage (FALSE, gImageHandle, NULL, At + 24, Size, &Handle)))
        gBS->StartImage (Handle, NULL, NULL);
      return;
    }
    if (At[0] == 0) return;
    At += 24 + ALIGN_VALUE ((UINTN)Size, 16);
  }
}

'''
s=s.replace(anchor,code+anchor)
anchor='  mPlatformConnectSequence = DeviceBootManagerAfterConsole ();'
assert s.count(anchor)==1
s=s.replace(anchor,anchor+'\n  EfiBootManagerConnectAll ();\n  HackiOSSweetStart ();')
p.write_text(s)
print('Reserved Sweet payload and installed HackiOS loader handoff')
