#include "EngineVersionDetect.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>


namespace UEAnalyzerKitty
{
	namespace
	{
		/*
		 * FEngineVersionBase { uint16 Major, Minor, Patch; uint32 Changelist; } followed by
		 * FString Branch, itself a TArray<TCHAR>: { TCHAR* Data; int32 Num; int32 Max; }.
		 *
		 * The base is 12 bytes on both architectures, but everything after it moves with the pointer
		 * width, so those offsets are derived. The dumper is built for the architecture it targets.
		 */
		constexpr size_t kPtrSize = sizeof(void*);

		constexpr size_t AlignUpTo(size_t Value, size_t Alignment)
		{
			return ((Value + Alignment - 1) / Alignment) * Alignment;
		}

		constexpr size_t kOffsetMajor      = 0x0;
		constexpr size_t kOffsetMinor      = 0x2;
		constexpr size_t kOffsetPatch      = 0x4;
		constexpr size_t kOffsetBasePad    = 0x6;
		constexpr size_t kOffsetChangelist = 0x8;
		constexpr size_t kSizeOfBase       = 0xC;

		constexpr size_t kOffsetBranchData = AlignUpTo(kSizeOfBase, kPtrSize);
		constexpr size_t kOffsetBranchNum  = kOffsetBranchData + kPtrSize;
		constexpr size_t kOffsetBranchMax  = kOffsetBranchNum + sizeof(int32_t);
		constexpr size_t kSizeOfVersion    = AlignUpTo(kOffsetBranchMax + sizeof(int32_t), kPtrSize);

		static_assert(kPtrSize != 8 || (kOffsetBranchData == 0x10 && kOffsetBranchNum == 0x18 && kSizeOfVersion == 0x20), "64-bit FEngineVersion layout changed");
		static_assert(kPtrSize != 4 || (kOffsetBranchData == 0x0C && kOffsetBranchNum == 0x10 && kSizeOfVersion == 0x18), "32-bit FEngineVersion layout changed");

		/* Wide enough for any real version; beyond this is other data. */
		constexpr uint16_t kMaxMinor = 40;
		constexpr uint16_t kMaxPatch = 200;

		/* A branch is short; anything longer is a pointer into something else. */
		constexpr int32_t kMinBranchLen = 2;
		constexpr int32_t kMaxBranchLen = 64;

		/*
		 * The shape test admits any small FString, and module data is full of them - texture format
		 * names and the like - so false hits are the norm and the real version can sit behind hundreds
		 * of them. This only bounds a pathological image; the scan normally stops early on finding
		 * both versions, so the budget is generous rather than tight.
		 */
		constexpr size_t kMaxBranchFetches = 4096;

		/* One match covers both versions, since the pair test spans Current and CompatibleWith. */
		constexpr size_t kExpectedCandidates = 1;

		/* Writable module data is a few MB, so one chunk usually covers a whole segment. */
		constexpr size_t kChunkBytes = 1u * 1024u * 1024u;

		/* Step past an unreadable hole without giving up on the rest of the chunk. */
		constexpr size_t kPageBytes = 4096u;

		/*
		 * FEngineVersion::Current() hands back a member of this lazy singleton. The object's own symbol
		 * names it outright; its guard variable shares the same static storage and so brackets it even
		 * when the object symbol is absent, which happens.
		 */
		constexpr const char* kSingletonSymbol = "_ZZN14TLazySingletonI21FGlobalEngineVersionsE7GetLazyEPFvPvEE9Singleton";
		constexpr const char* kGuardSymbol     = "_ZGVZN14TLazySingletonI21FGlobalEngineVersionsE7GetLazyEPFvPvEE9Singleton";

		/* How far below the guard to look for the object it guards. */
		constexpr size_t kGuardWindow = 0x1000;

		template <typename T>
		T Load(const uint8_t* At)
		{
			T Value{};
			std::memcpy(&Value, At, sizeof(T));
			return Value;
		}

