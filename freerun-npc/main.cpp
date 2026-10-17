// Freerun LE
//
// A parkour framework for Skyrim Legendary Edition: climb the ledge in front of
// you, vault what is waist high, catch an edge in mid-air, slide out of a
// sprint, roll out of a fall.
//
// This is a port of SkyParkour NG by Tsptds, whose source is licensed under the
// AGPLv3; this port is released under the same licence. The behaviour patch, the
// animations and the interface come from that project.
//
// What the port had to rebuild rather than carry over:
//
//   * Every engine call. Special Edition's plugin is written against
//     CommonLibSSE and the Address Library; neither exists here, and this
//     edition's engine is a different build with different structures, so every
//     address, offset and vtable slot in Engine.h was read out of the 1.9.32.0
//     executable and is annotated with the evidence.
//
//   * The physics queries. The hit records come back in a different order (the
//     collidable of the thing you hit sits four bytes further along), the
//     capsule shape is built by a single constructor that scales into Havok
//     itself, and the ray pick takes its all-hits collector in a different slot.
//
//   * The heartbeat. The mod ticks from a channel bound into the player's
//     animation graph. Special Edition's poll takes a bool; this edition's takes
//     no argument at all, and the manager is not an event sink here, so the
//     player's own sink is what the animation events are read from.
//
//   * The settings. The SKSE Menu Framework does not exist on this edition, so
//     the settings live in an INI and the in-game menu is a SkyUI mod
//     configuration menu driven through Papyrus.
//
//   * The camera, input and control-map work, which all sit at different
//     offsets and, in two cases, answer a better question than the original
//     asked (see the notes in Parkour.cpp and Slide.cpp).

#include "skse/PluginAPI.h"
#include "skse/skse_version.h"
#include "skse/GameAPI.h"
#include "skse/GameEvents.h"
#include "skse/GameMenus.h"
#include "skse/GameReferences.h"

#include "Engine.h"
#include "Settings.h"
#include "Graph.h"
#include "Parkour.h"
#include "Slide.h"
#include "Input.h"
#include "Camera.h"
#include "Indicator.h"
#include "Papyrus.h"
#include "NpcParkour.h"
#include "Verify.h"
#include "Log.h"

#include <shlobj.h>

#define PLUGIN_NAME				"FreerunLE"
#define PLUGIN_VERSION_STRING	"1.0.7-npc8"
#define PLUGIN_LOG_FILE			"\\My Games\\Skyrim\\SKSE\\FreerunLE.log"

static PluginHandle			g_pluginHandle	= kPluginHandle_Invalid;
static SKSEMessagingInterface *	g_messaging	= NULL;
static SKSEPapyrusInterface *	g_papyrus	= NULL;

namespace
{
	// The mod is idle while a menu has the game. Menus that pause it stop the
	// animation graph too, so the tick simply never comes; the ones that do not
	// pause it - dialogue, the tween menu, a message box - still have to be
	// caught, and the menu manager can be asked about each of them by name.
	const char * const kBlockingMenus[] =
	{
		"Console", "Dialogue Menu", "InventoryMenu", "MagicMenu", "MapMenu",
		"StatsMenu", "Journal Menu", "Crafting Menu", "ContainerMenu",
		"BarterMenu", "GiftMenu", "LevelUp Menu", "Lockpicking Menu", "Book Menu",
		"Sleep/Wait Menu", "TweenMenu", "FavoritesMenu", "MessageBoxMenu",
		"RaceSex Menu", "Loading Menu", "Main Menu", "Training Menu",
	};

	typedef bool (__thiscall * _IsMenuOpen)(MenuManager *, BSFixedString *);
	const _IsMenuOpen IsMenuOpen = (_IsMenuOpen)0x00A5CE90;

	bool AnyBlockingMenuOpen()
	{
		MenuManager * menus = Engine::Menus();
		if(!menus) return false;

		for(UInt32 i = 0; i < sizeof(kBlockingMenus) / sizeof(kBlockingMenus[0]); ++i)
		{
			BSFixedString name(kBlockingMenus[i]);
			const bool open = IsMenuOpen(menus, &name);
			CALL_MEMBER_FN(&name, Release)();
			if(open) return true;
		}
		return false;
	}

