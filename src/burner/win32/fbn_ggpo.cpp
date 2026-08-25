#include "version.h"
#include "burner.h"
#include "ggpoclient.h"
#include "ggpo_perfmon.h"
// #include "GGPOSession.h";

extern "C" {
#include "ggponet.h"
}

#include "../../ggpo/src/lib/log.h"


const size_t MAX_HOST = 128;

GGPOSession* ggpo = nullptr;
bool bSkipPerfmonUpdates = false;

void QuarkInitPerfMon();
void QuarkPerfMonUpdate(GGPONetworkStats* stats);

namespace Utils {
  extern void InitLogger(GGPOLogOptions& options_);
}

extern int nAcbVersion;
extern int nAcbLoadState;
extern int bMediaExit;
extern int nGGPOTimesyncFrames;

uint8_t _playerIndex = PLAYER_NOT_SET;
uint8_t _otherPlayerIndex = PLAYER_NOT_SET;

// rollback counter
// NOTE: These are only used in the video overlay. --> so they should be part of the overlay vars?
//int totalRollbackFrames = 0;              // The number of frames that were rolled back.
//int rollbackCount = 0;               // The total number of rollbacks.
//int lastRollbackFrame = 0;            // Frame number where the last rollback was encountered.
//int avgRollbackFrames = 0;

// rollback counter
int totalRollbackFrames = 0;
int totalRollbacks = 0;



static char pGameName[MAX_PATH];
static bool bDelayLoad = false;
static bool bDirect = false;
static bool bReplaySupport = false;
static bool bReplayStarted = false;
static bool bReplayRecord = false;
static bool bReplayRecording = false;
static int iRanked = 0;     // REFACTOR: boolean - 'isRanked'
//static int _playerIndex = 0;     // REFACTOR: uint16 'playerindex' --> NOTE: This is currently 1-based, and should be zero based!  --> NOTE: will be replaced with _playerIndex!
static int iDelay = 0;
static int iSeed = 0;
// Set whenever a GGPO timesync frame-skip correction fires (see nGGPOTimesyncFrames in
// GGPO_EVENTCODE_TIMESYNC below). Purely a UI latch: VidOverlaySetStats in
// src/intf/video/win32/vid_overlay.cpp (around the `if (bTimesyncDelayBumped)` block) checks
// this once per update to show a "timesync active" indicator and then clears it itself.
bool bTimesyncDelayBumped = false;

// True once GGPO_EVENTCODE_RUNNING has actually fired for the current session -- i.e. the match
// is truly synchronized, as opposed to kNetGame just being set (true for the whole "Connecting
// to Peer..."/"Synchronizing..." window too). See run.cpp's GetInput: real local input capture
// (the branch that mutates GameInpClearOpposites/GameInpFixDiagonals's SOCD history) is gated
// on this, not on kNetGame alone, so idle mashing during that window can't desync the match.
bool bGGPOMatchRunning = false;

// set by InitSpectatorConnection (fbn_spectate.cpp) before it calls ggpo_load_game_state_callback,
// so that function's kNetSpectator branch (below) can show the actual players' names in the
// overlay instead of a blank/generic label. The spectate relay itself never carries match
// metadata (only opaque confirmed-input bytes), so this has to be passed in from the launcher.
TCHAR g_spectateP1Name[128] = { 0 };
TCHAR g_spectateP2Name[128] = { 0 };

// Auto-delay negotiation (see ggpo_on_event_callback's GGPO_EVENTCODE_RUNNING and
// DATAGRAM_CODE_DELAY_NEGOTIATE handling, and the gate at the top of QuarkGetInput). Each side
// computes a candidate delay from its own measured ping and exchanges it with the peer; both
// settle on max(local, remote) so neither player ends up carrying more of the rollback burden
// than the other. Kicks off as soon as the connection can send data (GGPO_EVENTCODE_RUNNING,
// datagrams can't send before that: see UdpProtocol::SendData's `_current_state == Running`
// check in ggpo/src/lib/network/udp_proto.cpp) and is guaranteed to finish (or time out)
// strictly before any local input frame is queued: GGPO itself refuses local input while
// _synchronizing is set (Peer2PeerBackend::AddLocalInput in ggpo/src/lib/backends/p2p.cpp,
// returns GGPO_ERRORCODE_NOT_SYNCHRONIZED), and _synchronizing only clears right after RUNNING
// fires (Peer2PeerBackend::CheckInitialSync, same file). ggpo_set_frame_delay itself isn't
// safe once a frame is queued because InputQueue::AddInput bakes the delay into the frame
// number at queue time (ggpo/src/lib/input_queue.cpp, `frame += _frame_delay;`), changing it
// after the fact would make frame numbering discontinuous. ops.frameDelay stays in effect as
// the fallback value until negotiation resolves.
static bool bAutoDelay = false;
static bool bAutoDelayApplied = false;
static int nAutoDelayLocalCandidate = -1; // -1 = not yet computed
static int nAutoDelayBlockedFrames = 0;   // consecutive QuarkGetInput calls held off pending negotiation

static const int MIN_AUTO_DELAY_FRAMES = 0;
static const int MAX_AUTO_DELAY_FRAMES = 8;
// Safety net if the peer never replies (packet loss, or a peer build without this feature):
// stop holding off local input after this many frames (~2s @ 60fps) and just use what we have.
static const int AUTO_DELAY_TIMEOUT_FRAMES = 120;

// Aim the local candidate at roughly covering one-way trip time, so the remote's real input
// tends to arrive around when it's needed instead of relying entirely on rollback to cover it.
static int ComputeAutoDelayFrames(int pingMs)
{
  if (pingMs <= 0) return MIN_AUTO_DELAY_FRAMES;
  double frameMs = 100000.0 / (double)nBurnFPS;
  double oneWayMs = pingMs / 2.0;
  int frames = (int)((oneWayMs / frameMs) + 0.5);
  if (frames < MIN_AUTO_DELAY_FRAMES) frames = MIN_AUTO_DELAY_FRAMES;
  if (frames > MAX_AUTO_DELAY_FRAMES) frames = MAX_AUTO_DELAY_FRAMES;
  return frames;
}

// Sends our locally-computed candidate and, if we've already received the peer's, finalizes
// immediately. Safe to call from either the CONNECTED_TO_PEER handler or the datagram handler.
static void SendAutoDelayCandidate()
{
  if (nAutoDelayLocalCandidate < 0) {
    GGPONetworkStats stats;
    ggpo_get_stats(ggpo, &stats, _otherPlayerIndex);
    nAutoDelayLocalCandidate = ComputeAutoDelayFrames(stats.network.ping);
  }
  uint8_t candidate = (uint8_t)nAutoDelayLocalCandidate;
  QuarkSendData(DATAGRAM_CODE_DELAY_NEGOTIATE, &candidate, sizeof(candidate));
}

