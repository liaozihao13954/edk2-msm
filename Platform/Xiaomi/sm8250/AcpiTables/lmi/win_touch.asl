//------------------------------------------------------------------------------
// win_touch.asl - FocalTech (FTS) touch panel on lmi, HID-over-I2C attempt.
//
// Measured on THIS device from Android (2026-11-15):
//   getevent -p   : event4 "fts_ts"  KEYBOARD|TOUCH|TOUCH_MT
//   dumpsys input : /sys/devices/platform/soc/a94000.i2c/i2c-4/4-0038
//                   bus=0x0018 (I2C) -> slave address 0x38
//   lmi.dtb       : /soc/i2c@a94000 reg 0xA94000/0x4000, interrupts [0,358,4]
//                   GIC SPI 358 -> ACPI GSI 390  (matches I2C0 _CRS)
//   uinput-goodix : under-display FINGERPRINT, not touch
//
// Revision history / why this file looks minimal:
//   v1  Goodix @0x5D  -> wrong panel, could never enumerate.
//   v2  FTS @0x38 + GpioInt(Edge, ExclusiveAndWake) + GpioIo(reset) + _DSM
//       -> Windows ACPI.sys dropped the device entirely: Enum\ACPI showed no
//          PNP0C50 at all (ACPI total stayed 30), while PNP0D10/QCOM250D/
//          QCOM2511 from the same DSDT did enumerate. Suspects:
//          (a) GpioIo resource not resolvable by qcgpio -> whole _CRS
//              invalid -> no devnode;
//          (b) GpioInt Edge violates HID-over-I2C spec (needs Level).
//   v3  (this file) drop GpioIo, GpioInt -> Level/ActiveLow/Exclusive,
//       drop _DSM, _HID via EisaId. Matches the known-good Latitude 5495
//       DSDT shape: I2cSerialBusV2(...) + GpioInt(Level, ActiveLow, ...).
//       Microsoft HID-I2C requirements: _CID = PNP0C50, exact I2C address,
//       GPIO IRQ marked Exclusive / Level / ActiveLow.
//
// If this enumerates as ACPI\PNP0C50 but shows Code 10, the panel firmware
// does not expose a standard I2C-HID descriptor and this route is dead -
// there is no FTS ARM64 Windows driver in the driver package.
//------------------------------------------------------------------------------

Device (TPD0)
{
    Name (_HID, EisaId ("PNP0C50"))
    Name (_CID, "PNP0C50")
    Name (_UID, 1)

    Method (_STA, 0, NotSerialized) { Return (0x0F) }

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
