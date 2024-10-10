# Embedded Debugging with the Black Magic Probe
<img src="https://github.com/compuphase/Black-Magic-Probe-Book/blob/master/doc/blackmagicprobe-book.jpg" alt="Book cover" width="400" align="right">
This guide covers setting up and using the <a href="https://github.com/blackmagic-debug/blackmagic/">Black Magic Probe</a>. The Black Magic Probe is a low cost JTAG/SWD probe for ARM Cortex micro-controllers. A distinguishing feature of the Black Magic Probe is that it embeds a GDB server. As a result, the
GNU Debugger can directly connect to the Black Magic Probe.

&nbsp;<br/>
While setting up and using the Black Magic Probe has also been covered in wikis and blogs, I found that those description often only scratched the surface of the subject. With this guide, I set out to give a more comprehensive account. Over time, this guide has been updated to cover new firmware releases, as well as specific notes on [ctxLink](http://www.sidprice.com/ctxlink/), a derivative of the Black Magic Probe that offers a WiFi connection.

The guide is licensed under the [Creative Commons Attribution-NonCommercial-NoDerivatives 4.0 International License](https://creativecommons.org/licenses/by-nc-nd/4.0/). A printed copy (might you prefer that) is available from lulu and Amazon.<br clear="all"/>
## Utilities
Several utilities accompagny this guide. Some are small, such as `bmscan` to locate the (virtual) serial port at which the Black Magic Probe is found (or scans the local network for ctxLink). Another is a helper tool for a specific family of micro-controllers (`elf-postlink`). There are GUI utilities and text-mode utilities. All have been tested under Microsoft Windows and Linux.

The software is licensed under the [Apache License, version 2.0](https://www.apache.org/licenses/LICENSE-2.0).

A summary of the utilities:
<table>
<tr>
<td> <img src="https://www.compuphase.com/electronics/icon_debug_64.png" alt="Icon">    </td><td> bmdebug      </td><td> A front-end for GDB with specific support for the Black Magic Probe and debugging embedded targets. </td>
</tr><tr>
<td> <img src="https://www.compuphase.com/electronics/icon_download_64.png" alt="Icon"> </td><td> bmflash      </td><td> A utility to download firmware into the Flash memory of a microcontroller, with support for serialization and Tcl scripting. </td>
</tr><tr>
<td> <img src="https://www.compuphase.com/electronics/icon_profile_64.png" alt="Icon">  </td><td> bmprofile    </td><td> A sampling profiler using the CoreSight debugging features and the TRACESWO channel. </td>
</tr><tr>
<td> <img src="https://www.compuphase.com/electronics/icon_serial_64.png" alt="Icon">  </td><td> bmserial    </td><td> A serial monitor/terminal for monitoring data on a serial port/UART, or for console I/O to a serial terminal. </td>
</tr><tr>
<td> <img src="https://www.compuphase.com/electronics/icon_trace_64.png" alt="Icon">    </td><td> bmtrace      </td><td> A utility to monitor SWO trace messages, with support for filtering, multiple channels and the <a href="https://diamon.org/ctf/">Common Trace Format</a>. </td>
</tr><tr>
<td>                                                                                    </td><td> bmscan       </td><td> A command-line utility to check the COM port (Windows) or ttyACM device (Linux) that the Black Magic Probe is attached to. It can locate the IP address of a ctxLink probe by doing a network scan. </td>
</tr><tr>
<td>                                                                                    </td><td> elf&#x2011;postlink </td><td> A utility to set the checksum in the vector table for NXP microcontrollers in the LPC series. As the name suggests, this utility can be run on an ELF file after the "link" stage. </td>
</tr><tr>
<td>                                                                                    </td><td> tracegen     </td><td> A utility to generate C source files from a TSDL specification for the <a href="https://diamon.org/ctf/">Common Trace Format</a>. </td>
</tr>
</table>

### Screen captures of the GUI utilities
<a target="_blank" href="https://www.compuphase.com/electronics/blackmagicprobe-bmdebug.png"><img src="https://www.compuphase.com/electronics/blackmagicprobe-bmdebug.png" width="23%" title="Black Magid Debugger"></img></a>
<a target="_blank" href="https://www.compuphase.com/electronics/blackmagicprobe-bmflash.png"><img src="https://www.compuphase.com/electronics/blackmagicprobe-bmflash.png" width="23%" title="Black Magid Flash Programmer"></img></a>
<a target="_blank" href="https://www.compuphase.com/electronics/blackmagicprobe-bmtrace.png"><img src="https://www.compuphase.com/electronics/blackmagicprobe-bmtrace.png" width="23%" title="Black Magid SWO Trace Viewer"></img></a>
<a target="_blank" href="https://www.compuphase.com/electronics/blackmagicprobe-bmprofile.png"><img src="https://www.compuphase.com/electronics/blackmagicprobe-bmprofile.png" width="23%" title="Black Magid Profiler"></img></a>

## Building the software
Several makefiles are provided for various compilers. Use the one that is appropriate for your system. It is most convenient if you rename the correct makefile (for your system) to `Makefile`, so that you don't have to specify it on the command line each time you run `make`.

Most makefiles include a file called "makefile.cfg" for configuration. Each makefile has a short section near the top to document which macros you can put in makefile.cfg. The file makefile.cfg is not in this repository; it should be written by you (unless the defaults are fine for your workstation setup).

The makefiles also include a dependencies file, called "makefile.dep". A basic cross-platform dependencies file is provided. If you start tinkering with the code, and especially if you add new files, you will want to rebuild the dependencies by running `make depend`. This in turn requires that you have the utility [makedepend](https://github.com/compuphase/makedepend) installed.
### Nuklear GUI
The larger utilities (with a user interface) use the [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) toolkit. This library presents an "immediate mode" GUI. It is cross-platform and supports several back-ends. I have chosen GDI+ for Windows and OpenGL with GLFW for Linux.

All GUI utilities have a `-f` command line option to set the font size in pixels (excluding leading). The default size is 14. If that is too small for ease of reading, you can use `-f=16` (or simply `-f16`). The font size that you set is saved, so it is used on any next run of the utility too (without you needing to add the command line parameter every time). The font size must be set for each GUI utility individually, though. With recent releases, you can also set the names of the fonts used for the user interface with `-f=16,Arial,Inconsolata`. The first name is for the font for all buttons and controls, the second font is the monospaced font for the source code (if applicable). You can leave out any of the three parts by leaving the commas: `-f,,Inconsolata` changes only the monospaced font and leaves the other parameters at their defaults.

In Linux, you may need to experiment a little for the font size that gives the sharpest text. Due to the font handling in Nuklear, some fractional pixel sizes give better visual results than others. For example, on my system, the text is sharp at a size of 14.4 (but on your system, a different value may be optimal).

As an aside: the utilities have more command line options than just `-f`. The `-f` option is common to all GUI utilities. Use the `-h` or `-?` options to get a summary of the command line options for each utility.
### Linux
Prerequisites are
* libbsd-dev
* libfontconfig-dev
* libgtk-3-dev
* libusb-1.0-0-dev
* glfw-3.3 (libglfw3-dev)

The development packages for these modules must be present. If you build glfw from source (as a static library), you can configure the path to the include files and the library in makefile.cfg. In particular, on some Linux distributions the library is called `glfw` rather than `glfw3`; in this case, create a `makefile.cfg` file in the same directory as `Makefile.linux` and add the following line to it:
```
GLFW_LIBNAME := glfw
```
(Again, see the top of `Makefile.linux` for other options.)
### Windows with MingW
A makefile is provided for MingW, called `Makefile.mingw`.

The current release links to either WinUSB or libusbK dynamically. It is no longer necessary to get the headers for WinUSB from the Windows DDK or create an import library from a .def file. However, for the libusbK option, you will need to download the appropriate libusbK.dll and place it into the directory where the binaries are built. See the [libusbK project](https://sourceforge.net/projects/libusbk/).

To generate the `.res` files, use MingW's `windres` function as follows:
```
windres -O coff -J rc -i file.rc -o file.res
```
### Windows with Visual C/C++
The makefile for Visual C/C++ (`Makefile.msvc`) uses Microsoft's `nmake`, which is a bare-bones `make` clone.

You may have to create a `makefile.cfg` file with the paths that the Visual C/C++ compiler should look in for the include files.
```
INCLUDE = /I "C:\Program Files (x86)\Windows Kits\10\Include\10.0.17763.0\um"
```
There are several include paths that you may need to append to that `INCLUDE` macro.
