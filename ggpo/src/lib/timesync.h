/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _TIMESYNC_H
#define _TIMESYNC_H

#include "types.h"
#include "game_input.h"

#define FRAME_WINDOW_SIZE           40
#define MIN_UNIQUE_FRAMES           10
#define MIN_FRAME_ADVANTAGE          3
#define MAX_FRAME_ADVANTAGE          9

// If a correction has been warranted but blocked by the idle-input gate for this many
// consecutive checked frames (~3s at 60fps), apply it anyway. Otherwise a player who's
// rarely fully idle (aggressive/active playstyle) while running ahead can block the
// correction indefinitely, leaving their opponent to absorb rollback for the whole match.
// "Consecutive checked frames" is really consecutive real frames here, not once-every-few-
// seconds check cycles: Peer2PeerBackend::Poll2Players/PollNPlayers (ggpo/src/lib/backends/
// p2p.cpp) only advances `_next_recommended_sleep` (and so only skips calling
// RecommendFrameDelay -> recommend_frame_wait_duration) when a correction actually fires
// (`if (interval > 0) { ... _next_recommended_sleep = current_frame + RECOMMENDATION_INTERVAL;
// }`); while a correction is pending-but-blocked, that guard never advances, so this function
// runs on every single confirmed frame, making 180 a real ~3s wall-clock bound, not a guess.
#define IDLE_GATE_ESCALATION_FRAMES 180

class TimeSync {
public:
   TimeSync();

   void rollback_frame(GameInput &input, int localAdvantage, int remoteAdvantage);
   int recommend_frame_wait_duration(bool require_idle_input);

protected:
   int         _local[FRAME_WINDOW_SIZE];
   int         _remote[FRAME_WINDOW_SIZE];
   GameInput   _last_inputs[MIN_UNIQUE_FRAMES];
   int         _next_prediction;
   int         _idle_gate_blocked_frames = 0;
};

#endif
