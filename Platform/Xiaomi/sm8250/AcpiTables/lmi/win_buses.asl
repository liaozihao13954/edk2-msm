//------------------------------------------------------------------------------
// win_buses.asl - devices the Windows Qualcomm BSP expects but the stock lmi
// DSDT never declares.
//
// Resource values are taken from lmi.dtb (parsed by uefi_alt/dtb_dump.py).
// Interrupt mapping rule verified against the existing UFS0 node:
//     DTB GIC SPI 265  ->  ACPI GSI 297     (offset +32)
//
//   GPIO / TLMM     pinctrl@f000000  0x0F000000 len 0x01000000  SPI 208 -> 240
//   I2C  (QUPV3)    i2c@a94000       0x00A94000 len 0x00004000  SPI 358 -> 390
//                   ^ qupv3_se13_i2c: the bus the Goodix GT9889 touch sits on
//                     (resolved via /__symbols__ of lmi.dtb; addr 0x5d,
//                      tlmm GPIO 39 = irq, GPIO 38 = reset)
//   USB3 (DWC3)     dwc3@a600000     0x0A600000 len 0x0000D93C  SPI 133 -> 165
//------------------------------------------------------------------------------

// TLMM GPIO controller - QCOM250D (qcgpio8250.inf)
Device (GPIO)
{
    Name (_HID, "QCOM250D")
    Name (_UID, 0)
    Method (_STA, 0) { Return (0xF) }
    Method (_CRS, 0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite, 0x0F000000, 0x01000000)
            Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, , , ) { 240 }
        })
        Return (RBUF)
    }
}

// QUPV3 serial engine 13 (qupv3_se13_i2c) - the bus the touch panel uses.
// QCOM2511 (qci2c8250.inf)
Device (I2C0)
{
    Name (_HID, "QCOM2511")
    Name (_UID, 13)
    Method (_STA, 0) { Return (0xF) }
    Method (_CRS, 0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite, 0x00A94000, 0x00004000)
            Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, , , ) { 390 }
        })
        Return (RBUF)
    }
}

// Primary USB3 controller as a plain XHCI so Windows can use the inbox
// usbxhci.sys / USBHUB3 stack (no Qualcomm driver dependency).
Device (XHC0)
{
    Name (_HID, "PNP0D10")
    Name (_CID, "PNP0D10")
    Name (_UID, 0)
    Method (_STA, 0) { Return (0xF) }
    Method (_CRS, 0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite, 0x0A600000, 0x0000D93C)
            Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, , , ) { 165 }
        })
        Return (RBUF)
    }
}
