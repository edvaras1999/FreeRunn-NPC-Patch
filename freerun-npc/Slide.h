// Slide.h - the crouch slide and the landing roll.
//
// Both ride on the same behaviour patch and the same sneak key: tap sneak while
// sprinting and the player slides; tap it while falling and they roll out of the
// landing, which also takes the edge off the fall damage.

#pragma once

#include "skse/GameTypes.h"

class Actor;

namespace Slide
{
	bool Ongoing();
	bool RecoveryFrames();

	void Reset();
	void ClearOngoing();

	// True while the behaviour itself says a slide is playing - asked of the
	// graph rather than of our own state, so a slide started by anything else
	// counts too.
	bool IsSliding(Actor * actor);

	// The sneak key was pressed or held.
	void OnSneakPressed(Actor * player, bool down, bool held, float heldDuration);

	void OnStartStop(bool stop, Actor * player, bool isRoll);
	void OnAnimationEvent(Actor * player, const BSFixedString & tag, const BSFixedString & payload);
	void OnGraphTick(Actor * player);

	// Sets the sneak state the engine can see, for the moment a slide turns
	// into a crouch.
	void ForceSneak(Actor * player);
}
