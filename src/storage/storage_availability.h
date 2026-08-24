//
// Availability Storage Header
//

#pragma once

#include "RESTObjects/RESTAPI_AnalyticsObjects.h"
#include "framework/orm.h"
#include <string>
#include <vector>
#include <optional>

namespace OpenWifi {

	struct DeviceAvailabilityState;

	struct DeviceAvailabilityEvent {
		std::string id;
		std::string serialNumber;
		std::string board_id;
		std::string event_type; // 'online' or 'offline'
		uint64_t event_time = 0;
		std::string session_id;
		std::string event_id;
		std::string idempotency_key;

		void to_json(Poco::JSON::Object &Obj) const;
		bool from_json(const Poco::JSON::Object::Ptr &Obj);
	};

	typedef Poco::Tuple<std::string, std::string, std::string, std::string, uint64_t, std::string,
						std::string, std::string>
		DeviceAvailabilityEventRecordType;

	class DeviceAvailabilityEventsDB
		: public ORM::DB<DeviceAvailabilityEventRecordType, DeviceAvailabilityEvent> {
	  public:
		DeviceAvailabilityEventsDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L);
		bool Upgrade(uint32_t from, uint32_t &to) override;
		uint64_t GetOfflineEvents(const std::string &boardId, const std::string &serialNumber, uint64_t startTime,
								 uint64_t endTime, std::optional<uint64_t> &earliestTime,
								 std::optional<uint64_t> &latestTime);

		bool RecordEvent(const DeviceAvailabilityEvent &event);
		bool RecordTransitionAndState(const DeviceAvailabilityEvent &event,
									  const DeviceAvailabilityState &state);
	};

	struct DeviceAvailabilityState {
		std::string serialNumber;
		std::string board_id;
		std::string current_state = "unknown"; // 'online', 'offline', 'unknown'
		uint64_t last_event_time = 0;
		uint64_t updated_at = 0;

		void to_json(Poco::JSON::Object &Obj) const;
		bool from_json(const Poco::JSON::Object::Ptr &Obj);
	};

	typedef Poco::Tuple<std::string, std::string, std::string, uint64_t, uint64_t>
		DeviceAvailabilityStateRecordType;

	class DeviceAvailabilityStateDB
		: public ORM::DB<DeviceAvailabilityStateRecordType, DeviceAvailabilityState> {
	  public:
		DeviceAvailabilityStateDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L);

		bool GetState(const std::string &serialNumber, DeviceAvailabilityState &state);
		bool UpdateState(const DeviceAvailabilityState &state);
	};

	struct SystemProperty {
		std::string k;
		std::string v;

		void to_json(Poco::JSON::Object &Obj) const;
		bool from_json(const Poco::JSON::Object::Ptr &Obj);
	};

	typedef Poco::Tuple<std::string, std::string> SystemPropertyRecordType;

	class SystemPropertiesDB : public ORM::DB<SystemPropertyRecordType, SystemProperty> {
	  public:
		SystemPropertiesDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L);

		bool GetProperty(const std::string &key, std::string &value);
		bool SetProperty(const std::string &key, const std::string &value);
	};

} // namespace OpenWifi
