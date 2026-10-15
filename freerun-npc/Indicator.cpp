#include "Indicator.h"
#include "Engine.h"
#include "Parkour.h"
#include "Settings.h"
#include "Log.h"

#include "skse/GameMenus.h"
#include "skse/ScaleformLoader.h"
#include "skse/ScaleformMovie.h"
#include "skse/ScaleformCallbacks.h"
#include "skse/ScaleformAPI.h"
#include "skse/NiObjects.h"

#include <math.h>

namespace
{
	// The movie, and the name the menu is registered under.
	const char * const kMenuName	= "Freerun";
	const char * const kMovieName	= "Freerun";		// Interface\Freerun.swf

	// Exactly what the game's own HUD menu sets on itself, read out of its
	// constructor at 0x008652E0:
	//
	//     this->flags = 0x00018902;   this->unk0C = 2;
	//
	// and it leaves unk14 at the 0x12 the IMenu constructor put there. An
	// earlier build here dropped two of those flag bits, added one of its own
	// and overwrote unk14 with 1, on the reasoning that this menu takes no
	// input. That reasoning was wrong: the HUD takes no input either, and a
	// menu that lies about what kind of menu it is gets drawn at the wrong
	// times - over the title sequence - and not drawn at the right ones.
	const UInt32 kMenuFlags = 0x00018902;
	const UInt8  kMenuDepth = 2;

	// NiCamera::WorldPtToScreenPt3, and the two globals the game keeps the
	// current camera in.
	typedef bool (__cdecl * _WorldPtToScreenPt3)(float * worldToCam, NiRect<float> * port, NiPoint3 * point,
												 float * xOut, float * yOut, float * zOut, float zeroTolerance);
	const _WorldPtToScreenPt3 WorldPtToScreenPt3 = (_WorldPtToScreenPt3)0x00AB84C0;
	float * const			kWorldToCamMatrix	= (float *)0x01B3EA10;
	NiRect<float> * const	kViewPort			= (NiRect<float> *)0x01B3EA74;

	class IndicatorMenu;
	IndicatorMenu *	s_menu		= NULL;
	Indicator::Type	s_lastType	= Indicator::kInvisible;
	NiPoint2		s_lastScreen;
	float			s_hudOpacity = -1.0f;

	// The last line of defence on the one thing this mod does not own.
	//
	// Everything else here is the mod's own memory, with the mod's own
	// lifetime. The movie is not: the game creates it, the game destroys it,
	// and it does so on its own schedule during every load and every trip to
	// the title screen. The pointer is cleared in the destructor and checked
	// against the menu manager before use, and between the two that should be
	// the end of it - but "should be" is the word that put a crash on other
	// people's machines in the first place.
	//
	// So a fault in here is caught, the movie is written off, and the game
	// keeps running without a ledge marker. The marker is cosmetic; the crash
	// was not. Nothing else in the mod is wrapped like this: a fault in the
	// parkour logic is a bug I want to hear about, not one to swallow.
	//
	// No C++ object with a destructor may live in this function - the compiler
	// will not mix structured exception handling with unwinding.
	bool SafeInvoke(GFxMovieView * view, const char * name, GFxValue * args, UInt32 count)
	{
		if(!view) return false;

		__try
		{
			return view->Invoke(name, NULL, args, count);
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			s_menu = NULL;			// stop talking to it, now and for good
			_ERROR("the indicator movie faulted on %s: the marker is off for this session", name);
			return false;
		}
	}

	class IndicatorMenu : public IMenu
	{
	public:
		IndicatorMenu()
		{
			CALL_MEMBER_FN((GFxLoader *)Engine::ScaleformLoader(), LoadMovie)(this, &view, kMovieName, 1, 0.0f);

			flags	= kMenuFlags;
			unk0C	= kMenuDepth;
			// unk14 is left at the 0x12 the base constructor set, as the HUD does

			s_menu = this;
			s_lastScreen.x = -1.0f;
			s_lastScreen.y = -1.0f;

			if(!view)
				_ERROR("Interface\\%s.swf did not load: the indicator is off", kMovieName);
		}

