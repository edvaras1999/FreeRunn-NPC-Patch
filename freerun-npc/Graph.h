// Graph.h - the animation graph side of the mod.
//
// Three things happen here.
//
//  * The behaviour patch is driven through graph variables and animation
//    events, the same protocol the patched 0_master.hkx speaks.
//
//  * A channel is bound into the player's graph manager. The engine polls
//    every bound channel once per animation update and copies its value into
//    the graph variable of the same name, which is how the playback speed
//    reaches the behaviour - and, more importantly for us, it is a heartbeat
//    the whole mod can tick from. Special Edition's channel poll takes a bool;
//    this edition's takes nothing at all.
//
//  * The animation events coming back out of the graph are read. Special
//    Edition hooks the graph manager's own sink, which sees every actor in the
//    game; on this edition the manager is not a sink at all - the holder is
//    (SetupAnimEventSinks at 0x006C73C0 registers the reference's sink and one
//    global watcher), so the player's own sink is hooked instead. Cheaper, and
//    it cannot be confused by another actor's events.

#pragma once

#include "skse/GameTypes.h"

class Actor;
class TESObjectREFR;

namespace Graph
{
	// The names the behaviour patch uses. They are built once, after the game's
	// string pool exists.
	struct Names
	{
		// events we send
		BSFixedString	notify;			// start a parkour action
		BSFixedString	stop;
		BSFixedString	interrupt;
		BSFixedString	notifySlide;
		BSFixedString	slideStop;
		BSFixedString	slideSneak;
		BSFixedString	slideSneakAdvanced;
		BSFixedString	sprintStop;
		BSFixedString	sprintStart;
		BSFixedString	swimStart;
		BSFixedString	interruptCast;

		// events we listen for
		BSFixedString	start;
		BSFixedString	recovery;
		BSFixedString	staminaHit;
		BSFixedString	slideStart;
		BSFixedString	getUpExit;
		BSFixedString	ragdoll;
		BSFixedString	sneakStart;

		// payloads
		BSFixedString	lowEffort;
		BSFixedString	slidePayload;
		BSFixedString	rollPayload;

		// variables
		BSFixedString	ledge;
		BSFixedString	stepLeg;
		BSFixedString	grabVariant;
		BSFixedString	ongoing;
		BSFixedString	speedMult;
		BSFixedString	lowerBody;
		BSFixedString	sliding;
		BSFixedString	isRoll;
		BSFixedString	installedThird;
		BSFixedString	installedFirst;

		// engine variables we read
		BSFixedString	isFirstPerson;
		BSFixedString	isBeastRace;
		BSFixedString	animationDriven;
		BSFixedString	isEquipping;
		BSFixedString	isUnequipping;
		BSFixedString	isSynced;
		BSFixedString	isInSneak;
	};

	// The graph variable the bound channel feeds. It is part of the protocol
	// the patched behaviour speaks, so it keeps the patch's own spelling.
	extern const char * const kSpeedMultVariable;

	const Names & GetNames();

	void Init();				// build the strings, once the game is loaded
	bool InstallHooks();		// vtable hooks: channels, events, notify

	// Binds the per-frame channel to the player if it is not bound already.
	// Called from the graph-manager hook, and again after a race change.
	void BindChannel(Actor * actor);

	// The playback speed the channel feeds to the behaviour.
	void SetPlaybackSpeed(float value);

	// True when the behaviour patch for the point of view the player is in is
	// actually installed; warns once per point of view if it is not.
	bool BehaviourPatchPresent(Actor * player);

	// Convenience wrappers so the rest of the mod does not repeat the holder
	// dance for every variable.
	bool GetBool(TESObjectREFR * ref, const BSFixedString & name, bool defaultValue = false);
	void SetBool(TESObjectREFR * ref, const BSFixedString & name, bool value);
	void SetInt(TESObjectREFR * ref, const BSFixedString & name, SInt32 value);
	void SetFloat(TESObjectREFR * ref, const BSFixedString & name, float value);
	SInt32 GetInt(TESObjectREFR * ref, const BSFixedString & name, SInt32 defaultValue = 0);
	bool Notify(TESObjectREFR * ref, const BSFixedString & event);
}
