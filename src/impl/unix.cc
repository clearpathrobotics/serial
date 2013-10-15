/* Copyright 2012 William Woodall and John Harrison
 *
 * Additional authors:
 * - Mike Purvis, Clearpath Robotics, @mikepurvis
 */

#include <stdio.h>
#include <string.h>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <errno.h>
#include <paths.h>
#include <sysexits.h>
#include <termios.h>
#include <sys/param.h>
#include <pthread.h>

#if defined(__linux__)
# include <linux/serial.h>
#endif

#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#include "serial/impl/unix.h"

#ifndef TIOCINQ
#ifdef FIONREAD
#define TIOCINQ FIONREAD
#else
#define TIOCINQ 0x541B
#endif
#endif

#if defined(MAC_OS_X_VERSION_10_3) && (MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_3)
#include <IOKit/serial/ioss.h>
#endif

using std::string;
using std::stringstream;
using std::invalid_argument;
using serial::Serial;
using serial::SerialException;
using serial::PortNotOpenedException;
using serial::IOException;

/* Timespec related functions provided by @mikepurvis of Clearpath Robotics */

/* Smooth over platform variances in getting an accurate timespec
 * representing the present moment. */
static inline struct timespec
timespec_now ()
{
  struct timespec ts;
#ifdef __MACH__  // OS X does not have clock_gettime, use clock_get_time
  clock_serv_t cclock;
  mach_timespec_t mts;
  host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
  clock_get_time(cclock, &mts);
  mach_port_deallocate(mach_task_self(), cclock);
  ts.tv_sec = mts.tv_sec;
  ts.tv_nsec = mts.tv_nsec;
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return ts;
}

/* Simple function to normalize the tv_nsec field to [0..1e9), carrying
 * the remainder into the tv_sec field. This will not protect against the
 * possibility of an overflow in the nsec field--proceed with caution. */
static inline void
normalize (struct timespec* ts)
{
  while (ts->tv_nsec < 0)
  {
    ts->tv_nsec += 1e9;
    ts->tv_sec -= 1;
  }
  while (ts->tv_nsec >= 1e9)
  {
    ts->tv_nsec -= 1e9;
    ts->tv_sec += 1;
  }
}

/* Return a timespec which is the sum of two other timespecs. This
 * operator only makes logical sense when one or both of the arguments
 * represents a duration. */
static inline timespec
operator+ (const struct timespec &a, const struct timespec &b)
{
  struct timespec result = {
    a.tv_sec + b.tv_sec,
    a.tv_nsec + b.tv_nsec
  };
  normalize(&result);
  return result;
}

/* Return a timespec which is the difference of two other timespecs.
 * This operator only makes logical sense when one or both of the arguments
 * represents a duration. */
static inline timespec
operator- (const struct timespec &a, const struct timespec &b)
{
  struct timespec result = {
    a.tv_sec - b.tv_sec,
    a.tv_nsec - b.tv_nsec
  };
  normalize(&result);
  return result;
}

/* Return a timespec which is a multiplication of a timespec and a positive
 * integer. No overflow protection-- not suitable for multiplications with
 * large carries, eg a <1s timespec multiplied by a large enough integer
 * that the result is muliple seconds. Only makes sense when the timespec
 * argument is a duration. */
static inline timespec
operator* (const struct timespec &ts, const size_t mul)
{
  struct timespec result = {
    ts.tv_sec * mul,
    ts.tv_nsec * mul
  };
  normalize(&result);
  return result;
}

/* Return whichever of two timespec durations represents the shortest or most
 * negative period. */
static inline struct timespec
min (const struct timespec &a, const struct timespec &b)
{
  if (a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec))
  {
    return a;
  }
  return b;
}

/* Return a timespec duration set from a provided number of milliseconds. */
static struct timespec
timespec_from_millis (const size_t millis)
{
  struct timespec result = {0, millis * 1000000};
  normalize(&result);
  return result;
}

/* End timespec related functions */


Serial::SerialImpl::SerialImpl (const string &port, unsigned long baudrate,
                                bytesize_t bytesize,
                                parity_t parity, stopbits_t stopbits,
                                flowcontrol_t flowcontrol)
  : port_ (port), fd_ (-1), is_open_ (false), xonxoff_ (false), rtscts_ (false),
    baudrate_ (baudrate), parity_ (parity),
    bytesize_ (bytesize), stopbits_ (stopbits), flowcontrol_ (flowcontrol)
{
  pthread_mutex_init(&this->read_mutex, NULL);
  pthread_mutex_init(&this->write_mutex, NULL);
  serial::Timeout zero_timeout;
  setTimeout(zero_timeout);
  if (port_.empty () == false)
    open ();
}

