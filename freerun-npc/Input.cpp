#include "Input.h"
#include "Engine.h"
#include "Graph.h"
#include "Parkour.h"
#include "Slide.h"
#include "Settings.h"
#include "NpcParkour.h"
#include "Log.h"

#include "skse/GameInput.h"
#include "skse/GameEvents.h"
#include "skse/GameReferences.h"
#include "skse/InputMap.h"
#include "skse/SafeWrite.h"

namespace
{
	// ---- handler vtables ---------------------------------------------------
	//
	// Every player input handler has the same shape: destructor, CanProcess,
	// ProcessThumbstick, ProcessMouseMove, ProcessButton. Returning false from
	// CanProcess makes the handler ignore the event entirely.

	UInt32 * const kJumpHandler			= (UInt32 *)0x010D460C;
	UInt32 * const kSneakHandler		= (UInt32 *)0x010D4644;
	UInt32 * const kMovementHandler		= (UInt32 *)0x010D4594;
	UInt32 * const kActivateHandler		= (UInt32 *)0x010D453C;
	UInt32 * const kTogglePOVHandler	= (UInt32 *)0x010D4574;
	UInt32 * const kReadyWeaponHandler	= (UInt32 *)0x010D45C4;
	UInt32 * const kLookHandler			= (UInt32 *)0x010D45AC;

	enum { kSlot_CanProcess = 0x01, kSlot_ProcessButton = 0x04 };

	typedef bool (__thiscall * _CanProcess)(void * handler, InputEvent * event);
	typedef void (__thiscall * _ProcessButton)(void * handler, ButtonEvent * event, void * data);

	_CanProcess		s_origJumpCanProcess		= NULL;
	_CanProcess		s_origSneakCanProcess		= NULL;
	_CanProcess		s_origMovementCanProcess	= NULL;
	_CanProcess		s_origActivateCanProcess	= NULL;
	_CanProcess		s_origPOVCanProcess			= NULL;
	_CanProcess		s_origWeaponCanProcess		= NULL;
	_CanProcess		s_origLookCanProcess		= NULL;
	_ProcessButton	s_origJumpProcessButton		= NULL;

	Actor * ThePlayer() { return (Actor *)Engine::Player(); }

	bool __fastcall Hook_JumpCanProcess(void * handler, void *, InputEvent * event)
	{
		if(Settings::enabled)
		{
			// When the jump key is also the parkour key and there is no hold
			// delay, a ledge in front of the player turns the jump into a
			// climb: the jump itself must not also happen.
			if(Settings::usePresetKey &&
			   Settings::presetKey == Settings::kPresetKey_Jump &&
			   Settings::inputDelay == 0.0f &&
			   Parkour::SelectedLedge() != Parkour::kNoLedge)
				return false;

			if(Parkour::InProgress()) return false;
			if(Slide::Ongoing()) return false;
		}
		return s_origJumpCanProcess(handler, event);
	}

	bool __fastcall Hook_SneakCanProcess(void * handler, void *, InputEvent * event)
	{
		if(Settings::enabled && Parkour::InProgress()) return false;

		if(Settings::slideEnabled)
		{
			if(Slide::Ongoing()) return false;

			// Sneaking out of a sprint is what starts a slide, so the sneak
			// itself waits until the slide has had its say. Same in mid-air,
			// where the landing roll takes over.
			Actor * player = ThePlayer();
			if(player)
			{
				if(Parkour::IsSprinting(player)) return false;
				if(Engine::PlayerIsInMidair()) return false;
			}
		}

		return s_origSneakCanProcess(handler, event);
	}

	bool __fastcall Hook_MovementCanProcess(void * handler, void *, InputEvent * event)
	{
		if(Settings::enabled && Parkour::InProgress())
		{
			// During the recovery frames the player may steer out of the
			// animation early: the movement goes through, and the behaviour is
			// told to let go.
			if(Parkour::RecoveryFrames())
			{
				const bool allowed = s_origMovementCanProcess(handler, event);
				if(allowed)
				{
					Actor * player = ThePlayer();
					if(player) Graph::Notify(player, Graph::GetNames().interrupt);
				}
				return allowed;
			}
			return false;
		}

		if(Settings::slideEnabled && Slide::Ongoing()) return false;

		return s_origMovementCanProcess(handler, event);
	}

	bool __fastcall Hook_ActivateCanProcess(void * handler, void *, InputEvent * event)
	{
		if(Settings::enabled && (Parkour::InProgress() || Slide::Ongoing())) return false;
		return s_origActivateCanProcess(handler, event);
	}

