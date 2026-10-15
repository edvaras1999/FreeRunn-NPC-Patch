#include "Graph.h"
#include "Engine.h"
#include "Settings.h"
#include "Parkour.h"
#include "Slide.h"
#include "Log.h"

#include "skse/GameReferences.h"
#include "skse/SafeWrite.h"

namespace
{
	// ---------------------------------------------------------- vtable slots

	// The player's IAnimationGraphManagerHolder subobject lives at ref + 0x20
	// and has its own vtable. Only the player's copy is patched, so no other
	// actor in the game changes behaviour.
	UInt32 * const kPlayerHolderVtbl		= (UInt32 *)0x010D1FB4;
	enum { kHolder_Notify = 0x01, kHolder_CreateChannels = 0x0A, kHolder_PostCreate = 0x0B };

	// The player's BSTEventSink<BSAnimationGraphEvent>, the subobject at
	// ref + 0x1C. This is what SetupAnimEventSinks (0x006C73C0) registers with
	// every graph, so it sees first and third person alike.
	UInt32 * const kPlayerEventSinkVtbl		= (UInt32 *)0x010D2004;
	enum { kSink_ReceiveEvent = 0x01 };

	// Appends a channel to the scrap array the engine passes to
	// CreateAnimationChannels, taking a reference exactly as the engine's own
	// channels do.
	typedef SInt32 (__thiscall * _AppendChannel)(void * scrapArray, void * channel);
	const _AppendChannel AppendChannel = (_AppendChannel)0x006BA780;

	// ------------------------------------------------------------- the names

	Graph::Names	s_names;
	bool			s_namesReady = false;

	void SetName(BSFixedString & out, const char * text)
	{
		CALL_MEMBER_FN(&out, Set)(text);
	}

	// --------------------------------------------------------- bound channel

	// A BSAnimationGraphChannel as this edition lays it out: vtable, reference
	// count, name, value. Everything past that is ours.
	//
	// The engine polls this once per animation update of the player and copies
	// `value` into the graph variable named `name`. Two jobs ride on that: the
	// behaviour gets its playback speed, and the mod gets a heartbeat that is
	// in step with the animation graph rather than with the renderer.
	struct Channel
	{
		const void **	vtbl;			// 0x00
		volatile LONG	refCount;		// 0x04
		BSFixedString	name;			// 0x08
		UInt32			value;			// 0x0C
		Actor *			owner;			// 0x10

		// The position correction that carries the player from wherever they
		// were to where the animation needs them to start.
		bool	correctionInitialised;
		bool	correctionDone;
		bool	wasReset;
		float	missingGap;
		float	totalMissing[4];
	};

	Channel * s_channel = NULL;		// the one bound to the player, if any

	void ChannelReset(Channel * self)
	{
		self->correctionInitialised	= false;
		self->correctionDone		= false;
		self->missingGap			= 0.0f;
		memset(self->totalMissing, 0, sizeof(self->totalMissing));
	}

	// --- the three virtuals, in this edition's order: destroy, poll, reset ---

	void __fastcall Channel_Destroy(Channel * self, void *, UInt8 flags)
	{
		if(s_channel == self) s_channel = NULL;
		CALL_MEMBER_FN(&self->name, Release)();
		if(flags & 1)
			Engine::HeapFree(self, false);
	}

	// Special Edition's PollChannelUpdateImpl takes a bool. This edition's
	// takes nothing - the engine calls it with this in ECX and an empty stack.
	void __fastcall Channel_Poll(Channel * self, void *)
	{
		Parkour::OnGraphTick(self->owner);
		Slide::OnGraphTick(self->owner);
	}

	void __fastcall Channel_Reset(Channel * self, void *)
	{
		ChannelReset(self);
	}

	const void * s_channelVtbl[3] =
	{
		(const void *)&Channel_Destroy,
		(const void *)&Channel_Poll,
		(const void *)&Channel_Reset,
	};