		// The game owns this menu and destroys it whenever it tears the
		// interface down - going back to the title screen, loading a save. The
		// pointer above has to go with it.
		//
		// Without this, s_menu outlived the object it pointed at. Available()
		// then read ->view out of a freed Scaleform allocation and, if it liked
		// what it found, called Invoke on a movie that no longer existed. The
		// mod resets itself from kMessage_PreLoadGame, which is exactly when the
		// interface is being pulled down, so the window for it was every single
		// load. Whether it actually went down depended on what had reused that
		// block of heap - which is to say, on the rest of the load order. That
		// is why it took some people and not others.
		virtual ~IndicatorMenu()
		{
			if(s_menu == this)
			{
				s_menu       = NULL;
				s_lastType   = Indicator::kInvisible;
				s_hudOpacity = -1.0f;
				s_lastScreen.x = -1.0f;
				s_lastScreen.y = -1.0f;
			}
		}

		virtual void Accept(CallbackProcessor *) { }

		bool Invoke1(const char * name, GFxValue * arg)  { return SafeInvoke(view, name, arg, 1); }
		bool Invoke3(const char * name, GFxValue * args) { return SafeInvoke(view, name, args, 3); }
	};

	IMenu * CreateIndicatorMenu(void)
	{
		void * memory = ScaleformHeap_Allocate(sizeof(IndicatorMenu));
		if(!memory) return NULL;
		return new (memory) IndicatorMenu();
	}

	// The ledge point, projected into the movie's own coordinates.
	bool ScreenPosition(const NiPoint3 & world, NiPoint2 & out)
	{
		if(!s_menu || !s_menu->view) return false;

		NiPoint3 point = world;
		float x = 0.0f, y = 0.0f, depth = 0.0f;
		if(!WorldPtToScreenPt3(kWorldToCamMatrix, kViewPort, &point, &x, &y, &depth, 1e-5f))
			return false;

		const GRectF rect = s_menu->view->GetVisibleFrameRect();
		out.x = rect.left + (rect.right - rect.left) * x;
		out.y = rect.top + (rect.bottom - rect.top) * (1.0f - y);
		return true;
	}

	// Is our menu open, as far as the game is concerned?
	//
	// The destructor above clears the pointer when the game destroys the menu,
	// and that is the fix. This is the belt to go with it: the mod this is
	// ported from never trusts its own pointer either - every use of its menu
	// reads "menu && menu->IsOpen()" - and it is right to. A pointer says what
	// was true when it was written down; the menu manager says what is true now.
	typedef bool (__thiscall * _IsMenuOpen)(MenuManager *, BSFixedString *);
	const _IsMenuOpen IsMenuOpen = (_IsMenuOpen)0x00A5CE90;

	bool MenuIsOpen()
	{
		MenuManager * menus = Engine::Menus();
		if(!menus) return false;

		// Interned once. Asking the menu manager happens on every tick, and
		// building the string each time would hash and lock the game's string
		// table for an answer that never changes.
		static BSFixedString name(kMenuName);
		return IsMenuOpen(menus, &name);
	}
}

namespace Indicator
{
	void Register()
	{
		MenuManager * menus = Engine::Menus();
		if(!menus)
		{
			_ERROR("no menu manager: the indicator cannot be registered");
			return;
		}

		menus->Register(kMenuName, CreateIndicatorMenu);
		_MESSAGE("indicator menu registered");
	}

	void Show()
	{
		UIManager * ui = Engine::UI();
		if(!ui) return;

		BSFixedString name(kMenuName);
		CALL_MEMBER_FN(ui, AddMessage)(&name, UIMessage::kMessage_Open, NULL);
		CALL_MEMBER_FN(&name, Release)();
	}

