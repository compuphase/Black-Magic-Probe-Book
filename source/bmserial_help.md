# BlackMagic Serial Monitor

A serial monitor (or terminal) with a simple user interface. It features line
status control, and text or hexadecimal view.

The user interface of the Serial Monitor has a button bar on top. Below that, it
has the main view with all received data, and an input field for transmitting data
at the bottom.

At the right is a sidebar for settings & status with TAB panels that can be
folded in or out:
    [[Configuration]]
    [[Line status]]
    [[Display options]]
    [[Local options]]

---
Miscellaneous information:
    [[The Button Bar]]
    [[The Input Field]]
    [[Filters]]
    [[About BlackMagic Serial Monitor]]

# The Button Bar

*Connect / Disconnect*
: The Connect button opens a connection to the selected serial port, and the
  Disconnect button closes the connection.
  The label on the button changes from "Connect" to "Disconnect," depending on
  the status of the connection.

*Clear*
: Clears the terminal window.

*Save*
: Stores the data in the terminal window in a text file. The data is saved as it
  is displayed. For example, when in "Hexadecimal" view, the data is stored as a
  hex dump.

*Help*
: Opens up the on-line help that you are reading now.

---
See also:
    [[Configuration]]

# The Input Field

The input field is a single-line edit field at the bottom of the window. Any
text that you type in this field is transmitted on an Enter or when clicking
on the "Send" button.

A carriage-return and/or line-feed character may be appended to the text in the
edit field. This is set in the "Local options" panel.

You can transmit non-text bytes by giving their value in two hexadecimal digits.
The two digits must be prefixed with a \` (backtick). That is, to transmit a byte
with the value 123 (decimal), or 7B hexadecimal, type \`7B and transmit. Any number
of hexadecimal digits may follow a backtick, as long as there is an even number
of digits. When typing \`7B32, two bytes are sent (with decimal values 123 and 50
respectively), and in that order. The hexadecimal digits may be specified in
upper case or lower case.

---
See also:
    [[Display options]]
    [[Local options]]

# Configuration

The Configuration view is an expandable panel in the sidebar at the right. It
contains the following settings:

*Port*
: A list of serial ports that are available on the current system.

: On Linux, all predefined "ttyS*" devices are included in the list, but in
  general, most of these devices are not truly backed by hardware.

*Baudrate*
: A selection of the standard Baud rates.

*Data bits*
: Typically 8, but legacy systems can require 7 or fewer data bits in a word.

*Stop bits*
: Typically 1 when 8 data bits are selected; sometimes 2 when there are 7 data
  bits.

*Parity*
: The choice for parity check (on reception) and parity bit generation (on transmit).
  When parity checking is enabled, received data that fails the parity check
  creates a "Frame error." Both devices must use the same setting for parity.

*Flow control*
: Flow control lets both connected devices signal their readiness to accept more
  data. When a device can temporarily not accept more data (because its buffer
  is full), it signals the other host to stop transmitting. When the device has
  processed data and freed up buffer space, it signals the other host to restart
  transmission.

---
See also:
    [[Line status]]

# Line status

The Line Status view is an expandable panel in the sidebar at the right. It
shows the line or modem status signals.

At its core, RS232 communication used two data lines, TxD and RxD (for "transmit
data" and "receive data" respectively). Only three wires are needed for basic
RS232 communication (the third wire for ground). However, the 9-pin connector
has several more signals: the "line status" signals.

The signals are separated in "Local" and "Remote" signals. The local signals,
you can toggle on or off by clicking on them; the remote signals are read-only.

Note that there is a relation between the local and remote signals: the RTS line
of the local host is wired to CTS of the remote host, and likewise DTR is wired
to DSR.

Break and Frame error are added in this panel too, even though these are not
"line status" signals. They do, however, signal the status of data transmission
or reception. Frame error indicates that the number of data or stop bits is
incorrect, or that the parity check failed. Some devices use a break on the
transmission line to signal the other host to reset the communication.

# Display options

The Display options is an expandable panel in the sidebar at the right. It
contains the following settings:

*View mode*
: Either text mode or hexadecimal mode. Hex mode which shows the received data
  as rows of a hex dump.

: The choice between "text mode" or "hex mode" only applies to the visualization
  of the received data. To transmit binary data, use the "backtick syntax" in
  the input field.

*Word-wrap*
: This option is only available in "Text" mode. If enabled, lines of text wrap
  on the edge of the viewport.

*Bytes/line*
: This option is only available in "Hex" mode. It has the number of bytes for
  each row of the hex dump. Common values are 8 or 16, but any (non-zero) number
  may be selected.

*Scroll to last*
: When new data arrives, the main view automatically scrolls to the bottom, in
  order to make the new data visible. If this option is turned off, automatic
  scrolling is disabled.

*Line limit*
: The maximum number of lines kept in the viewport. When more data arrives, the
  oldest lines are dropped. If set to zero (or if this field is left empty), no
  limit applies.

---
See also:
    [[The Input Field]]

# Local options

The Local options is an expandable panel in the sidebar at the right. It
contains the following settings:

*Local echo*
: Whether the text or data that is transmitted, is copied to the main data view.
  Some devices echo the received data to the sender, in which case local echo is
  redundant. In other cases, local echo is practical, as it is an indication that
  the data was indeed transmitted.

*Append EOL*
: When transmitting text or data, the Serial Monitor can append a line-end
  character to the transmitted data. Some devices only operate on the received
  data upon reception of a carriage-return or line-feed character (or a combination
  of both). When this option is set, you can subsequently select which end-of-line
  character to append.

---
See also:
    [[The Input Field]]


# Filters

With filters, you can highlight rows in the viewport based on keywords. For each
filter, you can select a colour and the text to match. The Serial Monitor pre-selects
a colour for a new filter, but you can override that by clicking on the coloured
block at the left of the edit field.

After adding a filter, it is listed in the panel, and it has a checkbox at its
left. The checkbox allows to disable and re-enable a filter, without deleting it.

A row in the main view that matches the filter text gets the background colour
that is set for the filter. The entire row is colored, not just the matching text.

---
See also:
    [[Display options]]

# About BlackMagic Serial Monitor

The BlackMagic Serial Monitor is a companion tool of the book "Embedded Debugging
with the Black Magic Probe." It is, however, a stand-alone general-purpose
serial terminal, that does not require the Black Magic Probe hardware device.

The book is available as a free PDF file, and as a printed book.

---
Copyright 2022 CompuPhase \
Licensed under the Apache License version 2.0

