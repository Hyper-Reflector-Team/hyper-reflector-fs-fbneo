// Spectating via the backend relay (src/util-servers/spectate-relay/main.go in the
// hyper-reflector-backend repo), NOT GGPO. A relayed feed only ever carries already-confirmed
// input (the relay is a dumb forwarder, no prediction happens anywhere in this path), so the
// viewer side just applies input directly each tick like a live network replay -- there's no
// rollback, and GGPOSession/Peer2PeerBackend/SpectatorBackend are never touched by this file.
//
// The live per-frame feed rides TCP, not UDP. Earlier versions of this file used UDP (matching
// GGPO's own P2P input packets), but that was the wrong model: GGPO's UDP traffic tolerates loss
// because every packet carries several recent frames and rollback can correct a late/wrong
// prediction. A spectator feed has neither -- it's a straight, ordered stream of already-confirmed
// data with nothing analogous to rollback on the receiving end, so a single dropped UDP packet
// used to strand a spectator forever waiting on a frame number that would never be resent. TCP's
// own reliable, ordered delivery makes that whole class of bug impossible by construction (see
// Project Slippi's spectator protocol for prior art: it makes the same choice, over ENet's
// reliable channel, specifically to avoid needing any catch-up/gap-recovery machinery).
//
// Wire protocol (must match src/util-servers/spectate-relay/main.go exactly):
//
//   UDP -- presence and chat only. Both are fine to lose occasionally since they're
//   self-correcting (heartbeats resend periodically; a missed spectator-count update is
//   overwritten by the next change). Every packet is one JSON object:
//     {"type":"publish","matchId":"...","uid":"..."}         host -> relay, periodic keepalive
//     {"type":"unpublish","matchId":"..."}                   host -> relay, explicit stop
//     {"type":"watch","matchId":"...","uid":"..."}           viewer -> relay, periodic keepalive
//     {"type":"unwatch","matchId":"...","uid":"..."}         viewer -> relay, explicit stop
//     {"type":"chat","matchId":"...","uid":"...","userName":"...","text":"..."}
//     {"type":"spectator-count","matchId":"...","count":N}   relay -> host/viewers
//
//   TCP -- the state-snapshot handoff for mid-match join, AND the ongoing live frame feed itself.
//   A publisher holds ONE persistent connection per match; a spectator also keeps its connection
//   open for the match's duration once connected, rather than closing it after the snapshot.
//   Every message on the wire is [4-byte big-endian header length][header JSON][optional raw
//   payload immediately after]:
//     Host, on connect: {"role":"publisher","matchId":"..."} then keeps the connection open,
//       both reading further headers and proactively pushing:
//         {"cmd":"frame","frame":N,"data":"<base64>"}
//       for every confirmed frame once it's old enough to be safe to reveal (see
//       SpectatePublishTick) -- no separate payload, "data" carries the tiny (14-byte)
//       confirmed-input blob inline.
//     Relay pushes, per viewer request: {"cmd":"snapshot-request","requestId":"..."}
//     Host responds: {"cmd":"snapshot-response","requestId":"...","frame":N,"liveFrame":L,
//       "payloadSize":M} immediately followed by M raw (not base64'd) bytes: the state-snapshot
//       blob (tagged frame N) followed by (L-N) backfilled 14-byte confirmed-input entries for
//       frames N+1..L -- see SpectatePublishServiceIncoming for why the snapshot itself can't just
//       be captured fresh at N==L, and ApplySnapshotAndBackfill for how the viewer consumes both.
//     Viewer, on connect: {"role":"spectator","matchId":"..."}, then reads exactly one of:
//       {"cmd":"snapshot-response","frame":N,"liveFrame":L,"payloadSize":M} + M bytes, or
//       {"cmd":"error","reason":"..."}.
//     On success, the connection stays open: the relay streams every subsequent
//     {"cmd":"frame",...} push from the publisher down it, in the exact order the publisher sent
//     them (TCP guarantees this), until either side disconnects. The relay may also send
//     {"cmd":"publisher-disconnected"} if the publisher goes away first. The viewer validates each
//     arriving frame number against what the backfill promised (g_specExpectedNextFrame) and forces
//     a resync (fresh snapshot-request) on any mismatch -- see SpectateServiceIncomingTcp.
//
// Why "safe to publish" needs a delay: at the point QuarkGetInput records a frame, GGPO's
// returned input for it may still be predicted -- a rollback can still resimulate and correct
// it. Publishing immediately risks sending a value a viewer can never un-see (no rollback on
// their end). SpectatePublishRecordFrame just overwrites the ring buffer slot for a frame every
// time it's (re)simulated, so it always holds the latest value; SpectatePublishTick only
// actually transmits a frame once it's comfortably older than MAX_PREDICTION_FRAMES (15, see
// ggpo/src/lib/sync.h) frames behind the current one. Viewers are latency-tolerant, so the
// resulting fixed delay (SPECTATE_PUBLISH_DELAY_FRAMES, ~330ms at 60fps) is a non-issue.
#include "burner.h"
#include <winsock.h>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// For DATAGRAM_CODE_SPECTATOR_COUNT / QuarkSendData's use below (forwarding the spectator count
// to the opponent, who has no connection to the spectate relay of its own -- see SpectatePublishTick).
extern "C" {
#include "ggponet.h"
}

// ---------------------------------------------------------------------------------------------
// Per-frame confirmed input size: Peer2PeerBackend hardcodes exactly 2 players
// (ggpo/src/lib/backends/p2p.h, PLAYER_COUNT) at 7 bytes each (ggpo/src/lib/game_input.h,
// GAMEINPUT_MAX_BYTES) -- that's the whole combined buffer Sync::SynchronizeInputs fills
// (ggpo/src/lib/sync.cpp) and QuarkGetInput hands back. Spectating is scoped to 1v1 matches for
// exactly the same reason Peer2PeerBackend is.
static const int SPECTATE_INPUT_BYTES = 14;
// Must comfortably cover the worst-case gap between a served safe snapshot's tagged frame and the
// live publish cursor at the moment it's served -- see the backfill logic in
// SpectatePublishServiceIncoming's "snapshot-request" handling. That gap is bounded by roughly
// SPECTATE_SNAPSHOT_CAPTURE_INTERVAL_MS (how stale the safe snapshot can be) plus
// SPECTATE_PUBLISH_DELAY_FRAMES, around 80 frames at 60fps -- 256 leaves a wide margin.
static const int SPECTATE_RING_SIZE = 256;
static const int SPECTATE_PUBLISH_DELAY_FRAMES = 20; // must stay > MAX_PREDICTION_FRAMES (15)
static const DWORD SPECTATE_REGISTER_INTERVAL_MS = 5000; // relay drops a registration after 15s
static const DWORD SPECTATE_CONNECT_RETRY_BUDGET_MS = 20000; // total time to retry "host not publishing yet"
static const DWORD SPECTATE_CONNECT_RETRY_INTERVAL_MS = 750;

// ---------------------------------------------------------------------------------------------
// Minimal, purpose-built JSON helpers. NOT a general parser -- this only needs to handle the
// flat, fixed-shape messages in the wire protocol above, which this file and the relay both
// control. Nested objects/arrays and escaped quotes inside values are out of scope on purpose.
static bool JsonExtractString(const std::string& json, const char* key, std::string& out)
{
  std::string pattern = std::string("\"") + key + "\":\"";
  size_t pos = json.find(pattern);
  if (pos == std::string::npos) return false;
  pos += pattern.size();
  size_t end = json.find('"', pos);
  if (end == std::string::npos) return false;
  out = json.substr(pos, end - pos);
  return true;
}

static bool JsonExtractLong(const std::string& json, const char* key, long& out)
{
  std::string pattern = std::string("\"") + key + "\":";
  size_t pos = json.find(pattern);
  if (pos == std::string::npos) return false;
  pos += pattern.size();
  size_t end = pos;
  while (end < json.size() && (isdigit((unsigned char)json[end]) || json[end] == '-')) end++;
  if (end == pos) return false;
  out = strtol(json.substr(pos, end - pos).c_str(), nullptr, 10);
  return true;
}

