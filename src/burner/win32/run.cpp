// Run module
#include "burner.h"
#include <string.h>

int bRunPause = 0;
int bAltPause = 0;
int bAlwaysDrawFrames = 0;

// REFACTOR: Rename to 'ShowStats' or similar.  
int bShowFPS = SHOWSTATS_NONE;

static unsigned int nDoFPS = 0;

static bool bMute = false;
static int nOldAudVolume;

int kNetVersion = NET_VERSION;			// Network version
int kNetGame = 0;						// Non-zero if network is being used
int kNetSpectator = 0;					// Non-zero if network replay is active
int kNetLua = 1;						// Allow lua in network game
char kNetQuarkId[128] = {};				// Network quark id

#ifdef FBNEO_DEBUG
int counter;								// General purpose variable used when debugging
#endif

static double nFrameLast = 0;
static bool bAppDoStep = 0;
static bool bAppDoFast = 0;
static bool bAppDoFastToggled = 0;
static int nFastSpeed = 10;
static int nSkipFrames = 0;
static int nRunQuark = 1;

int bRestartVideo = 0;
int bDrvExit = 0;
int bMediaExit = 0;

// For System Macros (below)
static int prevPause = 0, prevFFWD = 0, prevFrame = 0, prevSState = 0, prevLState = 0, prevUState = 0;
UINT32 prevPause_debounce = 0;

void EmulatorAppDoFast(bool dofast) {
  bAppDoFast = dofast;
  bAppDoFastToggled = 0;
}

struct lua_hotkey_handler {
  int prev_value;
  UINT8* macroSystemLuaHotkey_ref;
  UINT8 macroSystemLuaHotkey_val;
  std::string hotkey_name;

  lua_hotkey_handler(
    UINT8* _macroSystemLuaHotkey_ref,
    UINT8 _macroSystemLuaHotkey_val,
    int _prev_value = 0,
    std::string hotkey_name = "lua_hotkey"
  ) : macroSystemLuaHotkey_ref(_macroSystemLuaHotkey_ref) {
  }

};

std::vector<lua_hotkey_handler> hotkey_debounces = {
  { &macroSystemLuaHotkey1, macroSystemLuaHotkey1, 0 },
  { &macroSystemLuaHotkey2, macroSystemLuaHotkey2, 0 },
  { &macroSystemLuaHotkey3, macroSystemLuaHotkey3, 0 },
  { &macroSystemLuaHotkey4, macroSystemLuaHotkey4, 0 },
  { &macroSystemLuaHotkey5, macroSystemLuaHotkey5, 0 },
  { &macroSystemLuaHotkey6, macroSystemLuaHotkey6, 0 },
  { &macroSystemLuaHotkey7, macroSystemLuaHotkey7, 0 },
  { &macroSystemLuaHotkey8, macroSystemLuaHotkey8, 0 },
  { &macroSystemLuaHotkey9, macroSystemLuaHotkey9, 0 },
};

static void CheckSystemMacros() // These are the Pause / FFWD macros added to the input dialog
{
  // Pause
  if (macroSystemPause && macroSystemPause != prevPause && timeGetTime() > prevPause_debounce + 90) {
    if (bHasFocus) {
      PostMessage(hScrnWnd, WM_KEYDOWN, VK_PAUSE, 0);
      prevPause_debounce = timeGetTime();
    }
  }
  prevPause = macroSystemPause;
  if (!kNetGame || kNetSpectator) {
    // FFWD
    if (macroSystemFFWD) {
      bAppDoFast = 1; prevFFWD = 1;
    }
    else if (prevFFWD) {
      bAppDoFast = 0; prevFFWD = 0;
    }
    // Frame
    if (macroSystemFrame) {
      bRunPause = 1;
      if (!prevFrame) {
        bAppDoStep = 1;
        prevFrame = 1;
      }
    }
    else {
      prevFrame = 0;
    }
  }
  if (!kNetGame) {
    // Lua hotkeys
    for (int hotkey_num = 0; hotkey_num <= hotkey_debounces.size() - 1; hotkey_num++) {
      // Use the reference to the hotkey variable in order to update our stored value
      // Because the hotkeys are hard coded into variables this allows us to iterate on them
      hotkey_debounces[hotkey_num].macroSystemLuaHotkey_val = *hotkey_debounces[hotkey_num].macroSystemLuaHotkey_ref;
      if (
        hotkey_debounces[hotkey_num].macroSystemLuaHotkey_val &&
        hotkey_debounces[hotkey_num].macroSystemLuaHotkey_val != hotkey_debounces[hotkey_num].prev_value
        ) {
        CallRegisteredLuaFunctions((LuaCallID)(LUACALL_HOTKEY_1 + hotkey_num));
      }
      hotkey_debounces[hotkey_num].prev_value = hotkey_debounces[hotkey_num].macroSystemLuaHotkey_val;
    }
  }
  // Load State
  if (macroSystemLoadState && macroSystemLoadState != prevLState) {
    PostMessage(hScrnWnd, WM_KEYDOWN, VK_F9, 0);
  }
  prevLState = macroSystemLoadState;
  // Save State
  if (macroSystemSaveState && macroSystemSaveState != prevSState) {
    PostMessage(hScrnWnd, WM_KEYDOWN, VK_F10, 0);
  }
  prevSState = macroSystemSaveState;
  // UNDO State
  if (macroSystemUNDOState && macroSystemUNDOState != prevUState) {
    scrnSSUndo();
  }
  prevUState = macroSystemUNDOState;
}

