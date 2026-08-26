// Spectating: watch a live match through the backend relay, not GGPO. No rollback.
#include "burner.h"
#include <winsock.h>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Needed for QuarkSendData below, to send the spectator count to the opponent.
extern "C" {
#include "ggponet.h"
}

// Both players' input combined: 2 x 7 bytes each. Spectating is 1v1 only, same as GGPO.
static const int SPECTATE_INPUT_BYTES = 14;
// How many recent frames we keep, so a new viewer's snapshot can catch up to live.
static const int SPECTATE_RING_SIZE = 256;
static const int SPECTATE_PUBLISH_DELAY_FRAMES = 20; // must stay > MAX_PREDICTION_FRAMES (15)
static const DWORD SPECTATE_REGISTER_INTERVAL_MS = 5000; // relay drops a registration after 15s
static const DWORD SPECTATE_CONNECT_RETRY_BUDGET_MS = 20000; // total time to retry "host not publishing yet"
static const DWORD SPECTATE_CONNECT_RETRY_INTERVAL_MS = 750;

// Simple hand-written JSON reading, not a full parser, just enough for our fixed message shapes.
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

// Escapes quotes and backslashes so a matchId/uid can't accidentally break the JSON.
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

// Base64 encode/decode, used for the "data" field of "frame" messages.
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

// Socket helpers, using the same older winsock.h API as main.cpp.
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

  // Switch back to blocking mode for the simple read/write helpers below.
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

// One message is [4-byte length][header JSON][optional raw data]. Only used for the snapshot fetch.
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

// Builds one complete framed message, ready to be queued for sending.
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

// Publisher (host) side, only runs on player 1's client with --spectate.
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

  // This TCP handling never blocks, since it runs every tick on the real game thread.
  enum PubReadState { PUB_READ_IDLE, PUB_READ_LEN, PUB_READ_HEADER };
  PubReadState g_pubReadState = PUB_READ_IDLE;
  unsigned char g_pubReadLenBuf[4];
  int g_pubReadLenGot = 0;
  uint32_t g_pubReadHeaderLen = 0;
  std::vector<char> g_pubReadHeaderBuf;
  int g_pubReadHeaderGot = 0;

  // Outgoing messages wait here, sent a bit at a time each tick without blocking.
  std::deque<std::vector<char>> g_pubTcpOutQueue;
  size_t g_pubTcpOutSent = 0;

  // Only hand a viewer a snapshot once it's old enough that GGPO can't roll it back.
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
  // A failed TCP connection just means viewers can't join, the match still plays fine.

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

// Takes a new snapshot roughly once a second, and promotes the previous one once it's safe.
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

// Reads incoming messages without blocking. Only reacts to "snapshot-request".
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
          // Relay closed the connection, drop it.
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
        return; // nothing more this tick, resume next tick
      }
      uint32_t len = ntohl(*(uint32_t*)g_pubReadLenBuf);
      if (len == 0 || len > 4096) {
        // Bad data, drop the connection.
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
        // Skip replying if we don't have a safe snapshot yet, the viewer just retries shortly.
        if (g_pubSafeSnapshotFrame >= 0) {
          // Backfill every frame since the snapshot so the viewer starts already caught up.
          std::vector<unsigned char> backfill;
          int liveFrame = g_pubSafeSnapshotFrame;
          for (int f = g_pubSafeSnapshotFrame + 1; f < g_pubNextFrameToPublish; f++) {
            PublishRingEntry& entry = g_pubRing[f % SPECTATE_RING_SIZE];
            if (entry.frame != f) break; // shouldn't happen, stop rather than send wrong data
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
      // Ignore anything else, and go around to check for the next message.
    }
  }
}

// Sends as much of the queue as the socket accepts right now, without blocking.
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
      // Skip a missing entry rather than get stuck.
      g_pubNextFrameToPublish++;
    }
  }

  // Spectator-count is the one thing the relay sends back to the publisher on its own.
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
      // Update both overlay systems, the host's own window uses the other one.
      VidOverlaySetGameSpectators((int)count);
      VidSSetGameSpectators((int)count);

      // Forward the count to the opponent over GGPO, since only player 1 talks to the relay.
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

