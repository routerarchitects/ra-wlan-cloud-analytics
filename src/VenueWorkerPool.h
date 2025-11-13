//
// Created to replace per-venue threads with a shared worker pool.
//

#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <functional>
#include <utility>

#include "Poco/Thread.h"
#include "Poco/Notification.h"
#include "Poco/NotificationQueue.h"
#include "Poco/Logger.h"

#include "framework/SubSystemServer.h"
#include "framework/MicroServiceFuncs.h"
#include "nlohmann/json.hpp"

namespace OpenWifi {

    class VenueWatcher;

    class VenueDispatchMessage : public Poco::Notification {
      public:
        VenueDispatchMessage(std::shared_ptr<VenueWatcher> VW,
                             uint64_t Serial,
                             uint64_t MsgType,
                             const std::shared_ptr<nlohmann::json> &Payload)
            : Watcher_(std::move(VW)), Serial_(Serial), MsgType_(MsgType), Payload_(Payload) {}

        inline std::shared_ptr<VenueWatcher> Watcher() const { return Watcher_; }
        inline uint64_t Serial() const { return Serial_; }
        inline uint64_t Type() const { return MsgType_; }
        inline const std::shared_ptr<nlohmann::json> &Payload() const { return Payload_; }

      private:
        std::shared_ptr<VenueWatcher> Watcher_;
        uint64_t Serial_;
        uint64_t MsgType_;
        std::shared_ptr<nlohmann::json> Payload_;
    };

    class VenueWorkerPool : public SubSystemServer {
      public:
        static auto instance() {
            static auto instance_ = new VenueWorkerPool;
            return instance_;
        }

        int Start() override;
        void Stop() override;

        // Enqueue a message for the venue's shard. Returns false if dropped due to backpressure.
        bool Enqueue(const std::shared_ptr<VenueWatcher> &VW,
                     uint64_t Serial,
                     uint64_t MsgType,
                     const std::shared_ptr<nlohmann::json> &Payload);

      private:
        struct Worker : public Poco::Runnable {
            Poco::NotificationQueue Queue_;
            Poco::Thread Thread_;
            std::atomic_bool Running_{false};
            Poco::Logger &Logger_;
            explicit Worker(Poco::Logger &L) : Logger_(L) {}
            void run() override;
        };

        std::vector<std::unique_ptr<Worker>> Workers_;
        std::atomic_bool Running_{false};
        std::size_t MaxQueueSize_ = 1024; // bounded per worker

        std::size_t ShardIndex(VenueWatcher *VW) const;

        VenueWorkerPool() noexcept : SubSystemServer("VenueWorkerPool", "VENUE-WORKERS", "openwifi.analytics") {}
    };

    inline auto VenueWorkerPool() { return VenueWorkerPool::instance(); }

} // namespace OpenWifi
