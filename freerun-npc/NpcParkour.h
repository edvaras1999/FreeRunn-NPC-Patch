// NpcParkour.h - EXPERIMENTAL, steps 1 and 2a of the NPC work.
//
// Lets ONE NPC at a time run FreeRun's own parkour moves.
//
// Step 1, by hand: a key press (F10 by default) picks the NPC the player is
// facing, turns it to the player's heading, looks for a ledge in front of it
// with the same search the player uses, and plays the matching move.
//
// Step 2a, tracking: a second key (F11) makes the NPC the player is facing the
// "tracked" one. Its state is logged once a second. If automatic moves are on,
// a tracked NPC that stands still far from the player, roughly facing the
// player, with a ledge in front of it, starts the move by itself.
//
// There are no new hooks. The mod already ticks once per animation update of
// the player (see Graph.cpp); this rides on that tick, and costs a handful of
// comparisons when nothing is requested and nothing is tracked.
//
// Licence: AGPL-3.0, like the rest of the project (see LICENSE).

#pragma once

#include "skse/GameTypes.h"
#include "skse/NiTypes.h"

class Actor;

namespace NpcParkour
{
    void    LoadSettings();             // the [Npc] section of Freerun.ini
    bool    Enabled();
    UInt32  TestKey();                  // DirectInput scan code: the manual test
    UInt32  TrackKey();                 // DirectInput scan code: track / stop tracking

    void    RequestTest();              // the test key was pressed
    void    RequestTrack();             // the track key was pressed
    void    OnTick(Actor * player);     // once per animation update of the player

    // Called by Parkour.cpp when FreeRun starts and ends one of the PLAYER's moves,
    // so that a follower can repeat it later (the player's trail, step 3).
    void    RecordPlayerMove(int type, const NiPoint3 & startMark, const NiPoint3 & ledge, const NiPoint3 & facing);
    void    RecordPlayerMoveEnd(Actor * player);
    void    RecordPlayerMoveSample(const NiPoint3 & position);     // every tick of one of his moves
    void    Reset();                    // graph rebuilt: forget the running move (the tracked NPC is kept)
    void    Forget();                   // a load or a new game: forget the running move AND the tracked NPC
}
