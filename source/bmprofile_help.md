#c-include svnrev.h
-- ---------------------------------------------------------------------------
# BlackMagic Profiler

A sampling profiler using SWO (Serial Wire Output) for non-intrusive profiling.
The utility requires that the firmware (that runs on the target device) is built
with debug information, but in many cases does not require changes to the source
code.

The user interface of the Profiler has a button bar on top, and the main view
on the left.

At the right is a sidebar for settings & status with TAB panels that can be
folded in or out:
    [[Configuration]]
    [[Profile options]]
    [[Status]]

---
Miscellaneous information:
    [[The Button Bar]]
    [[The Profile Graph]]
    [[About BlackMagic Profiler]]

# The Button Bar

*Start / Stop*
: The Start button starts capturing the sample data, and the Stop button stops
  the capture. When running, the label on the button changes from "Start" to
  "Stop," and after stopping, the button is relabeled back to "Start."

: Note that if you change any of the configuration options in the sidebar, you
  may need to stop and re-start the sampling, in order to let the Profiler re-configure
  the debug probe.

: You can also use function key F5 to start or stop profiling.

*Clear*
: Clears the profiling graph and all sampling data.

*Save*
: Stores the sampling data in a CSV file (Comma Separated Values). You can open
  these files in a spreadsheet program (Excel, LibreOffice Calc, ...) for further
  analysis.

: Note that the program always stores the full sampling data, and at the granularity
  of a source line. That is, the current view in the profile graph does not affect
  the format or contents of the data.

*Help*
: Opens up the on-line help that you are reading now.

---
See also:
    [[The Profile Graph]]
    [[Configuration]]
    [[Profile options]]

# The Profile Graph

After startup, the graph shows the list of all functions in the ELF file. Each
function has a bar to the left, as a indication of number of samples in that
function's address range. The percentage is also displayed. The function list
is sorted from "most sampled" to "least sampled."

After a mouse-click on a function name, utility zooms in on the function. It
shows the source lines of the function, plus bars on the left to indicate the
number of samples on each source line. The source lines are, of course, not
sorted.

A mouse-click on any line in the source view, returns to the main function-level
view.

---
See also:
    [[Configuration]]
    [[Status]]

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
  while the ctxLink probe supports only NRZ. If the Profiler detects the type of
  debug probe, this field is updated automatically.

*Configure Target*
: If selected, the Profiler configures the microcontroller on the target device
  for SWO tracing. For this to work, the Profiler must have support for the
  particular microcontroller. The Profiler supports several microcontroller
  families out of the box, and more can be added.

*Configure Debug Probe*
: If selected, the Profiler configures the Black Magic Probe for capturing
  SWO tracing.

: If neither this option, nor the option "Configure Target" are set, the Profiler
  does not open a connection with the debug interface of the Black Magic Probe.
  This allows you to control the debug probe and the target with another program
  (such as GDB) while sampling performance data with the Profiler.

*Reset target during connect*
: This option may be needed on some microcontrollers, especially if SWD pins get
  redefined.

*CPU clock*
: The clock of the microcontroller in Hz. This value is needed when the Profiler
  configures the target.

*Bit rate*
: The transfer speed in bits/second. This value is needed when the Profiler
  configures the target (it may also be needed for configuring the debug probe,
  depending on the mode).

*ELF file*
: The ELF file is needed to map sampled addresses to source code lines and
  functions. The ELF file that is set in this field, must of course match the
  firmware loaded into the target.

---
See also:
    [[Profile options]]

# Profile options

Profile options are in an expandable panel in the sidebar at the right. It
contains the following settings:

*Sample rate*
: The desired sample rate in Hz. A value of 1000 means 1000 samples per second.

: The actual sample rate depends on the configuration of the microcontroller. The
  true (detected) sample rate is printed in the Status panel.

*Refresh interval*
: The delay, in seconds, between two refreshes of the graph. A fractional value
  is allowed; you can set the delay to 0.5 to get two refreshes per second.

*Accumulate samples*
: If active, the graph totals the samples from the beginning of the profiling
  run. The longer it runs, the more accurate the profiling data becomes.

: If not active, the graph shows the sampling distribution since the last
  refresh. In other words, when the refresh is set to 1 second, the graph is
  updated each second with the sampling data of the past second. The graph is
  therefore more dynamic, and shows where the firmware spends time at that
  instant.

---
See also:
    [[Configuration]]
    [[Status]]

# Status

The Status view is an expandable panel in the sidebar at the right. It contains
the following values:

*Real sampling rate*
: The measured sampling rate. This value should be close to the sampling rate
  set in the Profile options.

*Overflow events*
: When the overflow event count is non-zero, sampling data arrives more quickly
  than can be processed. The sampling rate should be reduced.

*Overhead*
: This value refers to samples that have addresses that fall outside the memory
  range that the ELF file takes. This may indicate that the microcontroller is
  running code in ROM routines.

---
See also:
    [[Profile options]]

# About BlackMagic Profiler

The BlackMagic Profiler is a companion tool of the book "Embedded Debugging
with the Black Magic Probe." The book has a chapter on profiling, discussing the
pros and cons of sampling versus instrumented profiling, plus notes on diving
deeping into the profile analysis. It is available as a free PDF file, and
as a printed book.

The BlackMagic Profiler requires a debug probe that is compatible with the
Black Magic Probe. It is a self-contained utility; it does not require GDB.

---
Version {{SVNREV_STR}} \
Copyright 2022 CompuPhase \
Licensed under the Apache License version 2.0

