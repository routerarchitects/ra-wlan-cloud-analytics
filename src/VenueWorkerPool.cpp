//
// Created to replace per-venue threads with a shared worker pool.
//

#include "VenueWorkerPool.h"
#include "VenueWatcher.h"

#include "nlohmann/json.hpp"
#include "fmt/format.h"
#include "framework/utils.h"
#include <thread>

namespace OpenWifi {

    void VenueWorkerPool::Worker::run() {
        Utils::SetThreadName("venue-worker");
        Running_ = true;
        Poco::AutoPtr<Poco::Notification> Note(Queue_.waitDequeueNotification());
        while (Note && Running_) {
            auto Msg = dynamic_cast<VenueDispatchMessage *>(Note.get());
            if (Msg != nullptr) {
                try {
                    if (auto *VW = Msg->Watcher(); VW != nullptr) {
                        VW->Process(Msg->Serial(), static_cast<VenueMessage::MsgType>(Msg->Type()), const_cast<std::shared_ptr<nlohmann::json>&>(Msg->Payload()));
                    }
                } catch (const Poco::Exception &E) {
                    Logger_.log(E);
                } catch (...) {
                }
            }
            Note = Queue_.waitDequeueNotification();
        }
    }

    std::size_t VenueWorkerPool::ShardIndex(VenueWatcher *VW) const {
        // Use venue id string for stable sharding across restarts
        auto key = VW->Venue();
        // FNV-1a 64-bit simple hash
        uint64_t h = 1469598103934665603ULL;
        for (auto c : key) {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ULL;
        }
        return Workers_.empty() ? 0 : (h % Workers_.size());
    }

    int VenueWorkerPool::Start() {
        std::lock_guard G(Mutex_);
        poco_notice(Logger(), "Starting...");

        if (Running_)
            return 0;

        auto workers = MicroServiceConfigGetInt("openwifi.analytics.workers", std::max<uint64_t>(2, std::thread::hardware_concurrency()));
        if (workers < 2) workers = 2;
        if (workers > 128) workers = 128;
        MaxQueueSize_ = MicroServiceConfigGetInt("openwifi.analytics.queue.size", 1024);
        if (MaxQueueSize_ < 64) MaxQueueSize_ = 64;

        Workers_.reserve(workers);
        for (uint64_t i = 0; i < workers; ++i) {
            auto W = std::make_unique<Worker>(Logger());
            W->Running_ = true;
            W->Thread_.start(*W);
            Workers_.push_back(std::move(W));
        }

        Running_ = true;
        poco_notice(Logger(), fmt::format("Started with {} workers, queue size {}", workers, MaxQueueSize_));
        return 0;
    }

    void VenueWorkerPool::Stop() {
        std::lock_guard G(Mutex_);
        if (!Running_) return;

        poco_notice(Logger(), "Stopping...");
        for (auto &W : Workers_) {
            W->Running_ = false;
            W->Queue_.wakeUpAll();
        }
        for (auto &W : Workers_) {
            W->Thread_.join();
        }
        Workers_.clear();
        Running_ = false;
        poco_notice(Logger(), "Stopped...");
    }

    bool VenueWorkerPool::Enqueue(VenueWatcher *VW,
                                  uint64_t Serial,
                                  uint64_t MsgType,
                                  const std::shared_ptr<nlohmann::json> &Payload) {
        if (!Running_ || Workers_.empty())
            return false;
        auto idx = ShardIndex(VW);
        auto &Q = Workers_[idx]->Queue_;
        if (Q.size() >= MaxQueueSize_) {
            poco_warning(Logger(), fmt::format("Worker {} queue full ({}). Dropping message for venue {}.", idx, Q.size(), VW->Venue()));
            return false;
        }
        Q.enqueueNotification(new VenueDispatchMessage(VW, Serial, MsgType, Payload));
        return true;
    }

} // namespace OpenWifi
