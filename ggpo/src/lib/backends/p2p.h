/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _P2P_H
#define _P2P_H

#include "types.h"
#include "poll.h"
#include "sync.h"
#include "backend.h"
#include "timesync.h"
#include "network/udp_proto.h"
#include <string>

 // TEMP:  Using assumed player and input sizes for now.
static const uint16 PLAYER_COUNT = 2;

// NOTE: This is the input size that 3s uses.  We should not have a hard-coded way of doing this,
// or we should actually probably just change the way we do the asserts....
static const uint16 INPUT_SIZE = 5;

// ==========================================================================================================
class Peer2PeerBackend : public GGPOSession, IPollSink, Udp::Callbacks {
public:
   Peer2PeerBackend(GGPOSessionCallbacks *cb, const char *gamename, uint16 localport, char* remoteIp, uint16 remotePort, PlayerID playerIndex, std::string playerName);
   virtual ~Peer2PeerBackend();


public:
   virtual GGPOErrorCode DoPoll(int timeout);
   virtual GGPOErrorCode AddPlayer(GGPOPlayer *player);
   virtual GGPOErrorCode AddLocalInput(PlayerID playerIndex, void *values, int totalSize);
   virtual GGPOErrorCode SyncInput(void *values, int totalSize, int playerCount);
   virtual GGPOErrorCode IncrementFrame(void);
   virtual GGPOErrorCode DisconnectPlayer(PlayerID playerIndex);
   virtual bool GetNetworkStats(GGPONetworkStats *stats);
   virtual uint32 SetFrameDelay(int delay);
   virtual GGPOErrorCode SetDisconnectTimeout(int timeout);
   virtual GGPOErrorCode SetDisconnectNotifyStart(int timeout);

   virtual bool Chat(char* text);

public:
   virtual void OnMsg(sockaddr_in &from, UdpMsg *msg, int len);

protected:
   // GGPOErrorCode PlayerHandleToQueue(PlayerID player, int *queue);

   // [OBSOLETE]
   // NOTE: Having special functions to add / minus some number to an index will be removed.
   // PlayerID QueueToPlayerHandle(PlayerID playerIndex) { return playerIndex; }
   PlayerID QueueToSpectatorHandle(int queue) { return (PlayerID)(queue + 1000); } /* out of range of the player array, basically */
   void DisconnectPlayer(PlayerID playerIndex, int syncto);
   void PollSyncEvents(void);
   void PollUdpProtocolEvents(void);
   void CheckInitialSync(void);
   int Poll2Players(int current_frame);
   int PollNPlayers(int current_frame);
   void AddRemotePlayer(char *remoteip, uint16 reportport, int queue);
   
   // [OBSOLETE]
   GGPOErrorCode AddSpectator(char *remoteip, uint16 reportport);
   virtual void OnSyncEvent(Sync::Event &e) { }
   virtual void OnUdpProtocolEvent(UdpProtocol::Event &e, PlayerID playerIndex);
   virtual void OnUdpProtocolPeerEvent(UdpProtocol::Event &e, PlayerID playerIndex);
   virtual void OnUdpProtocolSpectatorEvent(UdpProtocol::Event &e, PlayerID playerIndex);

protected:
   GGPOSessionCallbacks  _callbacks;
   PollManager           _pollMgr;
   Sync                  _sync;
   Udp                   _udp;
   UdpProtocol           *_endpoints;

   // NOTE: Spectators will likely be removed from this backend...
   UdpProtocol           _spectators[GGPO_MAX_SPECTATORS];
   int                   _num_spectators;
   int                   _input_size;

   bool                  _synchronizing;
   int                   _num_players;
   int                   _next_recommended_sleep;

   int                   _next_spectator_frame;
   int                   _disconnect_timeout;
   int                   _disconnect_notify_start;

   UdpMsg::connect_status _local_connect_status[UDP_MSG_MAX_PLAYERS];


};

#endif
