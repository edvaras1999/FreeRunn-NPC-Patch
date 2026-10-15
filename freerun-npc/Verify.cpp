// Verify.cpp - does this game look like the one the port was written against?
//
// Every address in this plugin was read out of one executable, TESV.exe
// 1.9.32.0.8. The runtime version is checked at load and the plugin refuses
// anything else, which covers the honest cases. It does not cover a game that
// reports the right version while being laid out differently - a repack, a
// regional build, a binary someone has patched - and a plugin which goes ahead
// on wrong addresses does not fail where the mistake is. It writes over
// whatever happens to be there and the game dies later, somewhere unrelated,
// differently on every machine.
//
// So before a single hook is installed, the bytes at every address are compared
// against what the generator read out of the reference executable.
//
// THE PART THAT MATTERS: a byte that differs is usually not a different game.
//
// It is another SKSE plugin. A trampoline hook replaces the first five bytes of
// a function with a jump and pads out whatever instruction it cut in half; the
// rest of the function is untouched, and a call still arrives where it should,
// by way of that plugin. The first build of this check called that "wrong
// executable" and shut the mod down on any load order carrying an allocator
// replacement - which is a great many of them.
//
// So a mismatch is only fatal when it is NOT a hook: when the head does not
// begin with a jump, or when the body past the patched prologue has changed
// too. Where it is a hook, the log names the module that placed it, because
// knowing which plugin is sitting in front of the allocator is worth more than
// any amount of guessing from symptoms.

#include "Verify.h"
#include "Signatures.h"
#include "Log.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace
{
	// How much of the head a trampoline may have rewritten. Five bytes for the
	// jump, and up to four more padding out the instruction it landed inside.
	enum { kPrologue = 12 };

	bool Readable(const void * address, size_t length)
	{
		MEMORY_BASIC_INFORMATION info;
		if(!VirtualQuery(address, &info, sizeof(info)))		return false;
		if(info.State != MEM_COMMIT)						return false;
		if(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))		return false;
		return (const UInt8 *)address + length <= (const UInt8 *)info.BaseAddress + info.RegionSize;
	}

	// Which module owns an address - so the log can say who did this.
	const char * ModuleAt(UInt32 address, char * out, size_t size)
	{
		out[0] = 0;
		MEMORY_BASIC_INFORMATION info;
		if(!VirtualQuery((const void *)address, &info, sizeof(info)) || !info.AllocationBase)
			return "an unknown module";

		char path[MAX_PATH];
		if(!GetModuleFileNameA((HMODULE)info.AllocationBase, path, sizeof(path)))
			return "an unknown module";

		const char * leaf = strrchr(path, '\\');
		strncpy_s(out, size, leaf ? leaf + 1 : path, _TRUNCATE);
		return out;
	}

	// Does the head look like a jump someone planted?
	bool HeadIsAJump(const unsigned char * b)
	{
		if(b[0] == 0xE9) return true;					// jmp rel32
		if(b[0] == 0xE8) return true;					// call rel32
		if(b[0] == 0xEB) return true;					// jmp rel8
		if(b[0] == 0xFF && b[1] == 0x25) return true;	// jmp [address]
		if(b[0] == 0x68 && b[5] == 0xC3) return true;	// push addr; ret
		return false;
	}

	// Where a jmp rel32 lands, so the module behind it can be named.
	UInt32 JumpTarget(UInt32 at, const unsigned char * b)
	{
		if(b[0] == 0xE9 || b[0] == 0xE8)
			return at + 5 + *(const SInt32 *)(b + 1);
		if(b[0] == 0xFF && b[1] == 0x25)
		{
			const UInt32 * slot = *(const UInt32 **)(b + 2);
			return Readable(slot, 4) ? *slot : 0;
		}
		if(b[0] == 0x68)
			return *(const UInt32 *)(b + 1);
		return 0;
	}

	void Describe(char * out, size_t size, const unsigned char * bytes, unsigned char length)
	{
		out[0] = 0;
		for(unsigned char i = 0; i < length; ++i)
		{
			char one[4];
			sprintf_s(one, sizeof(one), "%02X ", bytes[i]);
			strcat_s(out, size, one);
		}
	}
}