static void FinalizeAutoDelay(int remoteCandidate)
{
  if (bAutoDelayApplied) return; // already settled, ignore stray/duplicate packets

  if (nAutoDelayLocalCandidate < 0) {
    // Peer's candidate beat ours here (unlikely, but handle it): compute ours now.
    GGPONetworkStats stats;
    ggpo_get_stats(ggpo, &stats, _otherPlayerIndex);
    nAutoDelayLocalCandidate = ComputeAutoDelayFrames(stats.network.ping);
  }

  int finalDelay = (std::max)(nAutoDelayLocalCandidate, remoteCandidate);
  finalDelay = (std::min)(finalDelay, MAX_AUTO_DELAY_FRAMES);

  ggpo_set_frame_delay(ggpo, finalDelay, nVidRunahead);
  iDelay = finalDelay;
  bAutoDelayApplied = true;

  Utils::LogIt(CATEGORY_CONNECTION, "auto-delay negotiated: local=%d remote=%d final=%d", nAutoDelayLocalCandidate, remoteCandidate, finalDelay);
}

const int ggpo_state_header_size = 6 * sizeof(int);

int GetHash(const char* id, int len)
{
  unsigned int hash = 1315423911;
  for (int i = 0; i < len; i++) {
    hash ^= ((hash << 5) + id[i] + (hash >> 2));
  }
  return (hash & 0x7FFFFFFF);
}

void SetBurnFPS(const char* name, int version)
{
  if (kNetVersion < NET_VERSION_60FPS) {
    // Version 1: use old framerate (59.94fps)
    bForce60Hz = 1;
    nBurnFPS = 5994;
    nAppVirtualFps = nBurnFPS;
    return;
  }

  // UMK3UC at 60fps rate
  if (kNetVersion >= NET_VERSION_UMK3UC_FRAMERATE) {
    if (!strcmp(name, "umk3uc")) {
      bForce60Hz = 1;
      nBurnFPS = 6000;
      nAppVirtualFps = nBurnFPS;
      return;
    }
  }

  if (kNetVersion < NET_VERSION_DISABLE_FORCE_60HZ) {
    bForce60Hz = 1;
    // MK Framerate at original rate (54fps)
    if (kNetVersion >= NET_VERSION_MK_FRAMERATE) {
      if (!strcmp(name, "mk") || !strcmp(name, "mk2") || !strcmp(name, "mk2p") || !strcmp(name, "mk3") ||
        !strcmp(name, "umk3") || !strcmp(name, "umk3p") || !strcmp(name, "umk3uc") || !strcmp(name, "umk3uk") ||
        !strcmp(name, "wwfman")) {
        bForce60Hz = 0;
        return;
      }
    }
    nBurnFPS = 6000;
    nAppVirtualFps = nBurnFPS;
  }
}

// [OBSOLETE] - This will be removed / parted out!
bool __cdecl ggpo_on_client_event_callback(GGPOClientEvent* info)
{
  switch (info->code)
  {
  case GGPOCLIENT_EVENTCODE_CONNECTING:
    VidOverlaySetSystemMessage(_T("Connecting..."));
    VidSSetSystemMessage(_T("Connecting..."));
    break;

  case GGPOCLIENT_EVENTCODE_CONNECTED:
    VidOverlaySetSystemMessage(_T("Connected"));
    VidSSetSystemMessage(_T("Connected"));
    break;

  case GGPOCLIENT_EVENTCODE_DISCONNECTED:
    VidOverlaySetSystemMessage(_T("Disconnected!"));
    VidSSetSystemMessage(_T("Disconnected!"));
    QuarkFinishReplay();
    break;

  case GGPOCLIENT_EVENTCODE_RETREIVING_MATCHINFO:
    VidOverlaySetSystemMessage(_T("Retrieving Match Info..."));
    VidSSetSystemMessage(_T("Retrieving Match Info..."));
    break;

  case GGPOCLIENT_EVENTCODE_MATCHINFO: {
    VidOverlaySetSystemMessage(_T(""));
    VidSSetSystemMessage(_T(""));
    if (kNetSpectator) {
      // NOTE: The net version could also be retrieved by accepting the V* chat command data....
      kNetVersion = strlen(info->u.matchinfo.netVersion) > 0 ? atoi(info->u.matchinfo.netVersion) : NET_VERSION;
      SetBurnFPS(pGameName, kNetVersion);
    }
    TCHAR szUser1[128];
    TCHAR szUser2[128];
    VidOverlaySetGameInfo(ANSIToTCHAR(info->u.matchinfo.p1, szUser1, 128), ANSIToTCHAR(info->u.matchinfo.p2, szUser2, 128), kNetSpectator, iRanked, _playerIndex);
    VidSSetGameInfo(ANSIToTCHAR(info->u.matchinfo.p1, szUser1, 128), ANSIToTCHAR(info->u.matchinfo.p2, szUser2, 128), kNetSpectator, iRanked, _playerIndex);
    break;
  }

  case GGPOCLIENT_EVENTCODE_SPECTATOR_COUNT_CHANGED:
    VidOverlaySetGameSpectators(info->u.spectator_count_changed.count);
    VidSSetGameSpectators(info->u.spectator_count_changed.count);
    break;

  default:
    break;
  }
  return true;
}

// --------------------------------------------------------------------------------------------------------------------
void __cdecl ggpo_on_rollback(int onFrame, int frameCount)
{
  totalRollbacks++;
  totalRollbackFrames += frameCount;
}

