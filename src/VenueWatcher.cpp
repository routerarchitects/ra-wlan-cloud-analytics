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
		{
			std::lock_guard G(Mutex_);
			for (const auto &mac : SerialNumbers_) {
				auto ap = std::make_shared<AP>(mac, venue_id_, boardId_, Logger());
				APs_[mac] = ap;
			}
		}

		for (const auto &i : SerialNumbers_)
			StateReceiver()->Register(i, this);

		DeviceStatusReceiver()->Register(SerialNumbers_, this);
		HealthReceiver()->Register(SerialNumbers_, this);
		Worker_.start(*this);
	}

	void VenueWatcher::Stop() {
		poco_notice(Logger(), "Stopping...");
		Running_ = false;
		Queue_.wakeUpAll();
		Worker_.join();
		for (const auto &i : SerialNumbers_)
			StateReceiver()->DeRegister(i, this);
		DeviceStatusReceiver()->DeRegister(this);
		HealthReceiver()->DeRegister(this);
		poco_notice(Logger(), "Stopped...");
	}

	void VenueWatcher::run() {
		Utils::SetThreadName("venue-watch");
		Running_ = true;
		Poco::AutoPtr<Poco::Notification> Msg(Queue_.waitDequeueNotification());
		while (Msg && Running_) {
			auto MsgContent = dynamic_cast<VenueMessage *>(Msg.get());
			if (MsgContent != nullptr) {
				try {
					std::shared_ptr<AP> ap;
					{
						std::lock_guard G(Mutex_);
						auto It = APs_.find(MsgContent->SerialNumber());
						if (It != end(APs_)) {
							ap = It->second;
						}
					}

					if (ap) {
						switch (MsgContent->Type()) {
							case VenueMessage::connection:
								ap->UpdateConnection(MsgContent->Payload());
								break;
							case VenueMessage::state:
								ap->UpdateStats(MsgContent->Payload());
								break;
							case VenueMessage::health:
								ap->UpdateHealth(MsgContent->Payload());
								break;
							default:
								break;
						}
					}
				} catch (const Poco::Exception &E) {
					Logger().log(E);
				} catch (...) {
				}
			} else {
			}
			Msg = Queue_.waitDequeueNotification();
		}
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
			APs_.erase(i);
		}
		for (const auto &i : ToAdd) {
			StateReceiver()->Register(i, this);
			auto ap = std::make_shared<AP>(i, venue_id_, boardId_, Logger());
			APs_[i] = ap;
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

} // namespace OpenWifi