	class MenuSink : public BSTEventSink<MenuOpenCloseEvent>
	{
	public:
		virtual EventResult ReceiveEvent(MenuOpenCloseEvent * event, EventDispatcher<MenuOpenCloseEvent> * dispatcher)
		{
			if(!event) return kEvent_Continue;

			static BSFixedString mainMenu("Main Menu");
			static BSFixedString hudMenu("HUD Menu");

			if(event->menuName == mainMenu && event->opening)
			{
				// Back at the title screen: nothing of the last game survives
				// into the next one.
				Parkour::Reset();
				Slide::Reset();
			}

			// The marker rides with the HUD, and it is opened when the HUD is.
			//
			// It used to be opened once, as the data finished loading - which is
			// during the title sequence, with the logo on screen. Two things
			// followed from that. A menu appearing while the logo plays makes the
			// logo flash, which is what the tester saw and what stopped when the
			// mod was removed. And the game tears its menus down on the way from
			// the title screen into a save, so the one menu this mod ever opened
			// was gone before gameplay began and was never opened again: the
			// marker could not appear no matter what the configuration menu said.
			// Opening it with the HUD fixes both, and puts it on the same
			// lifetime as the bar it sits beside.
			// NPC TEST BUILD: only when the ledge marker is switched on. With it
			// off, no menu of this mod exists at all, so there is nothing of ours
			// that can sit on top of the inventory.
			if(event->menuName == hudMenu && event->opening && Settings::showIndicators)
			{
				Indicator::Show();
				Indicator::ShowDebugOverlay(Settings::debug);
			}

			Parkour::SetMenuOpen(AnyBlockingMenuOpen());
			return kEvent_Continue;
		}
	};

	MenuSink s_menuSink;

	// Set when the startup check says this is not the game the port was built
	// for. Off means off: no hooks, and no message handled afterwards either.
	//
	// The first build of the check stopped installing hooks and then carried on
	// answering the other messages, so a log read "ABORTING: no hooks installed"
	// and, two lines later, "save was taken mid-action: unwinding" - the plugin
	// reaching into the animation graph of a game it had just declared itself
	// unfit for.
	bool s_disabled = false;

