#pragma once

#include "RESTObjects/RESTAPI_AnalyticsObjects.h"
#include "framework/orm.h"
#include <optional>

namespace OpenWifi {
	typedef Poco::Tuple<std::string, std::string, std::string, std::string, uint64_t,
						std::string, std::string, std::string, std::string, std::string>
		DeviceAvailabilityEventDBRecordType;
	typedef Poco::Tuple<uint64_t, uint64_t, uint64_t> DeviceAvailabilityEventCountDBRecordType;

	class DeviceAvailabilityEventsDB
		: public ORM::DB<DeviceAvailabilityEventDBRecordType,
						 AnalyticsObjects::DeviceAvailabilityEvent> {
	  public:
		DeviceAvailabilityEventsDB(OpenWifi::DBType T, Poco::Data::SessionPool &P,
								   Poco::Logger &L);

		bool CountOfflineEventsByBoardAndSerial(
			const std::string &boardId, const std::string &serialNumber,
			uint64_t startTime, uint64_t endTime, uint64_t &offlineCount,
			std::optional<uint64_t> &observedStartTime,
			std::optional<uint64_t> &observedEndTime);
		bool DeleteExpiredEventsForBoard(const std::string &boardId, uint64_t cutoffTime);
		bool DeleteEventsForBoard(const std::string &boardId);

		bool CreateEventIfAbsent(const AnalyticsObjects::DeviceAvailabilityEvent &Event);

	  private:
		bool Upgrade(uint32_t from, uint32_t &to) override;
	};
} // namespace OpenWifi
