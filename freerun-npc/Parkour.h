// Parkour.h - finding a ledge, deciding what it is, and starting the action.
//
// Every frame the player's animation graph ticks, the ledge in front of them is
// looked for again: a forward probe from head height, a downward probe where it
// clears, then a series of tests that ask whether a body could actually go
// there - headroom above the ledge, a gap behind it, a clear line from the
// player to it, and the state the player is in.
//
// The result is one of the ParkourType values below. It is written into the
// behaviour's own graph variable, the behaviour picks the matching animation,
// and the animation's root motion does the moving. The plugin only nudges the
// player onto the starting mark the animation expects.

#pragma once

#include "skse/NiTypes.h"
#include "skse/GameTypes.h"

class Actor;
class TESObjectREFR;

namespace Parkour
{
	enum Type
	{
		kNoLedge	= -1,
		kFailed		= 0,		// out of stamina: the "I cannot" animation
		kGrab,					// catch a ledge in mid-air
		kVault,					// over something waist high
		kStepLow,
		kStepHigh,
		kLow,
		kMedium,
		kHigh,
		kHighest,
	};

	// Heights, in game units, at the player's own scale. These are the numbers
	// the animations were authored against: each one ends at a known height, so
	// the starting mark is worked back from it.
	namespace Limits
	{
		const float kClimbMax			= 250.0f;
		const float kClimbMin			= 30.0f;

		const float kVaultMax			= 120.0f;
		const float kVaultMin			= 42.0f;

		const float kGrabMax			= 125.0f;
		const float kGrabHighVariant	= 75.0f;

		const float kHighestLedge		= 220.0f;
		const float kHighLedge			= 170.0f;
		const float kMediumLedge		= 130.0f;
		const float kLowLedge			= 80.0f;
		const float kHighStep			= 60.0f;

		const float kHighestElevation	= 250.0f;
		const float kHighElevation		= 200.0f;
		const float kMediumElevation	= 153.0f;
		const float kLowElevation		= 110.0f;
		const float kStepHighElevation	= 70.0f;
		const float kStepLowElevation	= 50.0f;
		const float kVaultElevation		= 60.0f;
		const float kGrabElevation		= 55.0f;
		const float kGrabHighElevation	= 100.0f;
	}

	// ---- state -------------------------------------------------------------

	bool		InProgress();
	bool		RecoveryFrames();
	Type		SelectedLedge();
	NiPoint3	LedgePoint();
	float		PlayerScale();

	void SetMenuOpen(bool open);
	bool MenuOpen();

	void Reset();					// forget everything; used on load and on menu changes
	void InvalidateLedge();			// no ledge right now

	// ---- the loop ----------------------------------------------------------

	void OnGraphTick(Actor * player);							// once per animation update
	void OnGraphReady(Actor * player);							// the graph manager was (re)built
	void OnAnimationEvent(Actor * player, const BSFixedString & tag, const BSFixedString & payload);

	bool TryActivate();											// a key was pressed, or auto-parkour fired
	void PostStaminaCost(Actor * player, bool lowEffort, bool swimming);
	void OnStartStop(bool stop, Actor * player);

	// ---- shared checks used by the slide as well ---------------------------

	bool IsKnockedOut(Actor * actor);
	bool IsSitting(Actor * actor);
	bool IsInDrawSheath(Actor * actor);
	bool IsAttacking(Actor * actor);
	bool IsAnimationDriven(Actor * actor);
	bool IsStaggering(Actor * actor);
	bool IsInSyncedAnimation(Actor * actor);
	bool IsBeastForm(Actor * actor);
	bool IsWeaponOut(Actor * actor);
	bool IsSwimming(Actor * actor);
	bool IsSneaking(Actor * actor);
	bool IsSprinting(Actor * actor);
	bool IsMoving(Actor * actor);
	bool ControlsEnabled();

	bool  HasEnoughStamina(Actor * actor);
	float StaminaCost(Actor * actor);
	void  DamageStamina(Actor * actor, float amount);

	NiPoint3 FacingFlat(Actor * actor);
	float    ForwardSpeed(Actor * actor);
	float    RelativeSpeed(Actor * actor);

	// ---- NPC TEST BUILD (experimental) -----------------------------------
	//
	// Looks for a ledge in front of `npc`, using that actor's own body and
	// heading, and works out the mark its animation starts from. Nothing of the
	// player's state is changed: the working values are borrowed and handed
	// back. Returns false when no usable ledge was found.
	//
	// `facing`, when given, replaces the actor's own facing for the search: a
	// flat unit vector, so that the search can look toward a place the actor is
	// not turned to. The actor itself is not turned.
	bool ProbeForActor(Actor * npc, Type & outType, NiPoint3 & outLedge,
					   NiPoint3 & outStart, float & outScale, const NiPoint3 * facing = NULL);
}
