#include "Parkour.h"
#include "Engine.h"
#include "Graph.h"
#include "Havok.h"
#include "Settings.h"
#include "Slide.h"
#include "Indicator.h"
#include "NpcParkour.h"
#include "Log.h"

#include "skse/GameReferences.h"

#include <math.h>

using namespace Parkour::Limits;

namespace
{
	// ---- runtime state -----------------------------------------------------

	Parkour::Type	s_selected		= Parkour::kNoLedge;
	NiPoint3		s_ledgePoint;
	NiPoint3		s_startPos;
	float			s_playerScale	= 1.0f;

	bool			s_inProgress	= false;
	bool			s_recovery		= false;
	bool			s_menuOpen		= false;
	bool			s_activating	= false;		// re-entrancy guard
	bool			s_npcProbe		= false;		// NPC test build: a probe for an NPC is running
	bool			s_probeFacingOn	= false;		// NPC test build: search in `s_probeFacing`, not where the actor faces
	NiPoint3		s_probeFacing;

	// ---- small maths -------------------------------------------------------

	NiPoint3 Add(const NiPoint3 & a, const NiPoint3 & b)		{ return NiPoint3(a.x + b.x, a.y + b.y, a.z + b.z); }
	NiPoint3 Sub(const NiPoint3 & a, const NiPoint3 & b)		{ return NiPoint3(a.x - b.x, a.y - b.y, a.z - b.z); }
	NiPoint3 Scale(const NiPoint3 & a, float s)					{ return NiPoint3(a.x * s, a.y * s, a.z * s); }
	float    Length(const NiPoint3 & a)							{ return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
	float    FlatDistance(const NiPoint3 & a, const NiPoint3 & b)
	{
		const float dx = a.x - b.x, dy = a.y - b.y;
		return sqrtf(dx * dx + dy * dy);
	}

	const NiPoint3 kUp(0.0f, 0.0f, 1.0f);
	const NiPoint3 kDown(0.0f, 0.0f, -1.0f);

	// The layer every probe casts as. Special Edition casts everything as
	// kTransparent, which collides with the world but not with the player's own
	// body, and the same holds here.
	const UInt32 kProbeLayer = Havok::kLayer_Transparent;

	// A ledge on one of these layers is not something to climb: the collision
	// belongs to a creature, a weapon, a projectile or a trigger volume.
	bool ExcludedClimbLayer(UInt32 layer)
	{
		switch(layer)
		{
			case Havok::kLayer_NonCollidable:
			case Havok::kLayer_CharController:
			case Havok::kLayer_Weapon:
			case Havok::kLayer_Projectile:
			case Havok::kLayer_Transparent:
			case Havok::kLayer_Clutter:
			case Havok::kLayer_Biped:
			case Havok::kLayer_ActorZone:
			case Havok::kLayer_DebrisLarge:
				return true;
			default:
				return false;
		}
	}

	bool ExcludedVaultLayer(UInt32 layer)
	{
		switch(layer)
		{
			case Havok::kLayer_Weapon:
			case Havok::kLayer_Projectile:
			case Havok::kLayer_CharController:
			case Havok::kLayer_Clutter:
			case Havok::kLayer_Biped:
			case Havok::kLayer_DeadBip:
				return true;
			default:
				return false;
		}
	}

	// Doors are the classic false positive: their frame reads as a perfect
	// ledge. Actors are excluded for the obvious reason.
	// NPC TEST BUILD: these take the form TYPE, a value the Havok layer checked
	// while it held the physics world, instead of a pointer to the reference.
	// Havok::kFormType_Unknown matches nothing, so nothing is excluded.
	bool ExcludedClimbForm(UInt8 formType)
	{
		return formType == kFormType_NPC;
	}

	bool ExcludedVaultForm(UInt8 formType)
	{
		return formType == kFormType_NPC || formType == kFormType_Activator;
	}

	// ---- scale -------------------------------------------------------------

	float NodeScale(Actor * actor, const char * boneName)
	{
		NiNode * root = Engine::Get3D(actor, false);
		if(!root) root = Engine::Get3D(actor, true);
		if(!root) return 1.0f;

		BSFixedString name(boneName);
		NiAVObject * node = root->GetObjectByName(&name.data);
		CALL_MEMBER_FN(&name, Release)();
		if(!node) return 1.0f;

		// NiAVObject::m_localTransform sits at 0x20, its scale at 0x30 inside.
		return *(float *)((UInt8 *)node + 0x20 + 0x30);
	}

	float ActorScale(Actor * actor)
	{
		float scale = 1.0f;

		NiNode * root = Engine::Get3D(actor, false);
		if(!root) root = Engine::Get3D(actor, true);
		if(root) scale *= *(float *)((UInt8 *)root + 0x20 + 0x30);

		// RaceMenu and several body mods scale these two instead of the root.
		const float npc = NodeScale(actor, "NPC");
		if(npc > 0.0f) scale *= npc;
		const float rootBone = NodeScale(actor, "NPC Root [Root]");
		if(rootBone > 0.0f) scale *= rootBone;

		if(scale < 0.15f) scale = 0.15f;
		if(scale > 250.0f) scale = 250.0f;
		return scale;
	}
}

namespace Parkour
{
	// ---- state -------------------------------------------------------------

