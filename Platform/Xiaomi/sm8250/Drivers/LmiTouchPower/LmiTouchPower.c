/** @file
  LmiTouchPower - power up the FTS touch panel, mux its I2C pins, and prove the
  I2C link works - all before Windows takes over.

  Why this exists
  ---------------
  The FocalTech panel on lmi is fed by pm8150a LDO1 (avdd, 1.8 V) plus the
  "vdd-gpio" enable line on TLMM GPIO 72.  In Android the fts_ts driver enables
  both at probe time: the RPMh regulator node in DT has neither
  regulator-always-on nor regulator-boot-on, so nothing else ever turns the rail
  on.  Windows ships no RPMh regulator driver, therefore by the time Windows
  probes the ACPI I2C device (PNP0C50) the panel has no power and every I2C
  transaction times out.

  What it does
  ------------
    1. waits for the Qualcomm PMIC VREG protocol, sets LDO1 to 1.8 V, enables it
    2. muxes TLMM GPIO 36/37 to function "qup13" (func 119) by direct MMIO RMW
    3. drives TLMM GPIO 72 high (vdd enable) and GPIO 38 high (out of reset)
    4. opens I2C SE13 @ 0x38 and reads a few registers (self test)
    5. writes the whole transcript to \\LMI_TX.TXT on the boot volume (ESP)
       and, as a backup channel, to the last sectors of that same partition
    6. if \\LMI_PROBE.FLG existed (and was successfully deleted) it cold-resets
       the phone, so the test is fully automatic: boot image -> reset -> Android

  Step 4 is the self test: a chip id of 0x54 proves rail + pinmux + clock + I2C
  controller are all good and whatever still fails afterwards is Windows-side.

  Note: GPIO 36/37 (SDA/SCL, function "qup13" = 119) cannot be muxed through
  EFI_QCOM_TLMM_PROTOCOL because EFI_GPIO_CFG only carries 4 bits of func, so
  the TLMM GPIO_CFG registers are poked directly.  The DSDT _INI does the same
  thing again from Windows; both are idempotent.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Protocol/EFIPmicVreg.h>
#include <Protocol/EFITlmm.h>
#include <Protocol/EFII2C.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Guid/EventGroup.h>

//
// QcomPkg.dec exports the I2C protocol as gQcomI2cProtocolGuid (lowercase "c")
// while EFII2C.h declares an extern spelled gQcomI2CProtocolGuid.
//
extern EFI_GUID  gQcomI2cProtocolGuid;

#define LMI_TOUCH_VREG_ID     EFI_PM_LDO_1
#define LMI_TOUCH_VREG_UV     1800000
#define LMI_MAX_PMIC_INDEX    6

#define LMI_TOUCH_GPIO_VDD    72
#define LMI_TOUCH_GPIO_RST    38
#define LMI_TOUCH_GPIO_SDA    36
#define LMI_TOUCH_GPIO_SCL    37

#define LMI_TOUCH_I2C_ADDR    0x38
#define LMI_TOUCH_REG_CHIPID  0xA3
#define LMI_TOUCH_CHIPID_FTS  0x54

//
// TLMM GPIO_CFGn = 0x0F000000 + 0x1000 * n   (kona / SM8250)
// [1:0] PULL, [9:2] FUNC_SEL, [12:10] DRV, [13] OE
//
#define TLMM_GPIO_CFG(n)      (0x0F000000U + 0x1000U * (n))
#define TLMM_FUNC_QUP13       119U
#define TLMM_MASK_PULL_FUNC   0xFFFFFC03U

#define LMI_TX_FILE           L"\\LMI_TX.TXT"
#define LMI_PROBE_FLAG        L"\\LMI_PROBE.FLG"

#define FILE_NAME(p)          ((CHAR16 *)(p))
#define LMI_ESP_SECTORS       1048575ULL     // 512 MiB / 512 - 1

//
// build_probe.py rewrites this to 1 in the copy it pushes to the CI.  The
// resulting probe image always cold-resets after the self test, so the whole
// experiment runs itself - no flag file on the ESP required.
//
#ifndef LMI_PROBE_ALWAYS_RESET
#define LMI_PROBE_ALWAYS_RESET  1
#endif

STATIC
VOID
Report (
  VOID
  );

STATIC CHAR8  mTx[4096];
STATIC UINTN  mTxLen = 0;
STATIC UINTN  mErr   = 0;

STATIC
VOID
LmiPrint (
  IN CONST CHAR16  *Format,
  ...
  )
{
  CHAR16   Buf[200];
  VA_LIST  Marker;

  VA_START (Marker, Format);
  UnicodeVSPrint (Buf, sizeof (Buf), Format, Marker);
  VA_END (Marker);

  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, Buf);
  }
  DEBUG ((EFI_D_INFO, "%s", Buf));
}

STATIC
VOID
TxAdd (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  UINTN    Room;

  Room = sizeof (mTx) - mTxLen;
  if (Room < 256) {
    return;
  }

  VA_START (Marker, Format);
  mTxLen += AsciiVSPrint (&mTx[mTxLen], Room, Format, Marker);
  VA_END (Marker);
}

STATIC
VOID
TxLine (
  IN CONST CHAR8  *Text
  )
{
  TxAdd ("%a\r\n", Text);
  LmiPrint (L"LMI: %a\r\n", Text);
}

STATIC
VOID
TxFail (
  IN CONST CHAR8  *Text,
  IN EFI_STATUS    Status
  )
{
  CHAR8  Buf[160];

  AsciiSPrint (Buf, sizeof (Buf), "%a -> %r", Text, Status);
  TxLine (Buf);
  mErr++;
}

STATIC
UINT32
RegRead (
  IN UINTN  Addr
  )
{
  return *(volatile UINT32 *)Addr;
}

STATIC
VOID
RegWrite (
  IN UINTN   Addr,
  IN UINT32  Value
  )
{
  *(volatile UINT32 *)Addr = Value;
}

/* ---------------------------------------------------------------- power ---- */