// matchId/uid are simple identifiers in practice (they come from Firebase uids / server-issued
// match ids); this just guards against an accidental quote or backslash breaking the envelope.
static std::string JsonEscapeSimple(const std::string& s)
{
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Base64, for the "data" field of {"cmd":"frame",...} pushes.
static const char kB64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const unsigned char* data, size_t len)
{
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len) {
    unsigned int n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out.push_back(kB64Chars[(n >> 18) & 0x3F]);
    out.push_back(kB64Chars[(n >> 12) & 0x3F]);
    out.push_back(kB64Chars[(n >> 6) & 0x3F]);
    out.push_back(kB64Chars[n & 0x3F]);
    i += 3;
  }
  size_t rem = len - i;
  if (rem == 1) {
    unsigned int n = data[i] << 16;
    out.push_back(kB64Chars[(n >> 18) & 0x3F]);
    out.push_back(kB64Chars[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  }
  else if (rem == 2) {
    unsigned int n = (data[i] << 16) | (data[i + 1] << 8);
    out.push_back(kB64Chars[(n >> 18) & 0x3F]);
    out.push_back(kB64Chars[(n >> 12) & 0x3F]);
    out.push_back(kB64Chars[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

static int Base64DecodeCharVal(char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static bool Base64Decode(const std::string& in, std::vector<unsigned char>& out)
{
  out.clear();
  if (in.size() % 4 != 0) return false;
  int vals[4];
  size_t i = 0;
  while (i + 4 <= in.size()) {
    int pad = 0;
    for (int k = 0; k < 4; k++) {
      char c = in[i + k];
      if (c == '=') {
        vals[k] = 0;
        pad++;
      }
      else {
        int v = Base64DecodeCharVal(c);
        if (v < 0) return false;
        vals[k] = v;
      }
    }
    unsigned int n = (vals[0] << 18) | (vals[1] << 12) | (vals[2] << 6) | vals[3];
    out.push_back((unsigned char)((n >> 16) & 0xFF));
    if (pad < 2) out.push_back((unsigned char)((n >> 8) & 0xFF));
    if (pad < 1) out.push_back((unsigned char)(n & 0xFF));
    i += 4;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Socket helpers. Deliberately using the classic (non-winsock2) API throughout, matching
// main.cpp's own #include <winsock.h> + WSAStartup -- that's the winsock generation already
// proven to compile alongside burner.h's own windows.h chain in this codebase. The ggpo lib
// uses <winsock2.h> internally, but that's a separate translation unit and never mixed with
// burner.h there, so it's not a precedent for this file.
static bool ResolveIPv4(const std::string& host, unsigned long* outAddr)
{
  unsigned long addr = inet_addr(host.c_str());
  if (addr != INADDR_NONE) {
    *outAddr = addr;
    return true;
  }
  hostent* he = gethostbyname(host.c_str());
  if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) return false;
  *outAddr = *(unsigned long*)he->h_addr_list[0];
  return true;
}

static bool MakeSockAddr(const std::string& host, UINT16 port, sockaddr_in* out)
{
  unsigned long addr;
  if (!ResolveIPv4(host, &addr)) return false;
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_addr.s_addr = addr;
  out->sin_port = htons(port);
  return true;
}

static SOCKET CreateNonBlockingUdpSocket()
{
  SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET) return INVALID_SOCKET;
  u_long nonBlocking = 1;
  ioctlsocket(s, FIONBIO, &nonBlocking);
  return s;
}

static SOCKET TcpConnectWithTimeout(const std::string& host, UINT16 port, int timeoutMs)
{
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return INVALID_SOCKET;

  sockaddr_in addr;
  if (!MakeSockAddr(host, port, &addr)) {
    closesocket(s);
    return INVALID_SOCKET;
  }

  u_long nonBlocking = 1;
  ioctlsocket(s, FIONBIO, &nonBlocking);

  int res = connect(s, (sockaddr*)&addr, sizeof(addr));
  if (res == SOCKET_ERROR) {
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
      closesocket(s);
      return INVALID_SOCKET;
    }
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(s, &writeSet);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int sel = select(0, NULL, &writeSet, NULL, &tv);
    if (sel <= 0) {
      closesocket(s);
      return INVALID_SOCKET;
    }
    int err = 0;
    int errLen = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &errLen);
    if (err != 0) {
      closesocket(s);
      return INVALID_SOCKET;
    }
  }

  // Back to blocking for the synchronous framed read/write helpers below.
  u_long blockingMode = 0;
  ioctlsocket(s, FIONBIO, &blockingMode);
  return s;
}

static bool RecvAll(SOCKET s, char* buf, int len, int timeoutMs)
{
  DWORD tv = (DWORD)timeoutMs;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
  int got = 0;
  while (got < len) {
    int n = recv(s, buf + got, len - got, 0);
    if (n <= 0) return false;
    got += n;
  }
  return true;
}

static bool SendAll(SOCKET s, const char* buf, int len)
{
  int sent = 0;
  while (sent < len) {
    int n = send(s, buf + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += n;
  }
  return true;
}

// [4-byte BE length][header JSON][optional raw payload] -- must match the Go relay's
// writeFramed/readFramedHeader exactly (src/util-servers/spectate-relay/main.go). Only used for
// the short-lived, synchronous handshake connections (the initial snapshot fetch(es)) -- the
// long-lived persistent connections (publisher and spectator live-feed sockets) use their own
// non-blocking incremental framing below instead, since they must never block a game thread.
static bool WriteFramedHeader(SOCKET s, const std::string& headerJson, const unsigned char* payload, int payloadLen)
{
  uint32_t beLen = htonl((uint32_t)headerJson.size());
  if (!SendAll(s, (const char*)&beLen, 4)) return false;
  if (!SendAll(s, headerJson.data(), (int)headerJson.size())) return false;
  if (payloadLen > 0 && payload) {
    if (!SendAll(s, (const char*)payload, payloadLen)) return false;
  }
  return true;
}

static bool ReadFramedHeader(SOCKET s, std::string& outHeaderJson, int timeoutMs)
{
  uint32_t beLen = 0;
  if (!RecvAll(s, (char*)&beLen, 4, timeoutMs)) return false;
  uint32_t len = ntohl(beLen);
  if (len == 0 || len > 65536) return false;
  std::vector<char> buf(len);
  if (!RecvAll(s, buf.data(), (int)len, timeoutMs)) return false;
  outHeaderJson.assign(buf.data(), len);
  return true;
}

// Builds one complete framed message ([4-byte BE length][header JSON][optional payload]) into a
// single buffer, for queuing onto a non-blocking connection's outgoing write queue.
static std::vector<char> BuildFramedMessage(const std::string& headerJson, const unsigned char* payload, int payloadLen)
{
  std::vector<char> msg;
  uint32_t beLen = htonl((uint32_t)headerJson.size());
  msg.reserve(4 + headerJson.size() + (payloadLen > 0 ? (size_t)payloadLen : 0));
  msg.insert(msg.end(), (char*)&beLen, (char*)&beLen + 4);
  msg.insert(msg.end(), headerJson.begin(), headerJson.end());
  if (payload && payloadLen > 0) msg.insert(msg.end(), (char*)payload, (char*)payload + payloadLen);
  return msg;
}

// =================================================================================================
// Publisher (host) side -- only active on the playerSlot-0 client, only when --spectate was
// passed. See InitDirectConnection in fbn_ggpo.cpp for where SpectatePublishInit is called.
// =================================================================================================

namespace {
  struct PublishRingEntry {
    int frame = -1;
    unsigned char bytes[SPECTATE_INPUT_BYTES] = {};
  };

  bool g_publishActive = false;
  std::string g_pubMatchId;
  std::string g_pubUid;

  SOCKET g_pubUdpSocket = INVALID_SOCKET;
  sockaddr_in g_pubRelayUdpAddr;
  SOCKET g_pubTcpSocket = INVALID_SOCKET;

  PublishRingEntry g_pubRing[SPECTATE_RING_SIZE];
  int g_pubHighestRecordedFrame = -1;
  int g_pubNextFrameToPublish = -1;
  DWORD g_pubLastRegisterTick = 0;

  // Incremental, always-non-blocking TCP handling -- this runs every tick of a live match on the
  // publisher's actual game thread, so it must never wait on the network even briefly: a stall
  // here stalls both real players' GGPO session, not just a spectator. Reading (incoming
  // snapshot-requests from the relay) and writing (outgoing snapshot-responses AND the live
  // frame feed, both queued below) are independent directions on the same full-duplex socket.
  enum PubReadState { PUB_READ_IDLE, PUB_READ_LEN, PUB_READ_HEADER };
  PubReadState g_pubReadState = PUB_READ_IDLE;
  unsigned char g_pubReadLenBuf[4];
  int g_pubReadLenGot = 0;
  uint32_t g_pubReadHeaderLen = 0;
  std::vector<char> g_pubReadHeaderBuf;
  int g_pubReadHeaderGot = 0;

  // Outgoing queue: both snapshot-responses and (much more frequently) live frame pushes land
  // here; a single drain step sends whatever fits without blocking, resuming next tick otherwise.
  std::deque<std::vector<char>> g_pubTcpOutQueue;
  size_t g_pubTcpOutSent = 0;

  // Snapshot safety. ggpo_save_game_state_callback's BurnAreaScan captures whatever the
  // publisher's live emulator memory holds AT THE INSTANT it's called -- which, exactly like any
  // single frame's confirmed input (see this file's top comment), may still be a GGPO prediction
  // that a later rollback silently corrects. Capturing "right now" the moment a snapshot-request
  // arrives risks handing a spectator a snapshot that gets invalidated by a rollback on the
  // PUBLISHER's own emulator moments later, with no way to tell the spectator -- its baseline is
  // just permanently wrong from that point on (observed as spectators showing the wrong
  // characters entirely, if the divergence happens to land on character-select). Fix: capture a
  // CANDIDATE periodically, and only ever serve one to spectators once its own tagged frame has
  // aged past MAX_PREDICTION_FRAMES, the same way a live frame has to before SpectatePublishTick
  // publishes it -- see PromoteSafeSnapshotIfReady.
  std::vector<unsigned char> g_pubSafeSnapshot;
  int g_pubSafeSnapshotFrame = -1;
  std::vector<unsigned char> g_pubPendingSnapshot;
  int g_pubPendingSnapshotFrame = -1;
  DWORD g_pubLastSnapshotCaptureTick = 0;
}

static const DWORD SPECTATE_SNAPSHOT_CAPTURE_INTERVAL_MS = 1000;

void SpectatePublishInit(const DirectConnectionOptions& ops)
{
  if (g_publishActive) return;
  if (!ops.spectate) return;

  g_pubMatchId = ops.spectateMatchId;
  g_pubUid = ops.spectateUid;

  for (int i = 0; i < SPECTATE_RING_SIZE; i++) g_pubRing[i].frame = -1;
  g_pubHighestRecordedFrame = -1;
  g_pubNextFrameToPublish = -1;
  g_pubReadState = PUB_READ_IDLE;
  g_pubTcpOutQueue.clear();
  g_pubTcpOutSent = 0;
  g_pubSafeSnapshot.clear();
  g_pubSafeSnapshotFrame = -1;
  g_pubPendingSnapshot.clear();
  g_pubPendingSnapshotFrame = -1;
  g_pubLastSnapshotCaptureTick = 0;

  g_pubUdpSocket = CreateNonBlockingUdpSocket();
  if (g_pubUdpSocket == INVALID_SOCKET) {
    bprintf(0, _T("*** spectate-relay: failed to create publisher UDP socket.\n"));
    return;
  }
  if (!MakeSockAddr(ops.spectateRelayHost, ops.spectateRelayUdpPort, &g_pubRelayUdpAddr)) {
    bprintf(0, _T("*** spectate-relay: failed to resolve relay host.\n"));
    closesocket(g_pubUdpSocket);
    g_pubUdpSocket = INVALID_SOCKET;
    return;
  }

  g_pubTcpSocket = TcpConnectWithTimeout(ops.spectateRelayHost, ops.spectateRelayTcpPort, 3000);
  if (g_pubTcpSocket != INVALID_SOCKET) {
    std::string hello = "{\"role\":\"publisher\",\"matchId\":\"" + JsonEscapeSimple(g_pubMatchId) + "\"}";
    if (!WriteFramedHeader(g_pubTcpSocket, hello, nullptr, 0)) {
      closesocket(g_pubTcpSocket);
      g_pubTcpSocket = INVALID_SOCKET;
    }
    else {
      u_long nb = 1;
      ioctlsocket(g_pubTcpSocket, FIONBIO, &nb);
    }
  }
  // A failed TCP connection means mid-match join AND the live feed both won't be servable from
  // this host; the match itself still plays fine, so this isn't treated as a hard failure.

  g_pubLastRegisterTick = 0; // force an immediate publish registration on the first tick
  g_publishActive = true;
}

void SpectatePublishRecordFrame(int frame, const void* values)
{
  if (!g_publishActive || frame < 0 || !values) return;
  PublishRingEntry& entry = g_pubRing[frame % SPECTATE_RING_SIZE];
  entry.frame = frame;
  memcpy(entry.bytes, values, SPECTATE_INPUT_BYTES);
  if (frame > g_pubHighestRecordedFrame) g_pubHighestRecordedFrame = frame;
  if (g_pubNextFrameToPublish < 0) g_pubNextFrameToPublish = frame;
}

// Captures a new candidate snapshot roughly once a second (tagged with the current highest
// recorded frame) and promotes the previous candidate to "the" safe snapshot once its own tagged
// frame has aged past SPECTATE_PUBLISH_DELAY_FRAMES -- the same aging rule the live feed itself
// uses before actually publishing a frame (see SpectatePublishTick). See this file's namespace
// block above for why capturing fresh at snapshot-request time isn't safe.
static void SpectatePublishCaptureSnapshotIfDue(DWORD now)
{
  if (g_pubHighestRecordedFrame < 0) return; // nothing confirmed yet to capture

  if (g_pubPendingSnapshotFrame >= 0 &&
      g_pubHighestRecordedFrame - g_pubPendingSnapshotFrame >= SPECTATE_PUBLISH_DELAY_FRAMES) {
    g_pubSafeSnapshot = std::move(g_pubPendingSnapshot);
    g_pubSafeSnapshotFrame = g_pubPendingSnapshotFrame;
    g_pubPendingSnapshot.clear();
    g_pubPendingSnapshotFrame = -1;
  }

  if (now - g_pubLastSnapshotCaptureTick < SPECTATE_SNAPSHOT_CAPTURE_INTERVAL_MS) return;
  g_pubLastSnapshotCaptureTick = now;

  unsigned char* buffer = nullptr;
  int len = 0;
  int checksum = 0;
  ggpo_save_game_state_callback(&buffer, &len, &checksum, g_pubHighestRecordedFrame);
  if (buffer && len > 0) {
    g_pubPendingSnapshot.assign(buffer, buffer + len);
    g_pubPendingSnapshotFrame = g_pubHighestRecordedFrame;
  }
  if (buffer) ggpo_free_buffer_callback(buffer);
}

// Fully non-blocking and incremental across ticks -- reads at most one complete incoming header
// per call (looping internally to drain a backlog within the tick if one's already buffered), and
// only ever reacts to "snapshot-request" (the only thing the relay ever proactively sends here).
static void SpectatePublishServiceIncoming()
{
  if (g_pubTcpSocket == INVALID_SOCKET) return;

  for (;;) {
    if (g_pubReadState == PUB_READ_IDLE) {
      g_pubReadLenGot = 0;
      g_pubReadState = PUB_READ_LEN;
    }

    if (g_pubReadState == PUB_READ_LEN) {
      while (g_pubReadLenGot < 4) {
        int n = recv(g_pubTcpSocket, (char*)g_pubReadLenBuf + g_pubReadLenGot, 4 - g_pubReadLenGot, 0);
        if (n > 0) { g_pubReadLenGot += n; continue; }
        if (n == 0) {
          // Relay closed the connection. Drop it -- mid-match join and the live feed just won't
          // be servable from this host until it reconnects (no reconnect logic exists here).
          closesocket(g_pubTcpSocket);
          g_pubTcpSocket = INVALID_SOCKET;
          g_pubReadState = PUB_READ_IDLE;
          return;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
          closesocket(g_pubTcpSocket);
          g_pubTcpSocket = INVALID_SOCKET;
          g_pubReadState = PUB_READ_IDLE;
        }
        return; // nothing more available this tick -- resume next tick, or already dropped above
      }
      uint32_t len = ntohl(*(uint32_t*)g_pubReadLenBuf);
      if (len == 0 || len > 4096) {
        // Malformed -- the relay only ever sends tiny fixed-shape headers on this connection.
        closesocket(g_pubTcpSocket);
        g_pubTcpSocket = INVALID_SOCKET;
        g_pubReadState = PUB_READ_IDLE;
        return;
      }
      g_pubReadHeaderLen = len;
      g_pubReadHeaderBuf.assign(len, 0);
      g_pubReadHeaderGot = 0;
      g_pubReadState = PUB_READ_HEADER;
    }

    if (g_pubReadState == PUB_READ_HEADER) {
      while ((uint32_t)g_pubReadHeaderGot < g_pubReadHeaderLen) {
        int n = recv(g_pubTcpSocket, g_pubReadHeaderBuf.data() + g_pubReadHeaderGot, (int)(g_pubReadHeaderLen - g_pubReadHeaderGot), 0);
        if (n > 0) { g_pubReadHeaderGot += n; continue; }
        if (n == 0) {
          closesocket(g_pubTcpSocket);
          g_pubTcpSocket = INVALID_SOCKET;
          g_pubReadState = PUB_READ_IDLE;
          return;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
          closesocket(g_pubTcpSocket);
          g_pubTcpSocket = INVALID_SOCKET;
          g_pubReadState = PUB_READ_IDLE;
        }
        return;
      }

      std::string headerJson(g_pubReadHeaderBuf.data(), g_pubReadHeaderBuf.size());
      g_pubReadState = PUB_READ_IDLE; // ready for the next message on the next loop iteration

      std::string cmd, requestId;
      JsonExtractString(headerJson, "cmd", cmd);
      JsonExtractString(headerJson, "requestId", requestId);

      if (cmd == "snapshot-request" && !requestId.empty()) {
        // Serve the pre-verified safe snapshot (see SpectatePublishCaptureSnapshotIfDue) instead
        // of capturing fresh here -- a capture taken at the exact instant this request happens to
        // arrive could still be a GGPO prediction that a later rollback silently corrects, which
        // would hand the spectator a permanently-wrong baseline with no way to tell it happened.
        // If none has aged into "safe" yet (e.g. the match only just started, under ~1-2s in),
        // don't respond at all -- the relay's own snapshotRequestTimeout, and the spectator's
        // existing connect-retry loop, already handle that as a routine retry-shortly case.
        if (g_pubSafeSnapshotFrame >= 0) {
          // The safe snapshot is necessarily stale relative to "now" -- it can only be promoted
          // once its own tagged frame has aged past SPECTATE_PUBLISH_DELAY_FRAMES, and it's only
          // recaptured roughly once a second (SPECTATE_SNAPSHOT_CAPTURE_INTERVAL_MS), so by the
          // time a request actually arrives it can be up to ~1-2s behind g_pubNextFrameToPublish.
          // The old spectator design relied on "whatever live frame arrives first on this fresh
          // connection is exactly the one right after the snapshot" -- true when the snapshot was
          // captured fresh at request time (a small, fixed ~20-frame gap), but false now: the live
          // feed keeps publishing the whole time the snapshot sat waiting to be promoted, so the
          // first live frame the spectator would actually receive could be dozens of frames past
          // where the snapshot left off, applied to state that never simulated the frames between
          // -- an immediate, undetectable desync (an emulator can't skip frames, only re-snapshot).
          // Fix: backfill every frame between the snapshot and the current publish cursor from the
          // ring buffer (which already holds them regardless of publish/safety status) and append
          // it to this same response, so the spectator arrives already caught up to the live edge.
          std::vector<unsigned char> backfill;
          int liveFrame = g_pubSafeSnapshotFrame;
          for (int f = g_pubSafeSnapshotFrame + 1; f < g_pubNextFrameToPublish; f++) {
            PublishRingEntry& entry = g_pubRing[f % SPECTATE_RING_SIZE];
            if (entry.frame != f) break; // shouldn't happen at this ring size -- stop rather than send wrong data
            backfill.insert(backfill.end(), entry.bytes, entry.bytes + SPECTATE_INPUT_BYTES);
            liveFrame = f;
          }

          std::vector<unsigned char> combined;
          combined.reserve(g_pubSafeSnapshot.size() + backfill.size());
          combined.insert(combined.end(), g_pubSafeSnapshot.begin(), g_pubSafeSnapshot.end());
          combined.insert(combined.end(), backfill.begin(), backfill.end());

          std::string resp = "{\"cmd\":\"snapshot-response\",\"requestId\":\"" + JsonEscapeSimple(requestId) +
            "\",\"frame\":" + std::to_string(g_pubSafeSnapshotFrame) +
            ",\"liveFrame\":" + std::to_string(liveFrame) +
            ",\"payloadSize\":" + std::to_string(combined.size()) + "}";
          g_pubTcpOutQueue.push_back(BuildFramedMessage(resp, combined.data(), (int)combined.size()));
        }
      }
      // Other cmds ignored for forward-compatibility -- loop back to try the next message.
    }
  }
}

// Fully non-blocking: sends as much of the queued outgoing messages as the socket will currently
// accept, resuming a partially-sent message (or the next queued one) on a later tick otherwise.
static void SpectatePublishDrainWriteQueue()
{
  if (g_pubTcpSocket == INVALID_SOCKET) return;

  while (!g_pubTcpOutQueue.empty()) {
    std::vector<char>& front = g_pubTcpOutQueue.front();
    while (g_pubTcpOutSent < front.size()) {
      int n = send(g_pubTcpSocket, front.data() + g_pubTcpOutSent, (int)(front.size() - g_pubTcpOutSent), 0);
      if (n > 0) { g_pubTcpOutSent += n; continue; }
      if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(g_pubTcpSocket);
        g_pubTcpSocket = INVALID_SOCKET;
      }
      return; // socket buffer full for now, or a real error was just handled above
    }
    g_pubTcpOutQueue.pop_front();
    g_pubTcpOutSent = 0;
  }
}

void SpectatePublishTick()
{
  if (!g_publishActive) return;

  DWORD now = GetTickCount();
  if (now - g_pubLastRegisterTick > SPECTATE_REGISTER_INTERVAL_MS) {
    if (g_pubUdpSocket != INVALID_SOCKET) {
      std::string msg = "{\"type\":\"publish\",\"matchId\":\"" + JsonEscapeSimple(g_pubMatchId) +
        "\",\"uid\":\"" + JsonEscapeSimple(g_pubUid) + "\"}";
      sendto(g_pubUdpSocket, msg.data(), (int)msg.size(), 0, (sockaddr*)&g_pubRelayUdpAddr, sizeof(g_pubRelayUdpAddr));
    }
    g_pubLastRegisterTick = now;
  }

  if (g_pubTcpSocket != INVALID_SOCKET && g_pubNextFrameToPublish >= 0) {
    while (g_pubHighestRecordedFrame - g_pubNextFrameToPublish >= SPECTATE_PUBLISH_DELAY_FRAMES) {
      int frame = g_pubNextFrameToPublish;
      PublishRingEntry& entry = g_pubRing[frame % SPECTATE_RING_SIZE];
      if (entry.frame == frame) {
        std::string b64 = Base64Encode(entry.bytes, SPECTATE_INPUT_BYTES);
        std::string msg = "{\"cmd\":\"frame\",\"frame\":" + std::to_string(frame) + ",\"data\":\"" + b64 + "\"}";
        g_pubTcpOutQueue.push_back(BuildFramedMessage(msg, nullptr, 0));
      }
      // A missing ring entry shouldn't normally happen (the ring is far larger than the publish
      // delay) -- if it does, skipping it beats stalling the publish cursor forever.
      g_pubNextFrameToPublish++;
    }
  }

  // The publisher UDP socket is otherwise send-only; this is the one thing the relay ever
  // pushes back to it unsolicited (see broadcastSpectatorCount in main.go).
  if (g_pubUdpSocket != INVALID_SOCKET) {
    char recvBuf[512];
    for (;;) {
      int n = recv(g_pubUdpSocket, recvBuf, sizeof(recvBuf) - 1, 0);
      if (n <= 0) break;
      recvBuf[n] = 0;
      std::string msg(recvBuf, n);

      std::string type;
      if (!JsonExtractString(msg, "type", type) || type != "spectator-count") continue;
      long count = -1;
      if (!JsonExtractLong(msg, "count", count) || count < 0) continue;
      // Two parallel overlay implementations exist in this codebase (VidOverlay* in
      // vid_overlay.cpp, VidS* in vid_directx_support.cpp) and a host/opponent's own window
      // renders through VidS*, not VidOverlay* -- both need updating or the host itself won't
      // show the count despite reading it directly from the relay right here.
      VidOverlaySetGameSpectators((int)count);
      VidSSetGameSpectators((int)count);

      // The opponent has no connection to the spectate relay at all (only playerSlot 0 ever
      // talks to it) -- forward the count over the existing GGPO P2P link so both players see
      // it, not just the one hosting. QuarkSendData/ggpo_send_data are null-safe no-ops if ggpo
      // isn't up yet, so no extra guard is needed here.
      uint8_t countByte = (uint8_t)(std::min)(count, 255L);
      QuarkSendData(DATAGRAM_CODE_SPECTATOR_COUNT, &countByte, sizeof(countByte));
    }
  }

  SpectatePublishCaptureSnapshotIfDue(now);
  SpectatePublishServiceIncoming();
  SpectatePublishDrainWriteQueue();
}

void SpectatePublishShutdown()
{
  if (!g_publishActive) return;

  if (g_pubUdpSocket != INVALID_SOCKET) {
    std::string msg = "{\"type\":\"unpublish\",\"matchId\":\"" + JsonEscapeSimple(g_pubMatchId) + "\"}";
    sendto(g_pubUdpSocket, msg.data(), (int)msg.size(), 0, (sockaddr*)&g_pubRelayUdpAddr, sizeof(g_pubRelayUdpAddr));
    closesocket(g_pubUdpSocket);
    g_pubUdpSocket = INVALID_SOCKET;
  }
  if (g_pubTcpSocket != INVALID_SOCKET) {
    closesocket(g_pubTcpSocket);
    g_pubTcpSocket = INVALID_SOCKET;
  }
  g_publishActive = false;
}

bool SpectatePublishActive()
{
  return g_publishActive;
}

// =================================================================================================
// Spectator (viewer) side.
// =================================================================================================

namespace {
  bool g_spectateActive = false;
  std::string g_specMatchId;
  std::string g_specUid;
  std::string g_specUserName;
  std::string g_specRelayHost;
  UINT16 g_specRelayTcpPort = 0;

  SOCKET g_specUdpSocket = INVALID_SOCKET; // presence/chat only now
  sockaddr_in g_specRelayUdpAddr;
  DWORD g_specLastWatchRegisterTick = 0;

  SOCKET g_specTcpSocket = INVALID_SOCKET; // the persistent live-feed connection
  std::deque<std::vector<unsigned char>> g_specFrameQueue; // decoded, not-yet-consumed frames, FIFO
  bool g_specFirstFrameApplied = false;

  // Incremental, always-non-blocking TCP read state for the live feed -- mirrors the publisher's
  // own incoming-header reader (SpectatePublishServiceIncoming).
  enum SpecReadState { SPEC_READ_IDLE, SPEC_READ_LEN, SPEC_READ_HEADER };
  SpecReadState g_specReadState = SPEC_READ_IDLE;
  unsigned char g_specReadLenBuf[4];
  int g_specReadLenGot = 0;
  uint32_t g_specReadHeaderLen = 0;
  std::vector<char> g_specReadHeaderBuf;
  int g_specReadHeaderGot = 0;

  // Frame this connection's next incoming "frame" push must carry, or -1 if not yet known (still
  // waiting on the first snapshot response). Set from the snapshot-response's backfilled liveFrame
  // (see ApplySnapshotAndBackfill) and validated in SpectateServiceIncomingTcp -- a mismatch means
  // frames slipped through unbackfilled in the narrow window between the publisher computing that
  // backfill and the relay actually registering this connection as a live watcher (see
  // SpectatePublishServiceIncoming's snapshot-request handling and main.go's handleSpectatorConn),
  // and applying it anyway would silently misalign input against state with no way to detect it
  // later. Forcing a resync (see SpectateServiceIncomingTcp) beats that.
  int g_specExpectedNextFrame = -1;

  // Diagnostics only, for the "still waiting" warning in SpectateGetInput below.
  DWORD g_specConnectTick = 0;
  DWORD g_specLastStallWarnTick = 0;
  long g_specMessagesReceived = 0;
  long g_specFramesReceived = 0;
  int g_specLastFrameNumberSeen = -1;

  // Reconnect-on-death tracking. TCP guarantees no data loss/corruption while a connection is
  // alive, but nothing guarantees the connection itself stays alive -- a transient NAT/firewall
  // hiccup or dropped route can kill it for reasons entirely outside this process (and possibly
  // outside the relay's awareness too, if the relay hasn't tried writing to it since). Without a
  // reconnect path, a spectator whose live-feed connection dies this way is stuck permanently,
  // frozen on its last frame, even though the match and the relay are both still fine.
  DWORD g_specLastReconnectAttemptTick = 0;
}

static const DWORD SPECTATE_RECONNECT_COOLDOWN_MS = 3000;

// One attempt at fetching a fresh state snapshot for matchId from the relay's TCP port. Doesn't
// touch any g_spec* state itself. On success, the connection is closed unless outSocket is
// non-null, in which case ownership of the still-open, non-blocking-ready socket transfers to the
// caller (used to keep the live-feed connection open once a snapshot fetch also doubles as the
// spectator's initial live-feed registration -- see InitSpectatorConnection).
static bool RequestSnapshotOnce(const std::string& host, UINT16 tcpPort, const std::string& matchId,
  int timeoutMs, long& outFrame, long& outLiveFrame, std::vector<unsigned char>& outPayload, std::string& outFailReason,
  SOCKET* outSocket = nullptr)
{
  SOCKET tcp = TcpConnectWithTimeout(host, tcpPort, timeoutMs);
  if (tcp == INVALID_SOCKET) {
    outFailReason = "could not connect to relay";
    return false;
  }

  std::string hello = "{\"role\":\"spectator\",\"matchId\":\"" + JsonEscapeSimple(matchId) + "\"}";
  std::string respHeaderJson;
  if (!WriteFramedHeader(tcp, hello, nullptr, 0) || !ReadFramedHeader(tcp, respHeaderJson, timeoutMs)) {
    outFailReason = "relay did not respond";
    closesocket(tcp);
    return false;
  }

  std::string cmd;
  JsonExtractString(respHeaderJson, "cmd", cmd);
  if (cmd != "snapshot-response") {
    JsonExtractString(respHeaderJson, "reason", outFailReason);
    closesocket(tcp);
    return false;
  }

  long payloadSize = -1;
  JsonExtractLong(respHeaderJson, "frame", outFrame);
  outLiveFrame = outFrame; // default if an older/mismatched relay ever omits liveFrame
  JsonExtractLong(respHeaderJson, "liveFrame", outLiveFrame);
  JsonExtractLong(respHeaderJson, "payloadSize", payloadSize);
  if (payloadSize < 0 || payloadSize > 64 * 1024 * 1024) {
    outFailReason = "bad payloadSize";
    closesocket(tcp);
    return false;
  }

  outPayload.assign((size_t)payloadSize, 0);
  if (payloadSize > 0 && !RecvAll(tcp, (char*)outPayload.data(), (int)payloadSize, timeoutMs)) {
    outFailReason = "snapshot payload read failed";
    closesocket(tcp);
    return false;
  }

  if (outSocket) {
    *outSocket = tcp;
  }
  else {
    closesocket(tcp);
  }
  return true;
}

// Splits a snapshot-response payload into its state-snapshot prefix and trailing per-frame
// backfill (see SpectatePublishServiceIncoming's snapshot-request handling for why the backfill
// exists), applies the state via ggpo_load_game_state_callback, queues the backfilled frames so
// SpectateGetInput consumes them first, and sets g_specExpectedNextFrame so the very next "frame"
// push actually received over the wire can be validated against what this connection was promised.
static void ApplySnapshotAndBackfill(std::vector<unsigned char>& payload, long frame, long liveFrame)
{
  size_t backfillCount = (liveFrame > frame) ? (size_t)(liveFrame - frame) : 0;
  size_t backfillBytes = backfillCount * SPECTATE_INPUT_BYTES;
  if (backfillBytes > payload.size()) {
    // Malformed/mismatched -- fall back to "no backfill" rather than misreading the snapshot.
    backfillCount = 0;
    backfillBytes = 0;
    liveFrame = frame;
  }
  size_t snapshotBytes = payload.size() - backfillBytes;

  ggpo_load_game_state_callback(payload.data(), (int)snapshotBytes);

  g_specFrameQueue.clear();
  for (size_t i = 0; i < backfillCount; i++) {
    const unsigned char* p = payload.data() + snapshotBytes + i * SPECTATE_INPUT_BYTES;
    g_specFrameQueue.emplace_back(p, p + SPECTATE_INPUT_BYTES);
  }
  g_specExpectedNextFrame = (int)liveFrame + 1;
}

bool InitSpectatorConnection(const SpectateConnectionOptions& ops)
{
  if (g_spectateActive) SpectateDisconnect();

  g_specMatchId = ops.matchId;
  g_specUid = ops.uid;
  g_specUserName = ops.userName.empty() ? ops.uid : ops.userName;
  g_specFrameQueue.clear();
  g_specFirstFrameApplied = false;
  g_specConnectTick = GetTickCount();
  g_specLastStallWarnTick = 0;
  g_specMessagesReceived = 0;
  g_specFramesReceived = 0;
  g_specLastFrameNumberSeen = -1;
  g_specExpectedNextFrame = -1;
  g_specReadState = SPEC_READ_IDLE;

  // Same globals InitDirectConnection sets before a match begins. ggpo_begin_game_callback,
  // EnsureSpectatorDriverLoaded, and ggpo_load_game_state_callback (all reused below, see
  // burner_win32.h) are already kNetSpectator-aware and GGPO-session-agnostic -- they just
  // serialize/deserialize driver state via BurnAreaScan, never touching GGPOSession at all.
  kNetVersion = NET_VERSION;
  kNetGame = 1;
  kNetSpectator = 1;
  kNetLua = 0;

  if (!ggpo_begin_game_callback(ops.romName.c_str())) {
    bprintf(0, _T("*** spectate: ROM \"%hs\" not found.\n"), ops.romName.c_str());
    kNetGame = 0;
    kNetSpectator = 0;
    return false;
  }

  // InitDirectConnection forces this for the host/opponent (see misc_win32.cpp) so coarse
  // default Windows timer granularity can't bias their frame pacing against each other; a
  // spectator never called it, an asymmetry worth closing even though it's normally moot in
  // practice (timeBeginPeriod affects the whole system while any process holds it, and the
  // host/opponent already do for the match's duration).
  ForceHighResolutionTimingForNetplay();

  // Deliberately NOT calling EnsureSpectatorDriverLoaded() here. An earlier version of this
  // function did, to start the ROM/driver load before the network round trip below -- but that
  // meant several spectator processes launched close together all hit the same heavy CPU-bound
  // ROM decompression/decryption work in near lock-step, instead of it being naturally spread
  // out in time the way it always was before: each one's jittery, variable-length network
  // connection attempt used to desynchronize when that work happened, purely as a side effect of
  // ordering. Leaving the load to fire from inside ggpo_load_game_state_callback below (its own
  // bDelayLoad check, unchanged) restores that natural staggering. This does still only cost ONE
  // relay round trip overall, same as the early-load version: the single fetch below both
  // supplies the snapshot ggpo_load_game_state_callback restores AND becomes the live feed
  // connection, so nothing is lost by leaving the load where it was.
  g_specUdpSocket = CreateNonBlockingUdpSocket();
  if (g_specUdpSocket == INVALID_SOCKET || !MakeSockAddr(ops.relayHost, ops.relayUdpPort, &g_specRelayUdpAddr)) {
    bprintf(0, _T("*** spectate: failed to set up relay UDP connection.\n"));
    if (g_specUdpSocket != INVALID_SOCKET) closesocket(g_specUdpSocket);
    g_specUdpSocket = INVALID_SOCKET;
    kNetGame = 0;
    kNetSpectator = 0;
    return false;
  }
  g_specRelayHost = ops.relayHost;
  g_specRelayTcpPort = ops.relayTcpPort;
  std::string watchMsg = "{\"type\":\"watch\",\"matchId\":\"" + JsonEscapeSimple(g_specMatchId) +
    "\",\"uid\":\"" + JsonEscapeSimple(g_specUid) + "\"}";

  // The host is very often still mid-launch (ROM load, GGPO handshake/sync, auto-delay
  // negotiation) when a viewer connects right after it -- the relay legitimately has no
  // publisher registered for this matchId yet. Retry the whole handshake for a while instead of
  // failing on the first attempt, so the caller doesn't have to guess a "host is ready" delay.
  VidOverlaySetSystemMessage(_T("Connecting to host..."));

  bool ok = false;
  long frame = -1;
  long liveFrame = -1;
  std::vector<unsigned char> payload;
  std::string lastFailReason;
  SOCKET liveSocket = INVALID_SOCKET;
  DWORD connectStartTick = GetTickCount();

  // One fetch does double duty: its snapshot+backfill data is what ApplySnapshotAndBackfill below
  // restores and catches up on (also triggering the deferred ROM/driver load, via
  // ggpo_load_game_state_callback's own bDelayLoad check), and (via outSocket) its connection is
  // kept open afterward to become the live feed itself. There's no backlog to worry about even
  // though the driver load happens after this connection is opened: the relay only starts fanning
  // frame pushes to a spectator connection from the moment its registration completes on this SAME
  // connection (see handleSpectatorConn in main.go), and by then this fetch's backfill has already
  // caught the snapshot up to the publish cursor as it stood at request time -- SpectateGetInput's
  // gap check (via g_specExpectedNextFrame) handles the narrow remaining window between that point
  // and the relay's registration actually completing.
  for (;;) {
    sendto(g_specUdpSocket, watchMsg.data(), (int)watchMsg.size(), 0, (sockaddr*)&g_specRelayUdpAddr, sizeof(g_specRelayUdpAddr));
    g_specLastWatchRegisterTick = GetTickCount();

    if (RequestSnapshotOnce(ops.relayHost, ops.relayTcpPort, g_specMatchId, 5000, frame, liveFrame, payload, lastFailReason, &liveSocket)) {
      ok = true;
      break;
    }

    if (GetTickCount() - connectStartTick >= SPECTATE_CONNECT_RETRY_BUDGET_MS) break;
    Sleep(SPECTATE_CONNECT_RETRY_INTERVAL_MS);
  }

  if (!ok) {
    bprintf(0, _T("*** spectate: giving up after %lums (%hs).\n"), GetTickCount() - connectStartTick, lastFailReason.c_str());
    closesocket(g_specUdpSocket);
    g_specUdpSocket = INVALID_SOCKET;
    kNetGame = 0;
    kNetSpectator = 0;
    return false;
  }

  // One more "watch" keepalive right before the potentially long, fully synchronous, network-
  // silent call just below (ggpo_load_game_state_callback triggers EnsureSpectatorDriverLoaded's
  // ROM decompression/decryption, which can run well past a normal keepalive interval on a slow
  // system like CPS3) -- maximizes how much of the relay's watcherTimeout budget is left when
  // that call finally returns and SpectateGetInput resumes sending real keepalives on its own.
  sendto(g_specUdpSocket, watchMsg.data(), (int)watchMsg.size(), 0, (sockaddr*)&g_specRelayUdpAddr, sizeof(g_specRelayUdpAddr));
  g_specLastWatchRegisterTick = GetTickCount();

  // Read by ggpo_load_game_state_callback's kNetSpectator branch (fbn_ggpo.cpp), called just
  // below -- must be set before that call, not after.
  ANSIToTCHAR(ops.player1Name.c_str(), g_spectateP1Name, 128);
  ANSIToTCHAR(ops.player2Name.c_str(), g_spectateP2Name, 128);

  ApplySnapshotAndBackfill(payload, frame, liveFrame);
  u_long nb = 1;
  ioctlsocket(liveSocket, FIONBIO, &nb);
  g_specTcpSocket = liveSocket;
  bprintf(0, _T("*** spectate: connected, snapshot frame %ld backfilled to %ld, streaming live from frame %d.\n"), frame, liveFrame, g_specExpectedNextFrame);

  VidOverlaySetSystemMessage(_T("Waiting for stream..."));
  g_spectateActive = true;
  return true;
}

// Fully non-blocking and incremental across ticks -- drains as many complete {"cmd":"frame",...}
// (or other) messages as are already buffered this tick, resuming a partial read next tick
// otherwise. Since this rides TCP, every message that was ever sent WILL eventually arrive, in
// order -- there is no gap/loss case to handle here, unlike the old UDP design.
static void SpectateServiceIncomingTcp()
{
  if (g_specTcpSocket == INVALID_SOCKET) return;

  for (;;) {
    if (g_specReadState == SPEC_READ_IDLE) {
      g_specReadLenGot = 0;
      g_specReadState = SPEC_READ_LEN;
    }

    if (g_specReadState == SPEC_READ_LEN) {
      while (g_specReadLenGot < 4) {
        int n = recv(g_specTcpSocket, (char*)g_specReadLenBuf + g_specReadLenGot, 4 - g_specReadLenGot, 0);
        if (n > 0) { g_specReadLenGot += n; continue; }
        if (n == 0) {
          bprintf(0, _T("*** spectate: live feed connection closed by relay.\n"));
          closesocket(g_specTcpSocket);
          g_specTcpSocket = INVALID_SOCKET;
          g_specReadState = SPEC_READ_IDLE;
          return;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
          closesocket(g_specTcpSocket);
          g_specTcpSocket = INVALID_SOCKET;
          g_specReadState = SPEC_READ_IDLE;
        }
        return; // nothing more available this tick -- resume next tick, or already dropped above
      }
      uint32_t len = ntohl(*(uint32_t*)g_specReadLenBuf);
      if (len == 0 || len > 4096) {
        closesocket(g_specTcpSocket);
        g_specTcpSocket = INVALID_SOCKET;
        g_specReadState = SPEC_READ_IDLE;
        return;
      }
      g_specReadHeaderLen = len;
      g_specReadHeaderBuf.assign(len, 0);
      g_specReadHeaderGot = 0;
      g_specReadState = SPEC_READ_HEADER;
    }

    if (g_specReadState == SPEC_READ_HEADER) {
      while ((uint32_t)g_specReadHeaderGot < g_specReadHeaderLen) {
        int n = recv(g_specTcpSocket, g_specReadHeaderBuf.data() + g_specReadHeaderGot, (int)(g_specReadHeaderLen - g_specReadHeaderGot), 0);
        if (n > 0) { g_specReadHeaderGot += n; continue; }
        if (n == 0) {
          closesocket(g_specTcpSocket);
          g_specTcpSocket = INVALID_SOCKET;
          g_specReadState = SPEC_READ_IDLE;
          return;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
          closesocket(g_specTcpSocket);
          g_specTcpSocket = INVALID_SOCKET;
          g_specReadState = SPEC_READ_IDLE;
        }
        return;
      }

      std::string headerJson(g_specReadHeaderBuf.data(), g_specReadHeaderBuf.size());
      g_specReadState = SPEC_READ_IDLE; // ready for the next message on the next loop iteration

      std::string cmd;
      JsonExtractString(headerJson, "cmd", cmd);
      g_specMessagesReceived++;

      if (cmd == "frame") {
        long frame = -1;
        std::string dataB64;
        JsonExtractLong(headerJson, "frame", frame);
        std::vector<unsigned char> decoded;
        if (JsonExtractString(headerJson, "data", dataB64) && Base64Decode(dataB64, decoded) && decoded.size() == SPECTATE_INPUT_BYTES) {
          // A mismatch means this connection's snapshot-response backfill (see
          // ApplySnapshotAndBackfill) didn't actually reach the live publish cursor -- the narrow
          // window between the publisher computing that backfill and the relay finishing this
          // connection's watcher registration (see handleSpectatorConn in main.go) let one or more
          // frames through unaccounted for. Applying this frame anyway would silently misalign
          // input against state with no way to detect it later (the exact bug this whole backfill
          // mechanism exists to prevent) -- force a resync instead, reusing the existing
          // reconnect-on-death path in SpectateGetInput, which re-fetches a fresh, fully-caught-up
          // snapshot rather than trying to patch just the missing frames in place.
          if (g_specExpectedNextFrame >= 0 && (int)frame != g_specExpectedNextFrame) {
            bprintf(0, _T("*** spectate: live feed gap detected (expected frame %d, got %ld) -- forcing resync.\n"),
              g_specExpectedNextFrame, frame);
            closesocket(g_specTcpSocket);
            g_specTcpSocket = INVALID_SOCKET;
            g_specReadState = SPEC_READ_IDLE;
            return;
          }
          g_specFramesReceived++;
          g_specLastFrameNumberSeen = (int)frame;
          g_specExpectedNextFrame = (int)frame + 1;
          g_specFrameQueue.push_back(std::move(decoded));
        }
      }
      else if (cmd == "publisher-disconnected") {
        bprintf(0, _T("*** spectate: publisher disconnected, ending stream.\n"));
        closesocket(g_specTcpSocket);
        g_specTcpSocket = INVALID_SOCKET;
        return;
      }
      // Other cmds ignored for forward-compatibility -- loop back to try the next message.
    }
  }
}

bool SpectateGetInput(void* values)
{
  if (!g_spectateActive) return false;

  DWORD now = GetTickCount();
  if (now - g_specLastWatchRegisterTick > SPECTATE_REGISTER_INTERVAL_MS) {
    if (g_specUdpSocket != INVALID_SOCKET) {
      std::string msg = "{\"type\":\"watch\",\"matchId\":\"" + JsonEscapeSimple(g_specMatchId) +
        "\",\"uid\":\"" + JsonEscapeSimple(g_specUid) + "\"}";
      sendto(g_specUdpSocket, msg.data(), (int)msg.size(), 0, (sockaddr*)&g_specRelayUdpAddr, sizeof(g_specRelayUdpAddr));
    }
    g_specLastWatchRegisterTick = now;
  }

  // Presence and chat only now -- the live frame feed itself arrives over g_specTcpSocket below.
  if (g_specUdpSocket != INVALID_SOCKET) {
    char recvBuf[2048];
    for (;;) {
      int n = recv(g_specUdpSocket, recvBuf, sizeof(recvBuf) - 1, 0);
      if (n <= 0) break;
      recvBuf[n] = 0;
      std::string msg(recvBuf, n);

      std::string type;
      if (!JsonExtractString(msg, "type", type)) continue;

      if (type == "chat") {
        // Spectator-only chat from another watcher of this match (see main.go's "chat" case --
        // it never touches the publishers map, so this can never be a message from a player).
        // Called unconditionally, same as the player-chat receive path in fbn_ggpo.cpp --
        // VidOverlayAddChatLine already suppresses display on its own when isSpectatorChatMuted is
        // set (vid_overlay.cpp checks isSpectatorChatMuted rather than isChatMuted here because
        // kNetSpectator is set for this process), so no separate check is needed here.
        std::string userName, text;
        if (!JsonExtractString(msg, "userName", userName) || !JsonExtractString(msg, "text", text)) continue;

        TCHAR nameBuf[64];
        ANSIToTCHAR(userName.c_str(), nameBuf, 64);
        TCHAR textBuf[MAX_CHAT_SIZE];
        ANSIToTCHAR(text.c_str(), textBuf, MAX_CHAT_SIZE);
        VidOverlayAddChatLine(nameBuf, textBuf);
      }
      else if (type == "spectator-count") {
        // Pushed by the relay whenever the watcher count for this match changes (see
        // broadcastSpectatorCount in main.go); includes the local viewer in the count.
        long count = -1;
        if (!JsonExtractLong(msg, "count", count) || count < 0) continue;
        // Both overlay implementations updated together, matching the publisher-side handling
        // of this same message above -- see the comment there for why both are needed.
        VidOverlaySetGameSpectators((int)count);
        VidSSetGameSpectators((int)count);
      }
      // "publish"/"watch"/etc acks, if the relay ever sends any, are ignored.
    }
  }

  SpectateServiceIncomingTcp();

  // Reconnect-on-death: TCP guarantees no data loss/corruption while a connection is alive, but
  // not that the connection stays alive -- a transient NAT/firewall hiccup or dropped route can
  // kill it for reasons outside this process (and possibly outside the relay's own awareness
  // too, if it hasn't tried writing to this spectator since). Without this, a spectator whose
  // live-feed connection dies this way would be stuck permanently on its last frame even though
  // the match and relay are both fine. Cooldown-gated so a relay/network outage doesn't turn into
  // a tight retry loop.
  if (g_specTcpSocket == INVALID_SOCKET && now - g_specLastReconnectAttemptTick > SPECTATE_RECONNECT_COOLDOWN_MS) {
    g_specLastReconnectAttemptTick = now;
    long freshFrame = -1;
    long freshLiveFrame = -1;
    std::vector<unsigned char> freshPayload;
    std::string freshFailReason;
    SOCKET freshSocket = INVALID_SOCKET;
    if (RequestSnapshotOnce(g_specRelayHost, g_specRelayTcpPort, g_specMatchId, 3000, freshFrame, freshLiveFrame, freshPayload, freshFailReason, &freshSocket)) {
      ApplySnapshotAndBackfill(freshPayload, freshFrame, freshLiveFrame);
      u_long nb = 1;
      ioctlsocket(freshSocket, FIONBIO, &nb);
      g_specTcpSocket = freshSocket;
      g_specReadState = SPEC_READ_IDLE;
      bprintf(0, _T("*** spectate: live feed reconnected, snapshot frame %ld backfilled to %ld, streaming live from frame %d.\n"),
        freshFrame, freshLiveFrame, g_specExpectedNextFrame);
    }
  }

  // On-screen diagnostics for a stalled connection -- covers both the pre-first-frame case
  // (still "Waiting for stream...") and a later reconnect in progress (the message text below
  // distinguishes them). bprintf is a no-op outside a FBNEO_DEBUG build (see AppDebugPrintf,
  // main.cpp), so the debug console can't be relied on to surface this either way.
  bool tcpAlive = g_specTcpSocket != INVALID_SOCKET;
  bool stalled = (!g_specFirstFrameApplied && now - g_specConnectTick > 3000) || (g_specFirstFrameApplied && !tcpAlive);
  if (stalled && now - g_specLastStallWarnTick > 1000) {
    g_specLastStallWarnTick = now;
    bprintf(0, _T("*** spectate: still waiting %lums after connect -- %ld TCP messages received (%ld were 'frame'), last frame seen #%d, %d frame(s) queued, connection %hs.\n"),
      now - g_specConnectTick, g_specMessagesReceived, g_specFramesReceived, g_specLastFrameNumberSeen, (int)g_specFrameQueue.size(), tcpAlive ? "alive" : "DEAD");

    TCHAR msg[200];
    if (g_specFirstFrameApplied) {
      _sntprintf(msg, 200, _T("Reconnecting... (%ld msgs, %ld frames, last #%d, queued %d, %hs)"),
        g_specMessagesReceived, g_specFramesReceived, g_specLastFrameNumberSeen, (int)g_specFrameQueue.size(), tcpAlive ? "alive" : "dead");
    }
    else {
      _sntprintf(msg, 200, _T("Waiting for stream... (%ld msgs, %ld frames, last #%d, queued %d, %hs)"),
        g_specMessagesReceived, g_specFramesReceived, g_specLastFrameNumberSeen, (int)g_specFrameQueue.size(), tcpAlive ? "alive" : "dead");
    }
    VidOverlaySetSystemMessage(msg);
  }
  else if (g_specFirstFrameApplied && tcpAlive) {
    // Clear a stale "Reconnecting..."/"Waiting..." message left over from a stall that has
    // since resolved -- the normal per-frame path only clears this once, the first time
    // g_specFirstFrameApplied flips true, so a later reconnect needs its own clear here.
    VidOverlaySetSystemMessage(_T(""));
  }

  if (g_specFrameQueue.empty()) return false; // not arrived yet -- caller retries next tick

  if (!g_specFirstFrameApplied) {
    g_specFirstFrameApplied = true;
    bprintf(0, _T("*** spectate: live stream started.\n"));
    VidOverlaySetSystemMessage(_T(""));
  }

  memcpy(values, g_specFrameQueue.front().data(), SPECTATE_INPUT_BYTES);
  g_specFrameQueue.pop_front();
  return true;
}

void SpectateSendChat(const char* text)
{
  if (!g_spectateActive || !text) return;

  // Local echo first -- the relay never echoes a chat message back to its own sender (see
  // main.go's "chat" case), so without this the sender would never see their own message. Gated
  // on !isSpectatorChatMuted, the spectator-only counterpart of isChatMuted (menu.cpp's
  // MENU_MUTE_SPECTATOR_CHAT, own persisted setting -- see cona.cpp) -- same reasoning as
  // QuarkSendChat's identical local-echo branch (fbn_ggpo.cpp): VidOverlayAddChatLine already
  // suppresses display internally when muted (vid_overlay.cpp, using isSpectatorChatMuted here
  // since kNetSpectator is set), but calling it unconditionally here would also fire its "let the
  // other side know you're muted" datagram notify for what would just be our own echo, not an
  // incoming message -- skip the call entirely instead. The message is still sent below
  // regardless of mute state, same as QuarkSendChat still calls ggpo_send_chat unconditionally:
  // muting only affects what shows on this screen, not whether other spectators who aren't muted
  // can still see it.
  if (!isSpectatorChatMuted) {
    TCHAR nameBuf[64];
    ANSIToTCHAR(g_specUserName.c_str(), nameBuf, 64);
    TCHAR textBuf[MAX_CHAT_SIZE];
    ANSIToTCHAR(text, textBuf, MAX_CHAT_SIZE);
    VidOverlayAddChatLine(nameBuf, textBuf);
  }

  if (g_specUdpSocket == INVALID_SOCKET) return;
  std::string msg = "{\"type\":\"chat\",\"matchId\":\"" + JsonEscapeSimple(g_specMatchId) +
    "\",\"uid\":\"" + JsonEscapeSimple(g_specUid) +
    "\",\"userName\":\"" + JsonEscapeSimple(g_specUserName) +
    "\",\"text\":\"" + JsonEscapeSimple(text) + "\"}";
  sendto(g_specUdpSocket, msg.data(), (int)msg.size(), 0, (sockaddr*)&g_specRelayUdpAddr, sizeof(g_specRelayUdpAddr));
}

bool SpectateActive()
{
  return g_spectateActive;
}

void SpectateDisconnect()
{
  if (!g_spectateActive) return;

  if (g_specUdpSocket != INVALID_SOCKET) {
    std::string msg = "{\"type\":\"unwatch\",\"matchId\":\"" + JsonEscapeSimple(g_specMatchId) +
      "\",\"uid\":\"" + JsonEscapeSimple(g_specUid) + "\"}";
    sendto(g_specUdpSocket, msg.data(), (int)msg.size(), 0, (sockaddr*)&g_specRelayUdpAddr, sizeof(g_specRelayUdpAddr));
    closesocket(g_specUdpSocket);
    g_specUdpSocket = INVALID_SOCKET;
  }
  if (g_specTcpSocket != INVALID_SOCKET) {
    closesocket(g_specTcpSocket);
    g_specTcpSocket = INVALID_SOCKET;
  }

  RestoreHighResolutionTimingAfterNetplay();

  g_specFrameQueue.clear();
  g_spectateActive = false;
  kNetGame = 0;
  kNetSpectator = 0;
}