	bool		InProgress()		{ return s_inProgress; }
	bool		RecoveryFrames()	{ return s_recovery; }
	Type		SelectedLedge()		{ return s_selected; }
	NiPoint3	LedgePoint()		{ return s_ledgePoint; }
	float		PlayerScale()		{ return s_playerScale; }
	bool		MenuOpen()			{ return s_menuOpen; }

	void SetMenuOpen(bool open)
	{
		s_menuOpen = open;
		if(open) InvalidateLedge();
	}

	void InvalidateLedge()
	{
		s_selected = kNoLedge;
		Indicator::SetType(Indicator::kInvisible);
	}

	void Reset()
	{
		s_inProgress	= false;
		s_recovery		= false;
		s_selected		= kNoLedge;
		Indicator::SetType(Indicator::kInvisible);
		NpcParkour::Reset();					// NPC test build
	}

	// ---- actor state -------------------------------------------------------

	bool IsKnockedOut(Actor * actor)		{ return Engine::KnockState(actor) != 0; }
	bool IsSitting(Actor * actor)			{ return Engine::SitSleepState(actor) != 0; }
	bool IsAttacking(Actor * actor)			{ return Engine::AttackState(actor) != 0; }
	bool IsWeaponOut(Actor * actor)			{ return Engine::WeaponState(actor) == Engine::kWeaponState_Drawn; }
	bool IsSwimming(Actor * actor)			{ return (Engine::ActorFlags1(actor) & Engine::kFlag1_Swimming) != 0; }
	bool IsSneaking(Actor * actor)			{ return (Engine::ActorFlags1(actor) & Engine::kFlag1_Sneaking) != 0; }
	bool IsSprinting(Actor * actor)			{ return (Engine::ActorFlags1(actor) & Engine::kFlag1_Sprinting) != 0; }
	bool IsStaggering(Actor * actor)		{ return (Engine::ActorFlags2(actor) & Engine::kFlag2_Staggered) != 0; }
	bool IsMoving(Actor * actor)			{ return Engine::MovementSpeed(actor) > 5.0f; }

	bool IsAnimationDriven(Actor * actor)	{ return Graph::GetBool(actor, Graph::GetNames().animationDriven); }
	bool IsInSyncedAnimation(Actor * actor)	{ return Graph::GetBool(actor, Graph::GetNames().isSynced); }
	bool IsBeastForm(Actor * actor)			{ return Graph::GetBool(actor, Graph::GetNames().isBeastRace); }

	bool IsInDrawSheath(Actor * actor)
	{
		const Graph::Names & n = Graph::GetNames();
		return Graph::GetBool(actor, n.isEquipping) || Graph::GetBool(actor, n.isUnequipping);
	}

	// Special Edition asks the player's chargen flags whether the "controls are
	// disabled" message is showing, which is its way of catching the intro with
	// the hands bound. Asking the control map whether movement and jumping are
	// enabled catches that and every other scripted lock-out as well.
	bool ControlsEnabled()
	{
		const UInt32 flags = Engine::EnabledControls();
		return (flags & Engine::kControl_Movement) && (flags & Engine::kControl_Jumping);
	}

	// ---- stamina -----------------------------------------------------------

	float StaminaCost(Actor * actor)
	{
		if(actor == (Actor *)Engine::Player() && Engine::IsGodMode())
			return -1.0f;

		// Heavier gear costs more. The equipped weight the engine caches sits
		// at actor + 0x134; a negative value means it has not been worked out
		// yet, in which case the base cost stands on its own.
		const float equipped = *(float *)((UInt8 *)actor + 0x134);
		const float extra = (equipped > 0.0f) ? equipped * 0.2f : 0.0f;
		return Settings::staminaDamage + extra;
	}

	bool HasEnoughStamina(Actor * actor)
	{
		if(!Settings::mustHaveStamina) return true;
		const float cost = StaminaCost(actor);
		if(cost < 0.0f) return true;
		return Engine::GetActorValue(actor, Engine::kActorValue_Stamina) > cost;
	}

	void DamageStamina(Actor * actor, float amount)
	{
		if(actor && amount > 0.0f)
			Engine::DamageActorValue(actor, Engine::kActorValue_Stamina, amount);
	}

	// ---- movement ----------------------------------------------------------

	// The character controller's forward vector points backwards, as the
	// vanilla code that uses it does too.
	NiPoint3 FacingFlat(Actor * actor)
	{
		void * ctrl = Engine::CharController(actor);
		if(!ctrl) return NiPoint3(0.0f, 0.0f, 0.0f);

		const float * fwd = (const float *)((UInt8 *)ctrl + Engine::Controller::kForwardVec);
		NiPoint3 flat(-fwd[0], -fwd[1], 0.0f);
		const float len = sqrtf(flat.x * flat.x + flat.y * flat.y);
		if(len <= 1e-5f) return NiPoint3(0.0f, 0.0f, 0.0f);
		return NiPoint3(flat.x / len, flat.y / len, 0.0f);
	}

	float ForwardSpeed(Actor * actor)
	{
		void * ctrl = Engine::CharController(actor);
		if(!ctrl) return 0.0f;

		__declspec(align(16)) float velocity[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		Engine::ControllerGetLinearVelocity(ctrl, velocity);

		const float inv = Engine::HavokScaleInverse();
		const float x = velocity[0] * inv;
		const float y = velocity[1] * inv;
		return sqrtf(x * x + y * y);
	}

	float RelativeSpeed(Actor * actor)
	{
		const float speed = ForwardSpeed(actor);
		const float top = Engine::MovementSpeed(actor);
		return speed / (top <= 0.0f ? 1.0f : top);
	}
}

namespace
{
	using namespace Parkour;

