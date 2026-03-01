#include <catch2/catch_test_macros.hpp>
#include "editor/TileThreadPool.hpp"
#include <atomic>
#include <set>
#include <thread>
#include <chrono>

TEST_CASE("TileThreadPool distributes work across workers", "[TileThreadPool]") {
    TileThreadPool pool(4, 0); // 4 workers, no rate limiting
    std::atomic<int> counter{0};
    const int N = 100;

    for (int i = 0; i < N; i++) {
        pool.submit([&counter] { counter++; });
    }
    pool.waitAll();

    REQUIRE(counter.load() == N);
    REQUIRE(pool.pendingCount() == 0);
}

TEST_CASE("TileThreadPool records thread IDs from multiple workers", "[TileThreadPool]") {
    TileThreadPool pool(4, 0);
    std::mutex idMutex;
    std::set<std::thread::id> threadIds;
    const int N = 40;

    for (int i = 0; i < N; i++) {
        pool.submit([&idMutex, &threadIds] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::lock_guard<std::mutex> lock(idMutex);
            threadIds.insert(std::this_thread::get_id());
        });
    }
    pool.waitAll();

    // With 4 workers and 40 items that each sleep 5ms, multiple threads should participate
    REQUIRE(threadIds.size() > 1);
}

TEST_CASE("TileThreadPool per-domain rate limiting", "[TileThreadPool]") {
    TileThreadPool pool(2, 100); // 100ms between same-domain requests
    std::mutex timeMutex;
    std::vector<std::chrono::steady_clock::time_point> timestamps;

    for (int i = 0; i < 3; i++) {
        pool.submit([&timeMutex, &timestamps] {
            std::lock_guard<std::mutex> lock(timeMutex);
            timestamps.push_back(std::chrono::steady_clock::now());
        }, "example.com");
    }
    pool.waitAll();

    REQUIRE(timestamps.size() == 3);
    // Successive requests to same domain should be at least ~90ms apart
    // (allowing some scheduling jitter)
    for (size_t i = 1; i < timestamps.size(); i++) {
        auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamps[i] - timestamps[i - 1]).count();
        REQUIRE(gap >= 80); // 80ms minimum to account for timing jitter
    }
}

TEST_CASE("TileThreadPool different domains not rate limited against each other", "[TileThreadPool]") {
    TileThreadPool pool(4, 200);
    std::atomic<int> counter{0};

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; i++) {
        pool.submit([&counter] { counter++; }, "domain" + std::to_string(i) + ".com");
    }
    pool.waitAll();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    REQUIRE(counter.load() == 4);
    // Different domains should all run in parallel, finishing well under 200ms
    REQUIRE(elapsed < 150);
}

TEST_CASE("TileThreadPool graceful shutdown", "[TileThreadPool]") {
    auto pool = std::make_unique<TileThreadPool>(4, 0);
    std::atomic<int> counter{0};

    for (int i = 0; i < 50; i++) {
        pool->submit([&counter] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            counter++;
        });
    }

    // Let some work start before shutting down
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Shutdown should join all workers without deadlock
    pool.reset();

    // Some work should have completed (at least in-flight items)
    REQUIRE(counter.load() > 0);
}

TEST_CASE("TileThreadPool shutdown with no work", "[TileThreadPool]") {
    // Should not deadlock on destruction with no work submitted
    auto pool = std::make_unique<TileThreadPool>(4, 0);
    pool.reset();
    REQUIRE(true); // Reached here = no deadlock
}
