#pragma once

#include <functional>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <chrono>

// Thread pool for tile download work items with per-domain rate limiting.
// Workers pull from a shared queue and respect minimum intervals between
// requests to the same domain (e.g. 100ms for OSM tile policy).
class TileThreadPool {
public:
    using WorkItem = std::function<void()>;

    // Create pool with numWorkers threads and per-domain rate limit in ms.
    explicit TileThreadPool(int numWorkers = 4, int rateLimitMs = 100);
    ~TileThreadPool();

    // Not copyable or movable
    TileThreadPool(const TileThreadPool&) = delete;
    TileThreadPool& operator=(const TileThreadPool&) = delete;

    // Submit work. domain is used for rate limiting (e.g. "server.arcgisonline.com").
    // If domain is empty, no rate limiting is applied.
    void submit(WorkItem work, const std::string& domain = "");

    // Number of items queued + in-flight.
    int pendingCount() const;

    // Wait for all submitted work to complete.
    void waitAll();

    // Graceful shutdown: finish in-flight work, discard remaining queue.
    void shutdown();

private:
    struct QueueEntry {
        WorkItem work;
        std::string domain;
    };

    void workerFunc();
    bool shouldRateLimit(const std::string& domain);
    void recordDomainAccess(const std::string& domain);

    std::vector<std::thread> workers;
    std::queue<QueueEntry> workQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::condition_variable doneCV;
    std::atomic<bool> running{true};
    std::atomic<int> pending{0};
    int rateLimitMs;

    // Per-domain last-access timestamps for rate limiting
    std::mutex domainMutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> domainLastAccess;
};
