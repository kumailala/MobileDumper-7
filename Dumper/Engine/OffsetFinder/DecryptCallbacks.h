#pragma once

#include <cstdint>
#include <functional>

#include "../Unreal/Enums.h"

/**
 * @brief Per-offset value decryption hooks.
 *
 * Each callback receives a value already obtained from the target and the address it came from, and returns
 *
 * Callbacks take a value rather than performing the read so they apply equally to a direct read and to a
 * value decoded from a bulk-read block during layout detection, where no per-field read happens at all.
 */
struct FDecryptCallbacks
{
	/// @brief Hooks for FName fields.
	struct FNameCallbacks
	{
		/// @brief Decrypts a comparison index.
		std::function<int32(int32, uintptr_t)> CompIdx = [](int32 Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts a name number.
		std::function<uint32(uint32, uintptr_t)> Number = [](uint32 Value, uintptr_t)
		{ return Value; };
	} FName;

	/// @brief Hooks for UObject fields.
	struct FUObjectCallbacks
	{
		/// @brief Decrypts an object flags field.
		std::function<EObjectFlags(EObjectFlags, uintptr_t)> Flags = [](EObjectFlags Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts an internal object index.
		std::function<int32(int32, uintptr_t)> Index = [](int32 Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts a UClass pointer.
		std::function<uintptr_t(uintptr_t, uintptr_t)> Class = [](uintptr_t Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts an outer UObject pointer.
		std::function<uintptr_t(uintptr_t, uintptr_t)> Outer = [](uintptr_t Value, uintptr_t)
		{ return Value; };
	} UObject;

	/// @brief Hooks for a legacy indirect name array.
	struct FNameArrayCallbacks
	{
		/// @brief Decrypts the chunk pointer array base.
		std::function<uintptr_t(uintptr_t, uintptr_t)> Chunks = [](uintptr_t Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts the live name count.
		std::function<int32(int32, uintptr_t)> NumElements = [](int32 Value, uintptr_t)
		{ return Value; };

		/// @brief Hooks for fields within a single FNameEntry.
		struct FNameEntryCallbacks
		{
			/// @brief Decrypts the packed `(Index << 1) | bIsWide` field.
			std::function<uint32(uint32, uintptr_t)> Index = [](uint32 Value, uintptr_t)
			{ return Value; };
		} FNameEntry;
	} NameArray;

	/// @brief Hooks for an FNamePool.
	struct FNamePoolCallbacks
	{
		/// @brief Decrypts the block pointer array base.
		std::function<uintptr_t(uintptr_t, uintptr_t)> Blocks = [](uintptr_t Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts the highest allocated block index.
		std::function<int32(int32, uintptr_t)> MaxChunkIndex = [](int32 Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts the write cursor within the current block.
		std::function<int32(int32, uintptr_t)> ByteCursor = [](int32 Value, uintptr_t)
		{ return Value; };

		/// @brief Hooks for fields within a single packed FNameEntry.
		struct FNameEntryCallbacks
		{
			/// @brief Decrypts the header holding length and flags.
			std::function<uint16(uint16, uintptr_t)> Header = [](uint16 Value, uintptr_t)
			{ return Value; };
		} FNameEntry;
	} NamePool;

	/// @brief Hooks for a contiguous object array.
	struct FFixedObjectsCallbacks
	{
		/// @brief Decrypts the pointer to the item allocation.
		std::function<uintptr_t(uintptr_t, uintptr_t)> Objects = [](uintptr_t Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts the live object count.
		std::function<int32(int32, uintptr_t)> NumObjects = [](int32 Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts the allocation capacity.
		std::function<int32(int32, uintptr_t)> MaxObjects = [](int32 Value, uintptr_t)
		{ return Value; };

		/// @brief Hooks for fields within a single FUObjectItem.
		struct FUObjectItemCallbacks
		{
			/// @brief Decrypts the UObject pointer within an item.
			std::function<uintptr_t(uintptr_t, uintptr_t)> Object = [](uintptr_t Value, uintptr_t)
			{ return Value; };
		} FUObjectItem;
	} FixedObjects;

	/// @brief Hooks for a chunked object array.
	struct FChunkedObjectsCallbacks
	{
		/// @brief Decrypts the pointer to the chunk pointer table.
		std::function<uintptr_t(uintptr_t, uintptr_t)> Objects = [](uintptr_t Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts the live object count.
		std::function<int32(int32, uintptr_t)> NumElements = [](int32 Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts the total capacity across all chunks.
		std::function<int32(int32, uintptr_t)> MaxElements = [](int32 Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts the chunk table capacity.
		std::function<int32(int32, uintptr_t)> MaxChunks = [](int32 Value, uintptr_t)
		{ return Value; };
		/// @brief Decrypts the allocated chunk count.
		std::function<int32(int32, uintptr_t)> NumChunks = [](int32 Value, uintptr_t)
		{ return Value; };

		/// @brief Hooks for fields within a single FUObjectItem.
		struct FUObjectItemCallbacks
		{
			/// @brief Decrypts the UObject pointer within an item.
			std::function<uintptr_t(uintptr_t, uintptr_t)> Object = [](uintptr_t Value, uintptr_t)
			{ return Value; };
		} FUObjectItem;
	} ChunkedObjects;
};

/// @brief Active value decryption hooks.
extern FDecryptCallbacks GDecryptCallbacks;
