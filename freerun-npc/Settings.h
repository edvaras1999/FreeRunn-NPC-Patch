// Settings.h - everything the player can change, and where it is kept.
//
// The Special Edition build puts its settings behind an ImGui overlay from the
// SKSE Menu Framework, which does not exist on this edition. The settings live
// in an INI here, and the in-game menu is a SkyUI mod configuration menu that
// talks to the plugin through Papyrus - the established way on Legendary.

#pragma once

namespace Settings
{
	extern bool		enabled;				// parkour on at all
	extern bool		showIndicators;
	extern float	playbackSpeed;

	extern bool		usePresetKey;
	extern SInt32	presetKey;				// 0 jump, 1 sprint, 2 activate
	extern UInt32	customKey;				// DirectInput scan code
	extern float	inputDelay;				// hold time before a parkour attempt
	extern SInt32	autoParkour;			// 0 off, 1 out of combat, 2 always

	extern bool		staminaEnabled;
	extern bool		mustHaveStamina;
	extern float	staminaDamage;

	extern bool		smartSteps;
	extern bool		smartVault;
	extern bool		smartClimb;

	extern bool		slideEnabled;
	extern bool		advancedSlideSneak;
	extern bool		slideTackle;
	extern bool		landRolling;

	extern bool		debug;

	enum { kPresetKey_Jump = 0, kPresetKey_Sprint, kPresetKey_Activate };
	enum { kAuto_Disabled = 0, kAuto_OutOfCombat, kAuto_Always };

	void Load();
	void Save();

	// Called after a value changes so anything that caches it can catch up.
	void Apply();
}
