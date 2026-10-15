#include "Engine.h"

#include "skse/GameReferences.h"

// The SDK declares this as a pointer to the engine's own lookup and defines it
// in GameReferences.cpp - a file that drags in half the reference layer for
// three functions this plugin never calls. The one symbol the menu code needs
// is defined here instead, at the address the SDK itself uses.
const _LookupREFRByHandle LookupREFRByHandle = (_LookupREFRByHandle)0x004A9180;

namespace Engine
{
	// The engine's own message box, reached the way Papyrus' Debug.MessageBox
	// reaches it (0x008EE530 -> 0x0087AC60): text, no callback, no buttons of
	// its own beyond the default.
	void MessageBox(const char * text)
	{
		if(!text) return;

		typedef void (__cdecl * fn_t)(const char *, void *, UInt32, UInt32, UInt32, void *, UInt32);
		((fn_t)0x0087AC60)(text, NULL, 0, 4, 0x0A, (void *)0x01094A2C, 0);
	}
}