		/// @brief Decodes @p Bytes at @p CharSize per character, or {} if it is not printable text.
		std::string DecodeBranch(const uint8_t* Bytes, int32_t Num, size_t CharSize)
		{
			std::string Out;
			Out.reserve(static_cast<size_t>(Num));

			for (int32_t i = 0; i < Num; i++)
			{
				const uint32_t Char = CharSize == 2 ? Load<uint16_t>(Bytes + (i * 2)) : Load<uint32_t>(Bytes + (i * 4));

				if (Char == 0)
					break;

				if (Char < 0x20 || Char >= 0x7F)
					return {};

				Out += static_cast<char>(Char);
			}

			return Out;
		}

		/**
		 * @brief Decodes the branch at whichever TCHAR width accounts for the whole string.
		 *
		 * FString's Num counts the terminator, so the right width ends at Num - 1 and nowhere earlier.
		 * Reading 4-byte TCHARs at 2-byte width stops on the first zero high half instead, which yields
		 * a plausible one-character branch rather than an empty one - so an empty decode cannot be the
		 * signal to retry at the other width. @p BytesRead bounds both attempts to bytes actually read.
		 */
		std::string DecodeBranchAnyWidth(const uint8_t* Bytes, int32_t Num, size_t BytesRead)
		{
			for (const size_t CharSize : { static_cast<size_t>(2), static_cast<size_t>(4) })
			{
				if (static_cast<size_t>(Num) * CharSize > BytesRead)
					continue;

				const std::string Decoded = DecodeBranch(Bytes, Num, CharSize);

				/* Num - 1 where the terminator is counted, Num where a fork stores none. */
				if (Decoded.size() == static_cast<size_t>(Num) - 1 || Decoded.size() == static_cast<size_t>(Num))
					return Decoded;
			}

			return {};
		}

		/// @brief Tests only FEngineVersionBase: a plausible triple with its alignment padding zeroed.
		bool MatchesBase(const uint8_t* At)
		{
			const uint16_t Major = Load<uint16_t>(At + kOffsetMajor);

			if (Major != 4 && Major != 5)
				return false;

			if (Load<uint16_t>(At + kOffsetBasePad) != 0)
				return false;

			return Load<uint16_t>(At + kOffsetMinor) <= kMaxMinor && Load<uint16_t>(At + kOffsetPatch) <= kMaxPatch;
		}

		/**
		 * @brief Tests for the Current/CompatibleWith pair FGlobalEngineVersions holds.
		 *
		 * The two sit one struct apart and agree on Major and Minor, with the compatible-with patch at
		 * zero. That pairing is what identifies the version: the base alone matches small integer data
		 * everywhere, and the branch string cannot be relied on - a fork may rename or empty it.
		 */
		bool MatchesVersionPair(const uint8_t* At)
		{
			if (!MatchesBase(At) || !MatchesBase(At + kSizeOfVersion))
				return false;

			if (Load<uint16_t>(At + kOffsetMajor) != Load<uint16_t>(At + kSizeOfVersion + kOffsetMajor))
				return false;

			if (Load<uint16_t>(At + kOffsetMinor) != Load<uint16_t>(At + kSizeOfVersion + kOffsetMinor))
				return false;

			if (Load<uint16_t>(At + kSizeOfVersion + kOffsetPatch) != 0)
				return false;

			/* An all-zero triple pairs with itself trivially, and zeroed data is everywhere, so a
			   version that is entirely Major.0.0 carries no evidence. */
			return Load<uint16_t>(At + kOffsetMinor) != 0 || Load<uint16_t>(At + kOffsetPatch) != 0;
		}

