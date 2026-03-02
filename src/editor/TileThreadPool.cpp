#include "TileThreadPool.hpp"

TileThreadPool::TileThreadPool(int numWorkers, int rateLimitMs)
    : rateLimitMs(rateLimitMs) {
    for (int i = 0; i < numWorkers; i++) {
        workers.emplace_back(&TileThreadPool::workerFunc, this);
    }
}

TileThreadPool::~TileThreadPool() {
    shutdown();
}

void TileThreadPool::submit(WorkItem work, const std::string& domain) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (!running) return;
        workQueue.push({std::move(work), domain});
        pending++;
    }
    queueCV.notify_one();
}

int TileThreadPool::pendingCount() const {
    return pending.load();
}

void TileThreadPool::waitAll() {
    std::unique_lock<std::mutex> lock(queueMutex);
    doneCV.wait(lock, [this] { return pending.load() == 0 || !running; });
}

void TileThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (!running) return;
        running = false;
    }
    queueCV.notify_all();
    doneCV.notify_all();
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
}

void TileThreadPool::workerFunc() {
    while (true) {
        QueueEntry entry;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, [this] { return !workQueue.empty() || !running; });
            if (!running) return;
            if (workQueue.empty()) continue;
            entry = std::move(workQueue.front());
            workQueue.pop();
        }

        // Rate limiting: wait if we recently hit this domain
        if (!entry.domain.empty() && rateLimitMs > 0) {
            while (shouldRateLimit(entry.domain)) {
                if (!running) {
                    pending--;
                    doneCV.notify_all();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            recordDomainAccess(entry.domain);
        }

        entry.work();

        pending--;
        doneCV.notify_all();
    }
}

bool TileThreadPool::shouldRateLimit(const std::string& domain) {
    std::lock_guard<std::mutex> lock(domainMutex);
    auto it = domainLastAccess.find(domain);
    if (it == domainLastAccess.end()) return false;
    auto elapsed = std::chrono::steady_clock::now() - it->second;
    return elapsed < std::chrono::milliseconds(rateLimitMs);
}

void TileThreadPool::recordDomainAccess(const std::string& domain) {
    std::lock_guard<std::mutex> lock(domainMutex);
    domainLastAccess[domain] = std::chrono::steady_clock::now();
}
