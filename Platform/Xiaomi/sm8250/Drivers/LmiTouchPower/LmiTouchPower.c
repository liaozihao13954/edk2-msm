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
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Protocol/EFIPmicVreg.h>
#include <Protocol/EFITlmm.h>
#include <Protocol/EFII2C.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>

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
  UINT32                        Voltage;
  CHAR8                         Buf[200];

  Vreg   = NULL;
  Status = gBS->LocateProtocol (&gQcomPmicVregProtocolGuid, NULL, (VOID **)&Vreg);
  if (EFI_ERROR (Status) || (Vreg == NULL)) {
    TxFail ("VREG protocol missing", Status);
    return;
  }

  for (Index = 0; Index < LMI_MAX_PMIC_INDEX; Index++) {
    ZeroMem (&St, sizeof (St));
    Voltage = 0;

    if (EFI_ERROR (Vreg->VregGetStatus (Index, LMI_TOUCH_VREG_ID, &St))) {
      continue;
    }

    Vreg->VregGetLevel (Index, LMI_TOUCH_VREG_ID, &Voltage);
    AsciiSPrint (
      Buf, sizeof (Buf),
      "VREG pmic[%u] LDO1 before: %u uV swEn=%u ok=%u",
      Index, Voltage, St.SwEnable, St.VregOk
      );
    TxLine (Buf);

    Status = Vreg->VregSetLevelUv (Index, LMI_TOUCH_VREG_ID, LMI_TOUCH_VREG_UV);
    if (EFI_ERROR (Status)) {
      TxFail ("  VregSetLevelUv(1.8V)", Status);
    }

    Status = Vreg->VregControl (Index, LMI_TOUCH_VREG_ID, TRUE);
    if (EFI_ERROR (Status)) {
      TxFail ("  VregControl(enable)", Status);
    }

    ZeroMem (&St, sizeof (St));
    Voltage = 0;
    Vreg->VregGetLevel (Index, LMI_TOUCH_VREG_ID, &Voltage);
    Vreg->VregGetStatus (Index, LMI_TOUCH_VREG_ID, &St);
    AsciiSPrint (
      Buf, sizeof (Buf),
      "VREG pmic[%u] LDO1 after : %u uV swEn=%u ok=%u",
      Index, Voltage, St.SwEnable, St.VregOk
      );
    TxLine (Buf);
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
BOOLEAN
CheckProbeFlag (
  IN  EFI_FILE_PROTOCOL  *Root,
  OUT EFI_STATUS         *DeleteStatus
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;

  *DeleteStatus = EFI_NOT_FOUND;
  File          = NULL;
  Status        = Root->Open (
                           Root,
                           &File,
                           FILE_NAME (LMI_PROBE_FLAG),
                           EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                           0
                           );
  if (EFI_ERROR (Status) || (File == NULL)) {
    return FALSE;
  }

  *DeleteStatus = File->Delete (File);
  return TRUE;
}

STATIC
VOID
WriteReport (
  OUT BOOLEAN  *DidReset
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                             Count;
  UINTN                             Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_BLOCK_IO_PROTOCOL            *Blk;
  CHAR8                             Line[200];
  BOOLEAN                           Found;
  UINTN                             Size;

  *DidReset = FALSE;
  Handles   = NULL;
  Count     = 0;
  Found     = FALSE;
  Root      = NULL;

  TxAdd ("TX-BYTES: %u\r\nEND\r\n", (UINT32)mTxLen);

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    TxFail ("no file system handles", Status);
  } else {
    for (Index = 0; Index < Count; Index++) {
      Fs   = NULL;
      Root = NULL;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
        continue;
      }
      if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || (Root == NULL)) {
        continue;
      }
      if (EFI_ERROR (ExistOnVolume (Root, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"))) {
        Root->Close (Root);
        Root = NULL;
        continue;
      }
      Found = TRUE;
      break;
    }
  }

  if (Found && (Root != NULL)) {
    TxLine ("ESP: found (volume holding \\EFI\\Microsoft\\Boot\\bootmgfw.efi)");

    //
    // probe flag first: delete it before anything else so a failed run can
    // never turn into a boot loop.
    //
    if (CheckProbeFlag (Root, &Status)) {
      *DidReset = !EFI_ERROR (Status);
      AsciiSPrint (Line, sizeof (Line), "ESP: probe flag present, delete -> %r", Status);
      TxLine (Line);
    }

    File   = NULL;
    Status = Root->Open (Root, &File, FILE_NAME (LMI_TX_FILE), EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR (Status) && (File != NULL)) {
      File->Delete (File);
    }

    File   = NULL;
    Status = Root->Open (
                     Root,
                     &File,
                     FILE_NAME (LMI_TX_FILE),
                     EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ,
                     0
                     );
    if (!EFI_ERROR (Status) && (File != NULL)) {
      Size   = mTxLen;
      Status = File->Write (File, &Size, mTx);
      File->Flush (File);
      File->Close (File);
      AsciiSPrint (Line, sizeof (Line), "ESP: wrote LMI_TX.TXT (%u bytes) -> %r", (UINT32)Size, Status);
      TxLine (Line);
    } else {
      TxFail ("ESP: create LMI_TX.TXT", Status);
    }

    Root->Close (Root);
  } else {
    TxLine ("ESP: not found, no file telemetry");
  }

  if (Handles != NULL) {
    gBS->FreePool (Handles);
  }

  //
  // Backup channel: raw sectors at the tail of the 512 MiB ESP.  Independent
  // of the FAT driver, readable with plain `dd` from Android/TWRP.
  //
  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < Count; Index++) {
      Blk = NULL;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Blk))) {
        continue;
      }
      if ((Blk->Media == NULL) || (Blk->Media->LastBlock != LMI_ESP_SECTORS)) {
        continue;
      }
      Status = Blk->WriteBlocks (Blk, Blk->Media->MediaId, LMI_ESP_SECTORS - 16, 1024, mTx);
      AsciiSPrint (Line, sizeof (Line), "RAW: esp tail write -> %r", Status);
      TxLine (Line);
      break;
    }
    gBS->FreePool (Handles);
  }
}

/* ------------------------------------------------------------------ main --- */

EFI_STATUS
EFIAPI
LmiTouchPowerEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_QCOM_TLMM_PROTOCOL  *Tlmm;
  EFI_STATUS               Status;
  BOOLEAN                  DoReset;

  LmiPrint (L"\r\nLMI: LmiTouchPower v2 start\r\n");
  TxLine ("LMI-TOUCH-PROBE v2");

  PowerUpTouchRail ();

  MuxI2cPins ();

  Tlmm   = NULL;
  Status = gBS->LocateProtocol (&gQcomTlmmProtocolGuid, NULL, (VOID **)&Tlmm);
  if (EFI_ERROR (Status) || (Tlmm == NULL)) {
    TxFail ("TLMM protocol missing", Status);
  } else {
    DriveGpio (Tlmm, LMI_TOUCH_GPIO_VDD, "VDD");
    gBS->Stall (20000);
    DriveGpio (Tlmm, LMI_TOUCH_GPIO_RST, "RST");
  }

  //
  // panel needs ~100 ms after reset release before it answers on I2C
  //
  gBS->Stall (120000);
  ProbeTouchChip ();

  WriteReport (&DoReset);

  LmiPrint (L"LMI: LmiTouchPower done (errors=%u, reset=%u)\r\n\r\n", (UINT32)mErr, DoReset ? 1 : 0);

  if (DoReset) {
    gBS->Stall (3000000);
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  }

  return EFI_SUCCESS;
}
