#include <DataTypes/Serializations/ISerialization.h>
#include <DataTypes/Serializations/SerializationObjectPool.h>
#include <Common/CacheLine.h>
#include <Common/CurrentMetrics.h>
#include <Common/HashTable/Hash.h>
#include <Common/SharedMutex.h>
#include <absl/container/flat_hash_map.h>

#include <array>
#include <mutex>
#include <shared_mutex>

namespace CurrentMetrics
{
    extern const Metric SerializationCacheBytesInMemoryAllocated;
    extern const Metric SerializationCacheBytesInMemory;
    extern const Metric SerializationCacheCount;
}

namespace DB
{

namespace SerializationObjectPool
{

using SerializationMap = absl::flat_hash_map<UInt128, std::weak_ptr<const ISerialization>>;

/// `SerializationObjectPool` is a weak canonicalization cache for immutable
/// `ISerialization` objects. Under high query and merge concurrency, a single
/// global `SharedMutex` makes unrelated serialization shapes contend on the
/// same futex word. Sharding preserves canonicalization per key while spreading
/// cache hits, insertions, and deleter cleanup across independent locks.
struct alignas(CH_CACHE_LINE_SIZE) Shard
{
    SharedMutex mutex;
    SerializationMap map;
    CurrentMetrics::Value map_allocated_bytes = 0;
};

struct Pool
{
    static constexpr size_t num_shards = 256;
    static_assert((num_shards & (num_shards - 1)) == 0);

    std::array<Shard, num_shards> shards;
    std::mutex metrics_mutex;
    CurrentMetrics::Value map_allocated_bytes = 0;

    Shard & getShard(UInt128 key)
    {
        return shards[UInt128Hash()(key) & (num_shards - 1)];
    }

    void updateMetricsLocked(Shard & shard)
    {
        const auto new_map_allocated_bytes = static_cast<CurrentMetrics::Value>(sizeof(SerializationMap::value_type) * shard.map.capacity());
        const auto old_map_allocated_bytes = shard.map_allocated_bytes;
        std::lock_guard metrics_lock(metrics_mutex);

        if (new_map_allocated_bytes != old_map_allocated_bytes)
        {
            shard.map_allocated_bytes = new_map_allocated_bytes;
            map_allocated_bytes += new_map_allocated_bytes - old_map_allocated_bytes;
        }

        CurrentMetrics::set(
            CurrentMetrics::SerializationCacheBytesInMemoryAllocated,
            map_allocated_bytes + CurrentMetrics::get(CurrentMetrics::SerializationCacheBytesInMemory));
    }
};

/// Intentionally leaked to avoid static destruction order issues: the custom
/// shared_ptr deleters reference the pool, but those deleters can fire from
/// any thread (including during thread_local / static destruction of caches
/// such as DataTypesCache or ColumnObject's getDynamicSerialization).  If the
/// pool were a regular static it could already be destroyed at that point.
static Pool & getPool()
{
    static Pool * pool = new Pool;
    return *pool;
}

SerializationPtr getOrCreate(UInt128 key, SerializationCreator creator)
{
    auto & pool = getPool();
    auto & shard = pool.getShard(key);
    {
        std::shared_lock read_lock(shard.mutex);
        auto it = shard.map.find(key);
        if (it != shard.map.end())
            if (auto res = it->second.lock())
                return res;
    }

    /// Creating the serialization object must be outside of the critical section
    /// because there might be nested serializations.
    auto tmp = std::unique_ptr<const ISerialization>(creator());
    auto allocated_bytes = tmp->allocatedBytes();

    std::lock_guard write_lock(shard.mutex);
    auto [it, inserted] = shard.map.emplace(key, std::weak_ptr<const ISerialization>());
    if (!inserted)
        if (auto res = it->second.lock())
            return res;

    CurrentMetrics::add(CurrentMetrics::SerializationCacheCount);
    CurrentMetrics::add(CurrentMetrics::SerializationCacheBytesInMemory, allocated_bytes);
    pool.updateMetricsLocked(shard);

    SerializationPtr ret
    (
        tmp.release(),
        [k = std::move(key), b = allocated_bytes](const ISerialization * ptr)
        {
            auto & p = getPool();
            auto & s = p.getShard(k);
            {
                std::unique_lock lock(s.mutex);
                auto map_it = s.map.find(k);
                if (map_it != s.map.end() && map_it->second.expired())
                    s.map.erase(map_it);

                CurrentMetrics::sub(CurrentMetrics::SerializationCacheCount);
                CurrentMetrics::sub(CurrentMetrics::SerializationCacheBytesInMemory, b);
                p.updateMetricsLocked(s);
            }
            delete ptr;
        }
    );

    it->second = ret;
    return ret;
}
}

}
