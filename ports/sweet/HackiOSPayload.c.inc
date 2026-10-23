/* Sweet adaptation of HackiOS's XNUP LoadImage/StartImage handoff.
 * BootShim reserves and copies this container; no storage access is needed.
 * Called after platform console/controller setup, before disk BootOrder.
 */
STATIC VOID
HackiOSSweetStart (VOID)
{
  UINT8      *Base = (UINT8 *)(UINTN)0xF0000000;
  UINT32     *Header = (UINT32 *)Base;
  UINT8      *At;
  UINT8      *End;
  UINTN       Remaining;
  UINTN       PaddedSize;
  UINT32      Size;
  EFI_HANDLE  Handle = NULL;
  EFI_STATUS  Status = EFI_NOT_FOUND;

  if (Header[0] != 0x50554E58) {
    return;
  }

  EfiBootManagerConnectAll ();
  Print (L"HackiOS Sweet: BDS setup complete; starting resident loader\n");
  if (Header[1] > 0x03FFF000) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Failed;
  }
  At = Base + 8;
  End = At + Header[1];
  while ((UINTN)(End - At) >= 24) {
    if (At[0] == 0) {
      break;
    }
    Remaining = (UINTN)(End - At) - 24;
    Size = *(UINT32 *)(At + 16);
    PaddedSize = ALIGN_VALUE ((UINTN)Size, 16);
    if (PaddedSize > Remaining) {
      Status = EFI_BAD_BUFFER_SIZE;
      goto Failed;
    }
    if (CompareMem (At, "loader.efi", 11) == 0) {
      Status = gBS->LoadImage (FALSE, gImageHandle, NULL, At + 24, Size, &Handle);
      Print (L"HackiOS Sweet: LoadImage: %r\n", Status);
      if (!EFI_ERROR (Status)) {
        Status = gBS->StartImage (Handle, NULL, NULL);
        Print (L"HackiOS Sweet: loader returned: %r\n", Status);
      }
      goto Failed;
    }
    At += 24 + PaddedSize;
  }

Failed:
  /* Do not obscure a loader failure by falling through to another OS/reset. */
  Print (L"HackiOS Sweet: handoff stopped (%r); hold Power to restart\n", Status);
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  for (;;) {
    gBS->Stall (100000);
  }
}