		/**
		 * @brief Reads and validates one FEngineVersion straight from the target.
		 *
		 * Used where an address is already known, so it can afford a handful of reads. @p bRequireBranch
		 * is false for a symbol-named address, which needs no corroboration.
		 */
		bool ReadVersionAt(const IMemory* Memory, uintptr_t Addr, bool bRequireBranch, FEngineVersionInfo& Out)
		{
			uint8_t Bytes[kSizeOfVersion];

			if (!Addr || Memory->ReadBytes(Addr, Bytes, sizeof(Bytes)) < sizeof(Bytes))
				return false;

			if (!MatchesBase(Bytes))
				return false;

			const uintptr_t BranchData = Load<uintptr_t>(Bytes + kOffsetBranchData);
			const int32_t BranchNum    = Load<int32_t>(Bytes + kOffsetBranchNum);
			const int32_t BranchMax    = Load<int32_t>(Bytes + kOffsetBranchMax);

			std::string Branch;

			if (BranchNum >= kMinBranchLen && BranchNum <= kMaxBranchLen && BranchMax >= BranchNum && BranchData)
			{
				uint8_t BranchBytes[kMaxBranchLen * 4] = {};

				size_t Got = Memory->ReadBytes(BranchData, BranchBytes, static_cast<size_t>(BranchNum) * 4);

				/* A UTF-16 branch at the end of its mapping has no second half to read, so retry narrow. */
				if (Got < static_cast<size_t>(BranchNum) * 2)
					Got = Memory->ReadBytes(BranchData, BranchBytes, static_cast<size_t>(BranchNum) * 2);

				Branch = DecodeBranchAnyWidth(BranchBytes, BranchNum, Got);
			}

			if (bRequireBranch && Branch.empty())
				return false;

			Out.Major      = Load<uint16_t>(Bytes + kOffsetMajor);
			Out.Minor      = Load<uint16_t>(Bytes + kOffsetMinor);
			Out.Patch      = Load<uint16_t>(Bytes + kOffsetPatch);
			Out.Changelist = Load<uint32_t>(Bytes + kOffsetChangelist);
			Out.Branch     = std::move(Branch);

			return Out.IsValid();
		}

	} // namespace

	std::string FEngineVersionInfo::ToString() const
	{
		if (!IsValid())
			return "<FAILED_TO_DETECT>";

		char Buffer[128];
		std::snprintf(Buffer, sizeof(Buffer), "%u.%u.%u-%u+%s", Major, Minor, Patch, Changelist, Branch.c_str());

		return Buffer;
	}

