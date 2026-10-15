// Input.h - the keys that start an action, and the ones that have to be held
// back while one is playing.
//
// Two halves. An event sink watches the stream of button events for the parkour
// key and the sneak key. And the player's own input handlers are hooked at
// CanProcess, so that during an action the game simply never sees a jump, a
// sneak, a weapon draw or a camera turn - which is what keeps an animation from
// being cut in half by a key press.

#pragma once

class Actor;

namespace Input
{
	bool InstallHooks();		// the handler vtables
	void Register();			// the event sink
	void Unregister();
}
