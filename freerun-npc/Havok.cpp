#include "Havok.h"
#include "Engine.h"

#include "skse/GameReferences.h"
#include "skse/GameRTTI.h"

#include <float.h>

namespace
{

	// Gives the slot a hit at `fraction` should be written to, or NULL when the
	// result already holds kMaxHits hits that are all nearer than this one.
	Havok::Hit * SlotFor(Havok::Result & result, float fraction)
	{
		if(result.hitCount < Havok::kMaxHits)
			return &result.hits[result.hitCount++];

		UInt32 farthest = 0;
		for(UInt32 i = 1; i < Havok::kMaxHits; ++i)
			if(result.hits[i].fraction > result.hits[farthest].fraction)
				farthest = i;

		if(fraction >= result.hits[farthest].fraction)
			return NULL;

		return &result.hits[farthest];
	}

	// ------------------------------------------------------------ addresses

	// bhkWorld's virtual table: PickObject is slot 0x2F and GetWorld1 slot
	// 0x23 (the vanilla ray cast at 0x006BF6E0 calls +0xBC, and PickObject
	// itself calls +0x8C to reach the hkpWorld).
	enum { kBhkWorld_GetWorld1 = 0x23, kBhkWorld_PickObject = 0x2F };

	// The bhkWorld of a cell: interior cells keep it in their ExtraHavok, the
	// exterior one lives in a global. 0x004C5430 answers for both.
	typedef void * (__fastcall * _CellBhkWorld)(TESObjectCELL * cell);
	const _CellBhkWorld CellBhkWorld = (_CellBhkWorld)0x004C5430;

	typedef void (__fastcall * _RayCollectorCtor)(void * self);
	const _RayCollectorCtor RayCollectorCtor = (_RayCollectorCtor)0x0045E010;
	const _RayCollectorCtor RayCollectorDtor = (_RayCollectorCtor)0x0045E140;

	// bhkCapsuleShape(this, vertexA, vertexB, radius) - takes game units and
	// scales into Havok itself, then builds the hkpCapsuleShape behind it.
	typedef void * (__thiscall * _CapsuleCtor)(void * self, const NiPoint3 * a, const NiPoint3 * b, float radius);
	const _CapsuleCtor CapsuleShapeCtor = (_CapsuleCtor)0x00D3F300;

	// hkpWorld::linearCast(collidable, input, castCollector, startCollector)
	typedef void (__thiscall * _LinearCast)(void * world, const void * collidable, const void * input,
											void * castCollector, void * startCollector);
	const _LinearCast LinearCast = (_LinearCast)0x00D4F190;

	// TESHavokUtilities::FindCollidableRef
	typedef TESObjectREFR * (__cdecl * _FindCollidableRef)(const void * collidable);
	const _FindCollidableRef FindCollidableRef = (_FindCollidableRef)0x004726B0;

	// The Havok allocator the hit arrays grow from. The object lives at this
	// address and its free is vtable slot 4, thiscall(this, memory, bytes) -
	// read out of the collector destructor at 0x00584E8A.
	void * const kHavokAllocator = (void *)0x012D1C00;

	void HavokFree(void * memory, UInt32 bytes)
	{
		if(!memory) return;
		typedef void (__thiscall * _Free)(void *, void *, UInt32);
		_Free free = (_Free)((void **)(*(void ***)kHavokAllocator))[4];
		free(kHavokAllocator, memory, bytes);
	}

	// ---------------------------------------------------------------- types

	// hkpAllRayHitTempCollector: the constructor above lays it out, each hit is
	// 0x60 bytes with the fraction at +0x10 and the collidable at +0x50.
	struct RayCollector
	{
		UInt8	raw[0x320];

		void *	Data() const	{ return *(void **)(raw + 0x10); }
		UInt32	Size() const	{ return *(UInt32 *)(raw + 0x14); }
		UInt8 *	At(UInt32 i) const	{ return (UInt8 *)Data() + i * 0x60; }
	};

	// hkpAllCdPointCollector. Built by hand rather than with the engine's
	// constructor at 0x005851B0, because that one also builds a phantom and a
	// shape for the engine's own sphere caster.
	struct PointCollector
	{
		// What the engine's own constructor writes: an early-out distance no
		// contact can beat, and room for sixteen contacts before the array
		// would have to reach for the heap.
		enum { kEarlyOut = 0x7F7FFFEE, kInplace = 16 };

