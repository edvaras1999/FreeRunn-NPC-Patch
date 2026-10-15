// Papyrus.h - the bridge the mod configuration menu talks over.
//
// Special Edition draws its settings with an ImGui overlay from the SKSE Menu
// Framework. Legendary has no such thing, and does not need one: SkyUI's mod
// configuration menu is the established place for settings, and it is written
// in Papyrus. These functions are what its script reads and writes; the values
// themselves live in the INI, so the mod is fully configurable even without
// SkyUI installed.

#pragma once

class VMClassRegistry;

namespace Papyrus
{
	bool Register(VMClassRegistry * registry);
}