	// ---- the checks that decide whether an action is allowed ---------------

	bool CameraAngleValid(Actor * player)
	{
		// In third person the character can face one way while the camera looks
		// another. Climbing sideways looks wrong, so the two have to agree.
		void * state = Engine::CameraState();
		if(Engine::CameraStateID(state) != Engine::kCameraState_ThirdPerson) return true;

		const float cameraYaw = *(float *)((UInt8 *)state + Engine::ThirdPerson::kTargetYaw);
		const float playerYaw = Engine::Rotation(player).z;

		float diff = cameraYaw - playerYaw;
		while(diff > 3.14159265f)  diff -= 6.28318531f;
		while(diff < -3.14159265f) diff += 6.28318531f;

		return fabsf(diff) <= 0.4f;
	}

	// Everything ParkourAllowed asks that does NOT depend on the ledge search.
	//
	// Split out because of what that search costs. VaultCheck and ClimbCheck
	// each walk a stepped probe of fifteen to thirty stations, two casts a
	// station and more wherever one lands, so a tick running both is sixty to a
	// hundred physics queries. All of it ran unconditionally, every animation
	// tick, whatever the player was doing - weapon drawn, mid-swing, sitting,
	// in a menu - because the one test that needs the search result (is there a
	// ledge at all) sat in the same list as the fifteen that do not. Asking
	// these first costs nothing and skips the search outright in every state
	// where nothing could fire anyway.
	bool ParkourPossible(Actor * player)
	{
		if(s_menuOpen)						return false;
		if(!ControlsEnabled())				return false;
		if(IsBeastForm(player))				return false;
		if(IsKnockedOut(player))			return false;
		if(Engine::LifeState(player) != Engine::kLifeState_Alive) return false;
		if(IsAnimationDriven(player))		return false;
		if(IsStaggering(player))			return false;
		if(IsInSyncedAnimation(player))		return false;
		if(IsSitting(player))				return false;
		if(IsInDrawSheath(player))			return false;
		if(IsAttacking(player))				return false;
		if(Slide::IsSliding(player))		return false;
		if(!CameraAngleValid(player))		return false;
		return true;
	}

	bool ParkourAllowed(Actor * player)
	{
		if(s_selected == kNoLedge)			return false;
		return ParkourPossible(player);
	}

	NiPoint3 ProbeFacing(Actor * actor);		// NPC test build: defined below, before FindLedge

	// Climbing while running at a wall reads as a stumble rather than a climb,
	// so a fast approach is only allowed when something is actually in the way
	// at face height.
	bool SmartClimbAllowed(Actor * player)
	{
		if(!Settings::smartClimb)		return true;
		if(!IsMoving(player))			return true;
		if(IsSwimming(player))			return true;

		const NiPoint3 start = Add(Engine::Position(player), NiPoint3(0.0f, 0.0f, 120.0f * s_playerScale));
		const Havok::Result face = Havok::RayCast(start, ProbeFacing(player), 150.0f, kProbeLayer, player);
		if(face.didHit) return true;

		return RelativeSpeed(player) <= 0.2f;
	}

	bool StepAllowed(Actor * player, float ledgeHeight, const NiPoint3 & ledgePoint)
	{
		if(Engine::ActorFlags1(player) & Engine::kFlag1_MovingBack) return false;

		// A step is only worth playing when the player is moving into it: the
		// faster they go, the higher a step they will take in stride.
		float multiplier = 0.0f;
		if(IsMoving(player))			multiplier = RelativeSpeed(player);
		else if(!Settings::smartSteps)	multiplier = 0.8f;

		const float base = kLowLedge - kClimbMin;
		const float threshold = base + multiplier * base;
		if(ledgeHeight < threshold * s_playerScale) return false;

		return FlatDistance(ledgePoint, Engine::Position(player)) <= 70.0f;
	}

	bool VaultAllowed(Actor * player)
	{
		// NPC TEST BUILD: the engine's mid-air test answers for the player only.
		// For anyone else the character controller's own state is asked.
		if(player == (Actor *)Engine::Player())
		{
			if(Engine::PlayerIsInMidair()) return false;
		}
		else
		{
			void * ctrl = Engine::CharController(player);
			if(ctrl && Engine::ControllerState(ctrl) != Engine::Controller::kState_OnGround) return false;
		}

		if(Engine::ActorFlags1(player) & Engine::kFlag1_MovingBack) return false;

		// NPC TEST BUILD: an NPC probed while standing still is not "moving".
		if(s_npcProbe) return true;

		if(!Settings::smartVault) return true;
		return IsMoving(player);
	}

	bool GrabAllowed(Actor * player, float ledgeHeight, const NiPoint3 & ledgePoint, bool & outHighVariant)
	{
		if(Engine::ActorFlags1(player) & Engine::kFlag1_MovingBack) return false;

		// Catching a ledge only makes sense in the air: if the ground is right
		// below, this is a step.
		const Havok::Result down = Havok::RayCast(Engine::Position(player), kDown, 35.0f, kProbeLayer, player);
		if(down.didHit) return false;

		if(FlatDistance(ledgePoint, Engine::Position(player)) > 70.0f) return false;
		if(ledgeHeight > kGrabMax) return false;

		outHighVariant = ledgeHeight > kGrabHighVariant;
		return true;
	}

	// ---- the ledge search --------------------------------------------------