	Channel * NewChannel(Actor * owner)
	{
		Channel * channel = (Channel *)Engine::HeapAllocate(sizeof(Channel), 0, false);
		if(!channel) return NULL;

		memset(channel, 0, sizeof(Channel));
		channel->vtbl		= s_channelVtbl;
		channel->refCount	= 0;						// the array below takes the first reference
		channel->owner		= owner;
		channel->value		= *(UInt32 *)&Settings::playbackSpeed;
		SetName(channel->name, Graph::kSpeedMultVariable);
		ChannelReset(channel);
		return channel;
	}

	// ----------------------------------------------------------------- hooks

	typedef bool (__thiscall * _CreateChannels)(void * holder, void * scrapArray);
	_CreateChannels s_origCreateChannels = NULL;

	bool __fastcall Hook_CreateChannels(void * holder, void *, void * scrapArray)
	{
		const bool result = s_origCreateChannels(holder, scrapArray);

		// The holder is a subobject at ref + 0x20; the reference itself starts
		// 0x20 bytes earlier.
		Actor * actor = (Actor *)((UInt8 *)holder - 0x20);

		Channel * channel = NewChannel(actor);
		if(channel)
		{
			AppendChannel(scrapArray, channel);
			s_channel = channel;
			_MESSAGE("channel bound to the player's graph");
		}

		return result;
	}

	typedef void (__thiscall * _PostCreate)(void * holder, void * managerPtr);
	_PostCreate s_origPostCreate = NULL;

	void __fastcall Hook_PostCreate(void * holder, void *, void * managerPtr)
	{
		s_origPostCreate(holder, managerPtr);

		Actor * actor = (Actor *)((UInt8 *)holder - 0x20);
		Parkour::OnGraphReady(actor);
	}

	typedef bool (__thiscall * _Notify)(void * holder, const BSFixedString & event);
	_Notify s_origNotify = NULL;

	bool __fastcall Hook_Notify(void * holder, void *, const BSFixedString & event)
	{
		Actor * actor = (Actor *)((UInt8 *)holder - 0x20);
		const Graph::Names & n = Graph::GetNames();

		// Anything else sending our stop event mid-action would leave the
		// character controller in the state the animation set up. Turn it into
		// the interrupt the behaviour knows how to unwind.
		if(event == n.stop && Parkour::InProgress())
			return s_origNotify(holder, n.interrupt);

		if(event == n.ragdoll)
		{
			const bool ragdolled = s_origNotify(holder, event);
			if(ragdolled)
			{
				if(Parkour::InProgress())	Parkour::OnStartStop(true, actor);
				else if(Slide::Ongoing())	Slide::OnStartStop(true, actor, false);
			}
			return ragdolled;
		}

		if(event == n.slideStop)
			Slide::ClearOngoing();

		if(event == n.sneakStart && Slide::Ongoing() && Slide::RecoveryFrames())
		{
			const bool res = s_origNotify(holder, event);
			if(res) Slide::ForceSneak(actor);
			return res;
		}

		return s_origNotify(holder, event);
	}

	typedef UInt32 (__thiscall * _ReceiveEvent)(void * sink, void * event, void * source);
	_ReceiveEvent s_origReceiveEvent = NULL;

	// BSAnimationGraphEvent on this edition: tag, holder, payload.
	struct AnimationGraphEvent
	{
		BSFixedString	tag;		// 0x00
		TESObjectREFR *	holder;		// 0x04
		BSFixedString	payload;	// 0x08
	};

	UInt32 __fastcall Hook_ReceiveEvent(void * sink, void *, AnimationGraphEvent * event, void * source)
	{
		if(event)
		{
			// The sink is the subobject at ref + 0x1C.
			Actor * actor = (Actor *)((UInt8 *)sink - 0x1C);
			Parkour::OnAnimationEvent(actor, event->tag, event->payload);
		}

		return s_origReceiveEvent(sink, event, source);
	}