// --------------------------------------------------------------------------------------------------------------------
bool __cdecl ggpo_on_event_callback(GGPOEvent* info)
{

  switch (info->event_code) {
  case GGPO_EVENTCODE_CONNECTED_TO_PEER:
  {
    VidOverlaySetSystemMessage(_T("Connected to Peer"));
    VidSSetSystemMessage(_T("Connected to Peer"));

    // NOTE: We can probably update the game info here....
    char* p1 = ggpo_get_playerName(ggpo, 0);
    char* p2 = ggpo_get_playerName(ggpo, 1);

    char p1Final[16 * 2];   // NOTE: NAME_MAX * 2 for formatting chars....
    char p2Final[16 * 2];   // NOTE: NAME_MAX * 2 for formatting chars....

    // TODO: REFACTOR:  In no universe should be encoding args into strings, and passing them to a function that will pull them back out via scanf.
    // This is a pretty gigundo inefficiency!
    sprintf(p1Final, "%s#0,0", p1);
    sprintf(p2Final, "%s#0,0", p2);

    TCHAR p1w[16 * 2];
    TCHAR p2w[16 * 2];
    TCHAR* buffer = ANSIToTCHAR(p1Final, NULL, NULL);
    wcscpy(p1w, buffer);

    buffer = ANSIToTCHAR(p2Final, NULL, NULL);
    wcscpy(p2w, buffer);


    VidOverlaySetGameInfo(p1w, p2w, false, false, _playerIndex);

    VidOverlaySetRemoteStats(info->u.connected.delay, info->u.connected.runahead);

  }
  break;

  case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
    //_stprintf(status, _T("Synchronizing with Peer (%d/%d)..."), info->u.synchronizing.count, info->u.synchronizing.total);
    VidOverlaySetSystemMessage(_T("Synchronizing with Peer..."));
    VidSSetSystemMessage(_T("Synchronizing with Peer..."));
    break;

  case GGPO_EVENTCODE_RUNNING: {
    VidOverlaySetSystemMessage(_T(""));
    VidSSetSystemMessage(_T(""));

    // The match is now actually synchronized. Let run.cpp's GetInput start capturing real
    // local input (see bGGPOMatchRunning's declaration above), and wipe whatever SOCD/diagonal
    // history built up from idle mashing during the "Connecting.../Synchronizing..." window so
    // it can't bias the first real synchronized frame differently on each machine.
    bGGPOMatchRunning = true;
    GameInpResetSOCDHistory();

    // NOTE: Version exchange happens during sync now.

    // NOT the earlier GGPO_EVENTCODE_CONNECTED_TO_PEER, that fires on the first sync reply,
    // several round trips before this one (see the `_connected`/QueueEvent(evt) block ahead of
    // the round-trip countdown in UdpProtocol::OnSyncReply, ggpo/src/lib/network/udp_proto.cpp),
    // while datagrams can't send until Running (see the auto-delay comment block up top for the
    // full chain of reasoning + citations for why this is still safe despite the round trip to
    // the peer not having completed yet by the time we get here).
    if (bAutoDelay && !kNetSpectator) {
      SendAutoDelayCandidate();
    }
    break;
  }

  case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
    VidOverlaySetSystemMessage(_T("Disconnected from Peer"));
    VidSSetSystemMessage(_T("Disconnected from Peer"));
    // The local simulation can't advance without the peer (rollback only covers
    // MAX_PREDICTION_FRAMES worth of missing input, see ggpo/src/lib/sync.h) -- once it's stuck
    // waiting, the sound card just keeps repeating whatever's left in its buffer, which is
    // audible as a buzz/drone rather than silence. Stop it instead of leaving that playing.
    AudSoundStop();
    if (bReplayRecording) {
      AviStop();
      bMediaExit = true;
    }
    break;

  case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
    // Same "stuck waiting for the peer" case as above, but before GGPO gives up and declares a
    // full disconnect (see SetDisconnectTimeout/SetDisconnectNotifyStart) -- the peer may still
    // come back, so this stops the buzzing without ending the match outright.
    VidOverlaySetSystemMessage(_T("Connection interrupted..."));
    VidSSetSystemMessage(_T("Connection interrupted..."));
    AudSoundStop();
    break;

  case GGPO_EVENTCODE_CONNECTION_RESUMED:
    VidOverlaySetSystemMessage(_T(""));
    VidSSetSystemMessage(_T(""));
    AudSoundPlay();
    break;

  case GGPO_EVENTCODE_TIMESYNC:
    // Frame-skip approach: stall the ahead client by up to 2 frames during idle.
    // ggpo_set_frame_delay is not safe mid-match (breaks input frame sequencing).
    if (!kNetSpectator) {
      const int add = info->u.timesync.frames_ahead;
      if (add > 0) {
        nGGPOTimesyncFrames = (std::min)(add, 2);
        bTimesyncDelayBumped = true; // signal overlay, overlay clears this after latching
      }
    }
    break;


  case GGPO_EVENTCODE_DATAGRAM:

    switch (info->u.datagram.code)
    {
    case DATAGRAM_CODE_GGPO_SETTINGS:
    {
      auto data = info->u.datagram.data;

      // DEFINED in vid_overlay.cpp
      uint8_t delay = data[0];
      uint8_t runahead = data[1];
      VidOverlaySetRemoteStats(delay, runahead);
    }
    break;

    case DATAGRAM_CODE_DELAY_NEGOTIATE:
    {
      if (bAutoDelay && !kNetSpectator) {
        int remoteCandidate = (uint8_t)info->u.datagram.data[0];
        if (nAutoDelayLocalCandidate < 0) {
          // We haven't sent ours yet (peer's packet beat our CONNECTED_TO_PEER handling),
          // send it now so the peer settles too, then finalize on our end.
          SendAutoDelayCandidate();
        }
        FinalizeAutoDelay(remoteCandidate);
      }
    }
    break;

    case DATAGRAM_CODE_SPECTATOR_COUNT:
    {
      // Forwarded by playerSlot 0 (see SpectatePublishTick in fbn_spectate.cpp) -- this player
      // has no connection to the spectate relay at all, so this datagram is the only way it
      // learns the current spectator count. Both overlay implementations updated together
      // (VidOverlay* in vid_overlay.cpp, VidS* in vid_directx_support.cpp) since a normal
      // player's own window renders through VidS*, not VidOverlay*.
      int count = (int)info->u.datagram.data[0];
      VidOverlaySetGameSpectators(count);
      VidSSetGameSpectators(count);
    }
    break;

    case DATAGRAM_CODE_CHAT:
    // Backwards compatibility: older builds used ASCII 'T' for chat datagrams.
    case 'T':
    {
      TCHAR szUser[MAX_CHAT_SIZE];
      TCHAR szText[MAX_CHAT_SIZE];

      auto msg = info->u.datagram.data;

      // NOTE: I have the player index, but not the actual lookup table for them... that should come from the client...
      // NOTE: I can't include 'ggposession.h' at this time because it will break the compilation.  I will go back and
      // find a proper way to include it later......
      char* playerName = ggpo_get_playerName(ggpo, info->player_index);
      ANSIToTCHAR(playerName ? playerName : "Unknown", szUser, MAX_NAME_SIZE);

      const uint8_t safeSize = (uint8_t)(std::min)((uint32_t)info->u.datagram.dataSize, (uint32_t)(MAX_CHAT_SIZE - 1));
      ANSIToTCHAR(msg, szText, safeSize);

      // Chat messages must be zero terminated...
      szText[safeSize] = 0;

      // NOTE: Kind of silly that we have to come up with another string when we already have the 'C' command code.
      // TCHAR* useName = first == 'C' ? _T("Command") : szUser;
      VidOverlayAddChatLine(szUser, szText);

      // ummmm.... do we know what this is all about?
      // --> It appears that there is some other overlay / OSD layer that isn't used.  Might be for the DDraw7 stuff which we don't care about.
      // I think in 2025 that such a rendering approach can be ignored / removed.
      //TCHAR szTemp[MAX_CHAT_SIZE];
      //_sntprintf(szTemp, MAX_CHAT_SIZE, _T("«%.32hs» "), info->u.chat.username);
      //VidSAddChatLine(szTemp, 0XFFA000, ANSIToTCHAR(info->u.chat.text, NULL, 0), 0xEEEEEE);
    }
    break;

    default:
      break;
    }

    break;

  default:
    break;
  }

  return true;
}