	Type ChooseClimbHeight(Actor * player, const NiPoint3 & ledgePoint, const NiPoint3 & playerPos)
	{
		const float scale = s_playerScale <= 0.0f ? 1.0f : s_playerScale;
		const float height = (ledgePoint.z - playerPos.z) / scale;

		void * ctrl = Engine::CharController(player);
		const bool grounded = ctrl &&
			(Engine::ControllerFlags(ctrl) & (Engine::Controller::kFlag_Support |
											  Engine::Controller::kFlag_PotentialSupportManifold)) != 0;

		if(grounded || IsSwimming(player))
		{
			if(height >= kHighestLedge)
			{
				if(!SmartClimbAllowed(player)) return kNoLedge;
				if(Settings::staminaEnabled && !IsSwimming(player) && !HasEnoughStamina(player)) return kFailed;
				return kHighest;
			}
			if(height >= kHighLedge)
			{
				if(!SmartClimbAllowed(player)) return kNoLedge;
				if(Settings::staminaEnabled && !IsSwimming(player) && !HasEnoughStamina(player)) return kFailed;
				return kHigh;
			}
			if(height >= kMediumLedge)
				return kMedium;

			if(height >= kLowLedge)
				return IsSwimming(player) ? kGrab : kLow;

			if(height >= kHighStep)
			{
				if(IsSwimming(player)) return kGrab;
				if(StepAllowed(player, height, ledgePoint)) return kStepHigh;
				return kNoLedge;
			}

			if(IsSwimming(player)) return kGrab;
			if(StepAllowed(player, height, ledgePoint)) return kStepLow;
			return kNoLedge;
		}

		// In the air: the only thing on offer is catching the ledge.
		bool highVariant = false;
		if(!GrabAllowed(player, height, ledgePoint, highVariant)) return kNoLedge;

		Graph::SetBool(player, Graph::GetNames().grabVariant, highVariant);
		return kGrab;
	}

	Type ClimbCheck(Actor * player, const NiPoint3 & facing, float minHeight, float maxHeight, NiPoint3 & outLedge)
	{
		const NiPoint3 playerPos = Engine::Position(player);
		const float playerHeight = 120.0f * s_playerScale;

		void * ctrl = Engine::CharController(player);
		const float speedPct = ctrl ? Engine::ControllerSpeedPct(ctrl) : 0.0f;

		// The faster the player moves, the further ahead the ledge may be.
		const int steps = (int)(speedPct * 15.0f + 15.0f);
		const float stepLength = 5.0f;
		const float minFlatness = 0.3f;

		// Start the probe at the ceiling rather than at the player's maximum
		// reach, so a low ceiling does not cancel a low climb.
		const NiPoint3 ceilingStart = Add(playerPos, NiPoint3(0.0f, 0.0f, playerHeight));
		const Havok::Result ceiling = Havok::ShapeCast(ceilingStart, kUp, maxHeight - playerHeight, 5.0f, kProbeLayer, player);

		const float downLength = maxHeight - minHeight -
								 (ceiling.didHit ? (maxHeight - playerHeight - ceiling.distance) : 0.0f);

		NiPoint3 forwardStart = playerPos;
		forwardStart.z = ceiling.didHit ? ceiling.hitPosition.z : (playerPos.z + maxHeight);

		// One ray forward, not one per station.
		//
		// Each station used to cast its own ray along the same line from the
		// same point, differing only in how far it looked - so thirty stations
		// asked the physics world thirty times for something a single ray of the
		// full length already answers: how far ahead the first obstruction is.
		// A station is skipped when the obstruction falls short of it, which is
		// exactly what comparing against that one distance says. Where nothing
		// is in the way a ray reports the length it was given, so the full-length
		// ray reports the full length and no station is skipped, as before.
		//
		// This halves the casts the climb search makes, and it halves them
		// hardest while sprinting, where the station count is at its highest -
		// which is where the stutter was reported.
		const float reach = stepLength * (float)(steps - 1);
		const float blockDistance = Havok::RayCast(forwardStart, facing, reach, kProbeLayer, player).distance;

		for(int i = 0; i < steps; ++i)
		{
			const float probeLength = stepLength * i;

			if(blockDistance < probeLength) continue;			// something in the way at head height

			const NiPoint3 downStart = Add(forwardStart, Scale(facing, probeLength));
			Havok::Result down = Havok::RayCast(downStart, kDown, downLength, kProbeLayer, player);

			// There is no one collision filter that answers for every surface
			// worth climbing, so every hit is walked and the first one on an
			// acceptable layer wins.
			float distance = down.distance;
			NiPoint3 normal = down.normal;
			UInt32 layer = down.layer;
			UInt8 formType = down.formType;
			for(UInt32 h = 0; h < down.hitCount; ++h)
			{
				if(ExcludedClimbLayer(down.hits[h].layer)) continue;
				// (A hit with no reference behind it - terrain - used to be skipped here,
				// but the old lookup never actually returned null, so terrain was always
				// climbable. That is kept: nothing is skipped for lacking a reference.)
				distance	= down.hits[h].distance;
				normal		= down.hits[h].normal;
				layer		= down.hits[h].layer;
				formType	= down.hits[h].formType;
				break;
			}
			if(!down.didHit) continue;

			const NiPoint3 ledgePoint = Add(downStart, Scale(kDown, distance));

			if(ExcludedClimbLayer(layer)) continue;
			if(normal.z < minFlatness) continue;					// a wall, not a ledge
			if(ledgePoint.z < playerPos.z + minHeight) continue;
			if(ledgePoint.z > playerPos.z + maxHeight) continue;

			// Is there room to stand behind the edge? Two probes: one straight
			// ahead, one along the surface normal, so that looking slightly
			// sideways cannot buy extra clearance.
			{
				const float backOffset = 15.0f;
				const float firstLength = backOffset + 15.0f;
				const float needed = backOffset + 3.0f;

				const NiPoint3 obsStart = Sub(NiPoint3(ledgePoint.x, ledgePoint.y, ledgePoint.z + 5.0f),
											  Scale(facing, backOffset));

				const Havok::Result obs1 = Havok::RayCast(obsStart, facing, firstLength, kProbeLayer, player);

				NiPoint3 alongNormal(-obs1.normal.x, -obs1.normal.y, 0.0f);
				const Havok::Result obs2 = Havok::RayCast(obsStart, alongNormal, needed, kProbeLayer, player);

				if(obs2.didHit || obs1.distance < needed) continue;
			}

			if(ExcludedClimbForm(formType)) return kNoLedge;			// a door frame is not a ledge

			// Headroom above the ledge for the body that is about to stand up.
			const float headRoomRadius = 15.0f;
			const NiPoint3 headRoomStart = Add(ledgePoint, NiPoint3(0.0f, 0.0f, headRoomRadius));
			if(Havok::ShapeCast(headRoomStart, kUp, playerHeight - headRoomRadius, headRoomRadius, kProbeLayer, player).didHit)
				return kNoLedge;

			// That sweep is blind to the first fifteen units above the ledge: the
			// sphere starts there, and a cast that begins already inside something
			// reports nothing at all unless a start collector is passed. A ceiling a
			// hand's breadth over the edge would let the climb run and leave the
			// player standing in solid rock, so the same span is asked again with a
			// ray, which has no radius to be caught inside.
			if(Havok::RayCast(Add(ledgePoint, NiPoint3(0.0f, 0.0f, 1.0f)), kUp,
							   playerHeight - 1.0f, kProbeLayer, player).didHit)
				return kNoLedge;

			// And room on the way up, for ledges tucked under something.
			const float upRadius = 10.0f;
			const float upLength = ledgePoint.z - playerPos.z - playerHeight;
			const NiPoint3 upStart = Sub(NiPoint3(playerPos.x, playerPos.y, playerPos.z + playerHeight),
										 Scale(facing, upRadius));
			if(upLength > 0.0f &&
			   Havok::ShapeCast(upStart, kUp, upLength, upRadius, kProbeLayer, player).didHit)
				return kNoLedge;

			// Finally: can the player see the ledge, or is something between?
			const float visionRadius = 5.0f;
			const float visionOffset = 5.0f + visionRadius;
			const NiPoint3 eye(playerPos.x, playerPos.y, ledgePoint.z + visionOffset);
			const NiPoint3 target = Add(ledgePoint, NiPoint3(0.0f, 0.0f, visionOffset));
			const float visionLength = Length(Sub(target, eye));
			if(Havok::ShapeCast(eye, facing, visionLength, visionRadius, kProbeLayer, player).didHit)
				return kNoLedge;

			outLedge = ledgePoint;
			return ChooseClimbHeight(player, ledgePoint, playerPos);
		}

		return kNoLedge;
	}