STATIC
VOID
PowerUpTouchRail (
  VOID
  )
{
  EFI_QCOM_PMIC_VREG_PROTOCOL  *Vreg;
  EFI_STATUS                    Status;
  EFI_PM_VREG_STATUS_TYPE       St;
  UINT32                        Index;
  CHAR8                         Buf[200];

  Vreg   = NULL;
  Status = gBS->LocateProtocol (&gQcomPmicVregProtocolGuid, NULL, (VOID **)&Vreg);

  AsciiSPrint (Buf, sizeof (Buf), "VREG: locate -> %r handle=%p", Status, Vreg);
  TxLine (Buf);
  Report ();

  if (EFI_ERROR (Status) || (Vreg == NULL)) {
    TxLine ("VREG: protocol unavailable, no rail control");
    return;
  }

  AsciiSPrint (
    Buf, sizeof (Buf),
    "VREG: Revision lo=0x%08x hi=0x%08x",
    (UINT32)(Vreg->Revision & 0xFFFFFFFFULL),
    (UINT32)(Vreg->Revision >> 32)
    );
  TxLine (Buf);
  Report ();

  //
  // Call the two oldest functions first (they sit at the front of the dispatch
  // table, so they exist in every revision of this protocol) and only touch
  // VregGetStatus/VregSetLevelUv afterwards - if the firmware implements an
  // older/shorter table those two land on garbage and take the board down,
  // which is exactly what happened last run.
  //
  for (Index = 0; Index < LMI_MAX_PMIC_INDEX; Index++) {
    AsciiSPrint (Buf, sizeof (Buf), "VREG: pmic[%u] VregControl(TRUE)...", Index);
    TxLine (Buf);
    Report ();

    Status = Vreg->VregControl (Index, LMI_TOUCH_VREG_ID, TRUE);

    AsciiSPrint (Buf, sizeof (Buf), "VREG:   Control -> %r", Status);
    TxLine (Buf);
    Report ();
  }

  for (Index = 0; Index < LMI_MAX_PMIC_INDEX; Index++) {
    AsciiSPrint (Buf, sizeof (Buf), "VREG: pmic[%u] VregSetLevel(1800 mV)...", Index);
    TxLine (Buf);
    Report ();

    Status = Vreg->VregSetLevel (Index, LMI_TOUCH_VREG_ID, 1800);

    AsciiSPrint (Buf, sizeof (Buf), "VREG:   SetLevel -> %r", Status);
    TxLine (Buf);
    Report ();
  }

  for (Index = 0; Index < LMI_MAX_PMIC_INDEX; Index++) {
    AsciiSPrint (Buf, sizeof (Buf), "VREG: pmic[%u] GetStatus...", Index);
    TxLine (Buf);
    Report ();

    ZeroMem (&St, sizeof (St));
    Status = Vreg->VregGetStatus (Index, LMI_TOUCH_VREG_ID, &St);

    AsciiSPrint (
      Buf, sizeof (Buf),
      "VREG:   -> %r swEn=%u ok=%u swMode=%u pinCtrled=%u pullDown=%u",
      Status, St.SwEnable, St.VregOk, St.SwMode, St.PinCtrled, St.PullDown
      );
    TxLine (Buf);
    Report ();
  }
}