	bool Available()
	{
		return s_menu && s_menu->view && MenuIsOpen();
	}

	void SetType(Type type)
	{
		if(s_lastType == type) return;		// the cheap question first
		if(!Available()) return;

		// Coming back from hidden, start at the ledge rather than sliding in
		// from wherever the last one was.
		const bool wasHidden = (s_lastType == kInvisible);
		s_lastType = type;

		GFxValue arg;
		arg.SetNumber((double)type);
		s_menu->Invoke1("_root.SetIndicatorType", &arg);

		if(wasHidden)
			s_lastScreen.x = s_lastScreen.y = -1.0f;
	}

	void ShowDebugOverlay(bool show)
	{
		if(!Available()) return;
		GFxValue arg;
		arg.SetBool(show);
		s_menu->Invoke1("_root.ShowDebugOverlay", &arg);
	}

	void DebugText(const char * text)
	{
		if(!Available() || !Settings::debug) return;
		GFxValue arg;
		arg.SetString(text);
		s_menu->Invoke1("_root.Debug", &arg);
	}

	void Update(int parkourType, const NiPoint3 & ledgePoint)
	{
		if(!Settings::showIndicators)
		{
			SetType(kInvisible);
			return;
		}
		if(!Available()) return;

		Type type = kInvisible;
		switch(parkourType)
		{
			case Parkour::kFailed:		type = kOutOfStamina;	break;
			case Parkour::kVault:		type = kVault;			break;
			case Parkour::kStepHigh:
			case Parkour::kStepLow:		type = kStep;			break;
			case Parkour::kGrab:
			case Parkour::kHigh:
			case Parkour::kHighest:
			case Parkour::kMedium:
			case Parkour::kLow:			type = kClimb;			break;
			default:					type = kInvisible;		break;
		}

		SetType(type);
		if(type == kInvisible) return;

		// Where on screen, a little above the ledge itself.
		NiPoint2 screen;
		if(ScreenPosition(NiPoint3(ledgePoint.x, ledgePoint.y, ledgePoint.z + 8.0f), screen))
		{
			// A long jump means the marker moved to a different ledge: put it
			// there rather than sliding it across the screen.
			bool instant = false;
			if(s_lastScreen.x < 0.0f && s_lastScreen.y < 0.0f) instant = true;
			else
			{
				const float dx = screen.x - s_lastScreen.x;
				const float dy = screen.y - s_lastScreen.y;
				if(sqrtf(dx * dx + dy * dy) > 100.0f) instant = true;
			}
			s_lastScreen = screen;

			GFxValue args[3];
			args[0].SetNumber((double)(int)screen.x);
			args[1].SetNumber((double)(int)screen.y);
			args[2].SetBool(instant);
			s_menu->Invoke3("_root.SetScreenPosition", args);
		}

		// Match the HUD's own opacity setting, and shrink with the third person
		// zoom so the marker keeps its place in the frame.
		const float opacity = Engine::HUDOpacity();
		if(opacity != s_hudOpacity)
		{
			s_hudOpacity = opacity;
			GFxValue arg;
			arg.SetNumber((double)opacity);
			s_menu->Invoke1("_root.SetHudOpacity", &arg);
		}

		void * state = Engine::CameraState();
		float scale = 1.0f;
		if(Engine::CameraStateID(state) == Engine::kCameraState_ThirdPerson)
		{
			float zoom = *(float *)((UInt8 *)state + Engine::ThirdPerson::kCurrentZoomOffset);
			if(zoom < 0.0f) zoom = 0.0f;
			if(zoom > 1.0f) zoom = 1.0f;
			scale = 1.0f - zoom * 0.3f;
		}

		GFxValue zoomArg;
		zoomArg.SetNumber((double)scale);
		s_menu->Invoke1("_root.ScaleIndicator", &zoomArg);
	}
}