static int GetInput(bool bCopy)
{
  // get input
  InputMake(bCopy);

  if (bCopy) {
    GameInpClearOpposites(bCopy);
    GameInpUpdateNext(bCopy);
    GameInpFixDiagonals(bCopy);
    GameInpUpdatePrev(bCopy);

  }

  if (!kNetGame || kNetSpectator) {
    CheckSystemMacros();
  }

  // Update Input dialog every 3rd frame
  static unsigned int i = 0;
  if ((i % 2) == 0) {
    InpdUpdate();
  }
  i++;

  // Update Input Set dialog
  InpsUpdate();
  return 0;
}

static time_t fpstimer;
static unsigned int nPreviousFrames;

static void DisplayFPSInit()
{
  nDoFPS = 0;
  fpstimer = 0;
  nPreviousFrames = nFramesRendered;
}

static void DisplayFPS()
{
  time_t temptime = clock();
  double fps = (double)(nFramesRendered - nPreviousFrames) * CLOCKS_PER_SEC / (temptime - fpstimer);
  if (bAppDoFast) {
    fps *= nFastSpeed + 1;
  }

  if (kNetGame) {
    QuarkUpdateStats(fps);
  }
  else {
    if (fpstimer && (temptime - fpstimer) > 0) { // avoid strange fps values
      VidSSetStats(fps, 0, 0);
      VidOverlaySetStats(fps, 0, 0);
    }
  }

  fpstimer = temptime;
  nPreviousFrames = nFramesRendered;
}

// define this function somewhere above RunMessageLoop()
void ToggleLayer(unsigned char thisLayer)
{
  nBurnLayer ^= thisLayer;				// xor with thisLayer
  VidRedraw();
  VidPaint(0);
}

int bRunaheadFrame = 0;
int gAcbGameBufferSize = 0;
char gAcbGameBuffer[16 * 1024 * 1024];
char* gAcbGameScanPointer;

int RunaheadReadAcb(struct BurnArea* pba)
{
  memcpy(gAcbGameScanPointer, pba->Data, pba->nLen);
  gAcbGameScanPointer += pba->nLen;
  return 0;
}

int RunaheadWriteAcb(struct BurnArea* pba)
{
  memcpy(pba->Data, gAcbGameScanPointer, pba->nLen);
  gAcbGameScanPointer += pba->nLen;
  return 0;
}

void RunaheadSaveState()
{
  bRunaheadFrame = 1;
  gAcbGameScanPointer = gAcbGameBuffer;
  BurnAcb = RunaheadReadAcb;
  BurnAreaScan(ACB_FULLSCANL | ACB_READ, NULL);
  gAcbGameBufferSize = gAcbGameScanPointer - gAcbGameBuffer;
  bRunaheadFrame = 0;
}

void RunaheadLoadState()
{
  bRunaheadFrame = 1;
  gAcbGameScanPointer = gAcbGameBuffer;
  BurnAcb = RunaheadWriteAcb;
  BurnAreaScan(ACB_FULLSCANL | ACB_WRITE, NULL);
  bRunaheadFrame = 0;
}