	template <typename T>
	void HookVFunc(UInt32 * vtable, UInt32 slot, T & original, void * replacement)
	{
		original = (T)vtable[slot];
		SafeWrite32((UInt32)&vtable[slot], (UInt32)replacement);
	}
}

namespace Graph
{
	const char * const kSpeedMultVariable = "SkyParkourSpeedMult";

	const Names & GetNames() { return s_names; }

	void Init()
	{
		if(s_namesReady) return;

		// Events the behaviour patch listens for and sends back. These are the
		// identifiers baked into the patched behaviour files, so they are kept
		// exactly as the patch spells them.
		SetName(s_names.notify,				"SkyParkour");
		SetName(s_names.stop,				"SkyParkour_Stop");
		SetName(s_names.interrupt,			"SkyParkour_Interrupt");
		SetName(s_names.notifySlide,		"SkyParkour_Slide");
		SetName(s_names.slideStop,			"SkyParkour_SlideStop");
		SetName(s_names.slideSneak,			"SkyParkour_SlideToSneak");
		SetName(s_names.slideSneakAdvanced,	"SkyParkour_SlideToSneak_Adv");
		SetName(s_names.start,				"SkyParkour_Start");
		SetName(s_names.recovery,			"SkyParkour_Recovery");
		SetName(s_names.staminaHit,			"SkyParkour_HitStamina");
		SetName(s_names.slideStart,			"SkyParkour_SlideStart");

		SetName(s_names.sprintStop,			"SprintStop");
		SetName(s_names.sprintStart,		"SprintStart");
		SetName(s_names.swimStart,			"SwimStart");
		SetName(s_names.interruptCast,		"InterruptCast");
		SetName(s_names.getUpExit,			"GetUpExit");
		SetName(s_names.ragdoll,			"Ragdoll");
		SetName(s_names.sneakStart,			"SneakStart");

		SetName(s_names.lowEffort,			"LowEffort");
		SetName(s_names.slidePayload,		"Slide");
		SetName(s_names.rollPayload,		"LandRoll");

		SetName(s_names.ledge,				"SkyParkourLedge");
		SetName(s_names.stepLeg,			"SkyParkourStepLeg");
		SetName(s_names.grabVariant,		"SkyParkourGrabVariant");
		SetName(s_names.ongoing,			"SkyParkourOngoing");
		SetName(s_names.speedMult,			kSpeedMultVariable);
		SetName(s_names.lowerBody,			"SkyParkourLowerBody");
		SetName(s_names.sliding,			"SkyParkourSliding");
		SetName(s_names.isRoll,				"SkyParkourIsLandingRoll");
		SetName(s_names.installedThird,		"SkyParkourTPPInstalled");
		SetName(s_names.installedFirst,		"SkyParkourFPPInstalled");

		SetName(s_names.isFirstPerson,		"IsFirstPerson");
		SetName(s_names.isBeastRace,		"IsBeastRace");
		SetName(s_names.animationDriven,	"bAnimationDriven");
		SetName(s_names.isEquipping,		"IsEquipping");
		SetName(s_names.isUnequipping,		"IsUnequipping");
		SetName(s_names.isSynced,			"bIsSynced");
		SetName(s_names.isInSneak,			"iIsInSneak");

		s_namesReady = true;
	}

	bool InstallHooks()
	{
		HookVFunc(kPlayerHolderVtbl,	kHolder_CreateChannels,	s_origCreateChannels,	&Hook_CreateChannels);
		HookVFunc(kPlayerHolderVtbl,	kHolder_PostCreate,		s_origPostCreate,		&Hook_PostCreate);
		HookVFunc(kPlayerHolderVtbl,	kHolder_Notify,			s_origNotify,			&Hook_Notify);
		HookVFunc(kPlayerEventSinkVtbl,	kSink_ReceiveEvent,		s_origReceiveEvent,		&Hook_ReceiveEvent);

		return s_origCreateChannels && s_origPostCreate && s_origNotify && s_origReceiveEvent;
	}

