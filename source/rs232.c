/*  rs232 - RS232 support, limited to the functions that the GDB RSP needs
 *
 *  Copyright 2012-2019, CompuPhase
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
#include <string.h>
#if defined _WIN32
  #include <windows.h>
#else
  #include <stdio.h>
  #include <fcntl.h>
  #include <termios.h>
  #include <unistd.h>
  #include <sys/ioctl.h>
#endif
#include "rs232.h"


#if !defined sizearray
  #define sizearray(a)    (sizeof(a) / sizeof((a)[0]))
#endif

#if defined _WIN32
  static HANDLE hCom = INVALID_HANDLE_VALUE;
#else /* _WIN32 */
  static int fdCom = -1;
  static struct termios oldtio;
#endif /* _WIN32 */


/** rs232_open() opens the RS232 port and sets the initial parameters.
 *
 *  \param port     Must be set to COM* (where * is a number) in Windows and a
 *                  serial tty device name (such as ttyS0 or ttyACM0) in Linux.
 *  \param baud     Set the baud rate, or 0 to keep the default baud rate.
 *  \param databits The number of data bits: should be 7 or 8 (or 0 to keep the
 *                  default value).
 *  \param stopbits The number of stop bits: should be 1 or 2 (the special case
 *                  of 1.5 stop bits is currently not supported). Can be set to
 *                  0 to keep the default value.
 *  \param parity   The parity setting for the serial connection.
 *
 *  \return An error code, 0 on success.
 *
 *  \note Flow control settings are currently not supported.
 */
