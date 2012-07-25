/**
@file	 AsyncAudioDeviceUDP.cpp
@brief   Handle simple streaming of audio samples via UDP
@author  Tobias Blomberg / SM0SVX
@date    2012-06-25

Implements a simple "audio interface" that stream samples via
UDP. This can for example be used to stream audio to/from
GNU Radio.

\verbatim
Async - A library for programming event driven applications
Copyright (C) 2003-2012 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/



/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <sys/ioctl.h>

#include <cassert>
#include <cstdio>
#include <vector>
#include <sstream>



/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncUdpSocket.h>
#include <AsyncIpAddress.h>
#include <common.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "AsyncAudioDeviceUDP.h"
#include "AsyncAudioDeviceFactory.h"



/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;
using namespace SvxLink;


/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Prototypes
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/




/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/

REGISTER_AUDIO_DEVICE_TYPE("udp", AudioDeviceUDP);



/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/

int AudioDeviceUDP::blocksize(void)
{
  return block_size;
} /* AudioDeviceUDP::blocksize */


bool AudioDeviceUDP::isFullDuplexCapable(void)
{
  return true;
} /* AudioDeviceUDP::isFullDuplexCapable */


void AudioDeviceUDP::audioToWriteAvailable(void)
{
  audioWriteHandler();
} /* AudioDeviceUDP::audioToWriteAvailable */


void AudioDeviceUDP::flushSamples(void)
{
  audioWriteHandler();
} /* AudioDeviceUDP::flushSamples */


int AudioDeviceUDP::samplesToWrite(void) const
{
  if ((mode() != MODE_WR) && (mode() != MODE_RDWR))
  {
    return 0;
  }
  assert(sock != 0);

  int len = 0;
  if (ioctl(sock->fd(), TIOCOUTQ, &len) == -1)
  {
    return 0;
  }
  
  return len / sizeof(*read_buf);
  
} /* AudioDeviceUDP::samplesToWrite */



/****************************************************************************
 *
 * Protected member functions
 *
 ****************************************************************************/


AudioDeviceUDP::AudioDeviceUDP(const string& dev_name)
  : AudioDevice(dev_name), block_size(block_size_hint), sock(0), read_buf(0),
    read_buf_pos(0), port(0)
{
  read_buf = new int16_t[block_size * channels];
} /* AudioDeviceUDP::AudioDeviceUDP */


AudioDeviceUDP::~AudioDeviceUDP(void)
{
  delete read_buf;
} /* AudioDeviceUDP::~AudioDeviceUDP */


bool AudioDeviceUDP::openDevice(Mode mode)
{
  vector<string> args;
  vector<string>::size_type cnt = splitStr(args, devName(), ":");
  if (cnt != 2)
  {
    cerr << "*** ERROR: Illegal UDP audio device specification (" << devName()
         << "). Should be udp:ip-addr:port\n";
    return false;
  }

  ip_addr = IpAddress(args[0]);
  port = 0;
  stringstream ss(args[1]);
  ss >> port;

  if (sock != 0)
  {
    closeDevice();
  }

  switch (mode)
  {
    case MODE_WR:
      if (ip_addr.isEmpty() || (port == 0))
      {
        cerr << "*** ERROR: Illegal UDP audio device specification ("
             << devName()
             << "). Should be udp:ip-addr:port\n";
        return false;
      }
      sock = new UdpSocket;
      if (!sock->initOk())
      {
        cerr << "*** ERROR: Could not create UDP socket for writing ("
             << devName() << ")\n";
        return false;
      }
      break;
      
    case MODE_RDWR:
      if (ip_addr.isEmpty())
      {
        cerr << "*** ERROR: Illegal UDP audio device specification ("
             << devName()
             << "). Should be udp:ip-addr:port\n";
        return false;
      }
      // Fall through!
      
    case MODE_RD:
      if (port == 0)
      {
        cerr << "*** ERROR: Illegal UDP audio device specification ("
             << devName()
             << "). Should be udp:ip-addr:port\n";
        return false;
      }
      sock = new UdpSocket(port, ip_addr);
      if (!sock->initOk())
      {
        cerr << "*** ERROR: Could not bind to UDP socket (" << devName()
             << ")\n";
        return false;
      }
      sock->dataReceived.connect(
              mem_fun(*this, &AudioDeviceUDP::audioReadHandler));
      break;
      
    case MODE_NONE:
      break;
  }

  return true;
  
} /* AudioDeviceUDP::openDevice */


void AudioDeviceUDP::closeDevice(void)
{
  delete sock;
  sock = 0;
  ip_addr = IpAddress();
  port = 0;
} /* AudioDeviceUDP::closeDevice */



/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/


void AudioDeviceUDP::audioReadHandler(const IpAddress &ip, void *buf, int count)
{
  for (unsigned i=0; i < count / (channels * sizeof(int16_t)); ++i)
  {
    for (int ch=0; ch < channels; ++ch)
    {
      read_buf[read_buf_pos * channels + ch] =
              ((int16_t *)buf)[i * channels + ch];
    }
    if (++read_buf_pos == block_size)
    {
      putBlocks(read_buf, block_size);
      read_buf_pos = 0;
    }
  }
} /* AudioDeviceUDP::audioReadHandler */


void AudioDeviceUDP::audioWriteHandler(void)
{
  assert(sock != 0);
  assert((mode() == MODE_WR) || (mode() == MODE_RDWR));
  
  unsigned frags_read;
  const unsigned frag_size = block_size * sizeof(int16_t) * channels;
  do
  {
    int16_t buf[block_size * channels];
    frags_read = getBlocks(buf, 1);
    if (frags_read == 0)
    {
      break;
    }
    
      // Write the samples to the socket
    if (!sock->write(ip_addr, port, (void *)buf, frag_size))
    {
      perror("write in AudioDeviceUDP::write");
      return;
    }
  } while(frags_read == 1);

} /* AudioDeviceUDP::audioWriteHandler */



/*
 * This file has not been truncated
 */