	Type VaultCheck(Actor * player, const NiPoint3 & facing, float minHeight, float maxHeight, NiPoint3 & outLedge)
	{
		if(!VaultAllowed(player)) return kNoLedge;

		const NiPoint3 playerPos = Engine::Position(player);
		const float playerHeight = 120.0f * s_playerScale;
		const NiPoint3 headPos = Add(playerPos, NiPoint3(0.0f, 0.0f, playerHeight));

		void * ctrl = Engine::CharController(player);
		const float speedPct = ctrl ? Engine::ControllerSpeedPct(ctrl) : 0.0f;
		const int steps = (int)(speedPct * 5.0f + 15.0f);

		Havok::Result down;
		NiPoint3 downStart;
		bool found = false;
		float foundHeight = -10000.0f;

		for(int i = 0; i < steps; ++i)
		{
			downStart = Add(playerPos, Scale(facing, (float)i * 5.0f));
			downStart.z = headPos.z;

			down = Havok::ShapeCast(downStart, kDown, playerHeight, 5.0f, kProbeLayer, player);
			if(!down.didHit) continue;
			if(ExcludedVaultLayer(down.layer)) continue;
			if(ExcludedVaultForm(down.formType)) continue;

			const float height = (headPos.z - down.distance) - playerPos.z;
			if(height > maxHeight) return kNoLedge;			// too tall to vault: let the climb decide
			if(height > minHeight)
			{
				foundHeight = height;
				outLedge = Add(downStart, Scale(kDown, down.distance));
				found = true;
				break;
			}
		}

		if(!found) return kNoLedge;
		outLedge.z = playerPos.z + foundHeight;

		// Nothing directly above the thing being vaulted.
		const float upRadius = 10.0f;
		if(Havok::ShapeCast(Add(outLedge, NiPoint3(0.0f, 0.0f, 5.0f)), kUp, playerHeight * 0.5f, upRadius, kProbeLayer, player).didHit)
			return kNoLedge;

		// And there has to be somewhere to land on the far side.
		const float landingRadius = 10.0f;
		const float landingOffset = 45.0f;
		const NiPoint3 landingStart = Add(NiPoint3(outLedge.x, outLedge.y, headPos.z),
										  Scale(facing, landingOffset + landingRadius));
		const Havok::Result landing = Havok::ShapeCast(landingStart, kDown, playerHeight - minHeight,
													   landingRadius, kProbeLayer, player);

		const bool landingOk = !landing.didHit ||
							   !(down.distance >= landing.distance + 10.0f) ||
							   !(landing.hitPosition.z >= down.hitPosition.z - 10.0f);
		if(!landingOk) return kNoLedge;

		// Same line of sight test as the climb.
		const float visionRadius = 5.0f;
		const float visionOffset = 5.0f + visionRadius;
		const NiPoint3 eye(playerPos.x, playerPos.y, outLedge.z + visionOffset);
		const NiPoint3 target(landingStart.x, landingStart.y, eye.z);
		const float visionLength = Length(Sub(target, eye));
		if(Havok::ShapeCast(eye, facing, visionLength, visionRadius, kProbeLayer, player).didHit)
			return kNoLedge;

		return kVault;
	}

