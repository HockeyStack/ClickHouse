#include <Functions/MultiSearchAhoCorasickCache.h>

#if USE_AHO_CORASICK

#include <algorithm>
#include <mutex>
#include <vector>

#include <Core/Defines.h>
#include <Common/Exception.h>
#include <Common/CurrentMetrics.h>
#include <Common/MemoryTrackerBlockerInThread.h>
#include <Common/ProfileEvents.h>
#include <Common/SipHash.h>
#include <Common/UTF8Helpers.h>
#include <Poco/Unicode.h>

namespace CurrentMetrics
{
    extern const Metric MultiSearchAutomatonCacheBytes;
}

namespace ProfileEvents
{
    extern const Event AhoCorasickCacheHit;
    extern const Event AhoCorasickCacheMiss;
    extern const Event AhoCorasickCacheCollision;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

bool appendFoldedForMultiSearch(MultiSearchCaseMode case_mode, const char * data, size_t size, PaddedPODArray<UInt8> & output)
{
    const auto * pos = reinterpret_cast<const UInt8 *>(data);
    const auto * const end = pos + size;

    switch (case_mode)
    {
        case MultiSearchCaseMode::Sensitive:
            output.insert(pos, end);
            return true;

        case MultiSearchCaseMode::InsensitiveAscii:
            for (; pos != end; ++pos)
                output.push_back((*pos >= 'A' && *pos <= 'Z') ? static_cast<UInt8>(*pos + ('a' - 'A')) : *pos);
            return true;

        case MultiSearchCaseMode::InsensitiveUtf8:
            break;
    }

    /// A marker that cannot occur in valid folded UTF-8, so it never matches a (valid) needle and
    /// never merges with neighbouring bytes into a spurious match across a malformed sequence.
    static constexpr UInt8 INVALID_SEQUENCE_MARKER = 0xFF;

    bool valid = true;
    while (pos != end)
    {
        auto code_point = UTF8::convertUTF8ToCodePoint(reinterpret_cast<const char *>(pos), end - pos);
        if (!code_point)
        {
            valid = false;
            output.push_back(INVALID_SEQUENCE_MARKER);
            pos += std::min<size_t>(UTF8::seqLength(*pos), end - pos);
            continue;
        }

        UInt8 folded[4];
        const size_t folded_size = UTF8::convertCodePointToUTF8(
            Poco::Unicode::toLower(static_cast<int>(*code_point)), reinterpret_cast<char *>(folded), sizeof(folded));
        output.insert(folded, folded + folded_size);
        pos += UTF8::seqLength(*pos);
    }
    return valid;
}

AhoCorasickAutomaton::~AhoCorasickAutomaton()
{
    /// Freeing the Rust handle routes through ClickHouse's allocator, which decrements the current
    /// thread's memory tracker. The matching allocation in buildAutomaton runs under a
    /// MemoryTrackerBlockerInThread, so block here too to keep the tracker balanced: the automaton
    /// outlives the query that built it and may be freed on an unrelated thread during eviction.
    MemoryTrackerBlockerInThread memory_blocker;
    if (handle)
        aho_corasick_free(handle);
}

namespace
{

UInt128 computeKey(MultiSearchCaseMode case_mode, const Array & needles)
{
    SipHash hash;
    hash.update(static_cast<uint8_t>(case_mode));
    for (const auto & needle : needles)
    {
        const String & s = needle.safeGet<String>();
        hash.update(s.size());
        hash.update(s.data(), s.size());
    }
    return hash.get128();
}

std::shared_ptr<AhoCorasickAutomaton> buildAutomaton(MultiSearchCaseMode case_mode, const Array & needles)
{
    /// The automaton stays in the server-global cache and outlives the current query, so do not
    /// charge its memory to the query's memory tracker.
    MemoryTrackerBlockerInThread memory_blocker;

    /// Fold needles into one contiguous buffer. Invalid-UTF8 needles cannot match anything in a
    /// UTF-8 search (the legacy searcher omits them too), so drop them here.
    PaddedPODArray<UInt8> folded_data;
    std::vector<uint64_t> folded_ends;
    folded_ends.reserve(needles.size());
    for (const auto & needle : needles)
    {
        const String & s = needle.safeGet<String>();
        const size_t start = folded_data.size();
        if (appendFoldedForMultiSearch(case_mode, s.data(), s.size(), folded_data))
            folded_ends.push_back(folded_data.size());
        else
            folded_data.resize(start);
    }

    std::vector<const uint8_t *> pattern_ptrs;
    std::vector<uint64_t> pattern_sizes;
    pattern_ptrs.reserve(folded_ends.size());
    pattern_sizes.reserve(folded_ends.size());
    uint64_t prev_end = 0;
    for (uint64_t folded_end : folded_ends)
    {
        pattern_ptrs.push_back(reinterpret_cast<const uint8_t *>(folded_data.data()) + prev_end);
        pattern_sizes.push_back(folded_end - prev_end);
        prev_end = folded_end;
    }

    std::unique_ptr<AhoCorasickHandle, decltype(&aho_corasick_free)> handle(
        aho_corasick_create(
            pattern_ptrs.data(),
            pattern_sizes.data(),
            static_cast<uint64_t>(pattern_ptrs.size())),
        &aho_corasick_free);

    if (!handle)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "Failed to build Aho-Corasick automaton for {} patterns (too many states or out of memory)",
            needles.size());

