#include "storage_device_availability_events.h"

#include "fmt/format.h"

namespace OpenWifi {
	static ORM::FieldVec DeviceAvailabilityEvent_Fields{
		ORM::Field{"id", 64, true},
		ORM::Field{"board_id", ORM::FieldType::FT_TEXT},
		ORM::Field{"serialNumber", ORM::FieldType::FT_TEXT},
		ORM::Field{"event_type", ORM::FieldType::FT_TEXT},
		ORM::Field{"event_time", ORM::FieldType::FT_BIGINT},
		ORM::Field{"reason", ORM::FieldType::FT_TEXT},
		ORM::Field{"connection_ip", ORM::FieldType::FT_TEXT},
		ORM::Field{"session_id", ORM::FieldType::FT_TEXT},
		ORM::Field{"event_id", ORM::FieldType::FT_TEXT},
		ORM::Field{"idempotency_key", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec DeviceAvailabilityEventsDB_Indexes{
		{std::string("availability_serial_time_index"),
		 ORM::IndexEntryVec{{std::string("serialNumber"), ORM::Indextype::ASC},
							{std::string("event_time"), ORM::Indextype::ASC}}},
		{std::string("availability_board_serial_time_index"),
		 ORM::IndexEntryVec{{std::string("board_id"), ORM::Indextype::ASC},
							{std::string("serialNumber"), ORM::Indextype::ASC},
							{std::string("event_time"), ORM::Indextype::ASC}}},
		{std::string("availability_board_time_index"),
		 ORM::IndexEntryVec{{std::string("board_id"), ORM::Indextype::ASC},
							{std::string("event_time"), ORM::Indextype::ASC}}},
		{std::string("availability_event_id_index"),
		 ORM::IndexEntryVec{{std::string("event_id"), ORM::Indextype::ASC}}}};

	DeviceAvailabilityEventsDB::DeviceAvailabilityEventsDB(OpenWifi::DBType T,
														   Poco::Data::SessionPool &P,
														   Poco::Logger &L)
		: DB(T, "device_availability_events", DeviceAvailabilityEvent_Fields,
			 DeviceAvailabilityEventsDB_Indexes, P, L, "dae") {}

	bool DeviceAvailabilityEventsDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		to = 1;
		return RunScript({"CREATE UNIQUE INDEX IF NOT EXISTS "
						  "availability_idempotency_key_unique "
						  "ON device_availability_events(idempotency_key)"});
	}

	bool DeviceAvailabilityEventsDB::CreateEventIfAbsent(
		const AnalyticsObjects::DeviceAvailabilityEvent &Event) {
		if (Event.idempotency_key.empty())
			return false;
		if (Exists("idempotency_key", Event.idempotency_key))
			return true;
		if (CreateRecord(Event))
			return true;
		return Exists("idempotency_key", Event.idempotency_key);
	}

	bool DeviceAvailabilityEventsDB::CountOfflineEventsByBoardAndSerial(
		const std::string &boardId, const std::string &serialNumber, uint64_t startTime,
		uint64_t endTime, uint64_t &offlineCount,
		std::optional<uint64_t> &observedStartTime,
		std::optional<uint64_t> &observedEndTime) {
		offlineCount = 0;
		observedStartTime.reset();
		observedEndTime.reset();
		if (endTime <= startTime)
			return true;

		auto WhereClause =
			fmt::format(" board_id='{}' and serialNumber='{}' and event_type='offline' and "
						"(event_time >= {}) and (event_time < {}) ",
						ORM::Escape(boardId), ORM::Escape(serialNumber), startTime, endTime);
		const auto Sql = fmt::format(
			"select count(*), coalesce(min(event_time), 0), coalesce(max(event_time), 0) "
			"from {} where {}",
			TableName_, WhereClause);

		std::vector<DeviceAvailabilityEventCountDBRecordType> Counts;
		if (!Join(Sql, Counts))
			return false;
		if (Counts.empty())
			return true;

		offlineCount = Counts.front().get<0>();
		if (offlineCount > 0) {
			observedStartTime = Counts.front().get<1>();
			observedEndTime = Counts.front().get<2>();
		}
		return true;
	}

	bool DeviceAvailabilityEventsDB::DeleteExpiredEventsForBoard(const std::string &boardId,
														  uint64_t cutoffTime) {
		return DeleteRecords(
			fmt::format(" board_id='{}' and event_time<{} ", ORM::Escape(boardId), cutoffTime));
	}

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::DeviceAvailabilityEventDBRecordType,
			 OpenWifi::AnalyticsObjects::DeviceAvailabilityEvent>::
	Convert(const OpenWifi::DeviceAvailabilityEventDBRecordType &In,
			OpenWifi::AnalyticsObjects::DeviceAvailabilityEvent &Out) {
	Out.id = In.get<0>();
	Out.board_id = In.get<1>();
	Out.serialNumber = In.get<2>();
	Out.event_type = In.get<3>();
	Out.event_time = In.get<4>();
	Out.reason = In.get<5>();
	Out.connection_ip = In.get<6>();
	Out.session_id = In.get<7>();
	Out.event_id = In.get<8>();
	Out.idempotency_key = In.get<9>();
}

template <>
void ORM::DB<OpenWifi::DeviceAvailabilityEventDBRecordType,
			 OpenWifi::AnalyticsObjects::DeviceAvailabilityEvent>::
	Convert(const OpenWifi::AnalyticsObjects::DeviceAvailabilityEvent &In,
			OpenWifi::DeviceAvailabilityEventDBRecordType &Out) {
	Out.set<0>(In.id);
	Out.set<1>(In.board_id);
	Out.set<2>(In.serialNumber);
	Out.set<3>(In.event_type);
	Out.set<4>(In.event_time);
	Out.set<5>(In.reason);
	Out.set<6>(In.connection_ip);
	Out.set<7>(In.session_id);
	Out.set<8>(In.event_id);
	Out.set<9>(In.idempotency_key);
}
