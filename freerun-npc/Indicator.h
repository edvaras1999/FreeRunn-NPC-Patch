// Indicator.h - the on-screen marker that says what the ledge in front of you
// would do.
//
// It is a menu of its own: a Scaleform movie registered with the menu manager,
// always open, drawn with the HUD, and told where to sit on screen each frame by
// projecting the ledge point through the game's own camera matrix.
//
// The movie is the same one the Special Edition build uses, and it speaks the
// same five calls. Nothing else about the menu carries over - Legendary has no
// IMenu base to inherit from in the modern sense, so the menu is built the way
// this edition's own menus are.

#pragma once

#include "skse/NiTypes.h"

namespace Indicator
{
	enum Type
	{
		kInvisible = 0,
		kClimb,
		kVault,
		kOutOfStamina,
		kStep,
	};

	// Registers the menu with the menu manager. Called once, when the interface
	// is up but before a save is loaded.
	void Register();

	// Asks the interface to open it. It never closes again.
	void Show();

	bool Available();

	void SetType(Type type);
	void Update(int parkourType, const NiPoint3 & ledgePoint);

	// Debug text, shown only while the debug setting is on.
	void ShowDebugOverlay(bool show);
	void DebugText(const char * text);
}