    const size_t memory_bytes = aho_corasick_heap_bytes(handle.get());
    auto automaton = std::make_shared<AhoCorasickAutomaton>(handle.get(), memory_bytes);
    handle.release();
    return automaton;
}

/// Direct-mapped cache, following the same pattern as Hyperscan's GlobalCacheTable
/// (Regexps.h). Collisions replace the old entry — no LRU or byte-size limit.
struct CacheBucket
{
    UInt128 key;
    std::shared_ptr<const AhoCorasickAutomaton> automaton;
};

struct Cache
{
    Cache()
        : buckets(DEFAULT_MULTI_SEARCH_AUTOMATON_CACHE_MAX_ENTRIES)
    {
    }

    std::mutex mutex;
    std::vector<CacheBucket> buckets;
};

Cache & cache()
{
    static Cache instance;
    return instance;
}

} // namespace

void setMultiSearchAutomatonCacheSlots(size_t slots)
{
    /// The setting is a slot count, not a byte size; cap it so a misconfigured value
    /// cannot allocate an absurd bucket array.
    slots = std::min<size_t>(slots, 1'000'000);

    auto & global_cache = cache();
    std::lock_guard lock(global_cache.mutex);
    if (global_cache.buckets.size() == slots)
        return;

    global_cache.buckets.assign(slots, {});
    CurrentMetrics::set(CurrentMetrics::MultiSearchAutomatonCacheBytes, 0);
}

std::shared_ptr<const AhoCorasickAutomaton> getOrBuildAhoCorasickAutomaton(MultiSearchCaseMode case_mode, const Array & needles)
{
    const UInt128 key = computeKey(case_mode, needles);
    auto & global_cache = cache();

    {
        std::lock_guard lock(global_cache.mutex);
        if (!global_cache.buckets.empty())
        {
            const auto & bucket = global_cache.buckets[static_cast<size_t>(key % global_cache.buckets.size())];
            if (bucket.automaton && bucket.key == key)
            {
                ProfileEvents::increment(ProfileEvents::AhoCorasickCacheHit);
                return bucket.automaton;
            }
        }
    }

    /// Build outside the lock so concurrent compilations for different keys are not serialised.
    auto automaton = buildAutomaton(case_mode, needles);
    bool collision = false;
    std::shared_ptr<const AhoCorasickAutomaton> resident;

    {
        std::lock_guard lock(global_cache.mutex);
        if (!global_cache.buckets.empty())
        {
            auto & bucket = global_cache.buckets[static_cast<size_t>(key % global_cache.buckets.size())];
            if (bucket.automaton && bucket.key == key)
                resident = bucket.automaton;
            else
            {
                if (bucket.automaton)
                {
                    collision = true;
                    CurrentMetrics::sub(CurrentMetrics::MultiSearchAutomatonCacheBytes, bucket.automaton->memory_bytes);
                }

                bucket = {key, automaton};
                CurrentMetrics::add(CurrentMetrics::MultiSearchAutomatonCacheBytes, automaton->memory_bytes);
            }
        }
    }

    ProfileEvents::increment(ProfileEvents::AhoCorasickCacheMiss);
    if (collision)
        ProfileEvents::increment(ProfileEvents::AhoCorasickCacheCollision);

    return resident ? resident : automaton;
}

}

#endif