	void OnMessage(SKSEMessagingInterface::Message * message)
	{
		if(s_disabled) return;

		switch(message->type)
		{
			case SKSEMessagingInterface::kMessage_DataLoaded:
			{
				Graph::Init();
				Settings::Load();
				NpcParkour::LoadSettings();			// NPC test build

				// Before anything is hooked: is this the executable the port
				// was written against? Every address here was read out of one
				// build, and a plugin that patches wrong addresses does not
				// fail where the mistake is - it fails later, somewhere else,
				// differently on every machine. See Verify.cpp.
				const Verify::Result check = Verify::Run();
				if(!check.ok)
				{
					s_disabled = true;
					_ERROR("ABORTING: the game does not match what this plugin was built for - "
						   "no hooks installed, nothing further handled, see the lines above");
					Engine::MessageBox(
						"Freerun: this game is not the build the mod was made for.\n\n"
						"Nothing has been changed and the mod is off. The log names every "
						"address that did not match:\n\n"
						"Documents\\My Games\\Skyrim\\SKSE\\FreerunLE.log\n\n"
						"Send it with your report - it says in one line what your game is.");
					break;
				}

				bool ok = true;
				ok &= Graph::InstallHooks();
				ok &= Input::InstallHooks();
				ok &= Camera::InstallHooks();

				MenuManager * menus = Engine::Menus();
				if(menus)
					menus->MenuOpenCloseEventDispatcher()->AddEventSink(&s_menuSink);

				Input::Register();

				_MESSAGE(ok ? "hooks installed" : "one or more hooks did not install: see above");
				break;
			}

			case SKSEMessagingInterface::kMessage_InputLoaded:
			{
				// The interface is up by now, and not before. The mod this is
				// ported from registers its menu on this same message; this port
				// did it while the data was still loading, which is earlier than
				// the game has a UI to register against.
				//
				// It is opened later still, when the HUD opens - see the menu
				// sink above.
				Indicator::Register();
				break;
			}

			case SKSEMessagingInterface::kMessage_PreLoadGame:
				NpcParkour::Forget();				// NPC test build: drop the tracked NPC at a load
				Parkour::Reset();
				Slide::Reset();
				break;

			case SKSEMessagingInterface::kMessage_PostLoadGame:
			{
				// A save taken in the middle of an action comes back with the
				// behaviour still in it. Ask it to unwind before anything else
				// runs.
				//
				// Only here. A new game gets the branch below and never touches
				// the graph - see the note there.
				Actor * player = (Actor *)Engine::Player();

				// The graph belongs to the player's 3D. Asking a player who has
				// none for a graph variable walks into a manager that was never
				// built, and how far the load has got by the time this message
				// arrives is a matter of how fast the machine is - which is why
				// it took some people down and not others.
				if(player && Engine::Get3D((TESObjectREFR *)player, false))
				{
					const SInt32 ledge = Graph::GetInt(player, Graph::GetNames().ledge, Parkour::kNoLedge);
					if(ledge != Parkour::kNoLedge)
					{
						_MESSAGE("save was taken mid-action: unwinding");
						Graph::Notify(player, Graph::GetNames().stop);
					}
					Graph::SetPlaybackSpeed(Settings::playbackSpeed);
				}
				else
				{
					_MESSAGE("the player has no 3D yet: leaving the graph alone");
				}

				NpcParkour::Forget();				// NPC test build: drop the tracked NPC at a load
				Parkour::Reset();
				Slide::Reset();
				break;
			}

			case SKSEMessagingInterface::kMessage_NewGame:
			{
				// Nothing but a reset, and deliberately so.
				//
				// This message arrives, in SKSE's own words, "after a new game is
				// created, before the game has loaded". There is no player 3D and
				// no animation graph to read yet. The mod this is ported from
				// keeps its new-game branch bare for exactly that reason; this
				// port had merged the two branches, so a new game ran the save
				// recovery meant for a loaded one and read a graph that did not
				// exist. That is the crash on starting a new game.
				//
				// The playback speed does not need setting here either: the
				// channel carries it into every graph the engine builds.
				NpcParkour::Forget();				// NPC test build: drop the tracked NPC at a load
				Parkour::Reset();
				Slide::Reset();
				break;
			}

			default:
				break;
		}
	}
}

extern "C"
{

bool SKSEPlugin_Query(const SKSEInterface * skse, PluginInfo * info)
{
	gLog.OpenRelative(CSIDL_MYDOCUMENTS, PLUGIN_LOG_FILE);

	_MESSAGE("Freerun LE " PLUGIN_VERSION_STRING " (built " __DATE__ " " __TIME__ ")");

	info->infoVersion	= PluginInfo::kInfoVersion;
	info->name			= PLUGIN_NAME;
	info->version		= 1;

	g_pluginHandle = skse->GetPluginHandle();

	if(skse->isEditor)
	{
		_MESSAGE("ABORTING: loaded in the editor");
		return false;
	}

	// Every address in this plugin was read out of this one build. On anything
	// else it does nothing at all rather than write to the wrong place.
	if(skse->runtimeVersion != RUNTIME_VERSION_1_9_32_0)
	{
		_MESSAGE("ABORTING: unsupported runtime %08X, expected 1.9.32.0 (%08X)",
				 skse->runtimeVersion, RUNTIME_VERSION_1_9_32_0);
		return false;
	}

	return true;
}

bool SKSEPlugin_Load(const SKSEInterface * skse)
{
	g_messaging = (SKSEMessagingInterface *)skse->QueryInterface(kInterface_Messaging);
	if(!g_messaging || !g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnMessage))
	{
		_MESSAGE("ABORTING: the messaging interface is not available");
		return false;
	}

	g_papyrus = (SKSEPapyrusInterface *)skse->QueryInterface(kInterface_Papyrus);
	if(g_papyrus)
		g_papyrus->Register(Papyrus::Register);
	else
		_MESSAGE("no Papyrus interface: the configuration menu will not be able to talk to the plugin");

	_MESSAGE("running");
	return true;
}

};
