#include "Camera.h"
#include "Engine.h"
#include "Parkour.h"
#include "Slide.h"
#include "Settings.h"
#include "Indicator.h"
#include "Log.h"

#include "skse/GameReferences.h"
#include "skse/GameInput.h"
#include "skse/SafeWrite.h"

namespace
{
	// TESCameraState's own table: destructor, Begin, End, Update, ...
	// and the PlayerInputHandler subobject each camera state carries at + 0x10.
	UInt32 * const kThirdPersonState		= (UInt32 *)0x010E29EC;
	UInt32 * const kThirdPersonStateInput	= (UInt32 *)0x010E29D4;
	UInt32 * const kFirstPersonState		= (UInt32 *)0x010E30C8;
	UInt32 * const kFirstPersonStateInput	= (UInt32 *)0x010E30B0;

	enum { kSlot_End = 0x02, kSlot_Update = 0x03, kSlot_CanProcess = 0x01 };

	// How far the head may pitch while an action plays. Anything more and the
	// first person camera looks through the player's own shoulders.
	const float kPitchClampParkour	= 1.0f;
	const float kPitchClampSlide	= 0.6f;

	typedef void (__thiscall * _End)(void * state);
	typedef void (__thiscall * _Update)(void * state, void * nextState);
	typedef bool (__thiscall * _CanProcess)(void * handler, InputEvent * event);

	_End		s_origThirdEnd		= NULL;
	_End		s_origFirstEnd		= NULL;
	_Update		s_origThirdUpdate	= NULL;
	_Update		s_origFirstUpdate	= NULL;
	_CanProcess	s_origThirdCanProcess = NULL;
	_CanProcess	s_origFirstCanProcess = NULL;

	void __fastcall Hook_ThirdEnd(void * state, void *)
	{
		// Leaving the camera state invalidates everything screen-space: the
		// next state picks a ledge up again on its own tick.
		Parkour::InvalidateLedge();
		s_origThirdEnd(state);
	}

	void __fastcall Hook_FirstEnd(void * state, void *)
	{
		Parkour::InvalidateLedge();
		s_origFirstEnd(state);
	}

	void __fastcall Hook_ThirdUpdate(void * state, void *, void * nextState)
	{
		if(Parkour::InProgress())
		{
			// Hold the zoom where it is. The animation moves the player a long
			// way in a short time, and the camera's own follow would otherwise
			// pull the zoom in and out on the way.
			float * target	= (float *)((UInt8 *)state + Engine::ThirdPerson::kTargetZoomOffset);
			float * current	= (float *)((UInt8 *)state + Engine::ThirdPerson::kCurrentZoomOffset);
			*target = *current;

			*(UInt8 *)((UInt8 *)state + Engine::ThirdPerson::kStateNotActive) = 0;
		}

		s_origThirdUpdate(state, nextState);
	}

	void __fastcall Hook_FirstUpdate(void * state, void *, void * nextState)
	{
		if(Parkour::InProgress() || Slide::Ongoing())
		{
			Actor * player = (Actor *)Engine::Player();
			if(player)
			{
				const float clamp = Parkour::InProgress() ? kPitchClampParkour : kPitchClampSlide;

				float & pitch = Engine::Rotation(player).x;
				if(pitch > clamp)		pitch = clamp;
				else if(pitch < -clamp)	pitch = -clamp;
			}

			// The horizontal offset the first person camera keeps for sitting
			// and for scripted scenes has no business being set here.
			*(float *)((UInt8 *)state + Engine::FirstPerson::kSittingRotation) = 0.0f;
		}

		s_origFirstUpdate(state, nextState);
	}

	bool __fastcall Hook_ThirdCanProcess(void * handler, void *, InputEvent * event)
	{
		if(Settings::enabled && Parkour::InProgress()) return false;
		return s_origThirdCanProcess(handler, event);
	}

	bool __fastcall Hook_FirstCanProcess(void * handler, void *, InputEvent * event)
	{
		if(Settings::enabled && Parkour::InProgress()) return false;
		return s_origFirstCanProcess(handler, event);
	}

	template <typename T>
	void HookVFunc(UInt32 * vtable, UInt32 slot, T & original, void * replacement)
	{
		original = (T)vtable[slot];
		SafeWrite32((UInt32)&vtable[slot], (UInt32)replacement);
	}
}

namespace Camera
{
	bool InstallHooks()
	{
		HookVFunc(kThirdPersonState,		kSlot_End,			s_origThirdEnd,			&Hook_ThirdEnd);
		HookVFunc(kThirdPersonState,		kSlot_Update,		s_origThirdUpdate,		&Hook_ThirdUpdate);
		HookVFunc(kThirdPersonStateInput,	kSlot_CanProcess,	s_origThirdCanProcess,	&Hook_ThirdCanProcess);

		HookVFunc(kFirstPersonState,		kSlot_End,			s_origFirstEnd,			&Hook_FirstEnd);
		HookVFunc(kFirstPersonState,		kSlot_Update,		s_origFirstUpdate,		&Hook_FirstUpdate);
		HookVFunc(kFirstPersonStateInput,	kSlot_CanProcess,	s_origFirstCanProcess,	&Hook_FirstCanProcess);

		return s_origThirdEnd && s_origThirdUpdate && s_origThirdCanProcess &&
			   s_origFirstEnd && s_origFirstUpdate && s_origFirstCanProcess;
	}
}
