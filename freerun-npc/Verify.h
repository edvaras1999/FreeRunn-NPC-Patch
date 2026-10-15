// Verify.h - the startup check that the game is the one the port was built for.
//
// See Verify.cpp for what each kind of address means and why a vtable mismatch
// is not an error while a code mismatch is.

#pragma once

#include "skse/GameTypes.h"

namespace Verify
{
	struct Result
	{
		bool	ok;				// nothing found that makes it unsafe to continue
		UInt32	checked;
		UInt32	codeWrong;		// a function is genuinely not what it should be: fatal
		UInt32	codeHooked;		// a function another plugin has trampolined: fine
		UInt32	unreadable;		// an address is not even mapped: fatal
		UInt32	tablesHooked;	// a vtable slot another plugin got to first: fine
	};

	Result Run();
}
