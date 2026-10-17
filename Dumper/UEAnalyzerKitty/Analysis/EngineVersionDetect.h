#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "../../Memory/IMemory.h"

namespace UEAnalyzerKitty
{

	/**
	 * @brief The engine version a build reports about itself.
	 *
	 * A label, not a capability test: a forked engine can carry the version it was
	 * branched from while shipping later features, so code paths belong on the
	 * measured layout flags instead.
	 */
	struct FEngineVersionInfo
	{
		uint16_t Major = 0;
		uint16_t Minor = 0;
		uint16_t Patch = 0;

		/// Changelist the build came from. Commonly 0 in shipped titles.
		uint32_t Changelist = 0;

		/// Branch it was built from, e.g. "++UE4+Release-4.27" or just "UE5".
		std::string Branch;

		/// @brief True once a version has actually been read.
		inline bool IsValid() const { return Major != 0; }

		/// @brief Formats as the engine does: Major.Minor.Patch-Changelist+Branch.
		std::string ToString() const;
	};

	/**
	 * @brief Reads the build's FEngineVersion out of the module's writable data.
	 *
	 * Symbol-preferred, then structural: the singleton symbol names the struct
	 * outright, its guard symbol places it within a short window, and failing both
	 * the shape is scanned for. Most shipped titles strip the symbols, so the scan
	 * is the path that usually answers.
	 *
	 * The struct alone is not distinctive - three small integers and a pointer match
	 * ordinary data - so a scan match must also be one of the Current/CompatibleWith
	 * pair FGlobalEngineVersions holds, with a Branch that decodes to printable text.
	 * The text itself is not tested: forks rename the branch freely. Both TCHAR
	 * widths are tried, since this runs before the width is known.
	 *
	 * @param OutNumCandidates Receives the number of confirmed matches. The scan stops
	 *                         at the first, so this is 1 on success and 0 on failure.
	 * @return The version, or an invalid info when none was found.
	 */
	FEngineVersionInfo DetectEngineVersion(IMemory* Memory, size_t* OutNumCandidates = nullptr);

} // namespace UEAnalyzerKitty