	// NPC TEST BUILD: where a search looks - the actor's facing, unless a probe
	// asked for a direction of its own.
	NiPoint3 ProbeFacing(Actor * actor)
	{
		return s_probeFacingOn ? s_probeFacing : FacingFlat(actor);
	}

	Type FindLedge(Actor * player)
	{
		s_playerScale = ActorScale(player);
		const NiPoint3 facing = ProbeFacing(player);
		if(facing.x == 0.0f && facing.y == 0.0f) return kNoLedge;

		NiPoint3 ledgePoint;

		Type type = VaultCheck(player, facing, kVaultMin * s_playerScale, kVaultMax * s_playerScale, ledgePoint);
		if(type == kNoLedge)
			type = ClimbCheck(player, facing, kClimbMin * s_playerScale, kClimbMax * s_playerScale, ledgePoint);

		if(type == kNoLedge) return kNoLedge;

		// Never parkour into water. Special Edition asks the cell for its water
		// height; the character controller already knows it, and is right even
		// where a cell has more than one water plane.
		void * ctrl = Engine::CharController(player);
		if(ctrl)
		{
			const float waterHeight = Engine::ControllerWaterHeight(ctrl);
			if(waterHeight > -100000.0f && ledgePoint.z < waterHeight - 10.0f)
				return kNoLedge;
		}

		s_ledgePoint = ledgePoint;
		return type;
	}

	// ---- starting position -------------------------------------------------

	bool StartingPosition(Actor * player, Type type, NiPoint3 & out)
	{
		float elevation = 0.0f;
		float backOffset = 55.0f;

		switch(type)
		{
			case kHighest:	elevation = kHighestElevation - 5.0f;	backOffset = 62.0f;	break;
			case kHigh:		elevation = kHighElevation - 5.0f;						break;
			case kMedium:	elevation = kMediumElevation - 5.0f;					break;
			case kLow:		elevation = kLowElevation - 5.0f;						break;
			case kStepHigh:	elevation = kStepHighElevation - 5.0f;	backOffset = 15.0f;	break;
			case kStepLow:	elevation = kStepLowElevation - 5.0f;	backOffset = 15.0f;	break;
			case kVault:	elevation = kVaultElevation - 5.0f;						break;
			case kGrab:
			{
				const bool high = Graph::GetBool(player, Graph::GetNames().grabVariant);
				elevation = (high ? kGrabHighElevation : kGrabElevation) - 5.0f;
				backOffset = 45.0f;
				break;
			}
			case kFailed:	break;		// played where the player stands
			default:
				_ERROR("no starting position for ledge type %d", (int)type);
				return false;
		}

		const NiPoint3 back = Scale(ProbeFacing(player), backOffset * s_playerScale);
		out.x = s_ledgePoint.x - back.x;
		out.y = s_ledgePoint.y - back.y;
		out.z = (type == kFailed) ? Engine::Position(player).z
								  : s_ledgePoint.z - elevation * s_playerScale;
		return true;
	}

	// ---- the per-frame correction -----------------------------------------

	bool	s_correctionInitialised	= false;
	bool	s_correctionDone		= false;
	float	s_missingGap			= 0.0f;
	float	s_totalMissing[4]		= { 0.0f, 0.0f, 0.0f, 0.0f };

	void ResetCorrection()
	{
		s_correctionInitialised	= false;
		s_correctionDone		= false;
		s_missingGap			= 0.0f;
		memset(s_totalMissing, 0, sizeof(s_totalMissing));
	}