	FEngineVersionInfo DetectEngineVersion(IMemory* Memory, size_t* OutNumCandidates)
	{
		FEngineVersionInfo Result;

		size_t NumCandidates  = 0;
		size_t NumBranchReads = 0;

		if (OutNumCandidates)
			*OutNumCandidates = 0;

		if (!Memory)
			return Result;

		const ModuleInfo Module = Memory->GetUnrealModule();

		/* The object symbol names the struct outright: authoritative, and no corroboration needed. */
		if (ReadVersionAt(Memory, Memory->FindUnrealSymbol(kSingletonSymbol), false, Result))
		{
			if (OutNumCandidates)
				*OutNumCandidates = 1;

			return Result;
		}

		/* Only the guard survived. It lives in the same static storage as the object it guards, so the
		   struct sits just below it - near enough to test directly rather than scan for. */
		if (const uintptr_t Guard = Memory->FindUnrealSymbol(kGuardSymbol))
		{
			for (uintptr_t Addr = Guard - kPtrSize; Addr + kSizeOfVersion > Guard - kGuardWindow; Addr -= kPtrSize)
			{
				/* Walking down from the guard reaches CompatibleWith (Current + sizeof) first, and it
				   validates on its own while carrying a zeroed Patch. Requiring the whole
				   Current/CompatibleWith pair pins Addr to Current. */
				uint8_t Pair[kSizeOfVersion * 2];

				if (Memory->ReadBytes(Addr, Pair, sizeof(Pair)) < sizeof(Pair))
					continue;

				if (!MatchesVersionPair(Pair))
					continue;

				if (!ReadVersionAt(Memory, Addr, true, Result))
					continue;

				if (OutNumCandidates)
					*OutNumCandidates = 1;

				return Result;
			}
		}

		std::vector<uint8_t> Chunk;
		uint8_t BranchBytes[kMaxBranchLen * 4];

		/* The version is constructed into a global, so only writable data can hold it. */
		Module.ForEachSegment([&](MemRegionInfo Segment) -> bool
		{
			if (!Segment.IsReadable() || !Segment.IsWriteable() || NumCandidates >= kExpectedCandidates)
				return false;

			const uintptr_t End = Segment.GetEnd();

			/* Chunked reads with the shape tested in-buffer: reading field by field from the target
			   would cost a syscall per field per address, millions of them across a segment. */
			for (uintptr_t Cur = Segment.GetStart(); Cur + kSizeOfVersion <= End;)
			{
				size_t ChunkSize = std::min(static_cast<size_t>(End - Cur), kChunkBytes);
				ChunkSize        = (ChunkSize / kPtrSize) * kPtrSize;

				if (ChunkSize < kSizeOfVersion)
					break;

				Chunk.resize(ChunkSize);

				/* ReadBytes reports how much it got, and a segment can contain unmapped holes, so a
				   short read is normal: scan what arrived and step past one page on a failure rather
				   than discarding the whole chunk. */
				const size_t Got = Memory->ReadBytes(Cur, Chunk.data(), ChunkSize);

				if (Got < kSizeOfVersion)
				{
					Cur += kPageBytes;
					continue;
				}

				for (size_t Off = 0; Off + (kSizeOfVersion * 2) <= Got; Off += kPtrSize)
				{
					if (!MatchesVersionPair(Chunk.data() + Off))
						continue;

					const uint8_t* At    = Chunk.data() + Off;
					const uint16_t Major = Load<uint16_t>(At + kOffsetMajor);
					const uint16_t Minor = Load<uint16_t>(At + kOffsetMinor);
					const uint16_t Patch = Load<uint16_t>(At + kOffsetPatch);

					const uintptr_t BranchData = Load<uintptr_t>(At + kOffsetBranchData);
					const int32_t BranchNum    = Load<int32_t>(At + kOffsetBranchNum);
					const int32_t BranchMax    = Load<int32_t>(At + kOffsetBranchMax);

					if (NumBranchReads >= kMaxBranchFetches)
						break;

					NumBranchReads++;

					/* One read serves either width; the narrower retry covers a UTF-16 branch at the
					   end of its mapping, which has no second half to read. Which width is right is
					   decided by the decode, never by the content: a fork may name its branch anything. */
					std::string Branch;

					const bool bBranchPlausible = BranchNum >= kMinBranchLen && BranchNum <= kMaxBranchLen && BranchMax >= BranchNum && BranchData != 0;

					if (bBranchPlausible)
					{
						size_t Got = Memory->ReadBytes(BranchData, BranchBytes, static_cast<size_t>(BranchNum) * 4);

						if (Got < static_cast<size_t>(BranchNum) * 2)
							Got = Memory->ReadBytes(BranchData, BranchBytes, static_cast<size_t>(BranchNum) * 2);

						Branch = DecodeBranchAnyWidth(BranchBytes, BranchNum, Got);
					}

					/* The pair is structural but weak on its own, so a readable branch is still required
					   as corroboration - just not a particular one. */
					if (Branch.empty())
						continue;

					NumCandidates++;

					/* Current precedes compatible-with, so the lowest address wins. */
					if (!Result.IsValid())
					{
						Result.Major      = Major;
						Result.Minor      = Minor;
						Result.Patch      = Patch;
						Result.Changelist = Load<uint32_t>(Chunk.data() + Off + kOffsetChangelist);
						Result.Branch     = std::move(Branch);
					}
				}

				if (NumCandidates >= kExpectedCandidates)
					break;

				/* Advance by what was actually read, overlapping by one struct so a version
				   straddling a chunk boundary is still seen whole. */
				Cur += Got - (kSizeOfVersion - kPtrSize);
			}

			return false;
		});

		if (OutNumCandidates)
			*OutNumCandidates = NumCandidates;

		return Result;
	}

} // namespace UEAnalyzerKitty
