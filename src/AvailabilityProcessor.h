//
// AvailabilityProcessor Header
//

#pragma once

#include "framework/SubSystemServer.h"
#include "framework/OpenWifiTypes.h"
#include "Poco/NotificationQueue.h"
#include "Poco/Thread.h"
#include <string>
#include <mutex>
#include <atomic>

namespace OpenWifi {

	class AvailabilityMessage : public Poco::Notification {
	  public:
		AvailabilityMessage(std::string key, std::string payload)
			: key_(std::move(key)), payload_(std::move(payload)) {}
		const std::string &Key() const { return key_; }
		const std::string &Payload() const { return payload_; }

	  private:
		std::string key_;
		std::string payload_;
	};

	class AvailabilityProcessor : public SubSystemServer, Poco::Runnable {
	  public:
		static auto instance() {
			static auto instance_ = new AvailabilityProcessor;
			return instance_;
		}

		int Start() override;
		void Stop() override;
		void run() override;

		void ProcessConnectionEvent(const std::string &key, const std::string &payload);

	  private:
		Poco::Thread Worker_;
		std::atomic_bool Running_ = false;
		Poco::NotificationQueue Queue_;
		uint64_t TopicWatcherId_ = 0;
		std::mutex Mutex_;

		AvailabilityProcessor() noexcept
			: SubSystemServer("AvailabilityProcessor", "AVAIL-PROC", "availability.processor") {}
	};

	inline auto AvailabilityProcessorService() { return AvailabilityProcessor::instance(); }

} // namespace OpenWifi