Serial::SerialImpl::~SerialImpl ()
{
  close();
  pthread_mutex_destroy(&this->read_mutex);
  pthread_mutex_destroy(&this->write_mutex);
}

void
Serial::SerialImpl::open ()
{
  if (port_.empty ()) {
    throw invalid_argument ("Empty port is invalid.");
  }
  if (is_open_ == true) {
    throw SerialException ("Serial port already open.");
  }

  fd_ = ::open (port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);

  if (fd_ == -1) {
    switch (errno) {
    case EINTR:
      // Recurse because this is a recoverable error.
      open ();
      return;
    case ENFILE:
    case EMFILE:
      THROW (IOException, "Too many file handles open.");
    default:
      THROW (IOException, errno);
    }
  }

  reconfigurePort();
  is_open_ = true;
}

void
Serial::SerialImpl::reconfigurePort ()
{
  if (fd_ == -1) {
    // Can only operate on a valid file descriptor
    THROW (IOException, "Invalid file descriptor, is the serial port open?");
  }

  struct termios options; // The options for the file descriptor

  if (tcgetattr(fd_, &options) == -1) {
    THROW (IOException, "::tcgetattr");
  }

  // set up raw mode / no echo / binary
  options.c_cflag |= (tcflag_t)  (CLOCAL | CREAD);
  options.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL |
                                       ISIG | IEXTEN); //|ECHOPRT

  options.c_oflag &= (tcflag_t) ~(OPOST);
  options.c_iflag &= (tcflag_t) ~(INLCR | IGNCR | ICRNL | IGNBRK);
#ifdef IUCLC
  options.c_iflag &= (tcflag_t) ~IUCLC;
#endif
#ifdef PARMRK
  options.c_iflag &= (tcflag_t) ~PARMRK;