// ------------------------------------------------------------------------------------------------------------------------------------
// With or without sound, run one frame.
// If bDraw is true, it's the last frame before we are up to date, and so we should draw the screen
int RunFrame(int bDraw, int bPause, bool updateNetInputs)
{
  if (!bDrvOkay) {
    return 1;
  }

  if (bPause && !bAppDoFast) {
    GetInput(false);						// Update burner inputs, but not game inputs
  }
  else {
    nFramesEmulated++;
    nCurrentFrame++;

    if (kNetGame) {
      if (updateNetInputs) {
        GetInput(true);						// Update inputs
        VidDisplayInputs(0, 0);
        if (NetworkGetInput()) {	// Synchronize input with Network
          VidDisplayInputs(1, 1);
          DetectFreeze();
          return 1;
        }
        VidDisplayInputs(1, 2);
        DetectTurbo();
      }
      else {
        VidDisplayInputs(0, 3);
        if (NetworkGetInput()) {
          VidDisplayInputs(1, 1);
          DetectFreeze();
          return 1;
        }
        VidDisplayInputs(1, 4);
      }
    }
    else {
      if (nReplayStatus == 2) {
        GetInput(false);					// Update burner inputs, but not game inputs
        if (ReplayInput()) {		  // Read input from file
          SetPauseMode(1);        // Replay has finished
          bAppDoFast = 0;
          bAppDoFastToggled = 0;  // Disable FFWD
          MenuEnableItems();
          InputSetCooperativeLevel(false, false);
          return 0;
        }
      }
      else {
        GetInput(true);						// Update inputs
        VidDisplayInputs(0, 0);
      }
    }

    if (kNetLua) {
      CallRegisteredLuaFunctions(LUACALL_BEFOREEMULATION); //TODO: find proper place
      if (FBA_LuaUsingJoypad()) {
        FBA_LuaReadJoypad();
      }
    }

    if (nReplayStatus == 1) {
      RecordInput();					  	// Write input to file
    }

    // Render frame with video or audio
    if (bDraw) {
      if (nVidRunahead > 0 && nVidRunahead <= 3 && !kNetSpectator) {
        // Runahead frames, first frame is audio only
        pBurnDraw = NULL;
        pBurnSoundOut = nAudNextSound;
        BurnDrvFrame();
        // Save state
        RunaheadSaveState();
        // Intermediate frames don't have audio or video
        for (int i = 0; i < (nVidRunahead - 1); i++) {
          pBurnDraw = NULL;
          pBurnSoundOut = NULL;
          BurnDrvFrame();
        }
        // Last frame is video only
        pBurnSoundOut = NULL;
        VidFrame();
        // Restore state
        RunaheadLoadState();
      }
      else {
        // Render video and audio
        pBurnSoundOut = nAudNextSound;
        VidFrame();
      }
      // add audio from last frame
      AudSoundFrame();
    }
    else {
      pBurnDraw = NULL;
      pBurnSoundOut = nAudNextSound;
      BurnDrvFrame();
    }

    if (kNetGame) {
      QuarkIncrementFrame();
    }

    DetectorUpdate();

    if (kNetLua) {
      FBA_LuaFrameBoundary();
      CallRegisteredLuaFunctions(LUACALL_AFTEREMULATION); // TODO: find proper place
    }

#ifdef INCLUDE_AVI_RECORDING
    if (nAviStatus) {
      if (AviRecordFrame(bDraw)) {
        AviStop();
      }
    }
#endif
  }

  return 0;
}

