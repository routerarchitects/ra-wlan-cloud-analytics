#include "ProvisioningChangeReceiver.h"
#include "VenueCoordinator.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "framework/utils.h"
#include "Poco/Logger.h"
#include "fmt/format.h"

namespace OpenWifi {

    int ProvisioningChangeReceiver::Start() {
        if (running_) return 0;
        if (!KafkaManager()->Enabled()) {
            poco_warning(Logger(), "Kafka disabled; provisioning change receiver inactive.");
            return 0;
        }
        running_ = true;
        worker_.start(*this);
        Types::TopicNotifyFunction fn = [this](const std::string &key, const std::string &value) {
            this->Enqueue(key, value);
        };
        watcher_id_ = KafkaManager()->RegisterTopicWatcher(KafkaTopics::PROVISIONING_CHANGE, fn);
        poco_notice(Logger(), "Provisioning change receiver started.");
        return 0;
    }

    void ProvisioningChangeReceiver::Stop() {
        if (!running_) return;
        running_ = false;
        if (KafkaManager()->Enabled()) {
            KafkaManager()->UnregisterTopicWatcher(KafkaTopics::PROVISIONING_CHANGE, watcher_id_);
        }
        queue_.wakeUpAll();
        worker_.join();
        poco_notice(Logger(), "Provisioning change receiver stopped.");
    }

    void ProvisioningChangeReceiver::run() {
        Utils::SetThreadName("prov-change");
        while (running_) {
            Poco::AutoPtr<Poco::Notification> note(queue_.waitDequeueNotification());
            if (!note) {
                if (!running_) break;
                continue;
            }
            auto msg = dynamic_cast<ProvisioningChangeNotification *>(note.get());
            if (!msg) continue;
            ProvisioningChangeEvent event;
            if (!ParseProvisioningChangeEvent(msg->Payload(), event, Logger())) {
                poco_warning(Logger(), "Ignoring invalid provisioning change event payload.");
                continue;
            }
            try {
                HandleEvent(event);
            } catch (const Poco::Exception &E) {
                Logger().log(E);
            } catch (const std::exception &E) {
                poco_error(Logger(), fmt::format("Provisioning event handling failed: {}", E.what()));
            } catch (...) {
                poco_error(Logger(), "Provisioning event handling failed: unknown error");
            }
        }
    }

    void ProvisioningChangeReceiver::HandleEvent(const ProvisioningChangeEvent &event) {
        if (!event.Valid()) {
            poco_warning(Logger(), "Received provisioning event without type or board id.");
            return;
        }
        VenueCoordinator()->HandleProvisioningEvent(event);
    }

    void ProvisioningChangeReceiver::Enqueue(const std::string &key, const std::string &payload) {
        if (!running_) return;
        queue_.enqueueNotification(new ProvisioningChangeNotification(key, payload));
    }

} // namespace OpenWifi