bool __cdecl ggpo_begin_game_callback(const char* name)
{
  WIN32_FIND_DATA fd;
  TCHAR tfilename[MAX_PATH];
  TCHAR tname[MAX_PATH];

  strcpy(pGameName, name);
  ANSIToTCHAR(name, tname, MAX_PATH);
  SetBurnFPS(name, kNetVersion);

  if (!kNetSpectator)
  {
    // ranked savestate
    if (iRanked) {
      _stprintf(tfilename, _T("savestates\\%s_fbneo_ranked.fs"), tname);
      HANDLE hFind = FindFirstFile(tfilename, &fd);
      if (hFind != INVALID_HANDLE_VALUE) {
        FindClose(hFind);
        // Load our save-state file (freeplay, event mode, etc.)
        const INT32 loadRes = BurnStateLoad(tfilename, 1, &DrvInitCallback);
        if (loadRes == 0) {
          DetectorLoad(name, false, iSeed);
          // if playing a direct game, we never get match information, so put anonymous
          if (bDirect) {
            // VidSSetGameInfo is dead code (unconditional early return, see its definition in
            // vid_directx_support.cpp) -- VidOverlaySetGameInfo is the actually-rendered overlay,
            // and it's also the only thing that sets game_enabled, the master gate around the
            // whole names+spectator-count block in vid_overlay.cpp's render function. Without
            // this call, a direct connection's own window could never show the spectator count
            // no matter what VidOverlaySetGameSpectators was told.
            VidOverlaySetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, _playerIndex);
            VidSSetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, _playerIndex);
          }
          return 0;
        }
        // If the savestate exists but can't be loaded (wrong format / wrong version),
        // fall back to normal game init instead of leaving the emulator half-initialized.
      }
    }

    // regular savestate
    _stprintf(tfilename, _T("savestates\\%s_fbneo.fs"), tname);
    HANDLE hFind = FindFirstFile(tfilename, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
      FindClose(hFind);
      // Load our save-state file (freeplay, event mode, etc.)
      const INT32 loadRes = BurnStateLoad(tfilename, 1, &DrvInitCallback);
      if (loadRes == 0) {
        DetectorLoad(name, false, iSeed);
        // if playing a direct game, we never get match information, so put anonymous
        if (bDirect) {
          // See the matching comment above -- VidSSetGameInfo does nothing.
          VidOverlaySetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, _playerIndex);
          VidSSetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, _playerIndex);
        }
        return 0;
      }
      // Fall back to normal init if load failed.
    }
  }

  // no savestate or replay
  UINT32 i;
  for (i = 0; i < nBurnDrvCount; i++) {
    nBurnDrvActive = i;
    if ((_tcscmp(BurnDrvGetText(DRV_NAME), tname) == 0) && (!(BurnDrvGetFlags() & BDF_BOARDROM))) {
      if (!kNetSpectator) {
        MediaInit();
        
        // NOTE: If this is not a kNetGame, then the default game state will be loaded in the DrvInit call.
        // In the block above (~line 288) we are loading a different state.  Since we are in a GGPO
        // callback, we can safely assume that kNetGame == true.
        DrvInit(i, true);
      }
      else {
        // I'm guessing we load this later so that sync up game state for the spectators later....?
        bDelayLoad = true;
      }
      DetectorLoad(name, false, iSeed);
      // if playing a direct game, we never get match information, so play anonymous
      if (bDirect) {
        // See the matching comment above (savestate-load branch) -- VidSSetGameInfo does
        // nothing; VidOverlaySetGameInfo is the one that actually matters (it's also what sets
        // game_enabled, gating the overlay's names+spectator-count block). This is the path a
        // fresh match with no pre-existing savestate actually takes, so without this call here,
        // a normal direct-connection match could never show the spectator count at all.
        VidOverlaySetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, _playerIndex);
        VidSSetGameInfo(_T("Player1#0,0"), _T("Player2#0,0"), false, iRanked, _playerIndex);
      }
      return 1;
    }
  }

  return 0;
}

bool __cdecl ggpo_rollback_frame_callback(int flags)
{
  bSkipPerfmonUpdates = true;
  nFramesEmulated--;

  // Run the frame.  This will not sync inputs and will draw / do sound, etc.
  // NOTE: The reference implementation syncs inputs on rollback....
  RunFrame(0, 0, false);
  bSkipPerfmonUpdates = false;
  return true;
}

static char gAcbBuffer[16 * 1024 * 1024];
static char* gAcbScanPointer;
static int gAcbChecksum;
static FILE* gAcbLogFp;

void ComputeIncrementalChecksum(struct BurnArea* pba)
{
  /*
   * Ignore checksums in release builds for now.  It takes a while.
   */
#if defined(FBA_DEBUG)
  int i;

#if 0
  static char* soundAreas[] = {
     "Z80",
     "YM21",
     "nTicksDone",
     "nCyclesExtra",
  };
  /*
   * This is a really crappy checksum routine, but it will do for
   * our purposes
   */
  for (i = 0; i < ARRAYSIZE(soundAreas); i++) {
    if (!strncmp(soundAreas[i], pba->szName, strlen(soundAreas[i]))) {
      return;
    }
  }
#endif

  for (i = 0; i < pba->nLen; i++) {
    int b = ((unsigned char*)pba->Data)[i];
    if (b) {
      if (i % 2)
        gAcbChecksum *= b;
      else
        gAcbChecksum += b * 317;
    }
    else
      gAcbChecksum++;
  }
#endif
}

static int QuarkLogAcb(struct BurnArea* pba)
{
  fprintf(gAcbLogFp, "%s:", pba->szName);
  int col = 10, row = 30;
  for (int i = 0; i < (int)pba->nLen; i++) {
    if ((i % row) == 0)
      fprintf(gAcbLogFp, "\noffset %9d :", i);
    else if ((i % col) == 0)
      fprintf(gAcbLogFp, " - ");
    fprintf(gAcbLogFp, " %02x", ((unsigned char*)pba->Data)[i]);
  }
  fprintf(gAcbLogFp, "\n");
  ComputeIncrementalChecksum(pba);
  return 0;
}

static int QuarkReadAcb(struct BurnArea* pba)
{
  //ComputeIncrementalChecksum(pba);
  memcpy(gAcbScanPointer, pba->Data, pba->nLen);
  gAcbScanPointer += pba->nLen;
  return 0;
}
static int QuarkWriteAcb(struct BurnArea* pba)
{
  memcpy(pba->Data, gAcbScanPointer, pba->nLen);
  gAcbScanPointer += pba->nLen;
  return 0;
}

