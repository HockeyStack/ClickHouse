#pragma once

#include <cstdint>

extern "C" {

/// Opaque handle to the Aho-Corasick automaton
struct AhoCorasickHandle;

/// Creates an Aho-Corasick automaton from the given patterns.
///
/// Uses daachorse (double-array trie Aho-Corasick) for fast multi-pattern search.
/// Patterns must already be case-folded by the caller (see MultiSearchAhoCorasickCache.h) and are
/// de-duplicated before building. The caller must ensure no pattern is empty (empty needles are
/// handled on the C++ side). An empty pattern set yields a valid automaton that matches nothing.
///
/// @param patterns Pointer to array of pattern data pointers
/// @param pattern_sizes Pointer to array of pattern sizes (uint64_t)
/// @param num_patterns Number of patterns
/// @returns Pointer to the automaton handle, or nullptr on error
AhoCorasickHandle * aho_corasick_create(
    const uint8_t * const * patterns,
    const uint64_t * pattern_sizes,
    uint64_t num_patterns);

/// Searches for any pattern match in a batch of haystacks.
///
/// Haystacks must already be case-folded by the caller the same way the patterns were.
/// This function is optimized for ClickHouse's ColumnString format where:
/// - haystack_data is a contiguous buffer of all strings (with null terminators)
/// - haystack_offsets is an array of cumulative end positions
///
/// @param handle The automaton handle from aho_corasick_create
/// @param haystack_data Pointer to contiguous haystack data
/// @param haystack_offsets Pointer to array of cumulative offsets (ClickHouse format)
/// @param num_rows Number of haystacks to search
/// @param results Output array of uint8_t (0 = no match, 1 = match found)
void aho_corasick_search_batch(
    const AhoCorasickHandle * handle,
    const uint8_t * haystack_data,
    const uint64_t * haystack_offsets,
    uint64_t num_rows,
    uint8_t * results);

/// Returns the total heap memory (in bytes) used by the automaton, for cache sizing.
///
/// @param handle The automaton handle from aho_corasick_create
/// @returns Heap bytes used by the automaton, or 0 if handle is nullptr
uint64_t aho_corasick_heap_bytes(const AhoCorasickHandle * handle);

/// Frees the Aho-Corasick automaton handle.
///
/// @param handle The handle to free (may be nullptr)
void aho_corasick_free(AhoCorasickHandle * handle);

} // extern "C"
