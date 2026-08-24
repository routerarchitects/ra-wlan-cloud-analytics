//
// AvailabilityProcessor Implementation
//

#include "AvailabilityProcessor.h"
#include "RouterIdResolver.h"
#include "VenueCoordinator.h"
#include "StorageService.h"
#include "fmt/format.h"
#include "framework/KafkaManager.h"
#include "framework/KafkaTopics.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/utils.h"
#include "nlohmann/json.hpp"

namespace OpenWifi {

	int AvailabilityProcessor::Start() {
		poco_notice(Logger(), "Starting AvailabilityProcessor...");
		Running_ = true;

		Types::TopicNotifyFunction F = [this](const std::string &Key, const std::string &Payload) {
			this->ProcessConnectionEvent(Key, Payload);
		};

		if (KafkaManager()->Enabled()) {
			TopicWatcherId_ = KafkaManager()->RegisterTopicWatcher(KafkaTopics::CONNECTION, F);
		}

		Worker_.start(*this);
		return 0;
	}

	void AvailabilityProcessor::Stop() {
		poco_notice(Logger(), "Stopping AvailabilityProcessor...");
		Running_ = false;
		if (KafkaManager()->Enabled() && TopicWatcherId_ != 0) {
			KafkaManager()->UnregisterTopicWatcher(KafkaTopics::CONNECTION, TopicWatcherId_);
		}
		Queue_.wakeUpAll();
		Worker_.join();
		poco_notice(Logger(), "AvailabilityProcessor stopped.");
	}

	void AvailabilityProcessor::ProcessConnectionEvent(const std::string &key,
														const std::string &payload) {
		std::lock_guard G(Mutex_);
		Queue_.enqueueNotification(new AvailabilityMessage(key, payload));
	}

	void AvailabilityProcessor::run() {
		Utils::SetThreadName("avail-proc");
		Poco::AutoPtr<Poco::Notification> Note(Queue_.waitDequeueNotification());

		while (Note && Running_) {
			auto Msg = dynamic_cast<AvailabilityMessage *>(Note.get());
			if (Msg != nullptr) {
				try {
					std::string serialNumber = Msg->Key();
					nlohmann::json msgObj = nlohmann::json::parse(Msg->Payload());

					std::string eventType;
					uint64_t eventTime = 0;
					std::string sessionId;
					std::string eventId;

					if (msgObj.contains("payload") && msgObj["payload"].is_object()) {
						auto payload = msgObj["payload"];
						if (payload.contains("ping")) {
							eventType = "online";
							auto ping = payload["ping"];
							if (ping.contains("timestamp")) eventTime = ping["timestamp"].get<uint64_t>();
							if (ping.contains("session_id")) sessionId = ping["session_id"].get<std::string>();
							if (ping.contains("event_id")) eventId = ping["event_id"].get<std::string>();
						} else if (payload.contains("capabilities")) {
							eventType = "online";
							auto caps = payload["capabilities"];
							if (caps.contains("timestamp")) eventTime = caps["timestamp"].get<uint64_t>();
							if (caps.contains("session_id")) sessionId = caps["session_id"].get<std::string>();
							if (caps.contains("event_id")) eventId = caps["event_id"].get<std::string>();
						} else if (payload.contains("disconnection")) {
							eventType = "offline";
							auto disc = payload["disconnection"];
							if (disc.contains("timestamp")) eventTime = disc["timestamp"].get<uint64_t>();
							if (disc.contains("session_id")) sessionId = disc["session_id"].get<std::string>();
							if (disc.contains("event_id")) eventId = disc["event_id"].get<std::string>();
						}
					}

					if (eventTime == 0) {
						eventTime = Utils::Now();
					}

					if (!eventType.empty() && !serialNumber.empty()) {
						// Service-authenticated internal resolution via VenueCoordinator
						std::string boardId;
						auto status = VenueCoordinator()->FindBoardForDevice(serialNumber, boardId);

						if (status == DeviceBoardLookupStatus::MultipleBoards) {
							poco_warning(Logger(), fmt::format("Skipping availability processing for router {}: ambiguous board mapping (multiple boards)", serialNumber));
							continue;
						} else if (status != DeviceBoardLookupStatus::Success || boardId.empty()) {
							poco_warning(Logger(), fmt::format("Skipping availability processing for router {}: board resolution failed in VenueCoordinator mapping", serialNumber));
							continue;
						}

						// Load existing availability state
						DeviceAvailabilityState currentState;
						bool stateExists = StorageService()->AvailabilityStateDB().GetState(serialNumber, currentState);

						uint64_t nowSec = Utils::Now();

						if (!stateExists) {
							// Initial state creation - NO transition event inserted
							DeviceAvailabilityState newState;
							newState.serialNumber = serialNumber;
							newState.board_id = boardId;
							newState.current_state = eventType;
							newState.last_event_time = eventTime;
							newState.updated_at = nowSec;

							if (!StorageService()->AvailabilityStateDB().UpdateState(newState)) {
								poco_error(Logger(), fmt::format("Failed to persist initial availability state for router {}", serialNumber));
								continue;
							}
						} else {
							// Check event time ordering
							if (eventTime > currentState.last_event_time) {
								bool stateChanged = (currentState.current_state != eventType);

								if (stateChanged) {
									// Insert transition event and update state atomically
									DeviceAvailabilityEvent eventRow;
									eventRow.id = MicroServiceCreateUUID();
									eventRow.serialNumber = serialNumber;
									eventRow.board_id = boardId;
									eventRow.event_type = eventType;
									eventRow.event_time = eventTime;
									eventRow.session_id = sessionId;
									eventRow.event_id = eventId;
									eventRow.idempotency_key =
										fmt::format("{}:{}:{}:{}", serialNumber, eventType, eventTime,
													eventId.empty() ? (sessionId.empty() ? "nosession" : sessionId) : eventId);

									DeviceAvailabilityState nextState = currentState;
									nextState.board_id = boardId;
									nextState.current_state = eventType;
									nextState.last_event_time = eventTime;
									nextState.updated_at = nowSec;

									if (!StorageService()->AvailabilityEventsDB().RecordTransitionAndState(eventRow, nextState)) {
										poco_error(Logger(), fmt::format("Failed to record availability transition for router {}", serialNumber));
										continue;
									}
								} else {
									// Update last event time and board_id if state didn't change
									currentState.board_id = boardId;
									currentState.last_event_time = eventTime;
									currentState.updated_at = nowSec;

									if (!StorageService()->AvailabilityStateDB().UpdateState(currentState)) {
										poco_error(Logger(), fmt::format("Failed to update availability state for router {}", serialNumber));
										continue;
									}
								}
							}
							// If eventTime <= last_event_time, ignore stale/duplicate event
						}
					}
				} catch (const Poco::Exception &E) {
					Logger().log(E);
				} catch (...) {
				}
			}
			Note = Queue_.waitDequeueNotification();
		}
	}

} // namespace OpenWifi
