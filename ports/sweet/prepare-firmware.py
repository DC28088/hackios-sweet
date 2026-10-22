#!/usr/bin/env python3
"""Apply Sweet-only payload reservation and HackiOS handoff to pinned Mu source."""
from pathlib import Path
import sys
root=Path(sys.argv[1])
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
