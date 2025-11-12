#pragma once

#include <atomic>

#include "Poco/Notification.h"
#include "Poco/NotificationQueue.h"
#include "Poco/Thread.h"
#include "framework/SubSystemServer.h"
#include "ProvisioningChangeEvent.h"
#include "framework/OpenWifiTypes.h"

namespace OpenWifi {

    class ProvisioningChangeNotification : public Poco::Notification {
      public:
        ProvisioningChangeNotification(std::string key, std::string payload)
            : key_(std::move(key)), payload_(std::move(payload)) {}
        inline const std::string &Key() const { return key_; }
        inline const std::string &Payload() const { return payload_; }

      private:
        std::string key_;
        std::string payload_;
    };

    class ProvisioningChangeReceiver : public SubSystemServer, Poco::Runnable {
      public:
        static auto instance() {
            static auto instance_ = new ProvisioningChangeReceiver;
            return instance_;
        }

        int Start() override;
        void Stop() override;
        void run() override;

        void HandleEvent(const ProvisioningChangeEvent &event);

      private:
        ProvisioningChangeReceiver() noexcept
            : SubSystemServer("ProvisioningChangeReceiver", "PROV-CHG-RCV", "provisioning.change.receiver") {}

        std::atomic_bool running_{false};
        Poco::Thread worker_;
        Poco::NotificationQueue queue_;
        uint64_t watcher_id_ = 0;

        void Enqueue(const std::string &key, const std::string &payload);
    };

    inline auto ProvisioningChangeReceiver() { return ProvisioningChangeReceiver::instance(); }

} // namespace OpenWifi