// ------------------------------------------------------------------------------------------------------------------------------------
int RunIdle()
{
  if (bAudPlaying) {
    AudSoundCheck();
  }

  // Render loop with sound
  double nTime = timeGetTime();
  double nAccTime = nTime - nFrameLast;
  double nFps = 1000.0 * 100.0 / nAppVirtualFps;
  double nFpsIdle = nFps - 1.0;

  if (nAccTime < nFps || (kNetGame && nRunQuark)) {
    // No need to do anything for a bit
    if (kNetGame) {
      if (nAccTime < nFpsIdle || nRunQuark) {
        GetInput(false);
        QuarkRunIdle(1);
        nRunQuark = 0;
      }
    }
    else {
      if (nAccTime < nFpsIdle) {
        GetInput(false);
        Sleep(1);
      }
    }
    return 0;
  }
  nRunQuark = 1;

  // frame accumulator
  int nCount = -1;
  do {
    nAccTime -= nFps;
    nFrameLast += nFps;
    nCount++;
  } while (nAccTime > nFps);

  if (nCount > 10) {						// Limit frame skipping
    nCount = 10;
  }

  if (bRunPause) {
    if (bAppDoStep) {					  // Step one frame
      nCount = 0;
    }
    else {
      RunFrame(1, 1, true);			  // Paused
      VidPaint(3);
      AudBlankSound();
      return 0;
    }
  }
  bAppDoStep = 0;

  if (kNetGame && !kNetSpectator)
  {
    int skip = bAlwaysDrawFrames ? nSkipFrames : (nCount > nSkipFrames ? nCount : nSkipFrames);
    // skip frames (for rift balance or auto frame skip)
    for (int i = skip; i > 0; i--) {
      RunFrame(0, 0, true);
    }
    nSkipFrames = 0;
    RunFrame(1, 0, true);					    // End-frame
  }
  else {

    // I believe this happends when the fast-forward buttons are held down....
    if (bAppDoFast) {				    // do more frames
      for (int i = 0; i < nFastSpeed; i++) {
#ifdef INCLUDE_AVI_RECORDING
        if (nAviStatus) {
          RunFrame(1, 0, 1);
        }
        else {
          RunFrame(0, 0, 1);
        }
#else
        RunFrame(0, 0, 1);
#endif
      }
    }
    if (!bAlwaysDrawFrames) {
      for (int i = nCount; i > 0; i--) {	// Mid-frames
        RunFrame(0, 0, 1);
      }
    }
    RunFrame(1, 0, 1);					// End-frame
  }

  // render
  nFramesRendered++;
  VidPaint(3);

  // fps
  if (nDoFPS < nFramesRendered) {
    DisplayFPS();
    nDoFPS = nFramesRendered + 30;
  }

  return 0;
}

// TODO: Find a better name for this function!
int RunReset()
{
  // Reset FPS display
  DisplayFPSInit();

  // Reset the speed throttling code
  nFrameLast = timeGetTime();

  return 0;
}

int RunInit()
{
  AudSoundPlay();

  RunReset();

  return 0;
}

int RunExit()
{
  // Stop sound if it was playing
  AudSoundStop();

  nFrameLast = 0;

  bAppDoFast = 0;
  bAppDoFastToggled = 0;

  return 0;
}

// Keyboard callback function, a way to provide a driver with shifted/unshifted ASCII data from the keyboard.  see drv/msx/d_msx.cpp for usage
void (*cBurnerKeyCallback)(UINT8 code, UINT8 KeyType, UINT8 down) = NULL;

static void BurnerHandlerKeyCallback(MSG* Msg, INT32 KeyDown, INT32 KeyType)
{
  INT32 shifted = (GetAsyncKeyState(VK_SHIFT) & 0x80000000) ? 0xf0 : 0;
  INT32 scancode = (Msg->lParam >> 16) & 0xFF;
  UINT8 keyboardState[256];
  GetKeyboardState(keyboardState);
  char charvalue[2];
  if (ToAsciiEx(Msg->wParam, scancode, keyboardState, (LPWORD)&charvalue[0], 0, GetKeyboardLayout(0)) == 1) {
    cBurnerKeyCallback(charvalue[0], shifted | KeyType, KeyDown);
  }
}