#endif

  // setup baud rate
  bool custom_baud = false;
  speed_t baud;
  switch (baudrate_) {
#ifdef B0
  case 0: baud = B0; break;
#endif
#ifdef B50
  case 50: baud = B50; break;
#endif
#ifdef B75
  case 75: baud = B75; break;
#endif
#ifdef B110
  case 110: baud = B110; break;
#endif
#ifdef B134
  case 134: baud = B134; break;
#endif
#ifdef B150
  case 150: baud = B150; break;
#endif
#ifdef B200
  case 200: baud = B200; break;
#endif
#ifdef B300
  case 300: baud = B300; break;
#endif
#ifdef B600
  case 600: baud = B600; break;
#endif
#ifdef B1200
  case 1200: baud = B1200; break;
#endif
#ifdef B1800
  case 1800: baud = B1800; break;
#endif
#ifdef B2400
  case 2400: baud = B2400; break;
#endif
#ifdef B4800
  case 4800: baud = B4800; break;
#endif
#ifdef B7200
  case 7200: baud = B7200; break;
#endif
#ifdef B9600
  case 9600: baud = B9600; break;
#endif
#ifdef B14400
  case 14400: baud = B14400; break;
#endif
#ifdef B19200
  case 19200: baud = B19200; break;
#endif
#ifdef B28800
  case 28800: baud = B28800; break;
#endif
#ifdef B57600
  case 57600: baud = B57600; break;
#endif
#ifdef B76800
  case 76800: baud = B76800; break;
#endif
#ifdef B38400
  case 38400: baud = B38400; break;
#endif
#ifdef B115200
  case 115200: baud = B115200; break;
#endif
#ifdef B128000
  case 128000: baud = B128000; break;
#endif
#ifdef B153600
  case 153600: baud = B153600; break;
#endif
#ifdef B230400
  case 230400: baud = B230400; break;
#endif
#ifdef B256000
  case 256000: baud = B256000; break;
#endif
#ifdef B460800
  case 460800: baud = B460800; break;
#endif
#ifdef B921600
  case 921600: baud = B921600; break;
#endif
#ifdef B1000000
  case 1000000: baud = B1000000; break;
#endif
#ifdef B1152000
  case 1152000: baud = B1152000; break;
#endif
#ifdef B1500000
  case 1500000: baud = B1500000; break;
#endif
#ifdef B2000000
  case 2000000: baud = B2000000; break;
#endif
#ifdef B2500000
  case 2500000: baud = B2500000; break;
#endif
#ifdef B3000000
  case 3000000: baud = B3000000; break;
#endif
#ifdef B3500000
  case 3500000: baud = B3500000; break;
#endif
#ifdef B4000000
  case 4000000: baud = B4000000; break;
#endif
  default:
    custom_baud = true;
    // OS X support
#if defined(MAC_OS_X_VERSION_10_4) && (MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_4)
    // Starting with Tiger, the IOSSIOSPEED ioctl can be used to set arbitrary baud rates
    // other than those specified by POSIX. The driver for the underlying serial hardware
    // ultimately determines which baud rates can be used. This ioctl sets both the input
    // and output speed.
    speed_t new_baud = static_cast<speed_t>(baudrate_);
    if (ioctl (fd_, IOSSIOSPEED, &new_baud, 1) < 0) {
      THROW (IOException, errno);
    }
    // Linux Support
#elif defined(__linux__) && defined (TIOCSSERIAL)
    struct serial_struct ser;
    ioctl (fd_, TIOCGSERIAL, &ser);
    // set custom divisor
    ser.custom_divisor = ser.baud_base / (int) baudrate_;
    // update flags
    ser.flags &= ~ASYNC_SPD_MASK;
    ser.flags |= ASYNC_SPD_CUST;

    if (ioctl (fd_, TIOCSSERIAL, &ser) < 0) {
      THROW (IOException, errno);
    }
#else
    throw invalid_argument ("OS does not currently support custom bauds");
#endif
  }
  if (custom_baud == false) {
#ifdef _BSD_SOURCE
    ::cfsetspeed(&options, baud);
#else
    ::cfsetispeed(&options, baud);
    ::cfsetospeed(&options, baud);
#endif
  }

  // setup char len
  options.c_cflag &= (tcflag_t) ~CSIZE;
  if (bytesize_ == eightbits)
    options.c_cflag |= CS8;
  else if (bytesize_ == sevenbits)
    options.c_cflag |= CS7;
  else if (bytesize_ == sixbits)
    options.c_cflag |= CS6;
  else if (bytesize_ == fivebits)
    options.c_cflag |= CS5;
  else
    throw invalid_argument ("invalid char len");
  // setup stopbits
  if (stopbits_ == stopbits_one)
    options.c_cflag &= (tcflag_t) ~(CSTOPB);
  else if (stopbits_ == stopbits_one_point_five)
    // ONE POINT FIVE same as TWO.. there is no POSIX support for 1.5
    options.c_cflag |=  (CSTOPB);
  else if (stopbits_ == stopbits_two)
    options.c_cflag |=  (CSTOPB);
  else
    throw invalid_argument ("invalid stop bit");
  // setup parity
  options.c_iflag &= (tcflag_t) ~(INPCK | ISTRIP);
  if (parity_ == parity_none) {
    options.c_cflag &= (tcflag_t) ~(PARENB | PARODD);
  } else if (parity_ == parity_even) {
    options.c_cflag &= (tcflag_t) ~(PARODD);
    options.c_cflag |=  (PARENB);
  } else if (parity_ == parity_odd) {
    options.c_cflag |=  (PARENB | PARODD);
  } else {
    throw invalid_argument ("invalid parity");
  }
  // setup flow control
  if (flowcontrol_ == flowcontrol_none) {
    xonxoff_ = false;
    rtscts_ = false;
  }
  if (flowcontrol_ == flowcontrol_software) {
    xonxoff_ = true;
    rtscts_ = false;
  }
  if (flowcontrol_ == flowcontrol_hardware) {
    xonxoff_ = false;
    rtscts_ = true;
  }
  // xonxoff
#ifdef IXANY
  if (xonxoff_)
    options.c_iflag |=  (IXON | IXOFF); //|IXANY)
  else
    options.c_iflag &= (tcflag_t) ~(IXON | IXOFF | IXANY);
#else
  if (xonxoff_)
    options.c_iflag |=  (IXON | IXOFF);
  else
    options.c_iflag &= (tcflag_t) ~(IXON | IXOFF);
#endif
  // rtscts
#ifdef CRTSCTS
  if (rtscts_)
    options.c_cflag |=  (CRTSCTS);
  else
    options.c_cflag &= (unsigned long) ~(CRTSCTS);
