/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _UDP_MSG_H
#define _UDP_MSG_H

#define MAX_COMPRESSED_BITS       4096
#define UDP_MSG_MAX_PLAYERS          4

#include "ggponet.h"

#pragma pack(push, 1)

struct UdpMsg
{
  enum MsgType {
    Invalid = 0,
    SyncRequest = 1,
    SyncReply = 2,
    Input = 3,
    QualityReport = 4,
    QualityReply = 5,
    KeepAlive = 6,
    InputAck = 7,
    ChatCommand = 8,
    StartSpectate = 9,
  };

  struct connect_status {
    unsigned int   disconnected : 1;
    int            last_frame : 31;
  };

  struct {
    uint16         magic;
    uint16         sequence_number;
    uint8          type;            /* packet type.  Corresponds to: 'MsgType' enum */
  } header;

  union {
    struct {
      uint32      random_request;  /* please reply back with this random data */
      uint16      remote_magic;
      uint8       remote_endpoint;
    } sync_request;

    struct {
      uint32      random_reply;           /* OK, here's your random data back */
      char playerName[MAX_NAME_SIZE];   /* The name of the player we synced to: */
    } sync_reply;

    struct {
      int8        frame_advantage; /* what's the other guy's frame advantage? */
      uint32      ping;
    } quality_report;

    struct {
      uint32      pong;
    } quality_reply;

    struct {
      connect_status    peer_connect_status[UDP_MSG_MAX_PLAYERS];

      uint32            start_frame;

      int               disconnect_requested : 1;
      int               ack_frame : 31;

      uint16            num_bits;
      uint8             input_size; // XXX: shouldn't be in every single packet!
      uint8             bits[MAX_COMPRESSED_BITS]; /* must be last */
    } input;

    struct {
      int               ack_frame : 31;
    } input_ack;

    struct {
      char text[MAX_GGPOCHAT_SIZE + 1];
    } chat;

  } u;

public:
  int PacketSize() {
    return sizeof(header) + PayloadSize();
  }

  int PayloadSize() {
    int size;

    switch (header.type) {
    case SyncRequest:   return sizeof(u.sync_request);
    case SyncReply:
    {
      int res = sizeof(u.sync_reply);
      return res;
    }

    case QualityReport: return sizeof(u.quality_report);
    case QualityReply:  return sizeof(u.quality_reply);
    case InputAck:      return sizeof(u.input_ack);
    case KeepAlive:     return 0;

    case Input:
      size = (int)((char*)&u.input.bits - (char*)&u.input);
      size += (u.input.num_bits + 7) / 8;
      return size;

    case ChatCommand:
      // Include one extra byte to ensure zero termination.
      size = strnlen_s(u.chat.text, MAX_GGPOCHAT_SIZE) + 1;
      return size;
    }


    ASSERT(false);
    return 0;
  }

  UdpMsg(MsgType t) { header.type = (uint8)t; }
};

#pragma pack(pop)

#endif   
