// Spectating via the backend relay (src/util-servers/spectate-relay/main.go in the
// hyper-reflector-backend repo), NOT GGPO. A relayed feed only ever carries already-confirmed
// input (the relay is a dumb forwarder, no prediction happens anywhere in this path), so the
// viewer side just applies input directly each tick like a live network replay -- there's no
// rollback, and GGPOSession/Peer2PeerBackend/SpectatorBackend are never touched by this file.
//
// Wire protocol (must match src/util-servers/spectate-relay/main.go exactly):
//
//   UDP -- the live per-frame confirmed-input feed. Every packet is one JSON object:
//     {"type":"publish","matchId":"...","uid":"..."}         host -> relay, periodic keepalive
//     {"type":"unpublish","matchId":"..."}                   host -> relay, explicit stop
//     {"type":"watch","matchId":"...","uid":"..."}           viewer -> relay, periodic keepalive
//     {"type":"unwatch","matchId":"...","uid":"..."}         viewer -> relay, explicit stop
//     {"type":"frame","matchId":"...","frame":N,"data":"<base64>"}  host -> relay -> viewers
//
//   TCP -- the one-shot state-snapshot handoff for mid-match join. Every message on the wire
//   is [4-byte big-endian header length][header JSON][optional raw payload immediately after]:
//     Host, on connect: {"role":"publisher","matchId":"..."} then keeps the connection open.
//     Relay pushes, per viewer request: {"cmd":"snapshot-request","requestId":"..."}
//     Host responds: {"cmd":"snapshot-response","requestId":"...","frame":N,"payloadSize":M}
//       immediately followed by M raw (not base64'd) bytes.
//     Viewer, on connect: {"role":"spectator","matchId":"..."}, then reads exactly one reply:
//       either the snapshot-response above, or {"cmd":"error","reason":"..."}.
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
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Per-frame confirmed input size: Peer2PeerBackend hardcodes exactly 2 players
// (ggpo/src/lib/backends/p2p.h, PLAYER_COUNT) at 7 bytes each (ggpo/src/lib/game_input.h,
// GAMEINPUT_MAX_BYTES) -- that's the whole combined buffer Sync::SynchronizeInputs fills
// (ggpo/src/lib/sync.cpp) and QuarkGetInput hands back. Spectating is scoped to 1v1 matches for
// exactly the same reason Peer2PeerBackend is.
static const int SPECTATE_INPUT_BYTES = 14;
static const int SPECTATE_RING_SIZE = 64;
static const int SPECTATE_PUBLISH_DELAY_FRAMES = 20; // must stay > MAX_PREDICTION_FRAMES (15)
static const DWORD SPECTATE_REGISTER_INTERVAL_MS = 5000; // relay drops a registration after 15s

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
// Base64, for the "data" field of {"type":"frame",...} packets.
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
// writeFramed/readFramedHeader exactly (src/util-servers/spectate-relay/main.go).
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
}

