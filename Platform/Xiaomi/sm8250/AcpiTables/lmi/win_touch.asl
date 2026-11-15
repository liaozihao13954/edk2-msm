//------------------------------------------------------------------------------
// win_touch.asl - FocalTech (FTS) touch panel on lmi, HID-over-I2C attempt.
//
// Measured on THIS device from Android (2026-11-15), not guessed from dtbo:
//   getevent -p      : event4 "fts_ts"  Classes KEYBOARD|TOUCH|TOUCH_MT
//   dumpsys input    : SysfsDevicePath
//                      /sys/devices/platform/soc/a94000.i2c/i2c-4/4-0038
//                      bus=0x0018 (I2C) -> slave address 0x38
//   a94000.i2c       : qupv3_se13  ->  DSDT \_SB.I2C0 (_UID 13, QCOM2511)
//   uinput-goodix    : under-display FINGERPRINT (goodix_fod6), NOT touch
//   xiaomi-touch     : virtual key layer
//
// !! Earlier revision wrongly used Goodix GT9889 @ 0x5D. That is a different
// !! panel variant in dtbo; it would never enumerate on this unit.
//
// Strategy: _HID PNP0C50 -> Windows inbox hidi2c.sys + hidclass, no vendor
// driver needed. There is NO FTS/Goodix ARM64 Windows driver in the driver
// package (only QCOM257B / QTC800S DSP touch, which does not match this panel).
// Success is NOT guaranteed: hidi2c.sys requires the panel firmware to expose
// a standard I2C HID descriptor + HID report descriptor. Most phone FTS panels
// use a proprietary register protocol; expect Code 10 rather than a bluescreen.
//
// IRQ/reset GPIOs (TLMM 39 / 38) and pm8150a_l1 supply come from the dtbo
// overlay and still need per-variant confirmation. Power resource is not
// modelled here; UEFI normally leaves the rail on.
//------------------------------------------------------------------------------

Device (TPD0)
{
    Name (_HID, "PNP0C50")
    Name (_CID, "PNP0C50")
    Name (_UID, 1)

    Method (_STA, 0) { Return (0xF) }

    // Microsoft requires the I2C connection and the interrupt to be the
    // first two resources in the resource list for HID-over-I2C devices.
    Method (_CRS, 0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            // 400 kHz fast-mode, 7-bit address 0x38 on \_SB.I2C0 (SE13)
            I2cSerialBusV2 (0x0038, ControllerInitiated, 0x00061A80,
                AddressingMode7Bit, "\\_SB.I2C0", 0x00, ResourceConsumer, , Exclusive, )
            // attention line: edge, active low, wake capable, internal pull-up
            GpioInt (Edge, ActiveLow, ExclusiveAndWake, PullUp, 0x0000,
                "\\_SB.GPIO", 0x00, ResourceConsumer, , ) { 39 }
            // reset line, host driven
            GpioIo (Exclusive, PullDefault, 0x0000, 0x0000,
                IoRestrictionOutputOnly, "\\_SB.GPIO", 0x00, ResourceConsumer, , ) { 38 }
        })
        Return (RBUF)
    }

    // I2C HID descriptor location hint for hidi2c.sys.
    // UUID 3CDFF6F7-4267-4555-AD05-B30A3D8938DE = HID-over-I2C DSM.
    Method (_DSM, 4, Serialized)
    {
        If ((Arg0 == ToUUID ("3cdff6f7-4267-4555-ad05-b30a3d8938de")))
        {
            If ((Arg2 == One))
            {
                Return (Buffer (One) { 0x01 })
            }
            If ((Arg2 == 0x0100))
            {
                // HID descriptor register 0x0001, length 0x20
                Return (Package (0x03) { 0x0001, 0x0001, 0x0020 })
            }
        }
        Return (Buffer (One) { 0x00 })
    }
}
