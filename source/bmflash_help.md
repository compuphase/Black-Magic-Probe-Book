#c-include svnrev.h
-- ---------------------------------------------------------------------------
# BlackMagic Flash Programmer

This utility downloads firmware into a micro-controller using the
Black Magic Probe. It automatically handles idiosyncrasies of some microcontroller
families, and supports setting a serial number during the download (i.e. serialization).

At the top of the main window, you can select the target firmware file to download
into the microcontroller. Below that, are panels for general options and serialization
settings. The utility supports ELF files, Intel HEX files and raw binary files.

The ELF and HEX files formats record at which addresses the data must be stored,
but the BIN file is a flat binary file. It can be downloaded to any address in
the microcontroller. When selecting a BIN file as the target file, a text field
for the base address appears. If you leave the the address field empty, the
firmware is downloaded to the base address of Flash memory of the microcontroller.

To start a download, you can also use function key F5.

More information:
  [[Options]]
  [[Serialization]]
  [[Tools]]
  [[About BlackMagic Flash Programmer]]

# Options

Various options can be set for the target. The utility saves these options per
target (firmware) file. So you can use different options for separate projects.

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

*Code Read Protection*
: This option allows you to protect the firmware from being read out of the
  microcontroller. It is currently supported for the NXP "LPC" family and the
  STMicroelectronics "STM32" family.

: When this option is selected, you can choose one of three levels. For the STM32
  family, only levels 1 and 2 are valid. Please see the microcontroller's reference
  guide for the effect of each level. For the "LPC" microcontrollers, the firmware
  needs to prepared for code read protection. See the documentation from NXP and
  the book "Embedded Debugging with the Black Magic Probe" for more information.

: Note: on many microcontrollers, code protection only becomes in effect after
  a power-cycle.

*Power Target*
: This option can be set to drive the power-sense pin with 3.3V (to power the
  target).

*Full Flash Erase before download*
: This option erases all Flash sectors in the microcontroller, before proceeding with the
  download. If not set, only the Flash sectors for which there is new data get
  erased & overwritten. Note that you can also clear all Flash memory via the
  Tools button.

*Reset Target during connect*
: This option may be needed on some microcontrollers, especially if SWD pins get redefined.

*Keep Log of downloads*
: On each successful firmware download, the utility adds a record to a log file,
  with the date & time of the download, plus information of the target file (checksum,
  RCS "ident" string). This is especially useful in combination with serialization,
  for tracking the firmware version and the date of the download.

*Show Download Time*
: On completing a download, print the time that the download took (in the Status
  view).

*Post-process*
: A post-processing script is run after every download, with information on the
  target file and the serial number (if serialization) is active. The script must be
  in the Tcl language. See the book "Embedded Debugging with the Black Magic Probe"
  for more information on scripting.

: By default, the utility runs the script only after each successful download (so,
  not after a failed download). The option "Post-process on failed downloads" runs
  the script on every download. The status of the download (success or failure)
  is passed onto the script.

---
See also:
    [[Serialization]]
    [[Tools]]
    [[About BlackMagic Flash Programmer]]

# Serialization

Each firmware download can store an incremental serial number in the firmware.
It does this by patching the target file on the flight, while it downloads to the
microcontroller (the original ELF/HEX/NIN file is not changed).

The serialization method is either "No serialization", "Address" or "Match".

*No serialization*
: Serialization is disabled.

*Address*
: This option stores the serial number at a specific address.

: If the target file is an ELF file, you specify the name of a section in the
  ELF file, plus the offset from the start of the section. If you leave the field
  of the section empty, the offset is the absolute address (in the memory space
  of the microcontroller). The offset is interpreted as a hexadecimal value.

: If the target file is in HEX or BIN formats, the section name is ignored, and
  the address is always an absolute address.

*Match*
: This option searches for a text or byte pattern in the target file, and replaces
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
  download.

*Size*
: The size of the serial number is in bytes. The serial number is padded with
  zeroes to fill up the size. Note that the prefix is part of the size.

: For example, if the serial number is 12, the size is 8 and the format is ASCII
  (and there is no prefix), the string stored is 00000012. But if the format is
  Unicode, where each character takes two bytes, 0012 would be stored. And if
  the format is binary, there would be 7 bytes with a zero value stored, followed
  by a single byte with the value 12.

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
: By default, the settings for serialization are linked to the target file.
  However, when you want to share the serialization settings (and the serial
  number sequence) between two or more target files, you can specify a file in this
  field, where the serialization settings will be written to. Note that you need
  to set this field to the same filename for both projects.

# Tools

The "Tools" button pops up a menu with a few auxiliary actions.

*Re-scan Probe List*
: If a Black Magic Probe was not detected on start-up, and you connected one
  afterwards, you may use this option to re-scan for available probes.

*Verify Download*
: Compare the contents of Flash memory to the selected target file. Note that
  only the memory range that is valid in the target file is checked. Contents of
  Flash memory outside the range of the ELF/HEX/BIN file are ignored.

*Erase Option Bytes*
: On microcontrollers that support option bytes, you can use this command to
  clear the option bytes. If Code-Read Protection was set in the option bytes,
  clearing the option bytes will also erase all Flash memory.

: A power-cycle may be required for the microcontroller to reload the option
  bytes.

*Full Flash Erase*
: Erase all Flash memory of the target microcontroller.

*Blank Check*
: Verifies that the Flash memory of the microcontroller is completely emty.

*Dump Flash to File*
: Writes the contents of Flash memory to a flat binary file. The file is trimmed
  to the portion of Flash memory that has data. Trailing empty Flash sectors are
  not saved.

# About BlackMagic Flash Programmer

The BlackMagic Flash utility is a companion tool of the book "Embedded Debugging
with the Black Magic Probe." More information on the utility and its use, can be
found in this book. It is available as a free PDF file, and as a printed book.

The utility requires a debug probe that is compatible with the Black Magic Probe.
It is a self-contained utility; it does not require GDB.

---
Version {{SVNREV_STR}} \
Copyright 2019-2023 CompuPhase \
Licensed under the Apache License version 2.0