	// The animation starts from an exact mark. Rather than teleport the player
	// there - which reads as a snap, and fights the character controller - the
	// remaining gap is walked off over the first few ticks of the animation.
	void RunCorrection(Actor * player)
	{
		void * ctrl = Engine::CharController(player);
		if(!ctrl) return;

		__declspec(align(16)) float current[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		Engine::ControllerGetPosition(ctrl, current, true);

		const float scale = Engine::HavokScale();

		if(!s_correctionInitialised)
		{
			s_totalMissing[0] = s_startPos.x * scale - current[0];
			s_totalMissing[1] = s_startPos.y * scale - current[1];
			s_totalMissing[2] = s_startPos.z * scale - current[2];
			s_totalMissing[3] = 0.0f;

			s_missingGap = sqrtf(s_totalMissing[0] * s_totalMissing[0] +
								 s_totalMissing[1] * s_totalMissing[1] +
								 s_totalMissing[2] * s_totalMissing[2]);
			s_correctionInitialised = true;
		}

		if(s_correctionDone || s_missingGap <= 0.1f)
		{
			s_correctionDone = true;
			return;
		}

		// A thirtieth of the gap per tick, slowed or sped up with the game's own
		// time multiplier so it still lands when time is scaled.
		float step = 1.0f / 30.0f;
		const float timeScale = Engine::GlobalTimeMultiplier();
		if(timeScale > 0.0f) step *= timeScale;

		__declspec(align(16)) float target[4];
		target[0] = current[0] + s_totalMissing[0] * step;
		target[1] = current[1] + s_totalMissing[1] * step;
		target[2] = current[2] + s_totalMissing[2] * step;
		target[3] = 0.0f;

		Engine::ControllerSetPosition(ctrl, target, true, false);

		const float moved = sqrtf((s_totalMissing[0] * step) * (s_totalMissing[0] * step) +
								  (s_totalMissing[1] * step) * (s_totalMissing[1] * step) +
								  (s_totalMissing[2] * step) * (s_totalMissing[2] * step));
		s_missingGap -= moved;
	}
}

namespace Parkour
{
	// ---- activation --------------------------------------------------------

	void OnStartStop(bool stop, Actor * player)
	{
		void * ctrl = Engine::CharController(player);

		if(stop)
		{
			// NPC TEST BUILD: the landing on top of the edge, and where the move ended, for the trail.
			if(s_inProgress)
				NpcParkour::RecordPlayerMoveEnd(player);

			if(ctrl)
				Engine::ControllerFlags(ctrl) &= ~Engine::Controller::kFlag_NoSim;

			Graph::SetInt(player, Graph::GetNames().ledge, kNoLedge);

			// The other point of view's graph cannot see this one, so the
			// interrupt is what tells it the action is over. Sending the stop
			// event here instead would come straight back and never end.
			Graph::Notify(player, Graph::GetNames().interrupt);

			// Leaving the ground with whatever velocity the animation ended on
			// throws the player across the room. Keep the direction, drop the
			// speed and the vertical part.
			if(ctrl && Engine::ControllerState(ctrl) != Engine::Controller::kState_OnGround)
			{
				const NiPoint3 facing = FacingFlat(player);
				__declspec(align(16)) float velocity[4];
				velocity[0] = facing.x;
				velocity[1] = facing.y;
				velocity[2] = 0.0f;
				velocity[3] = 0.0f;
				Engine::ControllerSetLinearVelocity(ctrl, velocity);
			}

			s_recovery		= false;
			s_inProgress	= false;
			ResetCorrection();
		}
		else
		{
			// Stop whatever else is driving the body, then hand it to the
			// animation: no simulation means the root motion moves the player,
			// including upwards, and a hit cannot take the controller back.
			Graph::Notify(player, Graph::GetNames().interruptCast);
			if(ctrl)
				Engine::ControllerFlags(ctrl) |= Engine::Controller::kFlag_NoSim;
		}

		Engine::ToggleControls(Engine::kControl_Jumping, stop);
		Engine::ToggleControls(Engine::kControl_MainFour, stop);
	}

	bool TryActivate()
	{
		if(s_activating) return false;
		s_activating = true;

		bool started = false;
		Actor * player = (Actor *)Engine::Player();

		do
		{
			if(!player) break;
			if(!Settings::enabled) break;
			if(s_selected == kNoLedge) break;
			if(s_inProgress) break;
			if(s_menuOpen) break;
			if(Graph::GetBool(player, Graph::GetNames().ongoing)) break;
			if(!ParkourAllowed(player)) break;

			void * ctrl = Engine::CharController(player);
			const float fallTime = ctrl ? Engine::ControllerFallTime(ctrl) : 0.0f;
			const bool swimming = IsSwimming(player);

			// Catching a ledge right after the jump starts looks like a bug, so
			// give the jump a moment first. Swimming is the exception: there the
			// grab animation is how the player leaves the water.
			if(s_selected == kGrab && fallTime < 0.17f && !swimming) break;

			if(!Graph::BehaviourPatchPresent(player)) break;

			const Type type = s_selected;
			s_inProgress = true;
			ResetCorrection();

			Graph::SetInt(player, Graph::GetNames().ledge, type);

			NiPoint3 start;
			s_startPos = StartingPosition(player, type, start) ? start : Engine::Position(player);

			// NPC TEST BUILD: the trail a follower can repeat.
			NpcParkour::RecordPlayerMove((int)type, s_startPos, s_ledgePoint, FacingFlat(player));

			// A step taken with the weapon drawn only plays on the legs, so the
			// player keeps their guard up.
			if(type == kStepHigh || type == kStepLow)
				Graph::SetBool(player, Graph::GetNames().lowerBody, IsWeaponOut(player));

			Graph::Notify(player, Graph::GetNames().notify);
			started = true;
		}
		while(false);

		s_activating = false;
		return started;
	}

	// ---- events ------------------------------------------------------------