	bool __fastcall Hook_POVCanProcess(void * handler, void *, InputEvent * event)
	{
		// Holding the point of view key during an action would start the
		// vanity-camera zoom.
		if(Settings::enabled && Parkour::InProgress()) return false;
		return s_origPOVCanProcess(handler, event);
	}

	bool __fastcall Hook_WeaponCanProcess(void * handler, void *, InputEvent * event)
	{
		// Drawing or sheathing mid-action leaves the weapon state stuck.
		if(Settings::enabled && (Parkour::InProgress() || Slide::Ongoing())) return false;
		return s_origWeaponCanProcess(handler, event);
	}

	bool __fastcall Hook_LookCanProcess(void * handler, void *, InputEvent * event)
	{
		if(Settings::enabled && Parkour::InProgress())
		{
			void * state = Engine::CameraState();
			if(Engine::CameraStateID(state) == Engine::kCameraState_ThirdPerson)
				return false;
		}
		return s_origLookCanProcess(handler, event);
	}

	// ---- the jump key, with a hold delay -----------------------------------

	// With a delay configured, a tap of the jump key should still jump. The
	// press is held back, and if the key comes up before the delay is over both
	// halves are handed to the handler at once, so the jump happens then.
	// A button event the handler will accept, built as raw bytes: the class has
	// engine-side virtuals, so a copy of one has to be a copy of its memory
	// rather than a C++ copy.
	struct ButtonEventCopy
	{
		UInt8 raw[0x20];

		void From(const ButtonEvent & original, float value, float heldDuration)
		{
			memcpy(raw, &original, sizeof(raw));
			*(void **)(raw + 0x0C)	= NULL;				// next
			*(float *)(raw + 0x18)	= value;			// pressed value
			*(float *)(raw + 0x1C)	= heldDuration;
		}

		ButtonEvent * Get() { return (ButtonEvent *)raw; }
	};

	void __fastcall Hook_JumpProcessButton(void * handler, void *, ButtonEvent * event, void * data)
	{
		Actor * player = ThePlayer();

		if(Settings::enabled && player && !Parkour::IsSitting(player) &&
		   Settings::usePresetKey && Settings::presetKey == Settings::kPresetKey_Jump &&
		   Settings::inputDelay != 0.0f && event)
		{
			const float value = *(float *)&event->flags;
			const bool isDown = (value != 0.0f) && (event->timer == 0.0f);
			const bool isUp   = (value == 0.0f) && (event->timer != 0.0f);

			if(isDown)
				return;						// wait and see whether this is a hold

			if(isUp && event->timer < Settings::inputDelay)
			{
				ButtonEventCopy down, up;
				down.From(*event, 1.0f, 0.0f);
				up.From(*event, 0.0f, event->timer);
				s_origJumpProcessButton(handler, down.Get(), data);
				s_origJumpProcessButton(handler, up.Get(), data);
				return;
			}
		}

		s_origJumpProcessButton(handler, event, data);
	}

	// ---- the event sink ----------------------------------------------------

	UInt32 KeyCodeOf(ButtonEvent * event)
	{
		const UInt32 mask = event->keyMask;
		switch(event->deviceType)
		{
			case kDeviceType_Mouse:		return InputMap::kMacro_MouseButtonOffset + mask;
			case kDeviceType_Gamepad:	return InputMap::GamepadMaskToKeycode(mask);
			default:					return mask;
		}
	}

	const BSFixedString * PresetEventName()
	{
		InputStringHolder * strings = Engine::UserEvents();
		if(!strings) return NULL;

		switch(Settings::presetKey)
		{
			case Settings::kPresetKey_Sprint:	return &strings->sprint;
			case Settings::kPresetKey_Activate:	return &strings->activate;
			case Settings::kPresetKey_Jump:
			default:							return &strings->jump;
		}
	}