/* ----------------------------------------------------------------- gpio ---- */

STATIC
VOID
MuxI2cPins (
  VOID
  )
{
  UINT32  Old;
  UINT32  New;
  CHAR8   Buf[160];
  UINTN   Pins[2];
  UINTN   i;

  Pins[0] = LMI_TOUCH_GPIO_SDA;
  Pins[1] = LMI_TOUCH_GPIO_SCL;

  for (i = 0; i < 2; i++) {
    Old = RegRead (TLMM_GPIO_CFG (Pins[i]));
    New = (Old & TLMM_MASK_PULL_FUNC) | (TLMM_FUNC_QUP13 << 2);
    RegWrite (TLMM_GPIO_CFG (Pins[i]), New);
    AsciiSPrint (
      Buf, sizeof (Buf),
      "TLMM gpio%u cfg: old=0x%08x new=0x%08x readback=0x%08x",
      Pins[i], Old, New, RegRead (TLMM_GPIO_CFG (Pins[i]))
      );
    TxLine (Buf);
  }
}

STATIC
VOID
DriveGpio (
  IN EFI_QCOM_TLMM_PROTOCOL  *Tlmm,
  IN UINT32                   Pin,
  IN CONST CHAR8             *Name
  )
{
  EFI_STATUS  Status;
  UINT32      Cfg;
  CHAR8       Buf[160];

  Cfg    = EFI_GPIO_CFG (Pin, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA);
  Status = Tlmm->ConfigGpio (Cfg, TLMM_GPIO_ENABLE);
  AsciiSPrint (Buf, sizeof (Buf), "%a gpio%u output -> %r", Name, Pin, Status);
  TxLine (Buf);

  Status = Tlmm->GpioOut (Cfg, GPIO_HIGH_VALUE);
  AsciiSPrint (Buf, sizeof (Buf), "%a gpio%u high   -> %r", Name, Pin, Status);
  TxLine (Buf);
}

/* ------------------------------------------------------------------ i2c ---- */

STATIC
VOID
ReadReg (
  IN EFI_QCOM_I2C_PROTOCOL  *I2c,
  IN VOID                   *Handle,
  IN I2C_SLAVE_CONFIG       *Cfg,
  IN UINT8                   Reg,
  IN CONST CHAR8            *What
  )
{
  UINT8      Buf[8];
  UINT32     Read;
  I2C_STATUS St;
  CHAR8      Line[200];

  ZeroMem (Buf, sizeof (Buf));
  Read = 0;
  St   = I2c->Read (Handle, Cfg, Reg, 1, Buf, 8, &Read, 300);
  AsciiSPrint (
    Line, sizeof (Line),
    "I2C %a reg 0x%02x: status=%d read=%u data=%02x %02x %02x %02x %02x %02x %02x %02x",
    What, Reg, St, Read,
    Buf[0], Buf[1], Buf[2], Buf[3], Buf[4], Buf[5], Buf[6], Buf[7]
    );
  TxLine (Line);
}

