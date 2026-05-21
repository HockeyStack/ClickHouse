#include <DataTypes/Serializations/SerializationObjectPool.h>
#include <Common/CurrentMetrics.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace CurrentMetrics
{
    extern const Metric SerializationCacheBytesInMemoryAllocated;
    extern const Metric SerializationCacheBytesInMemory;
    extern const Metric SerializationCacheCount;
}

namespace DB
{
namespace
{

class TestSerialization final : public ISerialization
{
public:
    explicit TestSerialization(size_t id_, size_t allocated_bytes_ = sizeof(TestSerialization))
        : id(id_)
        , allocated_bytes(allocated_bytes_)
    {
    }

    size_t allocatedBytes() const override { return allocated_bytes; }

    void serializeBinary(const Field &, WriteBuffer &, const FormatSettings &) const override {}
    void deserializeBinary(Field &, ReadBuffer &, const FormatSettings &) const override {}
    void serializeBinary(const IColumn &, size_t, WriteBuffer &, const FormatSettings &) const override {}
    void deserializeBinary(IColumn &, ReadBuffer &, const FormatSettings &) const override {}
    void serializeTextEscaped(const IColumn &, size_t, WriteBuffer &, const FormatSettings &) const override {}
    void deserializeTextEscaped(IColumn &, ReadBuffer &, const FormatSettings &) const override {}
    void serializeTextQuoted(const IColumn &, size_t, WriteBuffer &, const FormatSettings &) const override {}
    void deserializeTextQuoted(IColumn &, ReadBuffer &, const FormatSettings &) const override {}
    void serializeTextCSV(const IColumn &, size_t, WriteBuffer &, const FormatSettings &) const override {}
    void deserializeTextCSV(IColumn &, ReadBuffer &, const FormatSettings &) const override {}
    void serializeText(const IColumn &, size_t, WriteBuffer &, const FormatSettings &) const override {}
    void deserializeWholeText(IColumn &, ReadBuffer &, const FormatSettings &) const override {}
    void serializeTextJSON(const IColumn &, size_t, WriteBuffer &, const FormatSettings &) const override {}
    void deserializeTextJSON(IColumn &, ReadBuffer &, const FormatSettings &) const override {}

    size_t id;

private:
    size_t allocated_bytes;
};

UInt128 makeKey(UInt64 high, UInt64 low)
{
    return (UInt128(high) << 64) | low;
}

size_t getTestSerializationId(const SerializationPtr & serialization)
{
    return static_cast<const TestSerialization &>(*serialization).id;
}

}

TEST(SerializationObjectPool, SameKeyReturnsLiveObject)
{
    const auto key = makeKey(0x5e2f000000000001ULL, 1);
    std::atomic<size_t> created = 0;

    auto first = SerializationObjectPool::getOrCreate(key, [&] { return new TestSerialization(++created); });
    auto second = SerializationObjectPool::getOrCreate(key, [&] { return new TestSerialization(++created); });

    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(created.load(), 1);
}

TEST(SerializationObjectPool, ExpiredEntryIsRecreated)
{
    const auto key = makeKey(0x5e2f000000000002ULL, 1);
    std::atomic<size_t> created = 0;

    {
        auto serialization = SerializationObjectPool::getOrCreate(key, [&] { return new TestSerialization(++created); });
        EXPECT_EQ(getTestSerializationId(serialization), 1);
    }

    auto serialization = SerializationObjectPool::getOrCreate(key, [&] { return new TestSerialization(++created); });
    EXPECT_EQ(getTestSerializationId(serialization), 2);
    EXPECT_EQ(created.load(), 2);
}

TEST(SerializationObjectPool, DistinctKeysReturnDistinctObjects)
{
    std::vector<SerializationPtr> serializations;
    serializations.reserve(512);

    for (size_t i = 0; i != 512; ++i)
    {
        const auto key = makeKey(0x5e2f000000000003ULL, i);
        serializations.push_back(SerializationObjectPool::getOrCreate(key, [i] { return new TestSerialization(i); }));
    }

    for (size_t i = 0; i != serializations.size(); ++i)
    {
        EXPECT_EQ(getTestSerializationId(serializations[i]), i);
        for (size_t j = i + 1; j != serializations.size(); ++j)
            EXPECT_NE(serializations[i].get(), serializations[j].get());
    }
}

TEST(SerializationObjectPool, ConcurrentSameKeyCallsReturnOneLiveObject)
{
    const auto key = makeKey(0x5e2f000000000004ULL, 1);
    constexpr size_t num_threads = 32;

    std::atomic<size_t> created = 0;
    std::atomic<bool> start = false;
    std::vector<SerializationPtr> results(num_threads);
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (size_t i = 0; i != num_threads; ++i)
    {
        threads.emplace_back([&, i]
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            results[i] = SerializationObjectPool::getOrCreate(key, [&] { return new TestSerialization(++created); });
        });
    }

    start.store(true, std::memory_order_release);
    for (auto & thread : threads)
        thread.join();

    for (const auto & result : results)
        EXPECT_EQ(result.get(), results.front().get());
    EXPECT_GE(created.load(), 1);
    EXPECT_LE(created.load(), num_threads);
}

TEST(SerializationObjectPool, NestedCreationDoesNotDeadlock)
{
    const auto outer_key = makeKey(0x5e2f000000000005ULL, 1);
    const auto inner_key = makeKey(0x5e2f000000000005ULL, 2);

    SerializationPtr inner;
    auto outer = SerializationObjectPool::getOrCreate(outer_key, [&]
    {
        inner = SerializationObjectPool::getOrCreate(inner_key, [] { return new TestSerialization(1); });
        return new TestSerialization(2);
    });

    ASSERT_TRUE(inner);
    EXPECT_NE(outer.get(), inner.get());
    EXPECT_EQ(getTestSerializationId(inner), 1);
    EXPECT_EQ(getTestSerializationId(outer), 2);
}

TEST(SerializationObjectPool, MetricsReturnToBaselineAfterRelease)
{
    const auto key = makeKey(0x5e2f000000000006ULL, 1);
    constexpr CurrentMetrics::Value allocated_bytes = 4096;

    const auto count_before = CurrentMetrics::get(CurrentMetrics::SerializationCacheCount);
    const auto bytes_before = CurrentMetrics::get(CurrentMetrics::SerializationCacheBytesInMemory);

    {
        auto serialization = SerializationObjectPool::getOrCreate(key, [] { return new TestSerialization(1, allocated_bytes); });
        EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::SerializationCacheCount), count_before + 1);
        EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::SerializationCacheBytesInMemory), bytes_before + allocated_bytes);
        EXPECT_GE(
            CurrentMetrics::get(CurrentMetrics::SerializationCacheBytesInMemoryAllocated),
            CurrentMetrics::get(CurrentMetrics::SerializationCacheBytesInMemory));
    }

    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::SerializationCacheCount), count_before);
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::SerializationCacheBytesInMemory), bytes_before);
    EXPECT_GE(
        CurrentMetrics::get(CurrentMetrics::SerializationCacheBytesInMemoryAllocated),
        CurrentMetrics::get(CurrentMetrics::SerializationCacheBytesInMemory));
}

}
