//
// Created by stephane bourque on 2022-03-10.
//

#include "VenueWatcher.h"
#include "DeviceStatusReceiver.h"
#include "HealthReceiver.h"
#include "StateReceiver.h"

namespace OpenWifi {

	void VenueWatcher::Start() {
		poco_notice(Logger(), "Starting...");
		for (const auto &mac : SerialNumbers_) {
			auto ap = std::make_shared<AP>(mac, venue_id_, boardId_, Logger());
			APs_[mac] = ap;
		}

		for (const auto &i : SerialNumbers_)
			StateReceiver()->Register(i, this);

		DeviceStatusReceiver()->Register(SerialNumbers_, this);
		HealthReceiver()->Register(SerialNumbers_, this);
		// No per-venue worker thread; messages will be processed by VenueWorkerPool
	}

	void VenueWatcher::Stop() {
		poco_notice(Logger(), "Stopping...");
		Running_ = false;
		// No per-venue worker thread to stop
		for (const auto &i : SerialNumbers_)
			StateReceiver()->DeRegister(i, this);
		DeviceStatusReceiver()->DeRegister(this);
		HealthReceiver()->DeRegister(this);
		poco_notice(Logger(), "Stopped...");
	}

	void VenueWatcher::run() {
		// Legacy per-venue thread not used anymore.
}

	void VenueWatcher::ModifySerialNumbers(const std::vector<uint64_t> &SerialNumbers) {
		std::lock_guard G(Mutex_);

		std::vector<uint64_t> Diff;
		std::set_symmetric_difference(SerialNumbers_.begin(), SerialNumbers_.end(),
									  SerialNumbers.begin(), SerialNumbers.end(),
									  std::inserter(Diff, Diff.begin()));

		std::vector<uint64_t> ToRemove;
		std::set_intersection(SerialNumbers_.begin(), SerialNumbers_.end(), Diff.begin(),
							  Diff.end(), std::inserter(ToRemove, ToRemove.begin()));

		std::vector<uint64_t> ToAdd;
		std::set_intersection(SerialNumbers.begin(), SerialNumbers.end(), Diff.begin(), Diff.end(),
							  std::inserter(ToAdd, ToAdd.begin()));

		for (const auto &i : ToRemove) {
			StateReceiver()->DeRegister(i, this);
		}
		for (const auto &i : ToAdd) {
			StateReceiver()->Register(i, this);
		}

		HealthReceiver()->Register(SerialNumbers, this);
		DeviceStatusReceiver()->Register(SerialNumbers, this);

		SerialNumbers_ = SerialNumbers;
	}

	void VenueWatcher::GetDevices(std::vector<AnalyticsObjects::DeviceInfo> &DIL) {
		std::lock_guard G(Mutex_);

		DIL.reserve(APs_.size());
		for (const auto &[serialNumber, DI] : APs_)
			DIL.push_back(DI->Info());
	}

	void VenueWatcher::Process(uint64_t SerialNumber, VenueMessage::MsgType Type,
							 const std::shared_ptr<nlohmann::json> &Msg) {
		try {
			std::lock_guard G(Mutex_);
			auto It = APs_.find(SerialNumber);
			if (It == end(APs_))
				return;
			if (Type == VenueMessage::connection) {
				It->second->UpdateConnection(Msg);
			} else if (Type == VenueMessage::state) {
				It->second->UpdateStats(Msg);
			} else if (Type == VenueMessage::health) {
				It->second->UpdateHealth(Msg);
			}
		} catch (const Poco::Exception &E) {
			Logger().log(E);
		} catch (...) {
		}
	}

} // namespace OpenWifi