STATIC
VOID
ProbeTouchChip (
  VOID
  )
{
  EFI_QCOM_I2C_PROTOCOL  *I2c;
  EFI_STATUS              Status;
  VOID                   *Handle;
  I2C_SLAVE_CONFIG        SlaveCfg;
  UINT8                   Buf[4];
  UINT32                  Read;
  I2C_STATUS              I2cStatus;
  CHAR8                   Line[200];
  UINT32                  Se;
  UINT32                  Try[4] = { 13, 10, 4, 0 };
  UINT32                  FreqTry[2];
  UINTN                   i;
  UINTN                   f;

  I2c    = NULL;
  Handle = NULL;
  I2cStatus = I2C_ERROR_ADDR_NACK;   // any non-success value; overwritten below
  Status = gBS->LocateProtocol (&gQcomI2cProtocolGuid, NULL, (VOID **)&I2c);
  if (EFI_ERROR (Status) || (I2c == NULL)) {
    TxFail ("I2C protocol missing", Status);
    return;
  }

  for (i = 0; i < 4; i++) {
    Se     = Try[i];
    Handle = NULL;
    I2cStatus = I2c->Open (Se, &Handle);
    AsciiSPrint (Line, sizeof (Line), "I2C Open(SE%u) -> status=%d handle=%p", Se, I2cStatus, Handle);
    TxLine (Line);
    if ((I2cStatus == I2C_SUCCESS) && (Handle != NULL)) {
      break;
    }
  }

  if ((I2cStatus != I2C_SUCCESS) || (Handle == NULL)) {
    TxLine ("I2C: no instance opened, link test skipped");
    return;
  }

  ZeroMem (&SlaveCfg, sizeof (SlaveCfg));
  SlaveCfg.SlaveAddress         = LMI_TOUCH_I2C_ADDR;
  SlaveCfg.Mode                 = I2C;
  SlaveCfg.SlaveMaxClockStretch = 0;

  //
  // The header claims kHz, some Qualcomm ports take Hz.  Try both rather than
  // burn a whole boot cycle on a units mismatch.
  //
  FreqTry[0] = 400;
  FreqTry[1] = 400000;
  for (f = 0; f < 2; f++) {
    SlaveCfg.BusFrequency = FreqTry[f];
    ZeroMem (Buf, sizeof (Buf));
    Read      = 0;
    I2cStatus = I2c->Read (Handle, &SlaveCfg, LMI_TOUCH_REG_CHIPID, 1, Buf, 1, &Read, 300);
    AsciiSPrint (
      Line, sizeof (Line),
      "I2C chip id reg 0x%02x @%u = 0x%02x (want 0x%02x) status=%d read=%u",
      LMI_TOUCH_REG_CHIPID, FreqTry[f], Buf[0], LMI_TOUCH_CHIPID_FTS, I2cStatus, Read
      );
    TxLine (Line);
    if ((I2cStatus == I2C_SUCCESS) && (Read == 1)) {
      break;
    }
  }

  ReadReg (I2c, Handle, &SlaveCfg, 0xA8, "fw/app");
  ReadReg (I2c, Handle, &SlaveCfg, 0x00, "start");

  if ((I2cStatus == I2C_SUCCESS) && (Buf[0] == LMI_TOUCH_CHIPID_FTS)) {
    TxLine ("RESULT: TOUCH I2C LINK OK");
  } else {
    TxLine ("RESULT: TOUCH I2C LINK FAILED");
  }

  I2c->Close (Handle);
}

/* --------------------------------------------------------------- report ---- */

STATIC EFI_FILE_PROTOCOL  *mRoot  = NULL;    // root of the volume holding Windows
STATIC BOOLEAN             mEspOk = FALSE;

STATIC
EFI_STATUS
ExistOnVolume (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;

  File   = NULL;
  Status = Root->Open (Root, &File, FILE_NAME (Path), EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status) && (File != NULL)) {
    File->Close (File);
  }

  return Status;
}