// The main message loop
int RunMessageLoop()
{
  MSG Msg;

  do {
    bRestartVideo = 0;
    bDrvExit = 0;

    // Remove pending initialisation messages from the queue
    while (PeekMessage(&Msg, NULL, WM_APP + 0, WM_APP + 0, PM_NOREMOVE)) {
      if (Msg.message != WM_QUIT) {
        PeekMessage(&Msg, NULL, WM_APP + 0, WM_APP + 0, PM_REMOVE);
      }
    }

    RunInit();

    ShowWindow(hScrnWnd, nAppShowCmd);								// Show the screen window
    nAppShowCmd = SW_NORMAL;
    SetForegroundWindow(hScrnWnd);

    GameInpCheckLeftAlt();
    GameInpCheckMouse();															// Hide the cursor

    if (bVidDWMSync) {
      bprintf(0, _T("[Win7+] Sync to DWM is enabled (if available).\n"));
      SuperWaitVBlankInit();
    }

    /*
    if (bAutoLoadGameList && !nVidFullscreen) {
      static INT32 bLoaded = 0; // only do this once (at startup!)
      if (bLoaded == 0)
        PostMessage(hScrnWnd, WM_KEYDOWN, VK_F6, 0);
      bLoaded = 1;
    }
    */

    while (true) {
      if (PeekMessage(&Msg, NULL, 0, 0, PM_REMOVE)) {
        // A message is waiting to be processed
        if (Msg.message == WM_QUIT) {											// Quit program
          VidOverlayQuit();
          break;
        }
        if (Msg.message == (WM_APP + 0)) {										// Restart video
          bRestartVideo = 1;
          break;
        }

        if (bDrvExit)
        {
          DrvExit();
          bDrvExit = 0;
          continue;
        }

        if (bMediaExit)
        {
          MediaExit(true);
          break;
        }

        if (bMenuEnabled && nVidFullscreen == 0) {								// Handle keyboard messages for the menu
          if (MenuHandleKeyboard(&Msg)) {
            continue;
          }
        }

        if (Msg.message == WM_SYSKEYDOWN || Msg.message == WM_KEYDOWN) {
          if (Msg.lParam & 0x20000000) {
            // An Alt/AltGr-key was pressed
            switch (Msg.wParam) {

#if defined (FBNEO_DEBUG)
            case 'C': {
              static int count = 0;
              if (count == 0) {
                count++;
                { char* p = NULL; if (*p) { printf("crash...\n"); } }
              }
              break;
            }
#endif
            case 'W':
              if (kNetGame) {
                QuarkTogglePerfMon();
              }
              break;

              // Open (L)ua Dialog
            case 'L': {
              LuaOpenDialog();
              break;
            }
                    // (T)erminate Lua Window
            case 'T': {
              if (kNetLua) {
                PostMessage(LuaConsoleHWnd, WM_CLOSE, 0, 0);
              }
              break;
            }
                    // (E)xecute Lua Script
            case 'E': {
              if (kNetLua) {
                PostMessage(LuaConsoleHWnd, WM_COMMAND, IDC_BUTTON_LUARUN, 0);
              }
              break;
            }
                    // (P)ause Lua Scripting (This is the stop button)
            case 'P': {
              if (kNetLua) {
                PostMessage(LuaConsoleHWnd, WM_COMMAND, IDC_BUTTON_LUASTOP, 0);
              }
              break;
            }
                    // (B)rowse Lua Scripts
            case 'B': {
              if (kNetLua) {
                PostMessage(LuaConsoleHWnd, WM_COMMAND, IDC_BUTTON_LUABROWSE, 0);
              }
              break;
            }

                    // 'Silence' & 'Sound Restored' Code (added by CaptainCPS-X)
            case 'S': {
              TCHAR buffer[60];
              bMute = !bMute;

              if (bMute) {
                nOldAudVolume = nAudVolume;
                nAudVolume = 0;// mute sound
                _stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_MUTE, true), nAudVolume / 100);
              }
              else {
                nAudVolume = nOldAudVolume;// restore volume
                _stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_MUTE_OFF, true), nAudVolume / 100);
              }
              if (AudSoundSetVolume() == 0) {
                VidSNewShortMsg(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
                VidOverlayShowVolume(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
              }
              else {
                VidSNewShortMsg(buffer);
                VidOverlayShowVolume(buffer);
              }
              break;
            }

            case VK_OEM_PLUS: {
              if (bMute) break; // if mute, not do this
              nOldAudVolume = nAudVolume;
              nAudVolume += 500;
              if (nAudVolume > 10000) {
                nAudVolume = 10000;
              }

              if (AudSoundSetVolume() == 0) {
                VidSNewShortMsg(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
                VidOverlayShowVolume(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
              }
              else {
                TCHAR buffer[60];
                _stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_VOLUMESET, true), nAudVolume / 100);
                VidSNewShortMsg(buffer);
                VidOverlayShowVolume(buffer);
              }
              break;
            }
            case VK_OEM_MINUS: {
              if (bMute) break; // if mute, not do this
              nOldAudVolume = nAudVolume;
              nAudVolume -= 500;
              if (nAudVolume < 0) {
                nAudVolume = 0;
              }

              if (AudSoundSetVolume() == 0) {
                VidSNewShortMsg(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
                VidOverlayShowVolume(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
              }
              else {
                TCHAR buffer[60];
                _stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_VOLUMESET, true), nAudVolume / 100);
                VidSNewShortMsg(buffer);
                VidOverlayShowVolume(buffer);
              }
              break;
            }
            case VK_MENU: {
              continue;
            }
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
              if (kNetLua) {
                CallRegisteredLuaFunctions((LuaCallID)(LUACALL_HOTKEY_1 + Msg.wParam - '1'));
              }
              break;
            }
          }
          else {

            if (cBurnerKeyCallback)
              BurnerHandlerKeyCallback(&Msg, (Msg.message == WM_KEYDOWN) ? 1 : 0, 0);

            switch (Msg.wParam) {

#if 0	//defined (FBNEO_DEBUG)
            case 'N':
              counter--;
              if (counter < 0) {
                bprintf(PRINT_IMPORTANT, _T("*** New counter value: %04X (%d).\n"), counter, counter);
              }
              else {
                bprintf(PRINT_IMPORTANT, _T("*** New counter value: %04X.\n"), counter);
              }
              break;
            case 'M':
              counter++;
              if (counter < 0) {
                bprintf(PRINT_IMPORTANT, _T("*** New counter value: %04X (%d).\n"), counter, counter);
              }
              else {
                bprintf(PRINT_IMPORTANT, _T("*** New counter value: %04X.\n"), counter);
              }
              break;
#endif
            case VK_ESCAPE:
              if (bEditActive) {
                if (!nVidFullscreen) {
                  DeActivateChat();
                }
              }
              else {
                if (nVidFullscreen) {
                  nVidFullscreen = 0;
                  POST_INITIALISE_MESSAGE;
                }
              }
              break;

            case VK_RETURN:
              if (bEditActive) {
                if (kNetGame) {

                  bool allSpaces = true;
                  for (size_t i = 0; i < MAX_CHAT_SIZE; i++)
                  {
                    if (EditText[i] != 0x20) {
                      allSpaces = false;
                      break;
                    }
                  }
                  if (!allSpaces)
                  {
                    char text[MAX_CHAT_SIZE + 1];
                    TCHARToANSI(EditText, text, MAX_CHAT_SIZE + 1);
                    QuarkSendChatText(text);
                  }

                }
                DeActivateChat();
                break;
              }

              if (GetAsyncKeyState(VK_CONTROL) & 0x80000000) {
                bMenuEnabled = !bMenuEnabled;
                POST_INITIALISE_MESSAGE;
                break;
              }

              break;

            case VK_F1:
              if (!kNetGame) {
                if (Msg.lParam & 0x20000000) {
                  bool bOldAppDoFast = bAppDoFast;

                  if (((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT)) & 0x80000000) == 0) {
                    if (bRunPause) {
                      bAppDoStep = 1;
                    }
                    else {
                      bAppDoFast = 1;
                    }
                  }

                  if ((GetAsyncKeyState(VK_SHIFT) & 0x80000000) && !GetAsyncKeyState(VK_CONTROL)) { // Shift-F1: toggles FFWD state
                    bAppDoFast = !bAppDoFast;
                    bAppDoFastToggled = bAppDoFast;
                  }

                  if (bOldAppDoFast != bAppDoFast) {
                    DisplayFPSInit(); // resync fps display
                  }
                }
              }
              break;

            case VK_BACK:
              if ((GetAsyncKeyState(VK_SHIFT) & 0x80000000) && !GetAsyncKeyState(VK_CONTROL))
              { // Shift-Backspace: toggles recording/replay frame counter
                bReplayFrameCounterDisplay = !bReplayFrameCounterDisplay;
                if (!bReplayFrameCounterDisplay) {
                  VidSKillTinyMsg();
                }
              }
              else if (!bEditActive) { // Backspace: toggles FPS
                bShowFPS = (bShowFPS + 1) % (kNetGame ? 4 : 2);
                VidOverlaySetWarning(-5000, 1);
                VidOverlaySetWarning(-5000, 2);
                VidOverlaySetWarning(-5000, 3);
                DisplayFPS();
                MenuUpdate();
              }
              break;

            case 'T':
              if (kNetGame && !bEditActive) {
                if (AppMessage(&Msg)) {
                  ActivateChat();
                }
              }
              break;

            case VK_TAB:
              if (GetAsyncKeyState(VK_SHIFT) & 0x80000000) {
                nVidRunahead = (nVidRunahead + 1) % 3;
                MenuUpdate();
              }
              else if (kNetGame && !bEditActive) {
                if (!bVidOverlay) {
                  bVidOverlay = !bVidOverlay;
                  bVidBigOverlay = false;
                }
                else if (!bVidBigOverlay) {
                  bVidBigOverlay = true;
                }
                else {
                  bVidOverlay = false;
                  bVidBigOverlay = false;
                }
                MenuUpdate();
              }
              break;
              // 'Silence' & 'Sound Restored' Code (added by CaptainCPS-X)
            case VK_MULTIPLY: if (bKeypadVolume) {
              TCHAR buffer[60];
              bMute = !bMute;

              if (bMute) {
                nOldAudVolume = nAudVolume;
                nAudVolume = 0;// mute sound
                _stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_MUTE, true), nAudVolume / 100);
              }
              else {
                nAudVolume = nOldAudVolume;// restore volume
                _stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_MUTE_OFF, true), nAudVolume / 100);
              }
              if (AudSoundSetVolume() == 0) {
                VidSNewShortMsg(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
                VidOverlayShowVolume(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
              }
              else {
                VidSNewShortMsg(buffer);
                VidOverlayShowVolume(buffer);
              }
              break;
            }
            case VK_ADD: if (bKeypadVolume) {
              if (bMute) break; // if mute, not do this
              nOldAudVolume = nAudVolume;
              nAudVolume += 500;
              if (nAudVolume > 10000) {
                nAudVolume = 10000;
              }

              if (AudSoundSetVolume() == 0) {
                VidSNewShortMsg(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
                VidOverlayShowVolume(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
              }
              else {
                TCHAR buffer[60];
                _stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_VOLUMESET, true), nAudVolume / 100);
                VidSNewShortMsg(buffer);
                VidOverlayShowVolume(buffer);
              }
              break;
            }
            case VK_SUBTRACT: if (bKeypadVolume) {
              if (bMute) break; // if mute, not do this
              nOldAudVolume = nAudVolume;
              nAudVolume -= 500;
              if (nAudVolume < 0) {
                nAudVolume = 0;
              }

              if (AudSoundSetVolume() == 0) {
                VidSNewShortMsg(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
                VidOverlayShowVolume(FBALoadStringEx(hAppInst, IDS_SOUND_NOVOLUME, true));
              }
              else {
                TCHAR buffer[60];
                _stprintf(buffer, FBALoadStringEx(hAppInst, IDS_SOUND_VOLUMESET, true), nAudVolume / 100);
                VidSNewShortMsg(buffer);
                VidOverlayShowVolume(buffer);
              }
              break;
            }
            }
          }
        }
        else {
          if (Msg.message == WM_SYSKEYUP || Msg.message == WM_KEYUP) {

            if (cBurnerKeyCallback)
              BurnerHandlerKeyCallback(&Msg, (Msg.message == WM_KEYDOWN) ? 1 : 0, 0);

            switch (Msg.wParam) {
            case VK_MENU:
              continue;
            case VK_F1:
              if (!kNetGame) {
                bool bOldAppDoFast = bAppDoFast;

                if (!bAppDoFastToggled)
                  bAppDoFast = 0;
                bAppDoFastToggled = 0;
                if (bOldAppDoFast != bAppDoFast) {
                  DisplayFPSInit(); // resync fps display
                }
              }
              break;
            }
          }
        }

        // Check for messages for dialogs etc.
        if (AppMessage(&Msg)) {
          if (TranslateAccelerator(hScrnWnd, hAccel, &Msg) == 0) {
            if (bEditActive) {
              TranslateMessage(&Msg);
            }
            DispatchMessage(&Msg);
          }
        }
      }
      else {
        // No messages are waiting
        SplashDestroy(0);
        RunIdle();
      }
    }

    RunExit();
    MediaExit(true);
    if (bRestartVideo) {
      MediaInit();
      PausedRedraw();
    }
  } while (bRestartVideo);

  return 0;
}
