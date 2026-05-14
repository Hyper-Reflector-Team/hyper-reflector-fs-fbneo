/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "timesync.h"

 // ----------------------------------------------------------------------------------------------------
TimeSync::TimeSync()
{
  memset(_local, 0, sizeof(_local));
  memset(_remote, 0, sizeof(_remote));
  _next_prediction = FRAME_WINDOW_SIZE * 3;
}

// ----------------------------------------------------------------------------------------------------
void TimeSync::rollback_frame(GameInput& input, int localAdvantage, int remoteAdvantage)
{
  // Remember the last frame and frame advantage
  _last_inputs[input.frame % ARRAY_SIZE(_last_inputs)] = input;
  _local[input.frame % ARRAY_SIZE(_local)] = localAdvantage;
  _remote[input.frame % ARRAY_SIZE(_remote)] = remoteAdvantage;
}


float g_timesync_advantage = 0;
float g_timesync_radvantage = 0;

// ----------------------------------------------------------------------------------------------------
int TimeSync::recommend_frame_wait_duration(bool require_idle_input)
{
  // Average our local and remote frame advantages
  int i, sum = 0;
  float advantage, radvantage;
  for (i = 0; i < ARRAY_SIZE(_local); i++) {
    sum += _local[i];
  }
  advantage = sum / (float)ARRAY_SIZE(_local);

  sum = 0;
  for (i = 0; i < ARRAY_SIZE(_remote); i++) {
    sum += _remote[i];
  }
  radvantage = sum / (float)ARRAY_SIZE(_remote);

  static int count = 0;
  count++;

  // See if someone should take action.  The person furthest ahead
  // needs to slow down so the other user can catch up.
  // Only do this if both clients agree on who's ahead!!
  g_timesync_advantage = advantage;
  g_timesync_radvantage = radvantage;
  Utils::LogIt(CATEGORY_TIMESYNC, "timesync check: advantage=%.2f radvantage=%.2f diff=%.2f", advantage, radvantage, radvantage - advantage);
  if (advantage >= radvantage) {
    return 0;
  }

  // Both clients agree that we're the one ahead.  Split
  // the difference between the two to figure out how long to
  // sleep for.
  int sleep_frames = (int)(((radvantage - advantage) / 2) + 0.5);

  Utils::LogIt(CATEGORY_TIMESYNC, "iteration %d:  sleep frames is %d", count, sleep_frames);

  // Some things just aren't worth correcting for.  Make sure
  // the difference is relevant before proceeding.
  if (sleep_frames < MIN_FRAME_ADVANTAGE) {
    return 0;
  }

  // Make sure our input had been "idle enough" before recommending
  // a sleep.  This tries to make the emulator sleep while the
  // user's input isn't sweeping in arcs (e.g. fireball motions in
  // Street Fighter), which could cause the player to miss moves.
  if (require_idle_input) {
    // Idle window before timesync is allowed to fire. Tune between 8 (~133ms) and 16 (~267ms).
    // Lower = fires sooner after input stops. Higher = more conservative, less correction.
    const int IDLE_WINDOW = 8;
    for (i = 1; i < IDLE_WINDOW; i++) {
      // Compare bits only — equal() also checks frame number which always differs, so it always rejects.
      if (memcmp(_last_inputs[i].bits, _last_inputs[0].bits, _last_inputs[0].size) != 0) {
        Utils::LogIt(CATEGORY_TIMESYNC, "iteration %d:  rejecting due to input stuff at position %d...!!!", count, i);
        return 0;
      }
    }
  }

  // Success!!! Recommend the number of frames to sleep and adjust
  return MIN(sleep_frames, MAX_FRAME_ADVANTAGE);
}