void SpectatePublishInit(const DirectConnectionOptions& ops)
{
  if (g_publishActive) return;
  if (!ops.spectate) return;

  g_pubMatchId = ops.spectateMatchId;
  g_pubUid = ops.spectateUid;

  for (int i = 0; i < SPECTATE_RING_SIZE; i++) g_pubRing[i].frame = -1;
  g_pubHighestRecordedFrame = -1;
  g_pubNextFrameToPublish = -1;

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
  // A failed TCP connection only means mid-match join won't be able to fetch a snapshot from
  // this host; the live UDP feed below still works, so this isn't treated as a hard failure.

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

// Services at most one pending snapshot request per call -- this only fires when a new
// spectator joins mid-match, so a brief blocking read/respond on the otherwise non-blocking
// publisher TCP connection is simpler and safer than a partial-read buffering state machine
// for something this rare.
static void SpectatePublishServiceSnapshotRequest()
{
  if (g_pubTcpSocket == INVALID_SOCKET) return;

  char peekBuf[1];
  int n = recv(g_pubTcpSocket, peekBuf, 1, MSG_PEEK);
  if (n == 0) {
    // Relay closed the connection. Drop it -- the live UDP feed keeps working regardless;
    // mid-match join just won't be servable from this host until it reconnects.
    closesocket(g_pubTcpSocket);
    g_pubTcpSocket = INVALID_SOCKET;
    return;
  }
  if (n < 0) {
    return; // WSAEWOULDBLOCK (nothing pending) or a transient error -- nothing to do this tick.
  }

  u_long blockingMode = 0;
  ioctlsocket(g_pubTcpSocket, FIONBIO, &blockingMode);

  std::string headerJson;
  if (ReadFramedHeader(g_pubTcpSocket, headerJson, 2000)) {
    std::string cmd, requestId;
    JsonExtractString(headerJson, "cmd", cmd);
    JsonExtractString(headerJson, "requestId", requestId);

    if (cmd == "snapshot-request" && !requestId.empty()) {
      unsigned char* buffer = nullptr;
      int len = 0;
      int checksum = 0;
      int frame = g_pubHighestRecordedFrame >= 0 ? g_pubHighestRecordedFrame : 0;

      // Always populates buffer/len unconditionally -- ggpo_save_game_state_callback's own
      // body (fbn_ggpo.cpp) has no early-return path, so its bool return isn't meaningful here.
      ggpo_save_game_state_callback(&buffer, &len, &checksum, frame);

      std::string resp = "{\"cmd\":\"snapshot-response\",\"requestId\":\"" + JsonEscapeSimple(requestId) +
        "\",\"frame\":" + std::to_string(frame) + ",\"payloadSize\":" + std::to_string(len) + "}";
      WriteFramedHeader(g_pubTcpSocket, resp, buffer, len);

      if (buffer) ggpo_free_buffer_callback(buffer);
    }
  }

  u_long nb = 1;
  ioctlsocket(g_pubTcpSocket, FIONBIO, &nb);
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

  if (g_pubUdpSocket != INVALID_SOCKET && g_pubNextFrameToPublish >= 0) {
    while (g_pubHighestRecordedFrame - g_pubNextFrameToPublish >= SPECTATE_PUBLISH_DELAY_FRAMES) {
      int frame = g_pubNextFrameToPublish;
      PublishRingEntry& entry = g_pubRing[frame % SPECTATE_RING_SIZE];
      if (entry.frame == frame) {
        std::string b64 = Base64Encode(entry.bytes, SPECTATE_INPUT_BYTES);
        std::string msg = "{\"type\":\"frame\",\"matchId\":\"" + JsonEscapeSimple(g_pubMatchId) +
          "\",\"frame\":" + std::to_string(frame) + ",\"data\":\"" + b64 + "\"}";
        sendto(g_pubUdpSocket, msg.data(), (int)msg.size(), 0, (sockaddr*)&g_pubRelayUdpAddr, sizeof(g_pubRelayUdpAddr));
      }
      // A missing ring entry shouldn't normally happen (the ring is far larger than the
      // publish delay) -- but if it does, skipping it beats stalling the publish cursor
      // forever; a viewer just briefly rebuffers.
      g_pubNextFrameToPublish = frame + 1;
    }
  }

  SpectatePublishServiceSnapshotRequest();
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

  SOCKET g_specUdpSocket = INVALID_SOCKET;
  sockaddr_in g_specRelayUdpAddr;
  DWORD g_specLastWatchRegisterTick = 0;

  std::map<int, std::vector<unsigned char>> g_specFrameBuffer;
  int g_specNextFrameToApply = -1;
}

bool InitSpectatorConnection(const SpectateConnectionOptions& ops)
{
  if (g_spectateActive) SpectateDisconnect();

  g_specMatchId = ops.matchId;
  g_specUid = ops.uid;
  g_specFrameBuffer.clear();
  g_specNextFrameToApply = -1;

  // Same globals InitDirectConnection sets before a match begins. ggpo_begin_game_callback and
  // ggpo_load_game_state_callback (both reused below, see burner_win32.h) are already
  // kNetSpectator-aware: the former defers DrvInit/MediaInit to the latter (via bDelayLoad),
  // which is exactly the "wait for a snapshot before initializing the driver" sequence
  // mid-match join needs, and neither one touches GGPOSession at all.
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

  g_specUdpSocket = CreateNonBlockingUdpSocket();
  if (g_specUdpSocket == INVALID_SOCKET || !MakeSockAddr(ops.relayHost, ops.relayUdpPort, &g_specRelayUdpAddr)) {
    bprintf(0, _T("*** spectate: failed to set up relay UDP connection.\n"));
    if (g_specUdpSocket != INVALID_SOCKET) closesocket(g_specUdpSocket);
    g_specUdpSocket = INVALID_SOCKET;
    kNetGame = 0;
    kNetSpectator = 0;
    return false;
  }
  {
    std::string watchMsg = "{\"type\":\"watch\",\"matchId\":\"" + JsonEscapeSimple(g_specMatchId) +
      "\",\"uid\":\"" + JsonEscapeSimple(g_specUid) + "\"}";
    sendto(g_specUdpSocket, watchMsg.data(), (int)watchMsg.size(), 0, (sockaddr*)&g_specRelayUdpAddr, sizeof(g_specRelayUdpAddr));
    g_specLastWatchRegisterTick = GetTickCount();
  }

  SOCKET tcp = TcpConnectWithTimeout(ops.relayHost, ops.relayTcpPort, 5000);
  if (tcp == INVALID_SOCKET) {
    bprintf(0, _T("*** spectate: could not connect to relay for snapshot.\n"));
    closesocket(g_specUdpSocket);
    g_specUdpSocket = INVALID_SOCKET;
    kNetGame = 0;
    kNetSpectator = 0;
    return false;
  }

  std::string hello = "{\"role\":\"spectator\",\"matchId\":\"" + JsonEscapeSimple(g_specMatchId) + "\"}";
  std::string respHeaderJson;
  bool ok = WriteFramedHeader(tcp, hello, nullptr, 0) && ReadFramedHeader(tcp, respHeaderJson, 5000);

  if (ok) {
    std::string cmd;
    JsonExtractString(respHeaderJson, "cmd", cmd);
    if (cmd == "snapshot-response") {
      long frame = -1;
      long payloadSize = -1;
      JsonExtractLong(respHeaderJson, "frame", frame);
      JsonExtractLong(respHeaderJson, "payloadSize", payloadSize);
      if (payloadSize < 0 || payloadSize > 64 * 1024 * 1024) {
        ok = false;
      }
      else {
        std::vector<unsigned char> payload((size_t)payloadSize);
        if (payloadSize > 0 && !RecvAll(tcp, (char*)payload.data(), (int)payloadSize, 5000)) {
          ok = false;
        }
        else {
          ggpo_load_game_state_callback(payload.data(), (int)payload.size());
          g_specNextFrameToApply = (int)frame + 1;
        }
      }
    }
    else {
      std::string reason;
      JsonExtractString(respHeaderJson, "reason", reason);
      bprintf(0, _T("*** spectate: relay declined snapshot request (%hs).\n"), reason.c_str());
      ok = false;
    }
  }

  closesocket(tcp);

  if (!ok) {
    closesocket(g_specUdpSocket);
    g_specUdpSocket = INVALID_SOCKET;
    kNetGame = 0;
    kNetSpectator = 0;
    return false;
  }

  g_spectateActive = true;
  return true;
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

  if (g_specUdpSocket != INVALID_SOCKET) {
    char recvBuf[2048];
    for (;;) {
      int n = recv(g_specUdpSocket, recvBuf, sizeof(recvBuf) - 1, 0);
      if (n <= 0) break;
      recvBuf[n] = 0;
      std::string msg(recvBuf, n);

      std::string type;
      if (!JsonExtractString(msg, "type", type) || type != "frame") continue;

      long frame = -1;
      std::string dataB64;
      if (!JsonExtractLong(msg, "frame", frame) || !JsonExtractString(msg, "data", dataB64)) continue;
      if (frame < g_specNextFrameToApply) continue; // already applied / stale duplicate

      std::vector<unsigned char> decoded;
      if (!Base64Decode(dataB64, decoded) || decoded.size() != SPECTATE_INPUT_BYTES) continue;

      g_specFrameBuffer[(int)frame] = std::move(decoded);
    }
  }

  if (g_specNextFrameToApply < 0) return false;

  auto it = g_specFrameBuffer.find(g_specNextFrameToApply);
  if (it == g_specFrameBuffer.end()) return false; // not arrived yet -- caller retries next tick

  memcpy(values, it->second.data(), SPECTATE_INPUT_BYTES);
  g_specFrameBuffer.erase(it);
  g_specNextFrameToApply++;
  return true;
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

  g_specFrameBuffer.clear();
  g_specNextFrameToApply = -1;
  g_spectateActive = false;
  kNetGame = 0;
  kNetSpectator = 0;
}