// Spectator (viewer) side.
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

  SOCKET g_specTcpSocket = INVALID_SOCKET; // the ongoing live-feed connection
  std::deque<std::vector<unsigned char>> g_specFrameQueue; // frames received but not used yet
  bool g_specFirstFrameApplied = false;

  // Reads the live feed without blocking, same idea as the publisher's own reader.
  enum SpecReadState { SPEC_READ_IDLE, SPEC_READ_LEN, SPEC_READ_HEADER };
  SpecReadState g_specReadState = SPEC_READ_IDLE;
  unsigned char g_specReadLenBuf[4];
  int g_specReadLenGot = 0;
  uint32_t g_specReadHeaderLen = 0;
  std::vector<char> g_specReadHeaderBuf;
  int g_specReadHeaderGot = 0;

  // Frame number we expect next. Reconnect if a frame arrives with the wrong number.
  int g_specExpectedNextFrame = -1;

  // For the "still waiting" warning message in SpectateGetInput.
  DWORD g_specConnectTick = 0;
  DWORD g_specLastStallWarnTick = 0;
  long g_specMessagesReceived = 0;
  long g_specFramesReceived = 0;
  int g_specLastFrameNumberSeen = -1;

  // Reconnect after the connection dies, so a network hiccup doesn't freeze the viewer forever.
  DWORD g_specLastReconnectAttemptTick = 0;
}

static const DWORD SPECTATE_RECONNECT_COOLDOWN_MS = 3000;

// Tries once to fetch a fresh snapshot. If outSocket is given, keeps the connection open for reuse.
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
  outLiveFrame = outFrame; // fallback in case liveFrame is ever missing
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

// Applies the snapshot state and queues its catch-up frames for SpectateGetInput.
static void ApplySnapshotAndBackfill(std::vector<unsigned char>& payload, long frame, long liveFrame)
{
  size_t backfillCount = (liveFrame > frame) ? (size_t)(liveFrame - frame) : 0;
  size_t backfillBytes = backfillCount * SPECTATE_INPUT_BYTES;
  if (backfillBytes > payload.size()) {
    // Bad data, skip the catch-up frames.
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

  // Same globals a normal match sets, but no real GGPO session is needed for a spectator.
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

  // Same accurate timing players get.
  ForceHighResolutionTimingForNetplay();

  // ROM loading is deferred to ggpo_load_game_state_callback, so spectators don't all load at once.
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

  // The host may not be publishing yet, so keep retrying for a while.
  VidOverlaySetSystemMessage(_T("Connecting to host..."));

  bool ok = false;
  long frame = -1;
  long liveFrame = -1;
  std::vector<unsigned char> payload;
  std::string lastFailReason;
  SOCKET liveSocket = INVALID_SOCKET;
  DWORD connectStartTick = GetTickCount();

  // This fetch gets the starting snapshot and keeps the connection open as the live feed.
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

  // One more "still here" message before the ROM load, which can take a while.
  sendto(g_specUdpSocket, watchMsg.data(), (int)watchMsg.size(), 0, (sockaddr*)&g_specRelayUdpAddr, sizeof(g_specRelayUdpAddr));
  g_specLastWatchRegisterTick = GetTickCount();

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

// Reads incoming "frame" messages without blocking. TCP already guarantees order.
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
        return; // nothing more available this tick, resume next tick, or already dropped above
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
          // Wrong frame number means something slipped through, reconnect for a fresh snapshot.
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
      // Ignore anything else, and go around to check for the next message.
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

  // This UDP socket only carries presence and chat, the live feed arrives over TCP below.
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
        // Chat from another viewer. VidOverlayAddChatLine hides it itself if muted.
        std::string userName, text;
        if (!JsonExtractString(msg, "userName", userName) || !JsonExtractString(msg, "text", text)) continue;

        TCHAR nameBuf[64];
        ANSIToTCHAR(userName.c_str(), nameBuf, 64);
        TCHAR textBuf[MAX_CHAT_SIZE];
        ANSIToTCHAR(text.c_str(), textBuf, MAX_CHAT_SIZE);
        VidOverlayAddChatLine(nameBuf, textBuf);
      }
      else if (type == "spectator-count") {
        // The relay sends this whenever the number of viewers changes.
        long count = -1;
        if (!JsonExtractLong(msg, "count", count) || count < 0) continue;
        VidOverlaySetGameSpectators((int)count);
        VidSSetGameSpectators((int)count);
      }
      // Anything else is ignored.
    }
  }

  SpectateServiceIncomingTcp();

  // Reconnect if the connection died, with a cooldown so an outage doesn't cause rapid retries.
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

  // Show a message if we're stuck waiting for the first frame or a reconnect.
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
    // Clear a leftover "Reconnecting..." message once things are working again.
    VidOverlaySetSystemMessage(_T(""));
  }

  if (g_specFrameQueue.empty()) return false; // nothing yet, try again next tick

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

  // The relay doesn't echo our own message back, so show it locally unless we're muted.
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