bool __cdecl ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int frame)
{
  int payloadsize;

  gAcbChecksum = 0;
  gAcbScanPointer = gAcbBuffer;
  BurnAcb = QuarkReadAcb;
  BurnAreaScan(ACB_FULLSCANL | ACB_READ, NULL);
  payloadsize = gAcbScanPointer - gAcbBuffer;

  *checksum = gAcbChecksum;
  *len = payloadsize + ggpo_state_header_size;
  *buffer = (unsigned char*)malloc(*len);

  int* data = (int*)*buffer;
  data[0] = 'GGPO';
  data[1] = ggpo_state_header_size;
  data[2] = nBurnVer;
  data[3] = 0;
  data[4] = 0;
  data[5] = 0;
  // save game state in ranked match (so spectators can get the actual score)
  if (!kNetSpectator) {
    // 32 bits for state, scores and ranked
    int state, score1, score2, start1, start2;
    DetectorGetState(state, score1, score2, start1, start2);
    data[3] = state | ((score1 & 0xff) << 8) | ((score2 & 0xff) << 16) | (iRanked << 24);
    data[4] = (start1 & 0xff) | ((start2 & 0xff) << 8);
  }
  memcpy((*buffer) + ggpo_state_header_size, gAcbBuffer, payloadsize);
  return false;
}

// Extracted from ggpo_load_game_state_callback below so it can be triggered independently of any
// snapshot buffer. A spectator connection (fbn_spectate.cpp's InitSpectatorConnection) calls this
// right after ggpo_begin_game_callback, since the ROM/driver load doesn't depend on match state at
// all -- only on the ROM name already passed to that call -- so there's no reason to wait on a
// network round trip first just to have *some* buffer to invoke the old fused function with. A
// no-op if the driver's already loaded (bDelayLoad false), so calling it from both here and
// InitSpectatorConnection is safe regardless of which one runs first.
void EnsureSpectatorDriverLoaded()
{
  if (!bDelayLoad) return;
  DrvInit(nBurnDrvActive, true);
  MediaInit();
  RunInit();
  bDelayLoad = false;
}

bool __cdecl ggpo_load_game_state_callback(unsigned char* buffer, int len)
{
  // Used for initial loading.  Connecting + starting up spectator stuff and so on....
  EnsureSpectatorDriverLoaded();

  // The buffer is all of the game data that we are interested in.
  // It includes state?, scores, ranked, etc.
  int* data = (int*)buffer;
  if (data[0] == 'GGPO') {
    int headersize = data[1];
    int num = headersize / sizeof(int);
    // version
    nAcbVersion = data[2];
    int state = (data[3]) & 0xff;
    int score1 = (data[3] >> 8) & 0xff;
    int score2 = (data[3] >> 16) & 0xff;
    int ranked = (data[3] >> 24) & 0xff;
    int start1 = 0;
    int start2 = 0;
    if (num > 4) {
      start1 = (data[4]) & 0xff;
      start2 = (data[4] >> 8) & 0xff;
    }
    // if spectating, set ranked flag and score
    if (kNetSpectator) {
      iRanked = ranked;
      DetectorSetState(state, score1, score2, start1, start2);
      // g_spectateP1Name/g_spectateP2Name are set by InitSpectatorConnection (fbn_spectate.cpp)
      // before this callback runs -- the spectate relay itself never carries match metadata, so
      // the launcher passes these in directly rather than they being discoverable here.
      //
      // VidOverlaySetGameInfo/VidSSetGameInfo parse "Name#rank,score,country" out of each name
      // string (see VidOverlaySetGameInfo in vid_overlay.cpp) -- a bare name with no "#" leaves
      // the parsed rank position at 0, which truncates the displayed name to nothing (matches
      // the existing "Player1#0,0" placeholder pattern used elsewhere in this same function for
      // direct/non-matchmade connections). Append the same placeholder suffix here.
      TCHAR p1Buf[160], p2Buf[160];
      _sntprintf(p1Buf, 160, _T("%s#0,0"), g_spectateP1Name[0] ? g_spectateP1Name : _T("Player 1"));
      _sntprintf(p2Buf, 160, _T("%s#0,0"), g_spectateP2Name[0] ? g_spectateP2Name : _T("Player 2"));
      const TCHAR* p1Name = p1Buf;
      const TCHAR* p2Name = p2Buf;
      VidOverlaySetGameInfo(p1Name, p2Name, kNetSpectator, iRanked, 0);
      VidOverlaySetGameScores(score1, score2);
      VidSSetGameInfo(p1Name, p2Name, kNetSpectator, iRanked, 0);
      VidSSetGameScores(score1, score2);
    }

    // We move the pointer ahead to include the actual game / ROM data.
    // This gets loaded into the game state via 'BurnAreaScan' calls below.
    buffer += headersize;
  }
  gAcbScanPointer = (char*)buffer;
  BurnAcb = QuarkWriteAcb;
  nAcbLoadState = kNetSpectator;
  BurnAreaScan(ACB_FULLSCANL | ACB_WRITE, NULL);
  nAcbLoadState = 0;
  nAcbVersion = nBurnVer;

  return true;
}

bool __cdecl ggpo_log_game_state_callback(char* filename, unsigned char* buffer, int len)
{
  /*
   * Note: this is destructive since it relies on loading game
   * state before scanning!  Luckily, we only call the logging
   * routine for fatal errors (we should still fix this, though).
   */
  ggpo_load_game_state_callback(buffer, len);

  gAcbLogFp = fopen(filename, "w");

  gAcbChecksum = 0;
  BurnAcb = QuarkLogAcb;
  BurnAreaScan(ACB_FULLSCANL | ACB_READ, NULL);
  fprintf(gAcbLogFp, "\n");
  fprintf(gAcbLogFp, "Checksum:       %d\n", gAcbChecksum);
  fprintf(gAcbLogFp, "Buffer Pointer: %p\n", buffer);
  fprintf(gAcbLogFp, "Buffer Len:     %d\n", len);

  fclose(gAcbLogFp);

  return true;
}

// ----------------------------------------------------------------------------------------------------------------------------------------------
// NOTE: This looks like it could directly in the lib....
void __cdecl ggpo_free_buffer_callback(void* buffer)
{
  free(buffer);
}


// ----------------------------------------------------------------------------------------------------------------------------------------------
// Parse the address of the form [host]:port into host (ip) and port.
void ParseAddress(const char* addr, char* host, UINT16* port)
{
  memset(host, 0, MAX_HOST);

  int pos = -1;
  size_t len = strnlen_s(addr, MAX_HOST);
  for (UINT i = 0; i < len; i++)
  {
    if (addr[i] == ':') { pos = i; break; }
  }
  if (pos == -1) {
    memcpy(host, addr, len);
  }
  else {
    if (pos > 0) {
      memcpy(host, addr, pos);
    }
    char portPart[MAX_HOST];
    strncpy_s(portPart, len - (pos + 1) + 1, addr + pos + 1, MAX_HOST);

    UINT16 usePort = std::atoi(portPart);
    if (usePort != 0) {
      *port = usePort;
    }

  }
}

