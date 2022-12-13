# BlackMagic Trace Viewer

The Trace Viewer captures SWO trace messages. SWO (Serial Wire Output) is a
tracing protocol defined in the ARM Cortex core.

The user interface of the Trace Viewer has the main controls and views in the
left, with (from top to bottom):
    [[The Button Bar]]
    [[The Trace View]]
    [[The Timeline]]

At the right is a sidebar for settings & status with TAB panels that can be
folded in or out:
    [[Configuration]]
    [[Status]]
    [[Filters]]
    [[Channels]]

---
Miscellaneous information:
    [[The Common Trace Format]]
    [[About BlackMagic Trace Viewer]]

# The Button Bar

*Start / Stop*
: The Start button starts capturing the trace messages, and the Stop button stops
  the capture. When running, the label on the button changes from "Start" to
  "Stop," and after stopping, the button is relabeled to "Resume."

: Note that if you change any of the configuration options in the sidebar, you
  may need to stop and re-start the capture, in order to let the Trace Viewer
  re-configure the debug probe.

*Clear*
: Clears the messages from the list, and also clears the timeline.

*Search*
: Opens a dialog in which you can type a word to search for, in the list. While
  the dialog is open, you can click on the Find button to search for the next
  occurrence. The search wraps back to the top after searching beyond the last
  match.

*Save*
: Stores the currently displayed messages in a CSV file (Comma Separated Values).
  You can open these files in a spreadsheet program (Excel, LibreOffice Calc, ...)
  for further analysis.

*Help*
: Opens up the on-line help that you are reading now.

---
See also:
    [[The Timeline]]
    [[Configuration]]
    [[Filters]]
    [[Keyboard Interface]]

# The Trace View

All received messages (on channels that have been enabled) added to the list in
the main view.

While trace capture is running, only the last 400 trace messages are displayed.
This avoids the display update to slow down the message capture. As soon as you
stop trace capture, the full list of messages appears in the list.

The "Status" panel in the sidebar shows a few other values that are relevant to
trace capture and packet reception.

---
See also:
    [[The Timeline]]
    [[Status]]

# The Timeline

The timeline, at the lower part of the application viewport, has a row for each
enabled channel. When a trace message is captured, the time of the reception is
marked on the timeline. The timeline thus gives you an overview of:
* bursts of traces (busy moments versus quiet moments);
* correlation of traces between several channels;
* regularity (or lack of regularity) of the interval that trace messages arrive.

The timeline can be zoomed in and out. When several trace marks would collide
on the timeline (because they map to the same spot at that zoom level), the mark
gets higher. When zooming out, for an overview, you can therefore still get an
indication of the tracing activity.

A mouse click on the timeline scrolls the Trace View to the messages with timestamps
around that time.

---
See also:
    [[The Trace View]]

# Configuration

The Configuration view is an expandable panel in the sidebar at the right. It
contains the following settings:

*Probe*
: The debug probe to use. If only a single probe is connected to the work~station,
  it will be automatically selected. Otherwise, you can select it from the drop-down
  list.

: In the case of a ctxLink probe configured for Wi-Fi, you select "TCP/IP" from
  this list.

*IP Addr*
: This option is only visible when "TCP/IP" is selected from the drop-down list
  of the preceding "Probe" option. Here, you can fill in the IP address that
  the debug probe (likely a ctxLink probe) is configured at. Alternatively,
  you can click on the "..." button at the right of the edit field, to let the
  utility scan the network for the debug probe.

*Mode*
: Either Manchester or NRZ (asynchronous). Which mode to use, depends on the
  particular debug probe: the original Black Magic Probe supports only Manchester,
  while the ctxLink probe supports only NRZ. If the Trace Viewer detects the
  type of debug probe, this field is updated automatically.

*Configure Target*
: If selected, the Trace Viewer configures the microcontroller on the target
  device for SWO tracing. For this to work, the Trace Viewer must have support
  for the particular microcontroller. The Trace Viewer supports several
  microcontroller families out of the box, and more can be added.

*Configure Debug Probe*
: If selected, the Trace Viewer configures the Black Magic Probe for capturing
  SWO tracing.

: If neither this option, nor the option "Configure Target" are set, the Trace Viewer
  does not open a connection with the debug interface of the Black Magic Probe.
  This allows you to control the debug probe and the target with another program
  (such as GDB) while capturing SWO tracing with the Trace Viewer.

