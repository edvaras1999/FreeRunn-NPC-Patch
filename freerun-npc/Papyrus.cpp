#include "Papyrus.h"
#include "Settings.h"
#include "Indicator.h"
#include "Log.h"

#include "skse/PapyrusNativeFunctions.h"
#include "skse/PapyrusVM.h"
#include "skse/GameTypes.h"

namespace
{
	const char * const kScriptName = "FreerunNative";

	// --- getters ---

	bool	GetEnabled(StaticFunctionTag *)				{ return Settings::enabled; }
	bool	GetShowIndicators(StaticFunctionTag *)		{ return Settings::showIndicators; }
	float	GetPlaybackSpeed(StaticFunctionTag *)		{ return Settings::playbackSpeed; }

	bool	GetUsePresetKey(StaticFunctionTag *)		{ return Settings::usePresetKey; }
	SInt32	GetPresetKey(StaticFunctionTag *)			{ return Settings::presetKey; }
	SInt32	GetCustomKey(StaticFunctionTag *)			{ return (SInt32)Settings::customKey; }
	float	GetInputDelay(StaticFunctionTag *)			{ return Settings::inputDelay; }
	SInt32	GetAutoParkour(StaticFunctionTag *)			{ return Settings::autoParkour; }

	bool	GetStaminaEnabled(StaticFunctionTag *)		{ return Settings::staminaEnabled; }
	bool	GetMustHaveStamina(StaticFunctionTag *)		{ return Settings::mustHaveStamina; }
	float	GetStaminaCost(StaticFunctionTag *)			{ return Settings::staminaDamage; }

	bool	GetSmartSteps(StaticFunctionTag *)			{ return Settings::smartSteps; }
	bool	GetSmartVault(StaticFunctionTag *)			{ return Settings::smartVault; }
	bool	GetSmartClimb(StaticFunctionTag *)			{ return Settings::smartClimb; }

	bool	GetSlideEnabled(StaticFunctionTag *)		{ return Settings::slideEnabled; }
	bool	GetAdvancedSlide(StaticFunctionTag *)		{ return Settings::advancedSlideSneak; }
	bool	GetLandRolling(StaticFunctionTag *)			{ return Settings::landRolling; }

	bool	GetDebug(StaticFunctionTag *)				{ return Settings::debug; }

	// --- setters: each one saves, so a menu that is closed with the game is
	//     still remembered ---

	void SetEnabled(StaticFunctionTag *, bool value)			{ Settings::enabled = value;			Settings::Save(); Settings::Apply(); }
	void SetShowIndicators(StaticFunctionTag *, bool value)		{ Settings::showIndicators = value;		Settings::Save(); if(!value) Indicator::SetType(Indicator::kInvisible); }
	void SetPlaybackSpeed(StaticFunctionTag *, float value)		{ Settings::playbackSpeed = value;		Settings::Save(); Settings::Apply(); }

	void SetUsePresetKey(StaticFunctionTag *, bool value)		{ Settings::usePresetKey = value;		Settings::Save(); }
	void SetPresetKey(StaticFunctionTag *, SInt32 value)		{ Settings::presetKey = value;			Settings::Save(); }
	void SetCustomKey(StaticFunctionTag *, SInt32 value)		{ Settings::customKey = (UInt32)value;	Settings::Save(); }
	void SetInputDelay(StaticFunctionTag *, float value)		{ Settings::inputDelay = value;			Settings::Save(); }
	void SetAutoParkour(StaticFunctionTag *, SInt32 value)		{ Settings::autoParkour = value;		Settings::Save(); }

	void SetStaminaEnabled(StaticFunctionTag *, bool value)		{ Settings::staminaEnabled = value;		Settings::Save(); }
	void SetMustHaveStamina(StaticFunctionTag *, bool value)	{ Settings::mustHaveStamina = value;	Settings::Save(); }
	void SetStaminaCost(StaticFunctionTag *, float value)		{ Settings::staminaDamage = value;		Settings::Save(); }

	void SetSmartSteps(StaticFunctionTag *, bool value)			{ Settings::smartSteps = value;			Settings::Save(); }
	void SetSmartVault(StaticFunctionTag *, bool value)			{ Settings::smartVault = value;			Settings::Save(); }
	void SetSmartClimb(StaticFunctionTag *, bool value)			{ Settings::smartClimb = value;			Settings::Save(); }

	void SetSlideEnabled(StaticFunctionTag *, bool value)		{ Settings::slideEnabled = value;		Settings::Save(); }
	void SetAdvancedSlide(StaticFunctionTag *, bool value)		{ Settings::advancedSlideSneak = value;	Settings::Save(); }
	void SetLandRolling(StaticFunctionTag *, bool value)		{ Settings::landRolling = value;		Settings::Save(); }

	void SetDebug(StaticFunctionTag *, bool value)
	{
		Settings::debug = value;
		Settings::Save();
		Indicator::ShowDebugOverlay(value);
	}

	// The menu asks for this so it can say "settings reloaded" after someone
	// edits the INI by hand.
	void ReloadSettings(StaticFunctionTag *)
	{
		Settings::Load();
		Settings::Apply();
	}
}

namespace Papyrus
{
	bool Register(VMClassRegistry * registry)
	{
		if(!registry) return false;

		#define GET(name, type)  registry->RegisterFunction(new NativeFunction0<StaticFunctionTag, type>(#name, kScriptName, name, registry))
		#define SET(name, type)  registry->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, type>(#name, kScriptName, name, registry))

		GET(GetEnabled, bool);			SET(SetEnabled, bool);
		GET(GetShowIndicators, bool);	SET(SetShowIndicators, bool);
		GET(GetPlaybackSpeed, float);	SET(SetPlaybackSpeed, float);

		GET(GetUsePresetKey, bool);		SET(SetUsePresetKey, bool);
		GET(GetPresetKey, SInt32);		SET(SetPresetKey, SInt32);
		GET(GetCustomKey, SInt32);		SET(SetCustomKey, SInt32);
		GET(GetInputDelay, float);		SET(SetInputDelay, float);
		GET(GetAutoParkour, SInt32);	SET(SetAutoParkour, SInt32);

		GET(GetStaminaEnabled, bool);	SET(SetStaminaEnabled, bool);
		GET(GetMustHaveStamina, bool);	SET(SetMustHaveStamina, bool);
		GET(GetStaminaCost, float);		SET(SetStaminaCost, float);

		GET(GetSmartSteps, bool);		SET(SetSmartSteps, bool);
		GET(GetSmartVault, bool);		SET(SetSmartVault, bool);
		GET(GetSmartClimb, bool);		SET(SetSmartClimb, bool);

		GET(GetSlideEnabled, bool);		SET(SetSlideEnabled, bool);
		GET(GetAdvancedSlide, bool);	SET(SetAdvancedSlide, bool);
		GET(GetLandRolling, bool);		SET(SetLandRolling, bool);

		GET(GetDebug, bool);			SET(SetDebug, bool);

		registry->RegisterFunction(new NativeFunction0<StaticFunctionTag, void>("ReloadSettings", kScriptName, ReloadSettings, registry));

		#undef GET
		#undef SET

		_MESSAGE("Papyrus interface registered as %s", kScriptName);
		return true;
	}
}