#elif defined CNEW_RTSCTS
  if (rtscts_)
    options.c_cflag |=  (CNEW_RTSCTS);
  else
    options.c_cflag &= (unsigned long) ~(CNEW_RTSCTS);
#else
#error "OS Support seems wrong."
#endif

  // http://www.unixwiz.net/techtips/termios-vmin-vtime.html
  // this basically sets the read call up to be a polling read,
  // but we are using select to ensure there is data available
  // to read before each call, so we should never needlessly poll
  options.c_cc[VMIN] = 0;
  options.c_cc[VTIME] = 0;

  // activate settings
  ::tcsetattr (fd_, TCSANOW, &options);
}

void
Serial::SerialImpl::close ()
{
  if (is_open_ == true) {
    if (fd_ != -1) {
      ::close (fd_); // Ignoring the outcome
      fd_ = -1;
    }
    is_open_ = false;
  }
}

bool
Serial::SerialImpl::isOpen () const
{
  return is_open_;
}

size_t
Serial::SerialImpl::available ()
{
  if (!is_open_) {
    return 0;
  }
  int count = 0;
  int result = ioctl (fd_, TIOCINQ, &count);
  if (result == 0) {
    return static_cast<size_t> (count);
  } else {
    THROW (IOException, errno);
  }
}

size_t
Serial::SerialImpl::read (uint8_t *buf, size_t size)
{
  // If the port is not open, throw
  if (!is_open_) {
    throw PortNotOpenedException ("Serial::read");
  }

  // Add the total timeout time to the current time, and mark that as the
  // overall expiry point for the function.
  struct timespec timeout_endtime(timespec_now() +
      read_timeout_constant_ + (read_timeout_multiplier_ * size));

  // If there are already some bytes waiting to read, put those in the return
  // buffer before setting up the first select call. This is important for 
  // performance reasons, as select/pselect can relinquish the thread even 
  // with data waiting.
  size_t bytes_read = 0; 
  if (available() > 0) {
    ssize_t bytes_read_now = ::read (fd_, buf, size); 
    if (bytes_read_now < 1) {
      throw SerialException ("device reports readiness to read but "
                             "returned no data (device disconnected?)");
    }
    bytes_read += static_cast<size_t> (bytes_read_now);
  }

  while (bytes_read < size) {
    // Must determine whether the time remaining before endtime (total read
    // timeout) or the inter-byte timeout is sooner, and use that one as the
    // timeout for the pselect call.
    struct timespec timeout_remaining(timeout_endtime - timespec_now());
    struct timespec timeout(min(timeout_remaining, inter_byte_timeout_));

    // Call pselect to block for serial data or a timeout
    fd_set readfds;
    FD_ZERO (&readfds);
    FD_SET (fd_, &readfds);
    int r = pselect (fd_ + 1, &readfds, NULL, NULL, &timeout, NULL);

    // Figure out what happened by looking at pselect's response 'r'
    /** Error **/
    if (r < 0) {
      // Select was interrupted, try again
      if (errno == EINTR) {
        continue;
      }
      // Otherwise there was some error
      THROW (IOException, errno);
    }
    /** Timeout **/
    if (r == 0) {
      break;
    }
    /** Something ready to read **/
    if (r > 0) {
      // Make sure our file descriptor is in the ready to read list
      if (FD_ISSET (fd_, &readfds)) {
        // This should be non-blocking returning only what is available now
        //  Then returning so that pselect can block again.
        ssize_t bytes_read_now =
          ::read (fd_, buf + bytes_read, size - bytes_read);
        // read should always return some data as pselect reported it was
        // ready to read when we get to this point.
        if (bytes_read_now < 1) {
          // Disconnected devices, at least on Linux, show the
          // behavior that they are always ready to read immediately
          // but reading returns nothing.
          throw SerialException ("device reports readiness to read but "
                                 "returned no data (device disconnected?)");
        }
        // Update bytes_read
        bytes_read += static_cast<size_t> (bytes_read_now);
        // If bytes_read == size then we have read everything we need
        if (bytes_read == size) {
          break;
        }
        // If bytes_read < size then we have more to read
        if (bytes_read < size) {
          continue;
        }
        // If bytes_read > size then we have over read, which shouldn't happen
        if (bytes_read > size) {
          throw SerialException ("read over read, too many bytes where "
                                 "read, this shouldn't happen, might be "
                                 "a logical error!");
        }
      }
      // This shouldn't happen, if r > 0 our fd has to be in the list!
      THROW (IOException, "pselect reports ready to read, but our fd isn't"
             " in the list, this shouldn't happen!");
    }
  }
  return bytes_read;
}