STATIC
VOID
FindEsp (
  VOID
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                             Count;
  UINTN                             Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  CHAR8                             Line[200];

  mRoot  = NULL;
  mEspOk = FALSE;
  Handles = NULL;
  Count   = 0;

  if (EFI_ERROR (gBS->LocateHandleBuffer (
                        ByProtocol,
                        &gEfiSimpleFileSystemProtocolGuid,
                        NULL,
                        &Count,
                        &Handles
                        )))
  {
    TxLine ("ESP: no file system handles");
    return;
  }

  AsciiSPrint (Line, sizeof (Line), "ESP: %u volume(s)", (UINT32)Count);
  TxLine (Line);

  //
  // Pass 1: the volume that actually carries a Windows loader.  The firmware
  // may start bootmgfw.efi or (more likely here) \EFI\BOOT\BOOTAA64.EFI.
  //
  for (Index = 0; Index < Count; Index++) {
    Fs   = NULL;
    Root = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
      continue;
    }
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || (Root == NULL)) {
      continue;
    }
    if (!EFI_ERROR (ExistOnVolume (Root, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"))) {
      TxLine ("ESP: matched bootmgfw.efi");
      mRoot = Root;
      break;
    }
    if (!EFI_ERROR (ExistOnVolume (Root, L"\\EFI\\BOOT\\BOOTAA64.EFI"))) {
      TxLine ("ESP: matched BOOTAA64.EFI");
      mRoot = Root;
      break;
    }
    Root->Close (Root);
  }

  //
  // Pass 2: last resort - any volume that even has an \EFI directory.
  //
  if (mRoot == NULL) {
    for (Index = 0; Index < Count; Index++) {
      Fs   = NULL;
      Root = NULL;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
        continue;
      }
      if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || (Root == NULL)) {
        continue;
      }
      if (!EFI_ERROR (ExistOnVolume (Root, L"\\EFI"))) {
        TxLine ("ESP: fallback volume with \\EFI");
        mRoot = Root;
        break;
      }
      Root->Close (Root);
    }
  }

  gBS->FreePool (Handles);

  mEspOk = (mRoot != NULL);
  if (!mEspOk) {
    TxLine ("ESP: not found, telemetry unavailable");
  }
}

STATIC
BOOLEAN
CfgFlag (
  IN CONST CHAR8  *Buf,
  IN UINTN         Len,
  IN CONST CHAR8  *Key,
  IN BOOLEAN       Default
  )
{
  UINTN  KeyLen;
  UINTN  i;

  KeyLen = AsciiStrLen (Key);
  for (i = 0; i + KeyLen + 1 < Len; i++) {
    if (AsciiStrnCmp (Buf + i, Key, KeyLen) != 0) {
      continue;
    }
    if (Buf[i + KeyLen] != '=') {
      continue;
    }
    if (Buf[i + KeyLen + 1] == '1') {
      return TRUE;
    }
    if (Buf[i + KeyLen + 1] == '0') {
      return FALSE;
    }
  }

  return Default;
}

STATIC
VOID
LoadCfg (
  OUT BOOLEAN  *Mux,
  OUT BOOLEAN  *Gpio,
  OUT BOOLEAN  *Vreg,
  OUT BOOLEAN  *I2c,
  OUT BOOLEAN  *Reset
  )
{
  EFI_FILE_PROTOCOL  *File;
  EFI_STATUS          Status;
  CHAR8               Buf[257];
  CHAR8               Line[200];
  UINTN               Size;

  *Mux   = TRUE;
  *Gpio  = TRUE;
  *Vreg  = TRUE;
  *I2c   = TRUE;
  *Reset = (LMI_PROBE_ALWAYS_RESET != 0);

  if (mRoot == NULL) {
    return;
  }

  File   = NULL;
  Status = mRoot->Open (mRoot, &File, FILE_NAME (L"\\LMI_CFG.TXT"), EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || (File == NULL)) {
    TxLine ("CFG: no LMI_CFG.TXT, defaults (all steps on)");
    return;
  }

  ZeroMem (Buf, sizeof (Buf));
  Size = 256;
  File->Read (File, &Size, Buf);
  File->Close (File);

  *Mux   = CfgFlag (Buf, 256, "MUX",   *Mux);
  *Gpio  = CfgFlag (Buf, 256, "GPIO",  *Gpio);
  *Vreg  = CfgFlag (Buf, 256, "VREG",  *Vreg);
  *I2c   = CfgFlag (Buf, 256, "I2C",   *I2c);
  *Reset = CfgFlag (Buf, 256, "RESET", *Reset);

  AsciiSPrint (
    Line, sizeof (Line),
    "CFG: MUX=%u GPIO=%u VREG=%u I2C=%u RESET=%u",
    *Mux ? 1 : 0, *Gpio ? 1 : 0, *Vreg ? 1 : 0, *I2c ? 1 : 0, *Reset ? 1 : 0
    );
  TxLine (Line);
}

