/*
 *  rs232 - RS232 support, limited to the functions that the GDB RSP needs.
 *
 *  Copyright 2012-2023, CompuPhase
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may not
 *  use this file except in compliance with the License. You may obtain a copy
 *  of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *  License for the specific language governing permissions and limitations
 *  under the License.
 */
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if defined _WIN32
# include <windows.h>
#else
# include <ctype.h>
# include <dirent.h>
# include <fcntl.h>
# include <stdio.h>
# include <string.h>
# include <termios.h>
# include <unistd.h>
# include <sys/ioctl.h>
#endif
#include "rs232.h"

#if defined FORTIFY
# include <alloc/fortify.h>
#endif

#if defined _MSC_VER
# define strdup(s)   _strdup(s)
#endif


#if !defined sizearray
# define sizearray(a)    (sizeof(a) / sizeof((a)[0]))
#endif
#define MAX_COMPORTS  4

static HCOM comport[MAX_COMPORTS];
static int initialized = 0;

#if !defined _WIN32
# define INVALID_HANDLE_VALUE (-1)
  static struct termios oldtio;
#endif /* _WIN32 */


static void check_init(void)
{
  if (!initialized) {
    int i;
    for (i = 0; i < MAX_COMPORTS; i++)
      comport[i] = INVALID_HANDLE_VALUE;
    initialized = 1;
  }
}

/** rs232_open() opens the RS232 port and sets the initial parameters.
 *
 *  \param port     Must be set to COM* (where * is a number) in Windows and a
 *                  serial tty device name (such as ttyS0 or ttyACM0) in Linux.
 *  \param baud     Set the baud rate, or -1 to keep the default baud rate.
 *  \param databits The number of data bits: should be between 7 and 8 (or -1 to
 *                  keep the default value).
 *  \param stopbits The number of stop bits: should be 1 or 2 (the special case
 *                  of 1.5 stop bits is currently not supported). Can be set to
 *                  -1 to keep the default value.
 *  \param parity   The parity setting for the serial connection. Can be set to
 *                  -1 to keep the current value.
 *  \param flowctrl Flow control.
 *
 *  \return A handle (file descriptor) to the port, or NULL on failure.
 */