size_t
Serial::SerialImpl::write (const uint8_t *data, size_t length)
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::write");
  }

  // Add the total timeout time to the current time, and mark that as the
  // overall expiry point for the function.
  struct timespec timeout_endtime(timespec_now() +
      write_timeout_constant_ + (write_timeout_multiplier_ * length));

  size_t bytes_written = 0;
  while (bytes_written < length) {
    // Determine time remaining before the predetermined endpoint.
    struct timespec timeout_remaining(timeout_endtime - timespec_now());

    // Call pselect to wait on availability of port for writing.
    fd_set writefds;
    FD_ZERO (&writefds);
    FD_SET (fd_, &writefds);
    int r = pselect (fd_ + 1, NULL, &writefds, NULL, &timeout_remaining, NULL);

    // Figure out what happened by looking at select's response 'r'
    /** Error **/
    if (r < 0) {
      // Select was interrupted, try again
      if (errno == EINTR) {
        continue;
      }
      // Otherwise there was some error
      THROW (IOException, errno);
    }
    /** Timeout **/
    if (r == 0) {
      break;
    }
    /** Port ready to write **/
    if (r > 0) {
      // Make sure our file descriptor is in the ready to write list
      if (FD_ISSET (fd_, &writefds)) {
        // This will write some
        ssize_t bytes_written_now =
          ::write (fd_, data + bytes_written, length - bytes_written);
        // write should always return some data as select reported it was
        // ready to write when we get to this point.
        if (bytes_written_now < 1) {
          // Disconnected devices, at least on Linux, show the
          // behavior that they are always ready to write immediately
          // but writing returns nothing.
          throw SerialException ("device reports readiness to write but "
                                 "returned no data (device disconnected?)");
        }
        // Update bytes_written
        bytes_written += static_cast<size_t> (bytes_written_now);
        // If bytes_written == size then we have written everything we need to
        if (bytes_written == length) {
          break;
        }
        // If bytes_written < size then we have more to write
        if (bytes_written < length) {
          continue;
        }
        // If bytes_written > size then we have over written, which shouldn't happen
        if (bytes_written > length) {
          throw SerialException ("write over wrote, too many bytes where "
                                 "written, this shouldn't happen, might be "
                                 "a logical error!");
        }
      }
      // This shouldn't happen, if r > 0 our fd has to be in the list!
      THROW (IOException, "select reports ready to write, but our fd isn't"
                          " in the list, this shouldn't happen!");
    }
  }
  return bytes_written;
}

void
Serial::SerialImpl::setPort (const string &port)
{
  port_ = port;
}

string
Serial::SerialImpl::getPort () const
{
  return port_;
}

void
Serial::SerialImpl::setTimeout (const serial::Timeout &timeout)
{
  timeout_ = timeout;
  
  // Cache the timespec conversions, as that's what the rest of the inner
  // class operates on.
  inter_byte_timeout_ = timespec_from_millis(timeout.inter_byte_timeout);
  read_timeout_constant_ = timespec_from_millis(timeout.read_timeout_constant);
  read_timeout_multiplier_ = timespec_from_millis(timeout.read_timeout_constant);
  write_timeout_constant_ = timespec_from_millis(timeout.write_timeout_constant);
  write_timeout_multiplier_ = timespec_from_millis(timeout.write_timeout_multiplier);
}

serial::Timeout
Serial::SerialImpl::getTimeout () const
{
  return timeout_;
}

void
Serial::SerialImpl::setBaudrate (unsigned long baudrate)
{
  baudrate_ = baudrate;
  if (is_open_)
    reconfigurePort ();
}

unsigned long
Serial::SerialImpl::getBaudrate () const
{
  return baudrate_;
}

void
Serial::SerialImpl::setBytesize (serial::bytesize_t bytesize)
{
  bytesize_ = bytesize;
  if (is_open_)
    reconfigurePort ();
}

serial::bytesize_t
Serial::SerialImpl::getBytesize () const
{
  return bytesize_;
}

void
Serial::SerialImpl::setParity (serial::parity_t parity)
{
  parity_ = parity;
  if (is_open_)
    reconfigurePort ();
}

serial::parity_t
Serial::SerialImpl::getParity () const
{
  return parity_;
}

