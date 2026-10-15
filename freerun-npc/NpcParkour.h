// NpcParkour.h - EXPERIMENTAL, step 1 of the NPC work.
//
// Lets ONE NPC at a time run FreeRun's own parkour moves, so that the one
// question everything else depends on can be answered in the game:
//
//     does FreeRun's behaviour patch react when an NPC's animation graph is
//     driven with the same variables and events the player's is?
//
// Nothing here is automatic. A key press (F10 by default) picks the NPC the
// player is facing, turns it to the player's heading, looks for a ledge in front
// of it with the same search the player uses, and plays the matching move. All
// of it is written to the log, step by step.
//
// There are no new hooks. The mod already ticks once per animation update of the
// player (see Graph.cpp); this rides on that tick, and costs one comparison
// when no test is running.
//
// Licence: AGPL-3.0, like the rest of the project (see LICENSE).

#pragma once

#include "skse/GameTypes.h"

class Actor;

namespace NpcParkour
{
	void    LoadSettings();                // the [Npc] section of Freerun.ini
	bool    Enabled();
	UInt32    TestKey();                    // DirectInput scan code

	void    RequestTest();                // the test key was pressed
	void    OnTick(Actor * player);        // once per animation update of the player
	void    Reset();                    // load, new game, graph rebuilt: forget everything
}
