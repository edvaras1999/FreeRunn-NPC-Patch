#include "Settings.h"
#include "Graph.h"
#include "Log.h"

#include <windows.h>
#include <stdio.h>

namespace Settings
{
	bool	enabled				= true;
	bool	showIndicators		= true;
	float	playbackSpeed		= 0.95f;

	bool	usePresetKey		= true;
	SInt32	presetKey			= kPresetKey_Jump;
	UInt32	customKey			= 0;
	float	inputDelay			= 0.0f;
	SInt32	autoParkour			= kAuto_OutOfCombat;

	bool	staminaEnabled		= true;
	bool	mustHaveStamina		= true;
	float	staminaDamage		= 20.0f;

	bool	smartSteps			= true;
	bool	smartVault			= true;
	bool	smartClimb			= true;

	bool	slideEnabled		= true;
	bool	advancedSlideSneak	= true;
	bool	slideTackle			= false;
	bool	landRolling			= true;

	bool	debug				= false;
}

namespace
{
	const char * const kPath = ".\\Data\\SKSE\\Plugins\\Freerun.ini";

	bool ReadBool(const char * section, const char * key, bool fallback)
	{
		return GetPrivateProfileIntA(section, key, fallback ? 1 : 0, kPath) != 0;
	}

	SInt32 ReadInt(const char * section, const char * key, SInt32 fallback)
	{
		return (SInt32)GetPrivateProfileIntA(section, key, fallback, kPath);
	}

	float ReadFloat(const char * section, const char * key, float fallback)
	{
		char buffer[64];
		char def[64];
		sprintf_s(def, sizeof(def), "%.4f", fallback);
		GetPrivateProfileStringA(section, key, def, buffer, sizeof(buffer), kPath);
		return (float)atof(buffer);
	}

	void WriteBool(const char * section, const char * key, bool value)
	{
		WritePrivateProfileStringA(section, key, value ? "1" : "0", kPath);
	}

	void WriteInt(const char * section, const char * key, SInt32 value)
	{
		char buffer[32];
		sprintf_s(buffer, sizeof(buffer), "%d", value);
		WritePrivateProfileStringA(section, key, buffer, kPath);
	}

	void WriteFloat(const char * section, const char * key, float value)
	{
		char buffer[32];
		sprintf_s(buffer, sizeof(buffer), "%.4f", value);
		WritePrivateProfileStringA(section, key, buffer, kPath);
	}
}

namespace Settings
{
	void Load()
	{
		enabled				= ReadBool ("Main",		"bEnabled",				enabled);
		showIndicators		= ReadBool ("Main",		"bShowIndicators",		showIndicators);
		playbackSpeed		= ReadFloat("Main",		"fPlaybackSpeed",		playbackSpeed);

		usePresetKey		= ReadBool ("Input",	"bUsePresetKey",		usePresetKey);
		presetKey			= ReadInt  ("Input",	"iPresetKey",			presetKey);
		customKey			= (UInt32)ReadInt("Input", "iCustomKey",		(SInt32)customKey);
		inputDelay			= ReadFloat("Input",	"fInputDelay",			inputDelay);
		autoParkour			= ReadInt  ("Input",	"iAutoParkour",			autoParkour);

		staminaEnabled		= ReadBool ("Stamina",	"bEnabled",				staminaEnabled);
		mustHaveStamina		= ReadBool ("Stamina",	"bRequired",			mustHaveStamina);
		staminaDamage		= ReadFloat("Stamina",	"fBaseCost",			staminaDamage);

		smartSteps			= ReadBool ("Smart",	"bSmartSteps",			smartSteps);
		smartVault			= ReadBool ("Smart",	"bSmartVault",			smartVault);
		smartClimb			= ReadBool ("Smart",	"bSmartClimb",			smartClimb);

		slideEnabled		= ReadBool ("Slide",	"bEnabled",				slideEnabled);
		advancedSlideSneak	= ReadBool ("Slide",	"bAdvancedSneak",		advancedSlideSneak);
		slideTackle			= ReadBool ("Slide",	"bTackle",				slideTackle);
		landRolling			= ReadBool ("Slide",	"bLandRolling",			landRolling);

		debug				= ReadBool ("Debug",	"bDebug",				debug);

		_MESSAGE("settings loaded from %s", kPath);
	}

	void Save()
	{
		WriteBool ("Main",		"bEnabled",			enabled);
		WriteBool ("Main",		"bShowIndicators",	showIndicators);
		WriteFloat("Main",		"fPlaybackSpeed",	playbackSpeed);

		WriteBool ("Input",		"bUsePresetKey",	usePresetKey);
		WriteInt  ("Input",		"iPresetKey",		presetKey);
		WriteInt  ("Input",		"iCustomKey",		(SInt32)customKey);
		WriteFloat("Input",		"fInputDelay",		inputDelay);
		WriteInt  ("Input",		"iAutoParkour",		autoParkour);

		WriteBool ("Stamina",	"bEnabled",			staminaEnabled);
		WriteBool ("Stamina",	"bRequired",		mustHaveStamina);
		WriteFloat("Stamina",	"fBaseCost",		staminaDamage);

		WriteBool ("Smart",		"bSmartSteps",		smartSteps);
		WriteBool ("Smart",		"bSmartVault",		smartVault);
		WriteBool ("Smart",		"bSmartClimb",		smartClimb);

		WriteBool ("Slide",		"bEnabled",			slideEnabled);
		WriteBool ("Slide",		"bAdvancedSneak",	advancedSlideSneak);
		WriteBool ("Slide",		"bTackle",			slideTackle);
		WriteBool ("Slide",		"bLandRolling",		landRolling);

		WriteBool ("Debug",		"bDebug",			debug);

		// Windows keeps profile writes in a cache of its own; this empties it
		// into the file, so a crash later cannot lose the settings.
		WritePrivateProfileStringA(NULL, NULL, NULL, kPath);

		// And if the file could not be written at all - a read-only install, a
		// missing folder - the settings would quietly revert on the next load,
		// which is a miserable thing to debug from the outside.
		if(GetPrivateProfileIntA("Main", "bEnabled", -1, kPath) == -1)
			_MESSAGE("WARNING: %s could not be written (error %lu): settings will not persist",
					 kPath, GetLastError());
	}

	void Apply()
	{
		Graph::SetPlaybackSpeed(playbackSpeed);
	}
}
