# -------------------------------------------------------------
#
#       Makefile for Black Magic Probe utilities
#       o  uses Opus make & mkmf utility
#       o  uses Watcom C/C++ (32-bit)
#
#       For release version, use NDEBUG=
#       Do "make depend" to update dependencies in this makefile
#
# -------------------------------------------------------------

%include makefile.cfg


# -------------------------------------------------------------
#               Defines
# -------------------------------------------------------------

%if %defined(NDEBUG)
    # retail version
    C_DEBUG=-of+s -dNDEBUG
    L_DEBUG=d all op symf
    R_DEBUG=-dNDEBUG
%else
    # development version
    C_DEBUG=-d2 -hd
    L_DEBUG=d dwarf
    R_DEBUG=
%endif

# -------------------------------------------------------------
#               Autosearch paths macro's
# -------------------------------------------------------------

CCDIR     = $(CDIR)\binnt
RCDIR     = $(CDIR)\binnt
LIBDIR    = $(CDIR)\lib
INCLUDE   = $(PUBLIC)
ASSERT    = $(PUBLIC)\assert

OBJDIR    = .\obj
.PATH.obj = $(OBJDIR)
.PATH.res = $(OBJDIR)
.PATH.map = $(OBJDIR)

.PATH.exe = ..\bin


# -------------------------------------------------------------
#               Compilers and their options
# -------------------------------------------------------------

####### C compiler
CL        = $(CCDIR)\wcl386
CC        = $(CCDIR)\wcc386
CFLAGS    = -wx -e25 $(C_DEBUG) -dNOC_FILE_DIALOG_WIN32

####### Resource compiler
RC        = $(RCDIR)\wrc
RCFLAGS   = -bt=nt -dWIN32 -i=$(INCLUDE) $(R_DEBUG)

####### Linker
LNK       = $(CCDIR)\wlink
LFLAGS    = $(L_DEBUG) SYS win95 op maxe=25 op q


# -------------------------------------------------------------
#               Inference rules
# -------------------------------------------------------------

%.exe : %.c
	$(CL) -I$(INCLUDE) $(CFLAGS) -DSTANDALONE -bw -q -fe=$(.TARGET) $(.SOURCE)

%.obj : %.c
	$(CC) -I$(INCLUDE) $(CFLAGS) -fo=$(.TARGET) $(.SOURCE)

%.res : %.rc
	$(RC) -r $(RCFLAGS) -fo=$(.TARGET) $(.SOURCE)


# -------------------------------------------------------------
#               Project
# -------------------------------------------------------------

OBJLIST_BMDEBUG = bmdebug.obj bmscan.obj bmp-script.obj elf-postlink.obj \
                  guidriver.obj minini.obj rs232.obj \
                  specialfolder.obj \
                  decodectf.obj parsetsdl.obj swotrace.obj \
                  nuklear.obj nuklear_gdip.obj noc_file_dialog.obj

OBJLIST_BMFLASH = bmflash.obj bmscan.obj bmp-script.obj bmp-support.obj crc32.obj \
                  elf-postlink.obj gdb-rsp.obj guidriver.obj minIni.obj rs232.obj \
                  specialfolder.obj xmltractor.obj \
                  nuklear.obj nuklear_gdip.obj noc_file_dialog.obj

OBJLIST_BMTRACE = bmtrace.obj bmscan.obj bmp-script.obj bmp-support.obj crc32.obj \
                  elf-postlink.obj gdb-rsp.obj guidriver.obj minini.obj rs232.obj \
                  specialfolder.obj xmltractor.obj \
                  decodectf.obj parsetsdl.obj swotrace.obj \
                  nuklear.obj nuklear_gdip.obj noc_file_dialog.obj

project : bmdebug.exe bmflash.exe bmtrace.exe bmscan.exe elf-postlink.exe tracegen.exe

depend :
    mkmf -c -dS -s -f makefile.dep $(OBJLIST_BMDEBUG) $(OBJLIST_BMFLASH) $(OBJLIST_BMTRACE)


##### C files #####

bmdebug.obj : bmdebug.c

bmflash.obj : bmflash.c

bmscan.obj : bmscan.c

bmtrace.obj : bmtrace.c

bmp-script.obj : bmp-script.c

bmp-support.obj : bmp-support.c

crc32.obj : crc32.c

elf-postlink.obj : elf-postlink.c

gdb-rsp.obj : gdb-rsp.c

guidriver.obj : guidriver.c

minIni.obj : minIni.c

rs232.obj : rs232.c

specialfolder.obj : specialfolder.c

swotrace.obj : swotrace.c

xmltractor.obj : xmltractor.c

decodectf.obj : decodectf.c

parsetsdl.obj : parsetsdl.c

noc_file_dialog.obj : noc_file_dialog.c

nuklear.obj : nuklear.c

nuklear_gdip.obj : nuklear_gdip.c


##### Resource files #####

bmdebug.res : bmdebug.rc

bmflash.res : bmflash.rc

bmtrace.res : bmtrace.rc


##### Executables #####

bmdebug.exe : $(OBJLIST_BMDEBUG) bmdebug.res
    $(LNK) $(LFLAGS) @<<
    NAME $(.TARGET)
    FIL $(.SOURCES,M"*.obj",W\,)
    LIBR winmm.lib,shell32.lib,shlwapi,setupapi.lib,lib/gdiplus-w.lib,lib/winusb-w.lib
    op resource=$(.SOURCES,M"*.res")
    op m =$(.PATH.map)\$(.TARGET,B)
    <<

bmflash.exe : $(OBJLIST_BMFLASH) bmflash.res
    $(LNK) $(LFLAGS) @<<
    NAME $(.TARGET)
    FIL $(.SOURCES,M"*.obj",W\,)
    LIBR winmm.lib,shell32.lib,shlwapi,lib/gdiplus-w.lib
    op resource=$(.SOURCES,M"*.res")
    op m =$(.PATH.map)\$(.TARGET,B)
    <<

bmtrace.exe : $(OBJLIST_BMTRACE) bmtrace.res
    $(LNK) $(LFLAGS) @<<
    NAME $(.TARGET)
    FIL $(.SOURCES,M"*.obj",W\,)
    LIBR winmm.lib,shell32.lib,shlwapi,setupapi.lib,lib/gdiplus-w.lib,lib/winusb-w.lib
    op resource=$(.SOURCES,M"*.res")
    op m =$(.PATH.map)\$(.TARGET,B)
    <<

bmscan.exe : bmscan.c
    $(CL) -I$(INCLUDE) $(CFLAGS) -DSTANDALONE -bw -q -fe=$(.TARGET) $(.SOURCE)

elf-postlink.exe : elf-postlink.c
    $(CL) -I$(INCLUDE) $(CFLAGS) -DSTANDALONE -bw -q -fe=$(.TARGET) $(.SOURCE)

tracegen.exe : tracegen.c parsetsdl.c
    $(CL) -I$(INCLUDE) $(CFLAGS) -bw -q -fe=$(.TARGET) $(.SOURCES)

# put generated dependencies at the end, otherwise it does not blend well with
# inference rules, if an item also has an explicit rule.
%include makefile.dep

