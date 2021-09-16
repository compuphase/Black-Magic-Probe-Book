# Embedded Debugging with the Black Magic Probe
<img src="https://github.com/compuphase/Black-Magic-Probe-Book/blob/master/doc/BlackMagicProbe-front-cover.jpg" alt="Book cover" width="400" align="right">
This guide covers setting up and using the <a href="https://github.com/blacksphere/blackmagic/">Black Magic Probe</a>. The Black Magic Probe is a relatively cheap JTAG/SWD probe for ARM Cortex micro-controllers. A distinguishing feature of the Black Magic Probe is that it embeds a GDB server. As a result, the GNU Debugger can directly connect to the Black Magic Probe.<br>

While setting up and using the Black Magic Probe has also been covered in wikis and blogs, I found that those description often only scratched the surface of the subject. With this guide, I set out to give a more comprehensive account. Later, I also added specific notes on [ctxLink](http://www.sidprice.com/ctxlink/), a derivative of the Black Magic Probe that offers a WiFi connection.
## Utilities
Several utilities accompagny this guide. Some are small, such as `bmscan` to locate the (virtual) serial port at which the Black Magic Probe is found (or scans the local network for ctxLink). Another is a helper tool for a specific family of micro-controllers (`elf-postlink`). There are GUI utilities and text-mode utilities. All have been tested under Microsoft Windows and Linux.

For the purpose of troubleshooting, pre-build versions of the "hosted" variant of the Black Magic firmware are also available for Windows and Linux (see [Releases](https://github.com/compuphase/Black-Magic-Probe-Book/releases)).
## Building the software
Several makefiles are provided for various compilers. Use the one that is appropriate for your system. It is most convenient if you rename the correct makefile (for your system) to `Makefile`, so that you don't have to specify it on the command line each time you run `make`.

Most makefiles include a file called "makefile.cfg" for configuration. Each makefile has a short section near the top to document which macros you can put in makefile.cfg. The file makefile.cfg is not in this repository; it should be written by you (unless the defaults are fine for your workstation setup).

The makefiles also include a dependencies file, called "makefile.dep". A basic cross-platform dependencies file is provided. If you start tinkering with the code, and especially if you add new files, you will want to rebuild the dependencies by running `make depend`. This in turn requires that you have the utility [makedepend](https://github.com/compuphase/makedepend) installed.
### Nuklear GUI
The larger utilities (with a user interface) use the [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) toolkit. This library presents an "immediate mode" GUI. It is cross-platform and supports several back-ends. I have chosen GDI+ for Windows and OpenGL with GLFW for Linux.

All GUI utilities have a `-f` command line option to set the font size in pixels (excluding leading). The default size is 14. If that is too small for ease of reading, you can use `-f=16` (or simply `-f16`). The font size that you set is saved, so it is used on any next run of the utility too (without you needing to add the command line parameter every time). The font size must be set for each GUI utility individually, though. With recent releases, you can also set the names of the fonts used for the user interface with `-f=16,Arial,Inconsolata`. The first name is for the font for all buttons and controls, the second font is the monospaced font for the source code (if applicable). You can leave out any of the three parts by leaving the commas: `-f,,Inconsolata` changes only the monospaced font and leaves the other parameters at their defaults.

As an aside: the utilities have more command line options that just `-f`. The `-f` option is common to all GUI utilities. Use the `-h` or `-?` options to get a summary of the command line options for each utility.
### Linux
Prerequisites are
* libbsd-dev
* libusb-1.0-dev
* glfw-3.3

The development packages for these modules must be present. If you build glfw from source (as a static library), you can configure the path to the include files and the library in makefile.cfg. In particular, on some Linux distributions the library is called `glfw` rather than `glfw3`; in this case, create a `makefile.cfg` file in the same directory as `Makefile.linux` and add the following line to it:
```
GLFW_LIBNAME := glfw
```
(Again, see the top of `Makefile.linux` for other options.)
### Windows with MingW
A makefile is provided for MingW, called `makefile.mingw`.

The current release links to either WinUSB or libusbK dynamically. It is no longer necessary to get the headers for WinUSB from the Windows DDK or create an import library from a .def file. However, for the libusbK option, you will need to download the appropriate libusbK.dll and place it into the directory where the binaries are built. See the [libusbK project](https://sourceforge.net/projects/libusbk/).

### Windows with Visual C/C++
The makefile for Visual C/C++ uses Microsoft's `nmake`, which is a bare-bones `make` clone.