//
// Both channels carry the whole transcript, so whichever one survives tells us
// how far the driver got.  Raw block I/O is done first: it needs no FAT code.
//
STATIC
VOID
WriteRawTail (
  VOID
  )
{
  EFI_HANDLE             *Handles;
  UINTN                   Count;
  UINTN                   Index;
  EFI_BLOCK_IO_PROTOCOL  *Blk;
  CHAR8                   Line[160];

  Handles = NULL;
  Count   = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Blk = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Blk))) {
      continue;
    }
    if ((Blk->Media == NULL) || (Blk->Media->LastBlock != LMI_ESP_SECTORS)) {
      continue;
    }
    Blk->WriteBlocks (Blk, Blk->Media->MediaId, LMI_ESP_SECTORS - 64, 4096, mTx);
    AsciiSPrint (Line, sizeof (Line), "RAW: tail write of %u bytes", (UINT32)mTxLen);
    TxLine (Line);
    break;
  }

  gBS->FreePool (Handles);
}

STATIC
VOID
Report (
  VOID
  )
{
  EFI_FILE_PROTOCOL  *File;
  EFI_STATUS          Status;
  UINTN               Size;

  WriteRawTail ();

  if (mRoot == NULL) {
    return;
  }

  //
  // delete + create so a shorter report never leaves stale tail bytes
  //
  File = NULL;
  if (!EFI_ERROR (mRoot->Open (mRoot, &File, FILE_NAME (LMI_TX_FILE),
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))
      && (File != NULL))
  {
    File->Delete (File);
  }

  File   = NULL;
  Status = mRoot->Open (
                    mRoot,
                    &File,
                    FILE_NAME (LMI_TX_FILE),
                    EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ,
                    0
                    );
  if (!EFI_ERROR (Status) && (File != NULL)) {
    Size = mTxLen;
    File->Write (File, &Size, mTx);
    File->Flush (File);
    File->Close (File);
  }
}

/* ------------------------------------------------------------------ main --- */

//
// Rewrite RESET=1 as RESET=0 in the config before resetting: a boot that dies
// before it can report must never turn into a reset loop.
//
STATIC
VOID
ClearResetFlag (
  VOID
  )
{
  EFI_FILE_PROTOCOL  *File;
  EFI_STATUS          Status;
  CHAR8               Buf[257];
  UINTN               Size;
  UINTN               i;

  if (mRoot == NULL) {
    return;
  }

  File   = NULL;
  Status = mRoot->Open (mRoot, &File, FILE_NAME (L"\\LMI_CFG.TXT"),
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (EFI_ERROR (Status) || (File == NULL)) {
    return;
  }

  ZeroMem (Buf, sizeof (Buf));
  Size = 256;
  Status = File->Read (File, &Size, Buf);
  if (!EFI_ERROR (Status)) {
    for (i = 0; i + 7 < Size; i++) {
      if ((AsciiStrnCmp (Buf + i, "RESET", 5) == 0) && (Buf[i + 5] == '=') && (Buf[i + 6] == '1')) {
        Buf[i + 6] = '0';
        File->SetPosition (File, 0);
        Size = (Size < 256) ? 256 : Size;
        File->Write (File, &Size, Buf);
        File->Flush (File);
        break;
      }
    }
  }

  File->Close (File);
}

//
// Scheduling.  The driver must run after the FAT stack is up (otherwise there
// is no way to persist the transcript) but before Windows starts.  A protocol
// notify is useless here: if this driver is dispatched late - and on this
// platform it is, because the PMIC VREG protocol only appears during BDS's
// connect-all pass - the protocols are already installed and the notification
// never fires.  So: run right away if a volume exists, otherwise wait for
// ReadyToBoot (which BDS signals after everything is connected, immediately
// before loading Windows) with a periodic timer as the last resort.
//
STATIC EFI_EVENT  mReadyEvent = NULL;
STATIC EFI_EVENT  mTimer      = NULL;
STATIC UINTN      mTicks      = 0;
STATIC BOOLEAN    mWorkDone   = FALSE;

STATIC
VOID
DoWork (
  VOID
  );

STATIC
BOOLEAN
FsPresent (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  UINTN        Count;
  BOOLEAN      Present;

  Handles = NULL;
  Count   = 0;
  Present = FALSE;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (
                        ByProtocol,
                        &gEfiSimpleFileSystemProtocolGuid,
                        NULL,
                        &Count,
                        &Handles
                        )))
  {
    Present = (Count > 0);
    gBS->FreePool (Handles);
  }

  return Present;
}

STATIC
VOID
EFIAPI
OnDeferredWork (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  if (mWorkDone) {
    return;
  }

  mWorkDone = TRUE;

  if (mReadyEvent != NULL) {
    gBS->CloseEvent (mReadyEvent);
    mReadyEvent = NULL;
  }

  if (mTimer != NULL) {
    gBS->CloseEvent (mTimer);
    mTimer = NULL;
  }

  FindEsp ();
  DoWork ();
}