// ----------------------------------------------------------------------------------------------------------------------------------------------
int InitDirectConnection(DirectConnectionOptions& ops, GGPOLogOptions& logOps)
{

  Utils::InitLogger(logOps);

  const UINT16 DEFAULT_LOCAL_PORT = 7000;
  const UINT16 DEFAULT_REMOTE_PORT = 7000;

  // Let's parse out the ips/ports...
  char remoteIP[MAX_HOST];
  UINT16 localPort = DEFAULT_LOCAL_PORT;
  UINT16 remotePort = DEFAULT_REMOTE_PORT;

  char localHost[MAX_HOST];
  char remoteHost[MAX_HOST];

  try
  {
    ParseAddress(ops.localAddr.data(), localHost, &localPort);
    ParseAddress(ops.remoteAddr.data(), remoteHost, &remotePort);
  }
  catch (const std::exception&)
  {
    throw std::exception("Could not parse local or remote address");
  }

  kNetVersion = NET_VERSION;
  kNetGame = 1;
  kNetLua = 0;
  kNetSpectator = 0;
  kNetQuarkId[0] = 0;
  bForce60Hz = 0;
  iRanked = 0;
  _playerIndex = PLAYER_NOT_SET;
  iDelay = 0;
  nGGPOTimesyncFrames = 0;
  bTimesyncDelayBumped = false;
  bGGPOMatchRunning = false;
  bAutoDelay = ops.autoDelay;
  bAutoDelayApplied = false;
  nAutoDelayLocalCandidate = -1;
  nAutoDelayBlockedFrames = 0;
  nVidRunahead = 0; // reset so stale config value isn't sent to peer as our runahead

#ifdef _DEBUG
  kNetLua = 1;
#endif

  GGPOSessionCallbacks cb = { 0 };
  cb.begin_game = ggpo_begin_game_callback;
  cb.load_game_state = ggpo_load_game_state_callback;
  cb.save_game_state = ggpo_save_game_state_callback;
  cb.log_game_state = ggpo_log_game_state_callback;
  cb.free_buffer = ggpo_free_buffer_callback;
  cb.rollback_frame = ggpo_rollback_frame_callback;
  cb.on_event = ggpo_on_event_callback;
  cb.on_rollback = ggpo_on_rollback;


  // SET GLOBALS
  kNetLua = 1;
  bDirect = true;
  iRanked = 0;
  _playerIndex = ops.playerNumber - 1;
  _otherPlayerIndex = _playerIndex == 0 ? 1 : 0;

  iDelay = ops.frameDelay;
  iSeed = 0;

  ggpo = ggpo_start_session(&cb, ops.romName.data(), localPort, remoteHost, remotePort, _playerIndex, ops.playerName.data(), FS_VERSION);

  ggpo_set_frame_delay(ggpo, ops.frameDelay, nVidRunahead);
  VidOverlaySetSystemMessage(_T("Connecting..."));

  // Coarse OS timer resolution on either end skews frame pacing enough to look like
  // one-sided rollback to the other player. Force precise timing for netplay regardless
  // of the user's offline-play preference (bEnableHighResTimer / EnableHighResolutionTiming
  // in src/burner/win32/misc_win32.cpp, applied once at app startup, see
  // ForceHighResolutionTimingForNetplay in the same file for why a separate, session-scoped
  // call is used instead of touching that setting directly); restored in QuarkEnd().
  ForceHighResolutionTimingForNetplay();

  // Publisher (host) side of spectating (src/burner/win32/fbn_spectate.cpp) -- independent of
  // the GGPO peer handshake above (it's a separate connection to the relay, not to the other
  // player), so it can start immediately rather than waiting on any GGPO event. Only ever runs
  // on the playerSlot-0 client, per the websocket-server's spectate-request handler. No-ops
  // internally if ops.spectate wasn't set.
  if (ops.spectate && ops.playerNumber == 1) {
    SpectatePublishInit(ops);
  }

  return 0;


}