int rs232_open(const char *port, unsigned baud, int databits, int stopbits, int parity)
{
  #if defined _WIN32
    DCB dcb;
    COMMTIMEOUTS commtimeouts;

    /* set up the connection */
    hCom=CreateFileA(port,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (hCom==INVALID_HANDLE_VALUE && strlen(port)<10) {
      /* try with prefix */
      char buffer[40]="\\\\.\\";
      strcat(buffer,port);
      hCom=CreateFileA(buffer,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    }
    if (hCom==INVALID_HANDLE_VALUE)
      return ENXIO;

    GetCommState(hCom,&dcb);
    /* first set the baud rate only, because this may fail for a non-standard
     * baud rate
     */
    if (baud!=0) {
      dcb.BaudRate=baud;
      if (!SetCommState(hCom,&dcb) || dcb.BaudRate!=baud) {
        /* find the highest standard baud rate below the requated rate */
        static const unsigned stdbaud[] = {1200, 2400, 4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200, 230400 };
        int i;
        for (i=0; (i+1)<(int)sizearray(stdbaud) && stdbaud[i]<baud; i++)
          /* nothing */;
        dcb.BaudRate=stdbaud[i];
      }
    }
    if (databits>0)
      dcb.ByteSize=8;
    if (stopbits>0)
      dcb.StopBits=(stopbits==2) ? TWOSTOPBITS : ONESTOPBIT;
    if (parity>0)
      dcb.Parity=(BYTE)(parity-1);
    dcb.fDtrControl=DTR_CONTROL_DISABLE;
    dcb.fOutX=FALSE;
    dcb.fInX=FALSE;
    dcb.fNull=FALSE;
    dcb.fRtsControl=RTS_CONTROL_DISABLE;
    SetCommState(hCom,&dcb);
    SetCommMask(hCom,EV_RXCHAR|EV_TXEMPTY);

    commtimeouts.ReadIntervalTimeout        =0x7fffffff;
    commtimeouts.ReadTotalTimeoutMultiplier =0;
    commtimeouts.ReadTotalTimeoutConstant   =1;
    commtimeouts.WriteTotalTimeoutMultiplier=0;
    commtimeouts.WriteTotalTimeoutConstant  =0;
    SetCommTimeouts(hCom,&commtimeouts);
  #else /* _WIN32 */
    struct termios newtio;

    /* open the serial port device file
     * O_NDELAY   - tells port to operate and ignore the DCD line
     * O_NONBLOCK - same as O_NDELAY under Linux
     * O_NOCTTY   - this process is not to become the controlling
     *              process for the port. The driver will not send
     *              this process signals due to keyboard aborts, etc.
     */
    fdCom = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_NDELAY);
    if (fdCom < 0) {
      char portdev[60];
      sprintf(portdev, "/dev/%s", port);
      fdCom = open(portdev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_NDELAY);
      if (fdCom < 0)
        return errno;
    } /* if */

    tcgetattr(fdCom, &oldtio); /* save current port settings */
    memset(&newtio, 0, sizeof newtio);

    /* CREAD  - receiver enabled
       CLOCAL - ignore modem control lines */
    newtio.c_cflag = CLOCAL | CREAD;
    newtio.c_cflag |= (databits==7) ? CS7 : CS8;
    if (stopbits==2)
      newtio.c_cflag |= CSTOPB;
    if (parity>0) {
      newtio.c_cflag |= PARENB;
      if (parity==PAR_ODD)
        newtio.c_cflag |= PARODD;
    }
    #define NEWTERMIOS_SETBAUDARTE(bps) newtio.c_cflag |= bps;
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

    newtio.c_iflag = IGNPAR | IGNBRK; /* ignore parity and BREAK conditions */
    newtio.c_oflag = 0; /* set output mode (non-canonical, no processing,...) */
    newtio.c_lflag = 0; /* set input mode (non-canonical, no echo,...) */

    /* with VMIN==0 && VTIME==0, read() will always return immediately; if no
     * data is available it will return with no characters read
     */
    newtio.c_cc[VTIME]=0; /* inter-character timer used (increments of 0.1 second) */
    newtio.c_cc[VMIN] =0; /* blocking read until 0 chars received */

    tcflush(fdCom, TCIFLUSH);
    if (tcsetattr(fdCom, TCSANOW, &newtio))
      return EPROTONOSUPPORT;

    /* Set up for no delay, ie non-blocking reads will occur. When we read, we'll
     * get what's in the input buffer or nothing
     */
    fcntl(fdCom, F_SETFL,FNDELAY);
  #endif /* _WIN32 */

  return 0;
}

void rs232_close(void)
{
  #if defined _WIN32
    if (hCom != INVALID_HANDLE_VALUE) {
      BOOL result = FlushFileBuffers(hCom);
      if (result || GetLastError() != ERROR_INVALID_HANDLE)
        CloseHandle(hCom);
      hCom = INVALID_HANDLE_VALUE;
    }
  #else /* _WIN32 */
    if (fdCom >= 0) {
      tcflush(fdCom, TCOFLUSH);
      tcflush(fdCom, TCIFLUSH);
      tcsetattr(fdCom, TCSANOW, &oldtio);
      close(fdCom);
      fdCom = -1;
    }
  #endif /* _WIN32 */
}

int rs232_isopen(void)
{
  #if defined _WIN32
    return hCom != INVALID_HANDLE_VALUE;
  #else /* _WIN32 */
    return fdCom >= 0;
  #endif /* _WIN32 */
}

size_t rs232_send(const unsigned char *buffer, size_t size)
{
  #if defined _WIN32
    DWORD written = 0;
    if (hCom != INVALID_HANDLE_VALUE) {
      if (!WriteFile(hCom, buffer, size, &written, NULL))
        written = 0;
    }
    return (size_t)written;
  #else /* _WIN32 */
    return (fdCom>=0) ? write(fdCom, buffer, size) : 0;
  #endif /* _WIN32 */
}

size_t rs232_recv(unsigned char *buffer, size_t size)
{
  #if defined _WIN32
    DWORD read = 0;
    if (hCom != INVALID_HANDLE_VALUE) {
      if (!ReadFile(hCom, buffer, size, &read, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_HANDLE)
          hCom = INVALID_HANDLE_VALUE;
        else if (error == ERROR_ACCESS_DENIED)
          rs232_close();
        read = 0;
      }
    }
    return (size_t)read;
  #else /* _WIN32 */
    if (fdCom >= 0) {
      int num = (int)read(fdCom, buffer, size);
      if (num < 0)
        rs232_close();
      return num;
    }
    return 0;
  #endif /* _WIN32 */
}

void rs232_break(void)
{
  #if defined _WIN32
    if (hCom!=INVALID_HANDLE_VALUE) {
      SetCommBreak(hCom);
      Sleep(200);
      ClearCommBreak(hCom);
    }
  #else /* _WIN32 */
    if (fdCom>=0)
      tcsendbreak(fdCom, 0);
  #endif /* _WIN32 */
}

void rs232_dtr(int set)
{
  #if defined _WIN32
    if (hCom!=INVALID_HANDLE_VALUE)
      EscapeCommFunction(hCom, set ? SETDTR : CLRDTR);
  #else /* _WIN32 */
    if (fdCom>=0) {
      int flags;
      ioctl(fdCom,TIOCMGET,&flags);
      if (set)
        flags |= TIOCM_DTR;
      else
        flags &= ~TIOCM_DTR;
      ioctl(fdCom,TIOCMSET,&flags);
    }
  #endif /* _WIN32 */
}

void rs232_rts(int set)
{
  #if defined _WIN32
  if (hCom!=INVALID_HANDLE_VALUE)
    EscapeCommFunction(hCom, set ? SETRTS : CLRRTS);
  #else /* _WIN32 */
    if (fdCom>=0) {
      int flags;
      ioctl(fdCom,TIOCMGET,&flags);
      if (set)
        flags |= TIOCM_RTS;
      else
        flags &= ~TIOCM_RTS;
      ioctl(fdCom,TIOCMSET,&flags);
    }
  #endif /* _WIN32 */
}