	void BindChannel(Actor * actor)
	{
		// Nothing to do by hand: the engine asks the holder for its channels
		// whenever it builds a graph manager, and the hook above answers. This
		// is here so a caller that knows the graph was rebuilt can check.
		if(!s_channel)
			_MESSAGE("no channel bound yet - the graph manager has not been rebuilt");
		else
			s_channel->owner = actor;
	}

	void SetPlaybackSpeed(float value)
	{
		if(s_channel)
			s_channel->value = *(UInt32 *)&value;
	}

	bool BehaviourPatchPresent(Actor * player)
	{
		static bool warnedFirst = false;
		static bool warnedThird = false;

		const Names & n = GetNames();
		const bool firstPerson = GetBool(player, n.isFirstPerson);

		bool installed = false;
		if(firstPerson)
		{
			if(Engine::GetGraphVariableBool(player, n.installedFirst, installed) && installed)
				return true;
			if(warnedFirst) return false;
			warnedFirst = true;

			Engine::MessageBox(
				"Freerun: the first person behaviour patch is missing.\n\n"
				"Open Nemesis, tick BOTH \"SkyParkour 1st Person\" and \"SkyParkour 3rd Person\", press "
				"Update Engine, then Launch Nemesis Behavior Engine, and let it finish.\n\n"
				"If you have already done that, the output is losing the conflict: it has to sit at the "
				"very bottom of your load order and overwrite anything else that touches "
				"meshes\\actors\\character\\_1stperson\\behaviors\\0_master.hkx.\n\n"
				"Nemesis and FNIS cannot both be used. This mod needs Nemesis.");
			_ERROR("first person behaviour patch missing: 0_master.hkx under _1stperson is not patched");
		}
		else
		{
			if(Engine::GetGraphVariableBool(player, n.installedThird, installed) && installed)
				return true;
			if(warnedThird) return false;
			warnedThird = true;

			Engine::MessageBox(
				"Freerun: the third person behaviour patch is missing.\n\n"
				"Open Nemesis, tick BOTH \"SkyParkour 1st Person\" and \"SkyParkour 3rd Person\", press "
				"Update Engine, then Launch Nemesis Behavior Engine, and let it finish.\n\n"
				"If you have already done that, the output is losing the conflict: it has to sit at the "
				"very bottom of your load order and overwrite anything else that touches "
				"meshes\\actors\\character\\behaviors\\0_master.hkx.\n\n"
				"Nemesis and FNIS cannot both be used. This mod needs Nemesis.");
			_ERROR("third person behaviour patch missing: 0_master.hkx is not patched");
		}

		return false;
	}

	bool GetBool(TESObjectREFR * ref, const BSFixedString & name, bool defaultValue)
	{
		bool out = defaultValue;
		if(!ref) return defaultValue;
		if(!Engine::GetGraphVariableBool(ref, name, out)) return defaultValue;
		return out;
	}

	SInt32 GetInt(TESObjectREFR * ref, const BSFixedString & name, SInt32 defaultValue)
	{
		SInt32 out = defaultValue;
		if(!ref) return defaultValue;
		if(!Engine::GetGraphVariableInt(ref, name, out)) return defaultValue;
		return out;
	}

	void SetBool(TESObjectREFR * ref, const BSFixedString & name, bool value)
	{
		if(ref) Engine::SetGraphVariableBool(ref, name, value);
	}

	void SetInt(TESObjectREFR * ref, const BSFixedString & name, SInt32 value)
	{
		if(ref) Engine::SetGraphVariableInt(ref, name, value);
	}

	void SetFloat(TESObjectREFR * ref, const BSFixedString & name, float value)
	{
		if(ref) Engine::SetGraphVariableFloat(ref, name, value);
	}

	bool Notify(TESObjectREFR * ref, const BSFixedString & event)
	{
		return ref ? Engine::NotifyAnimationGraph(ref, event) : false;
	}
}