*Reset target during connect*
: This option may be needed on some microcontrollers, especially if SWD pins get redefined.

*CPU clock*
: The clock of the microcontroller in Hz. This value is needed when the Trace Viewer configures
  the target.

*Bit rate*
: The transfer speed in bits/second. This value is needed when the Trace Viewer
  configures the target (it may also be needed for configuring the debug probe,
  depending on the mode).

*Data size*
: Most targets implement SWO tracing with an 8-bit payload in a packet. However,
  16-bit or 32-bit payloads per packet are more efficient (less overhead, and
  thus a higher effective transfer speed).

: When set to "auto", the Trace Viewer attempts to detect the data size.

*TSDL file*
: When a file with CTF metadata (Common Trace Format) is selected in this field,
  the Trace Viewer will activate CTF decoding on the channels defined in the
  metadata file.

*ELF file*
: Selecting the ELF file, enables symbol lookup for addresses transmitted by the
  target firmware. The ELF file that is set in this field, must of course match
  the firmware loaded into the target. This option is furthermore only useful
  when CTF decoding is active (i.e. when a TSDL file is set), because the
  TSDL file must define which parameters in a trace packet represent symbol
  addresses.

---
See also:
    [[Status]]
    [[Common Trace Format]]

# Status

The Status view is an expandable panel in the sidebar at the right. It contains
the following values:

*Total received*
: The total number of packets received. While running, the number of messages
  in the Trace View is limited to 400 messages. This number has the total.

*Overflow events*
: When the overflow event count is non-zero, captured trace data is incomplete.
  The cause is that trace data arrives more quickly than can be processed.

: If an overflow occurs, the Trace Viewer automatically reduces the number of
  messages that it displays while running. This limit is 400 messages by default,
  but may be reduced down to 50 (in gradual steps) if overflow events are
  detected.

*Packet errors*
: When the number of packet errors is non-zero, the configuration of the SWO
  protocol is likely incorrect (Data size & CPU clock), or the Bit rate is too
  high.

---
See also:
    [[Configuration]]

# Filters

You can filter the trace messages on keywords. Trace messages that are filtered
out, are still saved in memory, but not displayed. When the filter is disabled
(or deleted), the messages re-appear.

Text matching is case-sensitive.

If the filter text starts with a tilde ('~'), it is an inverted filter: a trace
message passes the filter if it does not contain the text that follows the tilde.

A trace message passes all filters (and is displayed) if:
* it matches any of the standard filters,
* and matches none of the inverted filters

# Channels

SWO tracing allows for 32-channels. Most implementations use only channel 0, but
the advantage of using multiple channels is that each channel can be enabled and
disabled. When a channel is disabled, and provided that the Trace Viewer configures
the target (see the Configuration), it is disabled in the target microcontroller. As a result,
the traces on that channel are not transmitted, and thus have negligent overhead
on the target's execution.

When a TSDL file is configured, the channel names are copied from the stream names
in the TSDL file. You can set a name (and a colour) for a channel by right-clicking
on the channel name/number. This opens a pop-up with selection boxes for the RGB
colour values and a field for the name.

---
See also:
    [[Configuration]]

# Keyboard Interface

| F1      | Help                              |
| F5      | Start / Stop / Resume the capture |
| Ctrl+F  | Search                            |
| Ctrl+S  | Save traces (CSV format)          |

# The Common Trace Format

The Common Trace Format is a compact binary format for trace messages. Using CTF
in your firmware takes two parts: a C/C++ file with functions that transmit
binary encoded trace messages, and a matching metadata file in the "Trace Stream
Description Language" (TSDL). The metadata file ("TSDL file") maps the binary
data back into human-readable form.

The Trace Viewer comes with a tool to generate the C/C++ file from the metadata
file. The companion book "Embedded Debugging with the Black Magic Probe" has a
chapter on CTF, the syntax of TSDL and how to use the tools.

---
See also:
    [[Configuration]]
    [[About BlackMagic Trace Viewer]]

# About BlackMagic Trace Viewer

The BlackMagic Trace Viewer is a companion tool of the book "Embedded Debugging
with the Black Magic Probe." The book has a chapter on tracing, with several examples
and code snippets to do it efficiently. It is available as a free PDF file, and
as a printed book.

The BlackMagic Trace Viewer requires a debug probe that is compatible with the
Black Magic Probe. It is a self-contained utility; it does not require GDB.

---
Copyright 2019-2022 CompuPhase \
Licensed under the Apache License version 2.0

