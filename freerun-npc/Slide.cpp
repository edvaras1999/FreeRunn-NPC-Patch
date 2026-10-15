#include "Slide.h"
#include "Engine.h"
#include "Graph.h"
#include "Havok.h"
#include "Parkour.h"
#include "Settings.h"
#include "Log.h"

#include "skse/GameReferences.h"

namespace
{
	bool s_ongoing	= false;
	bool s_recovery	= false;

	const NiPoint3 kDown(0.0f, 0.0f, -1.0f);

	bool SlideAllowed(Actor * player, bool holdingKey, bool & outIsRoll)
	{
		outIsRoll = false;

		if(Parkour::InProgress())				return false;
		if(Parkour::MenuOpen())					return false;
		if(!Parkour::ControlsEnabled())			return false;
		if(Parkour::IsKnockedOut(player))		return false;
		if(Parkour::IsAnimationDriven(player))	return false;
		if(Parkour::IsStaggering(player))		return false;
		if(Parkour::IsInSyncedAnimation(player))return false;
		if(Parkour::IsBeastForm(player))		return false;
		if(Parkour::IsSitting(player))			return false;
		if(Parkour::IsInDrawSheath(player))		return false;
		if(Parkour::IsAttacking(player))		return false;
		if(Slide::IsSliding(player))			return false;
		if(Parkour::IsSwimming(player))			return false;

		void * ctrl = Engine::CharController(player);
		if(!ctrl) return false;

		const float fallTime	= Engine::ControllerFallTime(ctrl);
		const bool midair		= Engine::PlayerIsInMidair();

		// Falling for long enough to have built up speed: this is a landing,
		// and a roll is on offer if the ground is close.
		if(Settings::landRolling && midair && fallTime >= 0.5f)
		{
			const Havok::Result ground = Havok::RayCast(Engine::Position(player), kDown, 100.0f,
														Havok::kLayer_Transparent, player);
			if(!ground.didHit) return false;

			// Rolling out of a fall costs stamina; without it the player takes
			// the landing the hard way.
			if(Settings::staminaEnabled && !Parkour::HasEnoughStamina(player))
			{
				Engine::FlashMeter(Engine::kActorValue_Stamina);
				Engine::UpdateRegenDelay(player, Engine::kActorValue_Stamina, 3.0f);
				return false;
			}

			outIsRoll = true;
			return true;
		}

		if(!Settings::slideEnabled)				return false;
		if(holdingKey)							return false;	// a held key is a crouch, not a slide
		if(fallTime > 0.2f)						return false;
		if(!Parkour::IsSprinting(player))		return false;
		if(Parkour::IsSneaking(player))			return false;

		return true;
	}

	bool TrySlide(Actor * player, bool holdingKey)
	{
		bool isRoll = false;
		if(!SlideAllowed(player, holdingKey, isRoll)) return false;

		const Graph::Names & n = Graph::GetNames();
		Graph::SetBool(player, n.isRoll, isRoll);

		if(!Graph::Notify(player, n.notifySlide)) return false;

		s_ongoing = true;
		Engine::ToggleControls(Engine::kControl_MainFour, false);
		return true;
	}
}

namespace Slide
{
	bool Ongoing()			{ return s_ongoing; }
	bool RecoveryFrames()	{ return s_recovery; }

	void ClearOngoing()		{ s_ongoing = false; }

	void Reset()
	{
		s_ongoing	= false;
		s_recovery	= false;
	}

	bool IsSliding(Actor * actor)
	{
		return Graph::GetBool(actor, Graph::GetNames().sliding);
	}

	void ForceSneak(Actor * player)
	{
		Engine::ActorFlags1(player) |= Engine::kFlag1_Sneaking;
		Graph::SetInt(player, Graph::GetNames().isInSneak, 1);
	}

	void OnSneakPressed(Actor * player, bool down, bool held, float heldDuration)
	{
		if(!player) return;
		if(!Settings::slideEnabled && !Settings::landRolling) return;

		const Graph::Names & n = Graph::GetNames();

		if(down)
		{
			TrySlide(player, false);
			return;
		}

		if(!Settings::advancedSlideSneak && held)
		{
			// Simple mode: holding sneak slides and then crouches out of it.
			TrySlide(player, true);
			if(Graph::Notify(player, n.slideSneak))
				ForceSneak(player);
			return;
		}

		if(Settings::advancedSlideSneak && heldDuration > 0.2f)
		{
			// Advanced mode: a held key during the recovery frames ends the
			// slide in a crouch, otherwise it starts one.
			if(s_ongoing && s_recovery)
			{
				if(Engine::PlayerIsInMidair()) return;
				if(Graph::GetBool(player, n.isRoll)) return;
				if(Graph::Notify(player, n.slideSneakAdvanced))
					ForceSneak(player);
			}
			else
			{
				TrySlide(player, true);
			}
		}
	}

	void OnStartStop(bool stop, Actor * player, bool isRoll)
	{
		const Graph::Names & n = Graph::GetNames();

		if(stop)
		{
			s_ongoing	= false;
			s_recovery	= false;

			// The other point of view's graph has its own copy of the state.
			Graph::Notify(player, n.slideStop);

			if(!isRoll)
			{
				// Coming out of a slide into a crouch means the sprint is over;
				// coming out of it upright means the player keeps running.
				// Special Edition pokes a private player flag for this. Setting
				// the state the engine itself reads, and telling the graph, does
				// the same job with nothing hidden.
				if(Parkour::IsSneaking(player))
				{
					Engine::ActorFlags1(player) &= ~Engine::kFlag1_Sprinting;
					Graph::Notify(player, n.sprintStop);
				}
				else
				{
					Engine::ActorFlags1(player) |= Engine::kFlag1_Sprinting;
				}
			}
		}

		Engine::ToggleControls(Engine::kControl_Jumping, stop);
		Engine::ToggleControls(Engine::kControl_MainFour, stop);
	}

	void OnAnimationEvent(Actor * player, const BSFixedString & tag, const BSFixedString & payload)
	{
		const Graph::Names & n = Graph::GetNames();

		if(tag == n.slideStop)
		{
			const bool isRoll = (payload == n.rollPayload);
			OnStartStop(true, player, isRoll);

			// Sliding into water leaves the swim state unstarted, because the
			// animation took over the transition.
			void * ctrl = Engine::CharController(player);
			if(ctrl && Engine::ControllerState(ctrl) == Engine::Controller::kState_Swimming)
				Graph::Notify(player, n.swimStart);
		}
		else if(tag == n.slideStart)
		{
			if(payload == n.slidePayload)
				OnStartStop(false, player, false);
		}
		else if(tag == n.recovery)
		{
			s_recovery = true;
		}
		else if(tag == n.staminaHit)
		{
			const bool lowEffort = (payload == n.lowEffort);
			Parkour::PostStaminaCost(player, lowEffort, false);

			// A roll takes the sting out of the landing: the fall is measured
			// from a hundred units lower than it really started.
			void * ctrl = Engine::CharController(player);
			if(ctrl)
			{
				float & fallStart = Engine::ControllerFallStart(ctrl);
				if(fallStart - Engine::Position(player).z > 200.0f)
					fallStart -= 100.0f;
			}
		}
	}

	void OnGraphTick(Actor *)
	{
		// The slide has nothing to do per frame of its own: the behaviour drives
		// it and the events above keep the state in step. The hook is kept so
		// the two systems tick from the same place.
	}
}