	class Sink : public BSTEventSink<InputEvent>
	{
	public:
		virtual EventResult ReceiveEvent(InputEvent ** events, InputEventDispatcher * dispatcher)
		{
			if(!events || !*events) return kEvent_Continue;

			Actor * player = ThePlayer();
			if(!player) return kEvent_Continue;

			InputStringHolder * strings = Engine::UserEvents();

			for(InputEvent * event = *events; event; event = event->next)
			{
				if(event->eventType != InputEvent::kEventType_Button) continue;

				ButtonEvent * button = static_cast<ButtonEvent *>(event);
				const BSFixedString * control = event->GetControlID();

				const float	value	= *(float *)&button->flags;
				const bool	isDown	= (value != 0.0f) && (button->timer == 0.0f);
				const bool	isHeld	= (value != 0.0f) && (button->timer > 0.0f);

				// --- the parkour key ---
				if(Settings::enabled && !Parkour::InProgress())
				{
					bool isParkourKey = false;

					if(Settings::usePresetKey)
					{
						const BSFixedString * expected = PresetEventName();
						isParkourKey = expected && control && (*control == *expected);
					}
					else
					{
						isParkourKey = (Settings::customKey != 0) && (KeyCodeOf(button) == Settings::customKey);
					}

					if(isParkourKey && (isDown || isHeld) && button->timer >= Settings::inputDelay)
						Parkour::TryActivate();

					// Holding a direction is enough on its own for the small
					// actions, when the player asked for that.
					if(strings && control &&
					   (Settings::autoParkour == Settings::kAuto_Always ||
						(Settings::autoParkour == Settings::kAuto_OutOfCombat && !Parkour::IsWeaponOut(player))))
					{
						const Parkour::Type selected = Parkour::SelectedLedge();
						if((selected == Parkour::kStepHigh || selected == Parkour::kStepLow) &&
						   (*control == strings->forward || *control == strings->back ||
							*control == strings->strafeLeft || *control == strings->strafeRight) &&
						   button->timer >= 0.4f)
						{
							Parkour::TryActivate();
						}
					}
				}

				// --- NPC TEST BUILD: the key that makes the NPC in front try a parkour move ---
				if(isDown && NpcParkour::Enabled() && KeyCodeOf(button) == NpcParkour::TestKey())
					NpcParkour::RequestTest();

				// --- the sneak key, for the slide and the roll ---
				if(strings && control && (*control == strings->sneak))
					Slide::OnSneakPressed(player, isDown, isHeld, button->timer);
			}

			// Never swallow an event here: other mods and the game itself are
			// downstream of this sink.
			return kEvent_Continue;
		}
	};

	Sink s_sink;
	bool s_registered = false;

	template <typename T>
	void HookVFunc(UInt32 * vtable, UInt32 slot, T & original, void * replacement)
	{
		original = (T)vtable[slot];
		SafeWrite32((UInt32)&vtable[slot], (UInt32)replacement);
	}
}

namespace Input
{
	bool InstallHooks()
	{
		HookVFunc(kJumpHandler,			kSlot_CanProcess,	s_origJumpCanProcess,		&Hook_JumpCanProcess);
		HookVFunc(kSneakHandler,		kSlot_CanProcess,	s_origSneakCanProcess,		&Hook_SneakCanProcess);
		HookVFunc(kMovementHandler,		kSlot_CanProcess,	s_origMovementCanProcess,	&Hook_MovementCanProcess);
		HookVFunc(kActivateHandler,		kSlot_CanProcess,	s_origActivateCanProcess,	&Hook_ActivateCanProcess);
		HookVFunc(kTogglePOVHandler,	kSlot_CanProcess,	s_origPOVCanProcess,		&Hook_POVCanProcess);
		HookVFunc(kReadyWeaponHandler,	kSlot_CanProcess,	s_origWeaponCanProcess,		&Hook_WeaponCanProcess);
		HookVFunc(kLookHandler,			kSlot_CanProcess,	s_origLookCanProcess,		&Hook_LookCanProcess);
		HookVFunc(kJumpHandler,			kSlot_ProcessButton,s_origJumpProcessButton,	&Hook_JumpProcessButton);

		return s_origJumpCanProcess && s_origSneakCanProcess && s_origMovementCanProcess &&
			   s_origActivateCanProcess && s_origPOVCanProcess && s_origWeaponCanProcess &&
			   s_origLookCanProcess && s_origJumpProcessButton;
	}

	void Register()
	{
		if(s_registered) return;

		InputEventDispatcher * dispatcher = *g_inputEventDispatcher;
		if(!dispatcher)
		{
			_MESSAGE("no input dispatcher yet");
			return;
		}

		dispatcher->AddEventSink(&s_sink);
		s_registered = true;
		_MESSAGE("listening for input");
	}

	void Unregister()
	{
		if(!s_registered) return;

		InputEventDispatcher * dispatcher = *g_inputEventDispatcher;
		if(dispatcher) dispatcher->RemoveEventSink(&s_sink);
		s_registered = false;
	}
}
