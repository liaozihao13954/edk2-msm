/** @file
  LmiTouchPower - power up the FTS touch panel and prove the I2C link works,
  before Windows takes over.

  Why this exists
  ---------------
  The FocalTech panel on lmi is fed by pm8150a LDO1 (avdd, 1.8 V) plus the
  "vdd-gpio" enable line on TLMM GPIO 72.  In Android the fts_ts driver enables
  both at probe time: the RPMh regulator node in DT has neither
  regulator-always-on nor regulator-boot-on, so nothing else ever turns the rail
  on.  Windows ships no RPMh regulator driver, therefore by the time Windows
  probes the ACPI I2C-HID device (PNP0C50) the panel has no power and every I2C
  transaction times out - which is exactly the symptom we chased for days.

  What it does
  ------------
    1. waits for the Qualcomm PMIC VREG protocol, sets LDO1 to 1.8 V, enables it
    2. drives TLMM GPIO 72 high (func = GPIO, output, no pull, 2 mA)
    3. opens I2C SE13 @ 0x38 and reads register 0xA3 (FTS chip id, expect 0x54)

  Step 3 is the self-test: its result is printed to the UEFI console, so a
  successful read proves rail + pinmux + clock + I2C controller are all good and
  whatever still fails afterwards is Windows-side only.

  Note: GPIO 36/37 (I2C SDA/SCL, function "qup13" = 119) cannot be muxed through
  EFI_QCOM_TLMM_PROTOCOL because EFI_GPIO_CFG only carries 4 bits of func.  Those
  are programmed by the DSDT _INI via a raw TLMM register write instead.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Protocol/EFIPmicVreg.h>
#include <Protocol/EFITlmm.h>
#include <Protocol/EFII2C.h>

#define LMI_TOUCH_VREG_ID     EFI_PM_LDO_1
#define LMI_TOUCH_VREG_UV     1800000
#define LMI_MAX_PMIC_INDEX    6

#define LMI_TOUCH_GPIO_VDD    72
#define LMI_TOUCH_I2C_SE      13
#define LMI_TOUCH_I2C_ADDR    0x38
#define LMI_TOUCH_REG_CHIPID  0xA3
#define LMI_TOUCH_CHIPID_FTS  0x54

STATIC
VOID
LmiPrint (
  IN CONST CHAR16  *Format,
  ...
  )
{
  CHAR16  Buf[160];
  VA_LIST Marker;

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
PowerUpTouchRail (
  VOID
  )
{
  EFI_QCOM_PMIC_VREG_PROTOCOL  *Vreg;
  EFI_STATUS                    Status;
  EFI_PM_VREG_STATUS_TYPE       VregStatus;
  UINT32                        Index;
  UINT32                        Voltage;

  Vreg   = NULL;
  Status = gBS->LocateProtocol (&gQcomPmicVregProtocolGuid, NULL, (VOID **)&Vreg);
  if (EFI_ERROR (Status) || (Vreg == NULL)) {
    LmiPrint (L"LMI: PMIC VREG protocol missing (%r)\r\n", Status);
    return;
  }

  for (Index = 0; Index < LMI_MAX_PMIC_INDEX; Index++) {
    ZeroMem (&VregStatus, sizeof (VregStatus));
    Voltage = 0;

    if (EFI_ERROR (Vreg->VregGetStatus (Index, LMI_TOUCH_VREG_ID, &VregStatus))) {
      continue;
    }

    Vreg->VregGetLevel (Index, LMI_TOUCH_VREG_ID, &Voltage);
    LmiPrint (
      L"LMI: pmic[%u] LDO1 level=%u uV swEn=%u ok=%u\r\n",
      Index, Voltage, VregStatus.SwEnable, VregStatus.VregOk
      );

    Status = Vreg->VregSetLevelUv (Index, LMI_TOUCH_VREG_ID, LMI_TOUCH_VREG_UV);
    if (EFI_ERROR (Status)) {
      LmiPrint (L"LMI:   set 1.8V failed (%r)\r\n", Status);
    }

    Status = Vreg->VregControl (Index, LMI_TOUCH_VREG_ID, TRUE);
    LmiPrint (L"LMI:   enable -> %r\r\n", Status);
  }
}

STATIC
VOID
DriveVddGpio (
  VOID
  )
{
  EFI_QCOM_TLMM_PROTOCOL  *Tlmm;
  EFI_STATUS               Status;
  UINT32                   Cfg;

  Tlmm   = NULL;
  Status = gBS->LocateProtocol (&gQcomTlmmProtocolGuid, NULL, (VOID **)&Tlmm);
  if (EFI_ERROR (Status) || (Tlmm == NULL)) {
    LmiPrint (L"LMI: TLMM protocol missing (%r)\r\n", Status);
    return;
  }

  //
  // func 0 = plain GPIO, output, no pull, 2 mA
  //
  Cfg    = EFI_GPIO_CFG (LMI_TOUCH_GPIO_VDD, 0, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_2MA);
  Status = Tlmm->ConfigGpio (Cfg, TLMM_GPIO_ENABLE);
  LmiPrint (L"LMI: gpio%u config -> %r\r\n", LMI_TOUCH_GPIO_VDD, Status);

  Status = Tlmm->GpioOut (Cfg, GPIO_HIGH_VALUE);
  LmiPrint (L"LMI: gpio%u high -> %r\r\n", LMI_TOUCH_GPIO_VDD, Status);
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
  UINT8                   Buffer[1];
  UINT32                  Read;
  I2C_STATUS              I2cStatus;

  I2c    = NULL;
  Handle = NULL;
  Status = gBS->LocateProtocol (&gQcomI2CProtocolGuid, NULL, (VOID **)&I2c);
  if (EFI_ERROR (Status) || (I2c == NULL)) {
    LmiPrint (L"LMI: I2C protocol missing (%r)\r\n", Status);
    return;
  }

  I2cStatus = I2c->Open (LMI_TOUCH_I2C_SE, &Handle);
  LmiPrint (L"LMI: i2c SE%u open -> %d\r\n", LMI_TOUCH_I2C_SE, I2cStatus);
  if ((I2cStatus != I2C_SUCCESS) || (Handle == NULL)) {
    return;
  }

  ZeroMem (&SlaveCfg, sizeof (SlaveCfg));
  SlaveCfg.BusFrequency         = I2C_FAST_MODE_FREQ_KHZ;
  SlaveCfg.SlaveAddress         = LMI_TOUCH_I2C_ADDR;
  SlaveCfg.Mode                 = I2C;
  SlaveCfg.SlaveMaxClockStretch = 0;

  ZeroMem (Buffer, sizeof (Buffer));
  Read      = 0;
  I2cStatus = I2c->Read (
                      Handle,
                      &SlaveCfg,
                      LMI_TOUCH_REG_CHIPID,
                      1,
                      Buffer,
                      sizeof (Buffer),
                      &Read,
                      200
                      );

  LmiPrint (
    L"LMI: chip id @0x%02x = 0x%02x (want 0x%02x) status=%d read=%u\r\n",
    LMI_TOUCH_REG_CHIPID,
    Buffer[0],
    LMI_TOUCH_CHIPID_FTS,
    I2cStatus,
    Read
    );

  if ((I2cStatus == I2C_SUCCESS) && (Buffer[0] == LMI_TOUCH_CHIPID_FTS)) {
    LmiPrint (L"LMI: TOUCH I2C LINK OK\r\n");
  } else {
    LmiPrint (L"LMI: TOUCH I2C LINK FAILED\r\n");
  }

  I2c->Close (Handle);
}

EFI_STATUS
EFIAPI
LmiTouchPowerEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  LmiPrint (L"\r\nLMI: LmiTouchPower start\r\n");
  PowerUpTouchRail ();
  DriveVddGpio ();
  ProbeTouchChip ();
  LmiPrint (L"LMI: LmiTouchPower done\r\n\r\n");

  return EFI_SUCCESS;
}