STATIC
VOID
EFIAPI
OnTimer (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  CHAR8  Line[120];

  if (mWorkDone) {
    return;
  }

  mTicks++;
  if ((mTicks % 5) == 0) {
    AsciiSPrint (Line, sizeof (Line), "WAIT: %u s, file system present=%u, esp=%u",
                 (UINT32)mTicks, FsPresent () ? 1 : 0, mEspOk ? 1 : 0);
    LmiPrint (L"LMI: %a\r\n", Line);
  }

  //
  // ~15 s without ReadyToBoot means this boot is not going to reach Windows in
  // a normal way; power the panel anyway rather than leave it dead.
  //
  if (mTicks >= 15) {
    OnDeferredWork (Event, Context);
  }
}

STATIC
VOID
DoWork (
  VOID
  )
{
  EFI_QCOM_TLMM_PROTOCOL  *Tlmm;
  EFI_STATUS               Status;
  BOOLEAN                  Mux;
  BOOLEAN                  Gpio;
  BOOLEAN                  Vreg;
  BOOLEAN                  I2c;
  BOOLEAN                  Reset;

  TxLine ("LMI-TOUCH-PROBE v3");
  if (!mEspOk) {
    TxLine ("WARN: no volume, running blind (console only)");
  }

  LoadCfg (&Mux, &Gpio, &Vreg, &I2c, &Reset);
  TxLine ("HEARTBEAT: telemetry channel open");
  Report ();

  if (Vreg) {
    TxLine ("STEP: VREG (pm8150a LDO1 1.8V)");
    PowerUpTouchRail ();
    Report ();
  }

  if (Mux) {
    TxLine ("STEP: TLMM mux GPIO36/37 -> qup13");
    MuxI2cPins ();
    Report ();
  }

  if (Gpio) {
    TxLine ("STEP: GPIO72 vdd + GPIO38 reset");
    Tlmm   = NULL;
    Status = gBS->LocateProtocol (&gQcomTlmmProtocolGuid, NULL, (VOID **)&Tlmm);
    if (EFI_ERROR (Status) || (Tlmm == NULL)) {
      TxFail ("TLMM protocol missing", Status);
    } else {
      DriveGpio (Tlmm, LMI_TOUCH_GPIO_VDD, "VDD");
      gBS->Stall (20000);
      DriveGpio (Tlmm, LMI_TOUCH_GPIO_RST, "RST");
    }
    Report ();
  }

  if (I2c) {
    TxLine ("STEP: I2C self test");
    //
    // panel needs ~100 ms after reset release before it answers on I2C
    //
    gBS->Stall (120000);
    ProbeTouchChip ();
    Report ();
  }

  TxLine ("DONE: all requested steps finished");
  Report ();

  LmiPrint (
    L"LMI: LmiTouchPower done (errors=%u, reset=%u, esp=%u)\r\n\r\n",
    (UINT32)mErr, Reset ? 1 : 0, mEspOk ? 1 : 0
    );

  if (Reset) {
    ClearResetFlag ();
    gBS->Stall (3000000);
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  }
}

EFI_STATUS
EFIAPI
LmiTouchPowerEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  LmiPrint (L"\r\nLMI: LmiTouchPower v3 entry\r\n");

  if (FsPresent ()) {
    LmiPrint (L"LMI: volume already mounted, running now\r\n");
    OnDeferredWork (NULL, NULL);
    return EFI_SUCCESS;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnDeferredWork,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mReadyEvent
                  );
  if (EFI_ERROR (Status)) {
    mReadyEvent = NULL;
  }

  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, OnTimer, NULL, &mTimer);
  if (!EFI_ERROR (Status)) {
    gBS->SetTimer (mTimer, TimerPeriodic, 10000000);   // 1 s
  } else {
    mTimer = NULL;
  }

  if ((mReadyEvent == NULL) && (mTimer == NULL)) {
    LmiPrint (L"LMI: no way to defer, running now\r\n");
    OnDeferredWork (NULL, NULL);
  } else {
    LmiPrint (L"LMI: deferred until ReadyToBoot\r\n");
  }

  return EFI_SUCCESS;
}