// ----------------------------------------------------------------------------------------------------------------------------------------------
// [OBSOLETE] -- This will be parted out into individual functions as we go.  Leaving it here for historical purposes.
void QuarkInit(TCHAR* tconnect)
{
  char connect[MAX_PATH];
  TCHARToANSI(tconnect, connect, MAX_PATH);
  char gameName[128], quarkid[128], remoteIp[128];
  int port = 0;
  int delay = 0;
  int ranked = 0;
  int live = 0;
  int frames = 0;
  int playerNumber = 0;
  int localPort, remotePort;

  kNetVersion = NET_VERSION;
  kNetGame = 1;
  kNetLua = 0;
  kNetSpectator = 0;
  kNetQuarkId[0] = 0;
  bForce60Hz = 0;
  iRanked = 0;
  _playerIndex = 0;
  iDelay = 0;
  nGGPOTimesyncFrames = 0;

#ifdef _DEBUG
  kNetLua = 1;
#endif

  GGPOSessionCallbacks cb = { 0 };

  cb.begin_game = ggpo_begin_game_callback;
  cb.load_game_state = ggpo_load_game_state_callback;
  cb.save_game_state = ggpo_save_game_state_callback;
  cb.log_game_state = ggpo_log_game_state_callback;
  cb.free_buffer = ggpo_free_buffer_callback;
  cb.rollback_frame = ggpo_rollback_frame_callback;
  cb.on_event = ggpo_on_event_callback;

  // This path is used for connecting to other players via FC servers.
  // We need a centralized server for it.....
  //if (strncmp(connect, "quark:served", strlen("quark:served")) == 0) {
  //	sscanf(connect, "quark:served,%[^,],%[^,],%d,%d,%d", game, quarkid, &port, &delay, &ranked);
  //	iRanked = ranked;
  //	_playerIndex = atoi(&quarkid[strlen(quarkid) - 1]);
  //	if (nVidRunahead == 3 && delay < 2) {delay = 2;}
  //	iDelay = delay;
  //	iSeed = GetHash(quarkid, strlen(quarkid) - 2);
  //	ggpo = ggpo_client_connect(&cb, game, quarkid, port);
  //	ggpo_set_frame_delay(ggpo, delay);
  //	strcpy(kNetQuarkId, quarkid);
  //	VidOverlaySetSystemMessage(_T("Connecting..."));
  //}
  // Also not used....
  //if (strncmp(connect, "quark:training", strlen("quark:training")) == 0) {
  //	sscanf(connect, "quark:training,%[^,],%[^,],%d,%d", game, quarkid, &port, &delay);
  //	iRanked = 0;
  //	_playerIndex = atoi(&quarkid[strlen(quarkid) - 1]);
  //	if (nVidRunahead == 3 && delay < 2) {delay = 2;}
  //	iDelay = delay;
  //	iSeed = GetHash(quarkid, strlen(quarkid) - 2);
  //	ggpo = ggpo_client_connect(&cb, game, quarkid, port);
  //	ggpo_set_frame_delay(ggpo, delay);
  //	strcpy(kNetQuarkId, quarkid);
  //	VidOverlaySetSystemMessage(_T("Connecting..."));
  //	kNetLua = 1;
  //	FBA_LoadLuaCode("fbneo-training-mode/fbneo-training-mode.lua");
  //}
  if (strncmp(connect, "quark:direct", strlen("quark:direct")) == 0) {

    throw std::exception("obsolete branch!  don't come here!");
    //// REFERENCE for sscanf: https://en.cppreference.com/w/c/io/fscanf
    //int scanCount = sscanf(connect, "quark:direct,%[^,],%d,%[^,],%d,%d,%d,%d", gameName, &localPort, remoteIp, &remotePort, &playerNumber, &delay, &ranked);
    //if (scanCount != 7) {
    //  // TODO: Find the best way to handle bad CLI inputs.
    //  // A proper CLI parser might be nice at some point too.
    //  throw std::exception("bad command line!");
    //}
    //if (playerNumber < 1 || playerNumber > 2) {
    //  throw std::exception("Invalid player number.  Use 1 or 2!");
    //}


    //kNetLua = 1;
    //bDirect = true;
    //iRanked = 0;
    //_playerIndex = playerNumber;
    //iDelay = delay;
    //iSeed = 0;
    //// ggpo = ggpo_start_session(&cb, game, localPort, host, remotePort, playerNumber);
    //ggpo = ggpo_start_session(&cb, gameName, localPort, remoteIp, remotePort, playerNumber - 1);
    //ggpo_set_frame_delay(ggpo, delay);
    //VidOverlaySetSystemMessage(_T("Connecting..."));
  }
  /*
  else if (strncmp(connect, "quark:synctest", strlen("quark:synctest")) == 0) {
    sscanf(connect, "quark:synctest,%[^,],%d", game, &frames);
    ggpo = ggpo_start_synctest(&cb, game, frames);
  }
  */

  // NOTE: Streaming is not possible at this time.  We need a server to connect to.
  // quark:stream is how replay + spectating is handled.
  //else if (strncmp(connect, "quark:stream", strlen("quark:stream")) == 0) {
  //	sscanf(connect, "quark:stream,%[^,],%[^,],%d", game, quarkid, &remotePort);
  //	bVidAutoSwitchFullDisable = true;
  //	kNetSpectator = 1;
  //	kNetLua = 1;
  //	iSeed = 0;

  //	// NOTE: ggpo_start_spectating is what the original call probably was/is.
  //	ggpo = ggpo_start_streaming(&cb, game, quarkid, remotePort);
  //	strcpy(kNetQuarkId, quarkid);
  //	VidOverlaySetSystemMessage(_T("Connecting..."));
  //}
  // It appears that the replay functionality is not used.
  // Spectating and replay in FC is handled via 'quark:stream'

  //else if (strncmp(connect, "quark:replay", strlen("quark:replay")) == 0) {
  //	bVidAutoSwitchFullDisable = true;
  //	kNetSpectator = 1;
  //	kNetLua = 1;
  //	iSeed = 0;
  //	ggpo = ggpo_start_replay(&cb, connect + strlen("quark:replay,"));
  //	strcpy(kNetQuarkId, quarkid);
  //	VidOverlaySetSystemMessage(_T("Connecting..."));
  //}
  else if (strncmp(connect, "quark:debugdetector", strlen("quark:debugdetector")) == 0) {
    sscanf(connect, "quark:debugdetector,%[^,]", gameName);
    kNetGame = 0;
    iRanked = 1;
    _playerIndex = 0;
    iSeed = 0x133;
    kNetLua = 1;
    // load game
    TCHAR tgame[128];
    ANSIToTCHAR(gameName, tgame, 128);
    for (UINT32 i = 0; i < nBurnDrvCount; i++) {
      nBurnDrvActive = i;
      if ((_tcscmp(BurnDrvGetText(DRV_NAME), tgame) == 0) && (!(BurnDrvGetFlags() & BDF_BOARDROM))) {
        // Load game
        MediaInit();
        DrvInit(i, true);
        // Load our save-state file (freeplay, event mode, etc.)
        WIN32_FIND_DATA fd;
        TCHAR tfilename[MAX_PATH];
        _stprintf(tfilename, _T("savestates\\%s_fbneo.fs"), tgame);
        HANDLE hFind = FindFirstFile(tfilename, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
          FindClose(hFind);
          BurnStateLoad(tfilename, 1, &DrvInitCallback);
        }
        DetectorLoad(gameName, true, iSeed);
        VidOverlaySetGameInfo(_T("Detector1#0,0,0"), _T("Detector2#0,0,0"), false, iRanked, _playerIndex);
        VidOverlaySetGameSpectators(0);
        VidSSetGameInfo(_T("Detector1"), _T("Detector2"), false, iRanked, _playerIndex);
        VidSSetGameSpectators(0);
        break;
      }
    }
  }
}

// -------------------------------------------------------------------------------------------------------------------
void QuarkEnd()
{
  ConfigGameSave(bSaveInputs);
  ggpo_close_session(ggpo);
  SpectatePublishShutdown(); // no-op unless this instance was publishing
  SpectateDisconnect(); // no-op unless this instance was spectating -- sends "unwatch" so the
                         // relay drops this connection immediately instead of waiting out its
                         // ~15s UDP registration timeout before the spectator count updates
  RestoreHighResolutionTimingAfterNetplay();
  kNetGame = 0;
  nGGPOTimesyncFrames = 0;
  bTimesyncDelayBumped = false;
  bGGPOMatchRunning = false;
  bAutoDelay = false;
  bAutoDelayApplied = false;
  nAutoDelayLocalCandidate = -1;
  nAutoDelayBlockedFrames = 0;
  bMediaExit = true;

}

// -------------------------------------------------------------------------------------------------------------------
void QuarkTogglePerfMon()
{
  static bool initialized = false;
  if (!initialized) {
    ggpoutil_perfmon_init(hScrnWnd);
  }
  ggpoutil_perfmon_toggle();
}

// -------------------------------------------------------------------------------------------------------------------
void QuarkRunIdle(int ms)
{
  ggpo_idle(ggpo, ms);
}

// -------------------------------------------------------------------------------------------------------------------
// Disconnect ourselves from the system... This is what actually runs when the window is closed
// via its [X] button (WM_CLOSE -> OnClose's PostQuitMessage -> WM_QUIT in RunMessageLoop calls
// this, not QuarkEnd -- that's reserved for the explicit MENU_QUIT/MENU_EXIT commands). Without
// these two calls here, closing a host's or spectator's window this way left the relay unaware
// until its own registration timeouts caught up (~15s for a spectator's UDP "watch", longer for a
// publisher), instead of notifying it immediately.
void QuarkDisconnect()
{
  ggpo_disconnect(ggpo);
  SpectatePublishShutdown(); // no-op unless this instance was publishing
  SpectateDisconnect(); // no-op unless this instance was spectating
}