void
Serial::SerialImpl::setStopbits (serial::stopbits_t stopbits)
{
  stopbits_ = stopbits;
  if (is_open_)
    reconfigurePort ();
}

serial::stopbits_t
Serial::SerialImpl::getStopbits () const
{
  return stopbits_;
}

void
Serial::SerialImpl::setFlowcontrol (serial::flowcontrol_t flowcontrol)
{
  flowcontrol_ = flowcontrol;
  if (is_open_)
    reconfigurePort ();
}

serial::flowcontrol_t
Serial::SerialImpl::getFlowcontrol () const
{
  return flowcontrol_;
}

void
Serial::SerialImpl::flush ()
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::flush");
  }
  tcdrain (fd_);
}

void
Serial::SerialImpl::flushInput ()
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::flushInput");
  }
  tcflush (fd_, TCIFLUSH);
}

void
Serial::SerialImpl::flushOutput ()
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::flushOutput");
  }
  tcflush (fd_, TCOFLUSH);
}

void
Serial::SerialImpl::sendBreak (int duration)
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::sendBreak");
  }
  tcsendbreak (fd_, static_cast<int> (duration / 4));
}

void
Serial::SerialImpl::setBreak (bool level)
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::setBreak");
  }
  if (level) {
    ioctl (fd_, TIOCSBRK);
  } else {
    ioctl (fd_, TIOCCBRK);
  }
}

void
Serial::SerialImpl::setRTS (bool level)
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::setRTS");
  }
  if (level) {
    ioctl (fd_, TIOCMBIS, TIOCM_RTS);
  } else {
    ioctl (fd_, TIOCMBIC, TIOCM_RTS);
  }
}

void
Serial::SerialImpl::setDTR (bool level)
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::setDTR");
  }
  if (level) {
    ioctl (fd_, TIOCMBIS, TIOCM_DTR);
  } else {
    ioctl (fd_, TIOCMBIC, TIOCM_DTR);
  }
}

bool
Serial::SerialImpl::waitForChange ()
{
#ifndef TIOCMIWAIT
  while (is_open_ == true) {
    int s = ioctl (fd_, TIOCMGET, 0);
    if ((s & TIOCM_CTS) != 0) return true;
    if ((s & TIOCM_DSR) != 0) return true;
    if ((s & TIOCM_RI) != 0) return true;
    if ((s & TIOCM_CD) != 0) return true;
    usleep(1000);
  }
  return false;
#else
  if (ioctl(fd_, TIOCMIWAIT, (TIOCM_CD|TIOCM_DSR|TIOCM_RI|TIOCM_CTS)) != 0) {
    stringstream ss;
    ss << "waitForDSR failed on a call to ioctl(TIOCMIWAIT): "
       << errno << " " << strerror(errno);
    throw(SerialException(ss.str().c_str()));
  }
  return true;
#endif
}

bool
Serial::SerialImpl::getCTS ()
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::getCTS");
  }
  int s = ioctl (fd_, TIOCMGET, 0);
  return (s & TIOCM_CTS) != 0;
}

bool
Serial::SerialImpl::getDSR ()
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::getDSR");
  }
  int s = ioctl (fd_, TIOCMGET, 0);
  return (s & TIOCM_DSR) != 0;
}

bool
Serial::SerialImpl::getRI ()
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::getRI");
  }
  int s = ioctl (fd_, TIOCMGET, 0);
  return (s & TIOCM_RI) != 0;
}

bool
Serial::SerialImpl::getCD ()
{
  if (is_open_ == false) {
    throw PortNotOpenedException ("Serial::getCD");
  }
  int s = ioctl (fd_, TIOCMGET, 0);
  return (s & TIOCM_CD) != 0;
}

void
Serial::SerialImpl::readLock ()
{
  int result = pthread_mutex_lock(&this->read_mutex);
  if (result) {
    THROW (IOException, result);
  }
}

void
Serial::SerialImpl::readUnlock ()
{
  int result = pthread_mutex_unlock(&this->read_mutex);
  if (result) {
    THROW (IOException, result);
  }
}

void
Serial::SerialImpl::writeLock ()
{
  int result = pthread_mutex_lock(&this->write_mutex);
  if (result) {
    THROW (IOException, result);
  }
}

void
Serial::SerialImpl::writeUnlock ()
{
  int result = pthread_mutex_unlock(&this->write_mutex);
  if (result) {
    THROW (IOException, result);
  }
}