		void *	vtbl;				// 0x00
		float	earlyOutDistance;	// 0x04
		UInt32	pad08[2];			// 0x08
		void *	data;				// 0x10
		UInt32	size;				// 0x14
		UInt32	capacity;			// 0x18
		UInt32	pad1C;				// 0x1C
		UInt8	storage[kInplace * 0x30];	// 0x20

		void Init()
		{
			vtbl				= (void *)0x010A7C74;
			*(UInt32 *)&earlyOutDistance = kEarlyOut;
			data				= storage;
			size				= 0;
			capacity			= 0x80000000 | kInplace;	// inplace storage, nothing owned
			pad08[0] = pad08[1] = 0;
			pad1C = 0;
		}

		void Release()
		{
			// The array only owns memory if it had to grow past the inplace
			// storage, which clears the flag - the same test the engine's own
			// destructor makes at 0x00584E70.
			if((SInt32)capacity >= 0)
			{
				HavokFree(data, (capacity & 0x3FFFFFFF) * 0x30);
				data = NULL;
				capacity = 0x80000000;
			}
			size = 0;
		}

		UInt8 * At(UInt32 i) const { return (UInt8 *)data + i * 0x30; }
	};

	// hkpCollidable. The offsets are the ones FindCollidableRef reads:
	// ownerOffset at +0x10, the broad phase type at +0x18 and the collision
	// filter at +0x1C.
	struct Collidable
	{
		const void *	shape;			// 0x00
		UInt32			shapeKey;		// 0x04
		const void *	motion;			// 0x08 - an hkTransform for a free-standing shape
		const void *	parent;			// 0x0C
		SInt8			ownerOffset;	// 0x10
		UInt8			forceCollideOntoPpu;
		UInt16			shapeSizeOnSpu;
		UInt32			broadPhaseId;	// 0x14
		UInt8			broadPhaseType;	// 0x18
		SInt8			broadPhaseOwnerOffset;
		UInt8			objectQualityType;
		UInt8			pad1B;
		UInt32			collisionFilterInfo;	// 0x1C
		UInt8			boundingVolumeData[0x38];
		float			allowedPenetrationDepth;
	};

	struct LinearCastInput
	{
		float	to[4];					// 0x00
		float	maxExtraPenetration;	// 0x10
		float	startPointTolerance;	// 0x14
	};

	// ------------------------------------------------------------- helpers

	NiPoint3 Normalised(const NiPoint3 & v)
	{
		const float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
		if(len <= 1e-6f) return NiPoint3(0.0f, 0.0f, 0.0f);
		return NiPoint3(v.x / len, v.y / len, v.z / len);
	}

	// The caster's own filter, with the layer swapped for the one we want to
	// collide as. Keeping the caster's system group is what stops every cast
	// from hitting the player.
	UInt32 CastFilter(Actor * caster, UInt32 layer)
	{
		UInt32 filter = 0;
		if(caster)
		{
			void * ctrl = Engine::CharController(caster);
			if(ctrl) filter = Engine::ControllerCollisionFilter(ctrl);
		}
		return (filter & ~0x7F) | (layer & 0x7F);
	}

	void * WorldOf(Actor * caster)
	{
		if(!caster) return NULL;
		TESObjectCELL * cell = Engine::ParentCell(caster);
		if(!cell) return NULL;
		return CellBhkWorld(cell);
	}

	void SortHits(Havok::Result & result)
	{
		for(UInt32 i = 1; i < result.hitCount; ++i)
		{
			Havok::Hit key = result.hits[i];
			SInt32 j = (SInt32)i - 1;
			while(j >= 0 && result.hits[j].fraction > key.fraction)
			{
				result.hits[j + 1] = result.hits[j];
				--j;
			}
			result.hits[j + 1] = key;
		}
	}
}