HCOM* rs232_open(const char *port, unsigned baud, int databits, int stopbits, int parity, int flowctrl)
{
# if defined _WIN32
    DCB dcb;
    COMMTIMEOUTS commtimeouts;
# else
    struct termios newtio;
# endif
  HCOM *hCom = NULL;

  /* find available slot */
  check_init();
  for (int i=0; hCom==NULL && i<MAX_COMPORTS; i++)
    if (comport[i]==INVALID_HANDLE_VALUE)
      hCom=&comport[i];
  if (hCom==NULL)
    return NULL;

# if defined _WIN32
    /* set up the connection */
    *hCom = CreateFileA(port,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (*hCom==INVALID_HANDLE_VALUE && strlen(port)<10) {
      /* try with prefix */
      char buffer[40]="\\\\.\\";
      strcat(buffer,port);
      *hCom = CreateFileA(buffer,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    }
    if (*hCom == INVALID_HANDLE_VALUE)
      return NULL;

    GetCommState(*hCom,&dcb);
    /* first set the baud rate only, because this may fail for a non-standard
     * baud rate
     */
    if (baud>0) {
      dcb.BaudRate=baud;
      if (!SetCommState(*hCom,&dcb) || dcb.BaudRate!=baud) {
        /* find the highest standard baud rate below the requated rate */
        static const unsigned stdbaud[] = {1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200, 230400 };
        int i;
        for (i=0; (i+1)<(int)sizearray(stdbaud) && stdbaud[i]<baud; i++)
          /* nothing */;
        dcb.BaudRate=stdbaud[i];
      }
    }
    if (databits>=0)
      dcb.ByteSize=databits;
    if (stopbits>0)
      dcb.StopBits=(stopbits==2) ? TWOSTOPBITS : ONESTOPBIT;
    if (parity>=0)
      dcb.Parity=(BYTE)parity;
    dcb.fParity=(parity>0);
    dcb.fDtrControl=DTR_CONTROL_DISABLE;
    dcb.fRtsControl=(flowctrl==FLOWCTRL_RTSCTS) ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_DISABLE;
    dcb.fOutX=(flowctrl==FLOWCTRL_XONXOFF);
    dcb.fInX=(flowctrl==FLOWCTRL_XONXOFF);
    /* leave the buffer limits and others at their defaults (if there are defaults) */
    if (dcb.XonChar == 0)
      dcb.XonChar = 0x11;
    if (dcb.XoffChar == 0)
      dcb.XoffChar = 0x13;
    if (dcb.XoffLim == 0)
      dcb.XoffLim = 128;
    if (dcb.XonLim == 0)
      dcb.XonLim = 512;
    dcb.fNull=FALSE;
    SetCommState(*hCom,&dcb);
    SetCommMask(*hCom,EV_RXCHAR|EV_TXEMPTY);

    commtimeouts.ReadIntervalTimeout        =0x7fffffff;
    commtimeouts.ReadTotalTimeoutMultiplier =0;
    commtimeouts.ReadTotalTimeoutConstant   =1;
    commtimeouts.WriteTotalTimeoutMultiplier=0;
    commtimeouts.WriteTotalTimeoutConstant  =0;
    SetCommTimeouts(*hCom,&commtimeouts);
# else /* _WIN32 */
    /* open the serial port device file
     * O_NDELAY   - tells port to operate and ignore the DCD line
     * O_NONBLOCK - same as O_NDELAY under Linux
     * O_NOCTTY   - this process is not to become the controlling
     *              process for the port. The driver will not send
     *              this process signals due to keyboard aborts, etc.
     */
    *hCom = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_NDELAY);
    if (*hCom < 0) {
      char portdev[60];
      sprintf(portdev, "/dev/%s", port);
      *hCom = open(portdev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_NDELAY);
      if (*hCom < 0) {
        *hCom = INVALID_HANDLE_VALUE;
        return NULL;
      }
    }

    tcgetattr(*hCom, &oldtio); /* save current port settings */
    memset(&newtio, 0, sizeof newtio);

    /* CREAD  - receiver enabled
       CLOCAL - ignore modem control lines */
    newtio.c_cflag = CLOCAL | CREAD;
    newtio.c_cflag |= (databits==7) ? CS7 : CS8;
    if (stopbits==2)
      newtio.c_cflag |= CSTOPB;
    if (parity>0) {
      newtio.c_cflag |= PARENB;
      if (parity==PAR_ODD || parity==PAR_MARK)
        newtio.c_cflag |= PARODD;
      if (parity==PAR_MARK || parity==PAR_SPACE)
        newtio.c_cflag |= CMSPAR;
    }
    if (flowctrl==FLOWCTRL_RTSCTS)
      newtio.c_cflag |= CRTSCTS;
#   define NEWTERMIOS_SETBAUDARTE(bps) newtio.c_cflag |= bps;
    switch (baud) {
    #ifdef B1152000
      case 1152000: NEWTERMIOS_SETBAUDARTE( B1152000 ); break;
    #endif // B1152000
    #ifdef B576000
      case  576000: NEWTERMIOS_SETBAUDARTE( B576000 ); break;
    #endif // B576000
    #ifdef B230400
      case  230400: NEWTERMIOS_SETBAUDARTE( B230400 ); break;
    #endif // B230400
    #ifdef B115200
      case  115200: NEWTERMIOS_SETBAUDARTE( B115200 ); break;
    #endif // B115200
    #ifdef B57600
      case   57600: NEWTERMIOS_SETBAUDARTE( B57600 ); break;
    #endif // B57600
    #ifdef B38400
      case   38400: NEWTERMIOS_SETBAUDARTE( B38400 ); break;
    #endif // B38400
    #ifdef B19200
      case   19200: NEWTERMIOS_SETBAUDARTE( B19200 ); break;
    #endif // B19200
    #ifdef B9600
      case    9600: NEWTERMIOS_SETBAUDARTE( B9600 ); break;
    #endif // B9600
    }

    newtio.c_iflag = IGNPAR | IGNBRK; /* ignore parity and break conditions */
    if (flowctrl==FLOWCTRL_XONXOFF)
      newtio.c_iflag |= IXON | IXOFF;
    newtio.c_oflag = 0; /* set output mode (non-canonical, no processing,...) */
    newtio.c_lflag = 0; /* set input mode (non-canonical, no echo,...) */

    /* with VMIN==0 && VTIME==0, read() will always return immediately; if no
     * data is available it will return with no characters read
     */
    newtio.c_cc[VTIME]=0; /* inter-character timer used (increments of 0.1 second) */
    newtio.c_cc[VMIN] =0; /* blocking read until 0 chars received */

    tcflush(*hCom, TCIFLUSH);
    if (tcsetattr(*hCom, TCSANOW, &newtio)) {
      *hCom = INVALID_HANDLE_VALUE;
      return NULL;
    }

    /* Set up for no delay, ie non-blocking reads will occur. When we read, we'll
     * get what's in the input buffer or nothing
     */
    fcntl(*hCom, F_SETFL,FNDELAY);
# endif /* _WIN32 */

  return hCom;
}

HCOM *rs232_close(HCOM *hCom)
{
  if (rs232_isopen(hCom)) {
#   if defined _WIN32
      BOOL result = FlushFileBuffers(*hCom);
      if (result || GetLastError() != ERROR_INVALID_HANDLE)
        CloseHandle(*hCom);
#   else /* _WIN32 */
      tcflush(*hCom, TCOFLUSH);
      tcflush(*hCom, TCIFLUSH);
      tcsetattr(*hCom, TCSANOW, &oldtio);
      close(*hCom);
#   endif /* _WIN32 */
    *hCom = INVALID_HANDLE_VALUE;
  }
  return NULL;
}

bool rs232_isopen(const HCOM *hCom)
{
  if (hCom == NULL)
    return false;

  check_init();
  for (int i = 0; i < MAX_COMPORTS; i++)
    if (&comport[i] == hCom && comport[i] != INVALID_HANDLE_VALUE)
      return true;
  return false;
}

size_t rs232_xmit(HCOM *hCom, const unsigned char *buffer, size_t size)
{
  if (rs232_isopen(hCom)) {
#   if defined _WIN32
      DWORD written = 0;
      if (!WriteFile(*hCom, buffer, size, &written, NULL))
        written = 0;
      return (size_t)written;
#   else /* _WIN32 */
      size_t written = write(*hCom, buffer, size);
      tcdrain(*hCom);
      return written;
#   endif /* _WIN32 */
  }
  return 0;
}

/** rs232_recv() reads from the serial port; a read is non-blocking (if there
 *  is no data, the function returns immediately, with return value 0).
 *
 *  \return The number of bytes received and stored in the buffer.
 */
size_t rs232_recv(HCOM *hCom, unsigned char *buffer, size_t size)
{
  if (rs232_isopen(hCom)) {
#   if defined _WIN32
      DWORD read = 0;
      if (!ReadFile(*hCom, buffer, size, &read, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_HANDLE)
          *hCom = INVALID_HANDLE_VALUE; /* mark as invalid without attempting to close the handle */
        else if (error == ERROR_ACCESS_DENIED)
          rs232_close(hCom);
        read = 0;
      }
      return (size_t)read;
#   else /* _WIN32 */
      int num = (int)read(*hCom, buffer, size);
      if (num < 0) {
        rs232_close(hCom);
        num = 0;
      }
      return num;
#   endif /* _WIN32 */
  }
  return 0;
}

void rs232_flush(HCOM *hCom)
{
  if (rs232_isopen(hCom)) {
#   if defined _WIN32
      FlushFileBuffers(*hCom);
#   else
      tcdrain(*hCom);
      tcflush(*hCom, TCOFLUSH);
      tcflush(*hCom, TCIFLUSH);
#   endif
  }
}

size_t rs232_peek(HCOM *hCom)
{
  if (rs232_isopen(hCom)) {
#   if defined _WIN32
      DWORD errflags = 0;
      COMSTAT comstat;
      ClearCommError(*hCom, &errflags, &comstat);
      return comstat.cbInQue;
#   else
      int bytes;
      ioctl(*hCom, FIONREAD, &bytes);
      return bytes;
#   endif
  }
  return 0;
}

/** rs232_setstatus() sets a line status.
 *  \param hCom     Port handle.
 *  \param code     The line status id, must be LINESTAT_RTS, LINESTAT_DTR or
 *                  LINESTAT_LBREAK.
 *  \param status   1 to set, 0 to clear.
 */
void rs232_setstatus(HCOM *hCom, int code, int status)
{
  if (rs232_isopen(hCom)) {
#   if defined _WIN32
      switch (code) {
      case LINESTAT_RTS:
        EscapeCommFunction(*hCom, status ? SETRTS : CLRRTS);
        break;
      case LINESTAT_DTR:
        EscapeCommFunction(*hCom, status ? SETDTR : CLRDTR);
        break;
      case LINESTAT_LBREAK:
        if (status)
          SetCommBreak(*hCom);
        else
          ClearCommBreak(*hCom);
        break;
      }
#   else /* _WIN32 */
      int flags;
      ioctl(*hCom,TIOCMGET,&flags);
      switch (code) {
      case LINESTAT_RTS:
        if (status)
          flags |= TIOCM_RTS;
        else
          flags &= ~TIOCM_RTS;
        break;
      case LINESTAT_DTR:
        if (status)
          flags |= TIOCM_DTR;
        else
          flags &= ~TIOCM_DTR;
        break;
      case LINESTAT_LBREAK:
        if (status)
          tcsendbreak(*hCom, 0);
        break;
      }
      ioctl(*hCom,TIOCMSET,&flags);
#   endif /* _WIN32 */
  }
}

/** rs232_getstatus() sets a line status.
 *  \param hCom     Port handle.
 *
 *  \return A bit mask with all line statuses.
 */
unsigned rs232_getstatus(HCOM *hCom)
{
  unsigned result = 0;
  if (rs232_isopen(hCom)) {
#   if defined _WIN32
      DWORD flags;
      if (!GetCommModemStatus(*hCom,&flags))
        flags = 0;
      if (flags & MS_CTS_ON)
        result |= LINESTAT_CTS;
      if (flags & MS_DSR_ON)
        result |= LINESTAT_DSR;
      if (flags & MS_RING_ON)
        result |= LINESTAT_RI;
      if (flags & MS_RLSD_ON)
        result |= LINESTAT_CD;
#   else /* _WIN32 */
      int flags;
      ioctl(*hCom,TIOCMGET,&flags);
      if (flags & TIOCM_RTS)
        result |= LINESTAT_RTS;
      if (flags & TIOCM_DTR)
        result |= LINESTAT_DTR;
      if (flags & TIOCM_CTS)
        result |= LINESTAT_CTS;
      if (flags & TIOCM_DSR)
        result |= LINESTAT_DSR;
      if (flags & TIOCM_RI)
        result |= LINESTAT_RI;
      if (flags & TIOCM_CD)
        result |= LINESTAT_CD;
#   endif /* _WIN32 */
  }
  return result;
}

void rs232_framecheck(HCOM *hCom, int enable)
{
# if !defined _WIN32
    /* detect BREAK: ff 00 00 is sent when neither IGNBRK nor BRKINT are set,
       but PARMRK is set

       detect parity/framing error: ff 00 is sent when PARMRK is set, but
       IGNPAR is not set; INPCK must also be set

       When ff is a valid data byte, it is doubled, so that the caller can
       distinguish it from a framing error/break
     */
    struct termios tio;
    tcgetattr(*hCom, &tio);
    if (enable)
      tio.c_iflag = (tio.c_iflag & ~(IGNPAR | IGNBRK)) | (PARMRK | INPCK);
    else
      tio.c_iflag = (tio.c_iflag & ~(PARMRK | INPCK)) | (IGNPAR | IGNBRK);
    tcsetattr(*hCom, TCSANOW, &tio);
# endif
}

static int portname_compare(const char *name1,const char *name2)
{
  /* it is common for Linux to list 30 ttyS* devices, but typically only one
     or two are valid; our solution is to list them last */
  int stddev1=strncmp(name1,"ttyS",4)==0;
  int stddev2=strncmp(name2,"ttyS",4)==0;
  if (stddev1!= stddev2)
    return stddev1-stddev2;

  /* if both names have the same alphabetical prefix, sort them numerically (so
     COM10 comes after COM2), but if both have a different prefix, sort
     alphabetically */
  int pos1;
  for (pos1=0; isalpha(name1[pos1]); pos1++)
    {}
  int pos2;
  for (pos2=0; isalpha(name2[pos2]); pos2++)
    {}
  if (pos1==pos2 && strncmp(name1,name2,pos1)==0) {
    /* same base name, check the number behind it */
    int seq1=(int)strtol(name1+pos1,NULL,10);
    int seq2=(int)strtol(name2+pos2,NULL,10);
    return seq1-seq2;
  } else {
    return strcmp(name1,name2);
  }
}

/** rs232_collect() detects the available serial ports.
 *  \param portlist   A pointer to an array of character pointers.
 *  \param listsize   The number of pointers in the "portlist" parameter
 *
 *  \return The number of ports detected. This value may be larger or smaller
 *          than the listsize parameter. If it is larger, the last ports were
 *          not stored in the portlist array.
 *
 *  \note To query how many ports are available, call this function with
 *        portlist set to NULL and listsize set to zero. Use the return value to
 *        allocate the required array size. Then, call the function again with
 *        a valid array in "portlist" and the listsize apprpriately set.
 *
 *        The entries in the array must be freed with free().
 */
int rs232_collect(char **portlist, int listsize)
{
  int count;

  for (count = 0; count < listsize; count++) {
    assert(portlist != NULL);
    portlist[count] = NULL;
  }

# if defined _WIN32
    HKEY hkey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,"HARDWARE\\DEVICEMAP\\SERIALCOMM",0,KEY_READ,&hkey)==ERROR_SUCCESS) {
      for (count=0;; count++) {
        char name[128],value[128];
        DWORD sz_name=sizearray(name);
        DWORD sz_value=sizearray(value);
        if (RegEnumValue(hkey,count,name,&sz_name,NULL,NULL,(unsigned char*)value,&sz_value)!=ERROR_SUCCESS)
          break;
        if (count<listsize) {
          assert(portlist!=NULL);
          portlist[count]=strdup(value);
        }
      }
      RegCloseKey(hkey);
    }
# else /* _WIN32 */
    DIR *dp=opendir("/dev");
    struct dirent *dirp;
    count=0;
    while ((dirp=readdir(dp))!=NULL) {
      if (strncmp(dirp->d_name,"ttyACM",6)==0 || strncmp(dirp->d_name,"ttyUSB",6)==0
          || (strncmp(dirp->d_name,"ttyS",4)==0 && isdigit(dirp->d_name[4])))
      {
        if (count<listsize) {
          assert(portlist!=NULL);
          portlist[count]=strdup(dirp->d_name);
        }
        count++;
      }
    }
    closedir(dp);
# endif /* _WIN32 */

  if (portlist!=NULL) {
    /* sort the entries in the list (insertion sort) */
    int top=(count<listsize) ? count : listsize;
    for (int i=1; i<top; i++) {
      char *key=portlist[i];
      int j;
      for (j=i; j>0 && portname_compare(portlist[j-1],key)>0; j--)
        portlist[j]=portlist[j-1];
      portlist[j]=key;
    }
  }

  return count;
}
