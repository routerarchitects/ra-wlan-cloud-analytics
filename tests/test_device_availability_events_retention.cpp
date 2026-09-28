#include "storage/storage_device_availability_events.h"

#include "Poco/Data/SQLite/Connector.h"
#include "Poco/Data/SessionPool.h"
#include "Poco/Logger.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>

namespace OpenWifi {
	const std::string &MicroServiceDataDirectory() {
		static const std::string DataDirectory = "/tmp";
		return DataDirectory;
	}
} // namespace OpenWifi

using namespace OpenWifi;

namespace {
	AnalyticsObjects::DeviceAvailabilityEvent Event(const std::string &Id,
										  const std::string &BoardId,
										  const std::string &SerialNumber,
										  uint64_t EventTime) {
		AnalyticsObjects::DeviceAvailabilityEvent E;
		E.id = Id;
		E.board_id = BoardId;
		E.serialNumber = SerialNumber;
		E.event_type = "offline";
		E.event_time = EventTime;
		E.event_id = Id;
		E.idempotency_key = Id + "-idempotency";
		return E;
	}

	void AssertCount(DeviceAvailabilityEventsDB &DB, const std::string &BoardId,
					 const std::string &SerialNumber, uint64_t ExpectedCount,
					 std::optional<uint64_t> ExpectedStart,
					 std::optional<uint64_t> ExpectedEnd) {
		uint64_t Count = 0;
		std::optional<uint64_t> ObservedStart;
		std::optional<uint64_t> ObservedEnd;
		assert(DB.CountOfflineEventsByBoardAndSerial(BoardId, SerialNumber, 0, 2000, Count,
											 ObservedStart, ObservedEnd));
		assert(Count == ExpectedCount);
		assert(ObservedStart == ExpectedStart);
		assert(ObservedEnd == ExpectedEnd);
	}

	void TestDeleteExpiredEventsForBoard() {
		const std::string DbPath = "/tmp/test_device_availability_events_retention.sqlite";
		std::remove(DbPath.c_str());
		Poco::Data::SQLite::Connector::registerConnector();
		Poco::Data::SessionPool Pool("SQLite", DbPath, 1, 1, 60);
		auto &Logger = Poco::Logger::get("test_device_availability_events_retention");
		DeviceAvailabilityEventsDB DB(sqlite, Pool, Logger);
		assert(DB.Create());

		assert(DB.CreateRecord(Event("expired-a-router", "board-a", "router-1", 999)));
		assert(DB.CreateRecord(Event("cutoff-a-router", "board-a", "router-1", 1000)));
		assert(DB.CreateRecord(Event("inside-a-router", "board-a", "router-1", 1001)));
		assert(DB.CreateRecord(Event("expired-a-other-router", "board-a", "router-2", 900)));
		assert(DB.CreateRecord(Event("expired-b-same-router", "board-b", "router-1", 900)));

		assert(DB.DeleteExpiredEventsForBoard("board-a", 1000));

		AssertCount(DB, "board-a", "router-1", 2, 1000, 1001);
		AssertCount(DB, "board-a", "router-2", 0, std::nullopt, std::nullopt);
		AssertCount(DB, "board-b", "router-1", 1, 900, 900);

		assert(DB.DeleteExpiredEventsForBoard("board-b", 0));
		AssertCount(DB, "board-b", "router-1", 1, 900, 900);
		Pool.shutdown();
		std::remove(DbPath.c_str());
	}
} // namespace

int main() {
	TestDeleteExpiredEventsForBoard();
	std::cout << "test_device_availability_events_retention passed\n";
	return 0;
}