namespace Havok
{
	Result RayCast(const NiPoint3 & start, const NiPoint3 & direction, float distance, UInt32 layer, Actor * caster)
	{
		Result result;
		const NiPoint3 dir = Normalised(direction);

		result.start		= start;
		result.direction	= dir;
		result.distance		= distance;
		result.hitPosition	= NiPoint3(start.x + dir.x * distance, start.y + dir.y * distance, start.z + dir.z * distance);

		void * world = WorldOf(caster);
		if(!world || distance <= 0.0f) return result;

		const float scale = Engine::HavokScale();
		const float inv   = Engine::HavokScaleInverse();

		__declspec(align(16)) UInt8 pickData[0xC0];
		memset(pickData, 0, sizeof(pickData));

		RayCollector collector;
		RayCollectorCtor(&collector);

		float * from = (float *)(pickData + 0x00);
		float * to   = (float *)(pickData + 0x10);
		from[0] = start.x * scale;	from[1] = start.y * scale;	from[2] = start.z * scale;	from[3] = 0.0f;
		to[0] = result.hitPosition.x * scale;
		to[1] = result.hitPosition.y * scale;
		to[2] = result.hitPosition.z * scale;
		to[3] = 0.0f;

		*(UInt32 *)(pickData + 0x20) = 0;							// no shape collection filter
		*(UInt32 *)(pickData + 0x24) = CastFilter(caster, layer);

		// The output block starts out as "nothing hit", exactly as the vanilla
		// caller at 0x006BF6E0 primes it.
		*(float  *)(pickData + 0x40) = 1.0f;						// hit fraction
		*(UInt32 *)(pickData + 0x44) = 0xFFFFFFFF;					// extra info
		*(UInt32 *)(pickData + 0x48) = 0xFFFFFFFF;					// first shape key
		*(UInt32 *)(pickData + 0x50) = 0xFFFFFFFF;
		*(UInt32 *)(pickData + 0x70) = 0;							// shape key index
		*(UInt32 *)(pickData + 0x80) = 0;							// root collidable
		*(void  **)(pickData + 0xAC) = &collector;					// all hits, not just the nearest
		*(UInt8  *)(pickData + 0xB0) = 0;

		typedef bool (__thiscall * _PickObject)(void *, void *);
		const bool picked = (*(_PickObject **)world)[kBhkWorld_PickObject](world, pickData) &&
							*(float *)(pickData + 0x40) < 1.0f;

		const UInt32 count = collector.Size();
		for(UInt32 i = 0; i < count; ++i)
		{
			const UInt8 * entry = collector.At(i);
			const float fraction = *(const float *)(entry + 0x10);
			const void * collidable = *(const void **)(entry + 0x50);
			if(!collidable) continue;

			Havok::Hit * slot = SlotFor(result, fraction);
			if(!slot) continue;

			Hit & hit = *slot;
			hit.fraction	= fraction;
			hit.distance	= distance * fraction;
			hit.position	= NiPoint3(start.x + dir.x * hit.distance, start.y + dir.y * hit.distance, start.z + dir.z * hit.distance);
			const float * n = (const float *)(entry + 0x00);
			hit.normal		= NiPoint3(n[0], n[1], n[2]);
			hit.layer		= *(const UInt32 *)((const UInt8 *)collidable + 0x1C) & 0x7F;
			hit.ref			= FindCollidableRef(collidable);
		}

		SortHits(result);

		if(picked)
		{
			result.didHit		= true;
			result.distance		= distance * *(float *)(pickData + 0x40);
			result.hitPosition	= NiPoint3(start.x + dir.x * result.distance,
										   start.y + dir.y * result.distance,
										   start.z + dir.z * result.distance);
			const float * n = (const float *)(pickData + 0x30);
			result.normal		= NiPoint3(n[0], n[1], n[2]);
			const void * root = *(const void **)(pickData + 0x80);
			if(root)
			{
				result.layer	= *(const UInt32 *)((const UInt8 *)root + 0x1C) & 0x7F;
				result.ref		= FindCollidableRef(root);
			}
		}
		else if(result.hitCount)
		{
			// PickObject reports "no hit" when the nearest hit was filtered out
			// of its own output, but the collector still holds the list.
			result.didHit		= true;
			result.distance		= result.hits[0].distance;
			result.hitPosition	= result.hits[0].position;
			result.normal		= result.hits[0].normal;
			result.layer		= result.hits[0].layer;
			result.ref			= result.hits[0].ref;
		}

		(void)inv;
		RayCollectorDtor(&collector);
		return result;
	}

