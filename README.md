# Debugging, Tracing & Programming with the Black Magic Probe
This guide covers setting up and using the [Black Magic Probe](https://github.com/blacksphere/blackmagic). The Black Magic Probe is a relatively cheap JTAG/SWD probe for ARM Cortex micro-controllers. A distinguishing feature of the Black Magic Probe is that it embeds a GDB server. As a result, the GNU Debugger can directly connect to the Black Magic Probe.

While setting up and using the Black Magic Probe has also been covered in wikis and blogs, I found that those description often only scratched the surface of the subject. With this guide, I set out to give a more comprehensive account.
## Utilities
Several utilities accompagny this guide. Some are small, such as `bmscan` to locate the (virtual) serial port at which the Black Magic Probe is found. Another is a helper tool for a specific family of micro-controllers (`elf-postlink`). There are GUI utilities and text-mode utilities. All have been tested under Microsoft Windows and Linux.
## Building the software
Several makefiles are provided for various compilers. Use the one that is appropriate for your system. It is most convenient if you rename the correct makefile (for your system) to `Makefile`, so that you don't have to specify it on the command line each time you run `make`.

Most makefiles include a file called "makefile.cfg" for configuration. Each makefile has a short section near the top to document which macros you can put in makefile.cfg. The file makefile.cfg is not in this repository; it should be written by you (unless the defaults are fine for your workstation setup).

The makefiles also include a dependencies file, called "makefile.dep". A basic cross-platform dependencies file is provided. If you start tinkering with the code, and especially if you add new files, you will want to rebuild the dependencies by running `make depend`. This in turn requires that you have the utility [makedepend](https://github.com/compuphase/makedepend) installed.
### Nuklear GUI
The larger utilities (with a user interface) use the [Nuklear](https://github.com/vurtun/nuklear) toolkit. This library presents an "immediate mode" GUI. It is cross-platform and supports several back-ends. I have chosen GDI+ for Windows and OpenGL with GLFW for Linux.

All GUI utilities have a `-f` command line option to set the font size in pixels (excluding leading). The default size is 14. If that is too small for ease of reading, you can use `-f=16` (or simply `-f16`). The font size that you set is saved, so it is used on any next run of the utility too (without you needing to add the command line parameter every time). The font size must be set for each GUI utility individually, though.

As an aside: the utilities have more command line options that just `-f`. The `-f` option is common to all GUI utilities.
### Linux
Prerequisites are
* libbsd-dev
* libusb-1.0-dev
* glfw-3.3

The development packages for these modules must be present. If you build glfw from source (as a static library), you can configure the path to the include files and the library in makefile.cfg.
### Windows with MingW
A common stumbling block with the MingW compiler is that it lacks the header and library files for WinUSB. However the [MingW-w64](https://mingw-w64.org) fork should come with these files. The original header files are in the Microsoft WDK (and they may come with Visual Studio too). These files have the typical "All rights reserved" copyright banner in the header comment, so I cannot distribute them. If your installation lackas these files, you will have to get them from the WDK or another source.

The files that it concerns are:
* usb.h
* usb100.h
* usb200.h
* winusb.h
* winusbio.h

The repository for this project contains the file `winusb.def`. You can use this file to create an import library for MingW using `dlltool`. The command line options to use are documented on top of the file `winusbdef`.

The location of the header and library files for WinUSB can be set in `makefile.cfg`, see the `Makefile.mingw` for details.
### Windows with Visual C/C++
The makefile for Visual C/C++ uses Microsoft's `nmake`, which is a bare-bones `make` clone.
