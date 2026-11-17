//------------------------------------------------------------------------------
// win_touch.asl - FocalTech (FTS) touch panel on lmi, HID-over-I2C attempt.
//
// Panel measured on THIS device from Android (2026-11-15):
//   getevent -p   : event4 "fts_ts"  KEYBOARD|TOUCH|TOUCH_MT
//   dumpsys input : /sys/devices/platform/soc/a94000.i2c/i2c-4/4-0038
//                   bus=0x0018 (I2C) -> slave address 0x38
//   lmi.dtb       : /soc/i2c@a94000 reg 0xA94000/0x4000, interrupts [0,358,4]
//                   GIC SPI 358 -> ACPI GSI 390  (matches I2C0 _CRS)
//   uinput-goodix : under-display FINGERPRINT, not touch
//
// Revision history:
//   v1  Goodix @0x5D  -> wrong panel, could never enumerate.
//   v2  FTS @0x38 + GpioInt(Edge, ExclusiveAndWake) + GpioIo(reset) + _DSM
//       -> Windows ACPI.sys dropped the device entirely (Enum\ACPI had no
//          PNP0C50) while PNP0D10/QCOM250D/QCOM2511 from the same DSDT did
//          enumerate. Cause: GpioIo not resolvable by qcgpio -> whole _CRS
//          invalid -> no devnode.
//   v3  dropped GpioIo, GpioInt -> Level/ActiveLow/Exclusive, dropped _DSM,
//       _HID via EisaId. Result: PNP0C50 enumerates, hidi2c binds,
//       ConfigFlags=0, IRQ allocated -- but Enum\HID still absent, i.e. the
//       I2C-HID descriptor read fails.
//   v4  (this file) + TLMM pin mux fix, see below.
//
// WHY v4: Windows has no concept of pin function select. GpioClx only does
// direction / pull / interrupt; qcgpio.sys has no mux code and qci2c8250.sys
// has no GPIO imports at all. `fastboot boot` goes through XBL/ABL, so
// TLMM is reset to default function and SDA/SCL stay plain GPIO -> every I2C
// transaction to 0x38 NAKs -> no HID descriptor -> no touch.
//
// Mux values read out of lmi's OWN Android kernel (android_kernel.bin,
// link base 0xFFFFFF8008080000), not guessed:
//   msm_function[] table @0x1D29C50, 176 entries {name, groups, ngroups}:
//        index  98 = "gpio"
//        index 119 = "qup13"
//   msm_pingroup for gpio36 @0x1D2CA60 -> .pins = {36}, .nfuncs = 10,
//        .funcs = [98, 119, 120, 176 x7]  -> mux(qup13) = 119 = 0x77
//   lmi.dtb /soc/pinctrl@f000000/qupv3_se13_i2c_pins/qupv3_se13_i2c_active:
//        pins = gpio36, gpio37 ; function = "qup13" ;
//        drive-strength = 2 ; bias-disable
//
// TLMM GPIO_CFGn field layout (kona/SM8250):
//   [1:0]   PULL      0=no pull, 1=pull-down, 2=keeper, 3=pull-up
//   [9:2]   FUNC_SEL  8 bits  (176 functions exist -> 4 bits cannot work;
//                     kernel does `and w, w, #0xFFFFFC03` = keep [1:0],
//                     clear [9:2], i.e. exactly this field)
//   [12:10] DRV_STR   preserved (dtb drive-strength 2 = "least" anyway)
//   [13]    OE        preserved
//
// So: read-modify-write, clear PULL+FUNC_SEL, write FUNC_SEL=119, PULL=0
// (bias-disable: the board has external I2C pull-ups).
//     0x77 << 2 = 0x1DC
//------------------------------------------------------------------------------

Device (TPD0)
{
    // FTS0001 as the hardware ID: hardware IDs outrank compatible IDs during
    // driver ranking, so the injected FocalTechTouch.inf (which declares
    // ACPI\FTS0001) binds here instead of the inbox hidi2c.
    // _CID = PNP0C50 is kept so the PnP/power path treats this as a standard
    // I2C-HID device.  Without it the FTS0001 node drove qci2c into a
    // PoFx assertion (0xA0 INTERNAL_POWER_ERROR, P1=0xBEEF) during device start.
    Name (_HID, "FTS0001")
    Name (_CID, "PNP0C50")
    Name (_UID, 1)

    Method (_STA, 0, NotSerialized) { Return (0x0F) }

    // TLMM GPIO_CFG registers, one OperationRegion each (no Offset() trickery)
    OperationRegion (TLM0, SystemMemory, 0x0F024000, 0x00000004)   // GPIO36 (SDA)
    Field (TLM0, DWordAcc, NoLock, Preserve) { GCFG, 32 }

    OperationRegion (TLM1, SystemMemory, 0x0F025000, 0x00000004)   // GPIO37 (SCL)
    Field (TLM1, DWordAcc, NoLock, Preserve) { GCFH, 32 }

    // Runs before hidi2c starts, so the pins are already owned by QUP13 when
    // the first I2C-HID descriptor read happens.
    Method (_INI, 0, NotSerialized)
    {
        // SDA = GPIO36
        Local0 = GCFG
        And (Local0, 0xFFFFFC03, Local0)        // keep DRV/OE, drop PULL+FUNC
        Or (Local0, 0x000001DC, Local0)         // FUNC_SEL = 119 (qup13), PULL = 0
        GCFG = Local0

        // SCL = GPIO37
        Local0 = GCFH
        And (Local0, 0xFFFFFC03, Local0)
        Or (Local0, 0x000001DC, Local0)
        GCFH = Local0
    }

    Method (_CRS, 0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            // 400 kHz fast mode, 7-bit slave 0x38 on \_SB.I2C0 (SE13)
            I2cSerialBusV2 (0x0038, ControllerInitiated, 0x00061A80,
                AddressingMode7Bit, "\\_SB.I2C0", 0x00, ResourceConsumer, , Exclusive, )
            // HID-I2C attention line: Level, ActiveLow, Exclusive (spec)
            GpioInt (Level, ActiveLow, Exclusive, PullUp, 0x0000,
                "\\_SB.GPIO", 0x00, ResourceConsumer, , ) { 39 }
        })
        Return (RBUF)
    }
}