	Result ShapeCast(const NiPoint3 & start, const NiPoint3 & direction, float distance, float radius, UInt32 layer, Actor * caster)
	{
		Result result;
		const NiPoint3 dir = Normalised(direction);

		result.start		= start;
		result.direction	= dir;
		result.distance		= distance;
		result.hitPosition	= NiPoint3(start.x + dir.x * distance, start.y + dir.y * distance, start.z + dir.z * distance);

		void * world = WorldOf(caster);
		if(!world || distance <= 0.0f || radius <= 0.0f) return result;

		// A capsule whose two vertices sit on top of each other is a sphere,
		// which is what every cast in this plugin wants: no orientation to get
		// wrong as the cast direction changes.
		const NiPoint3 zero(0.0f, 0.0f, 0.0f);
		void * shapeWrapper = Engine::HeapAllocate(0x14, 0, false);
		if(!shapeWrapper) return result;
		CapsuleShapeCtor(shapeWrapper, &zero, &zero, radius);

		void * shape = *(void **)((UInt8 *)shapeWrapper + 0x08);	// bhkRefObject::referencedObject
		if(!shape)
		{
			Engine::HeapFree(shapeWrapper, false);
			return result;
		}

		const float scale = Engine::HavokScale();

		// hkTransform: three rotation columns then the translation.
		__declspec(align(16)) float transform[16];
		memset(transform, 0, sizeof(transform));
		transform[0] = 1.0f; transform[5] = 1.0f; transform[10] = 1.0f;
		transform[12] = start.x * scale;
		transform[13] = start.y * scale;
		transform[14] = start.z * scale;

		Collidable collidable;
		memset(&collidable, 0, sizeof(collidable));
		collidable.shape				= shape;
		collidable.shapeKey				= 0xFFFFFFFF;
		collidable.motion				= transform;
		collidable.parent				= NULL;
		collidable.ownerOffset			= 0;
		collidable.broadPhaseType		= 0;
		collidable.collisionFilterInfo	= CastFilter(caster, layer);
		collidable.allowedPenetrationDepth = 0.0f;

		LinearCastInput input;
		input.to[0] = result.hitPosition.x * scale;
		input.to[1] = result.hitPosition.y * scale;
		input.to[2] = result.hitPosition.z * scale;
		input.to[3] = 0.0f;
		input.maxExtraPenetration	= 0.01f;
		input.startPointTolerance	= 0.01f;

		PointCollector collector;
		collector.Init();

		typedef void * (__thiscall * _GetWorld1)(void *);
		void * hkWorld = (*(_GetWorld1 **)world)[kBhkWorld_GetWorld1](world);
		if(hkWorld)
			LinearCast(hkWorld, &collidable, &input, &collector, NULL);

		const UInt32 count = collector.size;
		for(UInt32 i = 0; i < count; ++i)
		{
			const UInt8 * entry = collector.At(i);
			const float * position	= (const float *)(entry + 0x00);
			const float * normal	= (const float *)(entry + 0x10);
			// The fourth component of the separating normal is how far along
			// the cast the contact happened.
			const float fraction	= normal[3];
			const void * collidableB = *(const void **)(entry + 0x28);
			if(!collidableB) continue;

			// A swept shape against a piece of architecture produces a contact
			// per face it touches, so this one overflows even more readily than
			// the ray does.
			Havok::Hit * slot = SlotFor(result, fraction);
			if(!slot) continue;

			Hit & hit = *slot;
			hit.fraction	= fraction;
			hit.distance	= distance * fraction;
			hit.position	= NiPoint3(position[0] * Engine::HavokScaleInverse(),
									   position[1] * Engine::HavokScaleInverse(),
									   position[2] * Engine::HavokScaleInverse());
			hit.normal		= NiPoint3(normal[0], normal[1], normal[2]);
			hit.layer		= *(const UInt32 *)((const UInt8 *)collidableB + 0x1C) & 0x7F;
			hit.ref			= FindCollidableRef(collidableB);
		}

		SortHits(result);

		if(result.hitCount)
		{
			result.didHit		= true;
			result.distance		= result.hits[0].distance;
			result.hitPosition	= result.hits[0].position;
			result.normal		= result.hits[0].normal;
			result.layer		= result.hits[0].layer;
			result.ref			= result.hits[0].ref;
		}

		collector.Release();

		// The shape was made for this one cast: hand it straight back. The
		// wrapper's deleting destructor releases the hkpCapsuleShape behind it
		// and frees the wrapper from the same heap it came from.
		typedef void (__thiscall * _DeleteThis)(void *, UInt8);
		(*(_DeleteThis **)shapeWrapper)[0](shapeWrapper, 1);

		return result;
	}
}