namespace Verify
{
	Result Run()
	{
		Result result;
		result.checked      = Signatures::kSiteCount;
		result.codeWrong    = 0;
		result.codeHooked   = 0;
		result.tablesHooked = 0;
		result.unreadable   = 0;

		for(UInt32 i = 0; i < Signatures::kSiteCount; ++i)
		{
			const Signatures::Site & site = Signatures::kSites[i];
			const unsigned char * at = (const unsigned char *)site.address;

			if(!Readable(at, site.length ? site.length : 4))
			{
				++result.unreadable;
				_ERROR("verify: %s (%s) at %08X is not mapped at all",
					   site.name, site.where, site.address);
				continue;
			}

			if(site.kind == Signatures::kVariable || site.length == 0)
				continue;

			if(memcmp(at, site.bytes, site.length) == 0)
				continue;

			char owner[MAX_PATH];
			char wanted[128], got[128];
			Describe(wanted, sizeof(wanted), site.bytes, site.length);
			Describe(got, sizeof(got), at, site.length);

			if(site.kind == Signatures::kTable)
			{
				// A vtable slot pointing somewhere new is another plugin that
				// got there first. The hooks here read whatever is in the slot
				// and call through to it, so this is how it is meant to work.
				++result.tablesHooked;
				_MESSAGE("verify: %s (%s) at %08X - a slot was taken by %s, and this mod will chain through it",
						 site.name, site.where, site.address,
						 ModuleAt(*(const UInt32 *)(at + 4), owner, sizeof(owner)));
				continue;
			}

			// A function. Is the difference a trampoline, or is this simply not
			// the function this port was built against?
			const bool jump = HeadIsAJump(at);
			const bool bodyIntact = site.length > kPrologue &&
									memcmp(at + kPrologue, site.bytes + kPrologue,
										   site.length - kPrologue) == 0;

			// A jump in the head is a hook, and that is the end of it. The call
			// made from here goes through whoever placed it and arrives where
			// it should, so there is nothing to stop for.
			//
			// The state of the body is logged but does not change the verdict.
			// Refusing to run is the worse outcome of the two: the first build
			// of this check was stricter and shut the mod down on every load
			// order carrying an allocator replacement.
			if(jump)
			{
				++result.codeHooked;
				const UInt32 target = JumpTarget(site.address, at);
				_MESSAGE("verify: %s (%s) at %08X is hooked by %s - the call goes through it and arrives%s",
						 site.name, site.where, site.address,
						 target ? ModuleAt(target, owner, sizeof(owner)) : "another plugin",
						 bodyIntact ? ", and the rest of the function is untouched"
									: " (note: the body past the prologue differs too)");
				continue;
			}

			// No jump, and the bytes are not ours: this really is a different
			// function from the one the port was written against.
			++result.codeWrong;
			_ERROR("verify: %s (%s) at %08X IS NOT WHAT THIS PLUGIN WAS BUILT AGAINST\n"
				   "        expected %s\n        found    %s\n"
				   "        (no hook here - the head is not a jump, so this is not another plugin)",
				   site.name, site.where, site.address, wanted, got);
		}

		result.ok = (result.codeWrong == 0 && result.unreadable == 0);

		_MESSAGE("verify: %d sites - %d wrong, %d unreadable, %d functions hooked by other plugins, "
				 "%d vtable slots taken by other plugins",
				 result.checked, result.codeWrong, result.unreadable,
				 result.codeHooked, result.tablesHooked);

		if(result.ok)
			_MESSAGE("verify: this is the executable the port was built against");

		return result;
	}
}