	void OnAnimationEvent(Actor * player, const BSFixedString & tag, const BSFixedString & payload)
	{
		if(!player || !(Actor *)Engine::Player()) return;
		if(player != (Actor *)Engine::Player()) return;
		if(!Settings::enabled && !Settings::slideEnabled) return;

		const Graph::Names & n = Graph::GetNames();

		if(tag == n.getUpExit)
		{
			// Back on their feet after a ragdoll: nothing of ours survives that.
			Reset();
			Slide::Reset();
			return;
		}

		if(Slide::Ongoing())
		{
			Slide::OnAnimationEvent(player, tag, payload);
			return;
		}

		if(!s_inProgress) return;

		if(tag == n.start)
		{
			OnStartStop(false, player);
		}
		else if(tag == n.recovery)
		{
			s_recovery = true;

			// The recovery frames are where the player can already break out of
			// the animation - but only if there is ground under them. In the
			// air, end the action instead of dropping out of it.
			const bool closeToGround = Havok::RayCast(Engine::Position(player), kDown, 35.0f, kProbeLayer, player).didHit;
			if(!closeToGround)
				Graph::Notify(player, n.stop);
		}
		else if(tag == n.stop)
		{
			OnStartStop(true, player);
		}
		else if(tag == n.staminaHit)
		{
			const bool lowEffort = (payload == n.lowEffort);
			PostStaminaCost(player, lowEffort, IsSwimming(player));
		}
	}

	void PostStaminaCost(Actor * player, bool lowEffort, bool swimming)
	{
		if(!Settings::staminaEnabled) return;

		const float cost = StaminaCost(player);
		if(cost < 0.0f) return;		// god mode

		if(lowEffort || swimming)
		{
			DamageStamina(player, cost * 0.5f);
		}
		else if(HasEnoughStamina(player))
		{
			DamageStamina(player, cost);
		}
		else
		{
			Engine::FlashMeter(Engine::kActorValue_Stamina);
		}

		Engine::UpdateRegenDelay(player, Engine::kActorValue_Stamina, 2.0f);
	}

	void OnGraphReady(Actor * player)
	{
		if(!player || player != (Actor *)Engine::Player()) return;

		Graph::SetPlaybackSpeed(Settings::playbackSpeed);
		Reset();
		_MESSAGE("player graph rebuilt: state cleared");
	}

	// ---- the tick ----------------------------------------------------------

	void OnGraphTick(Actor * player)
	{
		// The channel is bound to the player's graph and to no other - the
		// vtable it hangs off belongs to PlayerCharacter alone, which the RTTI
		// in the executable says outright. The check is here anyway because it
		// costs one comparison and OnGraphReady already makes it: if that ever
		// stopped being true, this would quietly run the whole ledge search for
		// every actor in the cell instead of going wrong somewhere visible.
		if(!player || player != (Actor *)Engine::Player()) return;

		// NPC TEST BUILD: one comparison when no test is running.
		NpcParkour::OnTick(player);

		if(s_inProgress)
		{
			// NPC TEST BUILD: the path the body takes, for a follower to repeat.
			NpcParkour::RecordPlayerMoveSample(Engine::Position(player));

			RunCorrection(player);
			InvalidateLedge();
			return;
		}

		ResetCorrection();

		// The two cheap questions come first, and the search only runs if both
		// pass. InvalidateLedge clears the selection and hides the marker, so
		// skipping the search here leaves exactly the state a search that found
		// nothing would have left.
		if(!Settings::enabled || !ParkourPossible(player))
		{
			InvalidateLedge();
			return;
		}

		s_selected = FindLedge(player);

		if(s_selected == kNoLedge)
		{
			Indicator::SetType(Indicator::kInvisible);
			return;
		}

		// Auto parkour: steps and vaults can fire on their own while the player
		// holds a direction, and a ledge can be caught during a long fall.
		const bool autoAllowed =
			(Settings::autoParkour == Settings::kAuto_Always) ||
			(Settings::autoParkour == Settings::kAuto_OutOfCombat && !IsWeaponOut(player));

		if(autoAllowed)
		{
			switch(s_selected)
			{
				case kStepHigh:
				case kStepLow:
				case kVault:
					if(Engine::AutoMove()) TryActivate();
					break;

				case kGrab:
				{
					void * ctrl = Engine::CharController(player);
					if(ctrl && Engine::ControllerFallTime(ctrl) > 0.5f) TryActivate();
					break;
				}

				default:
					break;
			}
		}

		Indicator::Update(s_selected, s_ledgePoint);
	}

	// ---- NPC test build ----------------------------------------------------

	bool ProbeForActor(Actor * npc, Type & outType, NiPoint3 & outLedge,
					   NiPoint3 & outStart, float & outScale, const NiPoint3 * facing)
	{
		outType		= kNoLedge;
		outLedge	= NiPoint3(0.0f, 0.0f, 0.0f);
		outStart	= NiPoint3(0.0f, 0.0f, 0.0f);
		outScale	= 1.0f;
		if(!npc) return false;

		// FindLedge and StartingPosition keep their working values in the
		// player's globals. They are borrowed for the NPC and handed back.
		const NiPoint3	savedLedge	= s_ledgePoint;
		const float		savedScale	= s_playerScale;

		s_npcProbe = true;
		if(facing)
		{
			s_probeFacing	= *facing;
			s_probeFacingOn	= true;
		}

		const Type type = FindLedge(npc);

		bool ok = false;
		if(type != kNoLedge)
		{
			outLedge = s_ledgePoint;
			ok = StartingPosition(npc, type, outStart);
		}

		s_probeFacingOn	= false;
		s_npcProbe		= false;

		outType		= type;
		outScale	= s_playerScale;

		s_ledgePoint	= savedLedge;
		s_playerScale	= savedScale;
		return ok;
	}
}
