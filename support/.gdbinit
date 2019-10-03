define bmconnect
    if $argc < 1 || $argc > 2
        help bmconnect
    else
        target extended-remote $arg0
        if $argc == 2
            monitor $arg1 enable
        end
        monitor swdp_scan
        attach 1
    end
end
document bmconnect
Attach to the Black Magic Probe at the given serial port/device:

    bmconnect PORT [tpwr]

Specify PORT as COMx in Microsoft Windows or as /dev/ttyACMx in Linux.
If the second parameter is set as "tpwr", the power-sense pin is driven to 3.3V.
end

# ===========================================================================

define mmap-flash
    set mem inaccessible-by-default off
    if $argc < 1 || $argc > 1
        help mmap-flash
    else
        set $ok = 0
        if $arg0 == 800 || $arg0 == 1100 || $arg0 == 1200 || $arg0 == 1300
            set {int}0x40048000 = 2
            set $ok = 1
        end
        if $arg0 == 1500
            set {int}0x40074000 = 2
            set $ok = 1
        end
        if $arg0 == 1700
            set {int}0x400FC040 = 1
            set $ok = 1
        end
        if $arg0 == 4300
            set {int}0x40043100 = 0
            set $ok = 1
        end
        if $ok == 0
            help mmap-flash
        end
    end
end
document mmap-flash
Set the SYSMEMREMAP register for NXP LPC devices to map address 0 to Flash memory.

    mmap-flash mcu-type

The parameter mcu-type must be one of the following:
    800         for LPC8xx series
    1100        for LPC11xx series
    1200        for LPC12xx series
    1300        for LPC13xx series
    1500        for LPC15xx series
    1700        for LPC17xx series
    4300        for LPC43xx series
end