// -------------------------------------------------------------------------------------------------------------------
bool QuarkGetInput(void* values, int isize, int playerIndex)
{
  // Hold off feeding local input until auto-delay negotiation resolves. GGPO itself refuses
  // local input until _synchronizing clears, see the `if (_synchronizing) return
  // GGPO_ERRORCODE_NOT_SYNCHRONIZED;` check at the top of Peer2PeerBackend::AddLocalInput, and
  // Peer2PeerBackend::CheckInitialSync where _synchronizing is only cleared right *after*
  // GGPO_EVENTCODE_RUNNING is dispatched (both ggpo/src/lib/backends/p2p.cpp), but our
  // negotiation needs one more round trip after that to settle, and ggpo_set_frame_delay isn't
  // safe once a frame has been queued (see the auto-delay comment block near the top of this
  // file for why), so this closes that gap. Returning false here is the same "not ready yet"
  // signal GGPO's own _synchronizing gate already produces during the pre-Running window, the
  // caller already handles it as a routine skip-and-retry, not an error: NetworkGetInput()
  // (src/burner/win32/fbn_network.cpp) returns early, and RunFrame (src/burner/win32/run.cpp,
  // the `if (NetworkGetInput())` branches) just retries next tick without advancing the
  // emulator or calling ggpo_advance_frame. Times out and proceeds with whatever candidate we
  // have if the peer never replies, so a lost packet can't hang the match.
  if (bAutoDelay && !bAutoDelayApplied) {
    nAutoDelayBlockedFrames++;
    if (nAutoDelayBlockedFrames < AUTO_DELAY_TIMEOUT_FRAMES) {
      return false;
    }
    Utils::LogIt(CATEGORY_CONNECTION, "auto-delay negotiation timed out after %d frames, using local candidate", nAutoDelayBlockedFrames);
    if (nAutoDelayLocalCandidate >= 0) {
      ggpo_set_frame_delay(ggpo, nAutoDelayLocalCandidate, nVidRunahead);
      iDelay = nAutoDelayLocalCandidate;
    }
    bAutoDelayApplied = true;
  }

  // NOTE: This call is handling both the addition of the local inputs, and the sync call....
  bool res = ggpo_synchronize_input(ggpo, values, isize, GGPO_MAX_PLAYERS) == GGPO_OK;

  // Record this frame's confirmed input for the spectate-relay publisher (fbn_spectate.cpp),
  // if active. Called on every ggpo_synchronize_input, including rollback resimulations (see
  // ggpo_rollback_frame_callback below -> RunFrame(0,0,false) -> NetworkGetInput() ->
  // QuarkGetInput), so a mispredicted frame's ring buffer entry gets overwritten with the
  // corrected value before SpectatePublishTick ever considers it old enough to publish.
  if (res && SpectatePublishActive()) {
    SpectatePublishRecordFrame(ggpo_get_current_frame(ggpo), values);
  }

  return res;
}

// -------------------------------------------------------------------------------------------------------------------
bool QuarkIncrementFrame()
{
  // start auto replay
  if (bReplayRecord) {
    bReplayRecord = false;
    bReplayRecording = true;
    AviStart();
  }

  if (ggpo) {
    // Everything in this block assumes a live GGPO session (host or player). A relay spectator
    // never starts one (see InitSpectatorConnection, fbn_spectate.cpp), so skip it entirely --
    // ggpo_advance_frame/ggpo_get_stats are null-safe on their own, but ggpoutil_perfmon_update
    // isn't verified to be, and there's nothing useful for any of this to do for a spectator.
    ggpo_advance_frame(ggpo);

    if (!bSkipPerfmonUpdates) {
      GGPONetworkStats stats;

      ggpo_get_stats(ggpo, &stats, _otherPlayerIndex);
      ggpoutil_perfmon_update(ggpo, stats);
    }

    SpectatePublishTick(); // no-op unless this instance is publishing
  }

  if (!bReplaySupport && !bReplayStarted) {
    bReplayStarted = true;
  }

  return true;
}

// --------------------------------------------------------------------------------------------------------
void QuarkSendChat(char* text)
{
  if (_playerIndex != PLAYER_NOT_SET && ggpo && !isChatMuted) {
    auto playerName = ggpo_get_playerName(ggpo, _playerIndex);
    wchar_t nameBuffer[16 * 2];
    wcscpy(nameBuffer, ANSIToTCHAR(playerName ? playerName : "Unknown", NULL, NULL));
    VidOverlayAddChatLine(nameBuffer, ANSIToTCHAR(text, NULL, NULL));
  }
  ggpo_send_chat(ggpo, text);
}


// --------------------------------------------------------------------------------------------------------
void QuarkSendData(uint8_t code, void* data, uint8_t dataSize)
{
  if (code == 'T' && _playerIndex != PLAYER_NOT_SET && ggpo && !isChatMuted)
  {
    static char msgBuffer[MAX_GGPO_DATA_SIZE]; // command chat  +1 for command char +1 for zero termination.
    auto useSize = (std::min)(dataSize, (uint8_t)(MAX_GGPO_DATA_SIZE - 1));
    memcpy_s(msgBuffer, MAX_GGPO_DATA_SIZE, data, useSize);
    msgBuffer[useSize] = 0;

    auto playerName = ggpo_get_playerName(ggpo, _playerIndex);
    wchar_t nameBuffer[16 * 2];
    wcscpy(nameBuffer, ANSIToTCHAR(playerName, NULL, NULL));

    VidOverlayAddChatLine(nameBuffer, ANSIToTCHAR(msgBuffer + 1, NULL, NULL));
  }


  ggpo_send_data(ggpo, code, data, dataSize);
}

// --------------------------------------------------------------------------------------------------------
// [OBSOLETE:  THIS DOES NOTHING!]
void QuarkSendChatCmd(char* text, char code)
{
  return;

  static char msgBuffer[MAX_CHAT_SIZE]; // command chat  +1 for command char +1 for zero termination.
  memset(msgBuffer, 0, MAX_CHAT_SIZE);

  msgBuffer[0] = code;
  strncpy(&msgBuffer[1], text, MAX_CHAT_SIZE - 1);

  // Print the chat line on our local:
  if (code == 'T' && _playerIndex != PLAYER_NOT_SET && ggpo && !isChatMuted) {
    auto playerName = ggpo_get_playerName(ggpo, _playerIndex);
    wchar_t nameBuffer[16 * 2];
    wcscpy(nameBuffer, ANSIToTCHAR(playerName, NULL, NULL));

  }
  ggpo_send_chat(ggpo, msgBuffer);
}

// --------------------------------------------------------------------------------------------------------
void QuarkUpdateStats(double fps)
{
  GGPONetworkStats stats;
  ggpo_get_stats(ggpo, &stats, _otherPlayerIndex);
  // VidSSetStats(fps, stats.network.ping, iDelay);

  // NOTE: This is where the rollback, etc. data is sent.  Pretty sure that 'VidSSetStats' doesn't get used anymore, or is for other overlay systems that we will never see again.....
  VidOverlaySetStats(fps, stats.network.ping, iDelay);
}

// --------------------------------------------------------------------------------------------------------
void QuarkRecordReplay()
{
  bReplayRecord = true;
  bReplayRecording = false;
}

// --------------------------------------------------------------------------------------------------------
void QuarkFinishReplay()
{
  if (!bReplaySupport && bReplayStarted) {
    bReplayStarted = false;
    kNetGame = 0;
    if (bReplayRecording) {
      AviStop();
      bMediaExit = true;
    }
  }
}
