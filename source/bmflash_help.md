# BlackMagic Flash Programmer

This utility downloads firmware into a micro-controller using the
Black Magic Probe. It automatically handles idiosyncrasies of some microcontroller
families, and supports setting a serial number during the download (i.e. serialization).

At the top of the main window, you can select the ELF file to download into the
microcontroller. Below that, are panels for general options and serialization
settings.

To start a download, you can also use function key F5.

More information:
  [[Options]]
  [[Serialization]]
  [[Tools]]
  [[About BlackMagic Flash Programmer]]

# Options

Various options can be set for the target. These options are linked to the ELF
file that is selected. So you can use different options for separate projects.

*Black Magic Probe*
: This field is typically pre-selected, if you only have a single probe connected
  to USB. When you have multiple probes connected at the same time, you need to
  select the appropriate one from the drop-down list.

: If you have a ctxLink probe configured for Wi-Fi, you need to select "TCP/IP"
  from the drop-down list.

*IP Address*
: This option is only visible when "TCP/IP" is selected from the drop-down list
  of the "Black Magic Probe" option. Here, you can fill in the IP address that
  the debug probe (likely a ctxLink probe) is configured at. Alternatively,
  you can click on the "..." button at the right of the edit field, to let the
  utility scan the network for the debug probe.

*MCU Family*
: The microcontroller family must be set only for specific micro~controller families
  that require specific configuration or processing before the download. It is
  currently needed for the "LPC" family by NXP. For other micro~controllers, this
  field should be set to "Standard".

*Post-process*
: A post-processing program (or shell script) is run after every successful
  download, with the name of the ELF file and the serial number (if serialization)
  is active.

*Power Target*
: This option can be set to drive the power-sense pin with 3.3V (to power the
  target).

*Full Flash Erase*
: This option erases all Flash sectors in the microcontroller, before proceeding with the
  download. If not set, only the Flash sectors for which there is new data get
  erased & overwritten. Note that you can also clear all Flash memory via the
  Tools button.

*Reset target during connect*
: This option may be needed on some microcontrollers, especially if SWD pins get redefined.

*Keep Log of downloads*
: On each successful firmware download, the utility adds a record to a log file,
  with the date & time of the download, plus information of the ELF file (checksum,
  RCS "ident" string). This is especially useful in combination with serialization,
  for tracking the firmware version and the date of the download.

*Print Download Time*
: On completing a download, print the time that the download took (in the Status
  view).

---
See also:
    [[Serialization]]
    [[Tools]]


# Serialization

Each firmware download can store an incremental serial number in the firmware.
It does this by patching the ELF file on the flight, while it downloads to the
target (the original ELF file on disk is not changed).

The serialization method is either "No serialization", "Address" or "Match".

*No serialization*
: Serialization is disabled.

*Address*
: This option stores the serial number at a specific address.

: You specify the name of a section in the ELF file, plus the offset from the
  start of the section. If you leave the field of the section empty, the offset
  is relative to the beginning of the ELF file. The offset is interpreted as a
  hexadecimal value.

*Match*
: This option searches for a text or byte pattern in the ELF file, and replaces
  it with the serial number.

: You give a pattern to match and an optional prefix. When the pattern is found,
  it is overwritten by the prefix, immediately followed the serial number.

: The match and prefix strings may contain \\### and \\x## specifications (where
  "#" represents a decimal or hexadecimal digit) for non-ASCII byte values. It
  may furthermore contain the sequence \U* to interpret the text that follows as
  Unicode, or \A* to switch back to ASCII. When a literal \\ is part of the
  pattern, it must be doubled, as in \\\\.

*Serial*
: The serial number is a decimal value. It is incremented after each successful
  download. The size of the serial number is in bytes.

*Format*
: The format that the serial number is written in. It can be chosen as binary,
  ASCII or Unicode. In the latter two cases, the serial number is stored as
  readable text.

: Note that in the case of Unicode text, each character takes two bytes (the size
  of the serial number is in bytes).

*Increment*
: The value by which the serial number is incremented after a successful download.
  It is typically 1, but it may be any value.

*File*
: By default, the settings for serialization are linked to the ELF file.
  However, when you want to share the serialization settings (and the serial
  number sequence) between two or more ELF files, you can specify a file in this
  field, where the serialization settings will be written to. Note that you need
  to set this field to the same filename for both projects.

# Tools

The "Tools" button pops up a menu with a few auxiliary actions.

*Re-scan Probe List*
: If a Black Magic Probe was not detected on start-up, and you connected one
  afterwards, you may use this option to re-scan for available probes.

*Full Flash Erase*
: Erase all Flash memory of the target microcontroller.

*Erase Option Bytes*
: On microcontrollers that support option bytes, you can use this command to
  clear the option bytes. If Code-Read Protection was set in the option bytes,
  clearing the option bytes will also erase all Flash memory.

: A power-cycle may be required for the microcontroller to reload the option
  bytes.

*Set CRP Option*
: On microcontrollers that support Code-Read Protection (e.g. STM32 family),
  you can set CRP (after downloading the firmware). The Code-Read protection
  becomes active after a power cycle.

# About BlackMagic Flash Programmer

The BlackMagic Flash utility is a companion tool of the book "Embedded Debugging
with the Black Magic Probe." More information on the utility and its use, can be
found in this book. It is available as a free PDF file, and as a printed book.

The utility requires a debug probe that is compatible with the Black Magic Probe.
It is a self-contained utility; it does not require GDB.

---
Copyright 2019-2022 CompuPhase \
Licensed under the Apache License version 2.0

