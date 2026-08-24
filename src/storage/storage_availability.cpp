//
// Availability Storage Implementation
//

#include "storage_availability.h"
#include "fmt/format.h"
#include "framework/RESTAPI_utils.h"
#include "Poco/Data/Transaction.h"

template <>
void ORM::DB<OpenWifi::DeviceAvailabilityEventRecordType, OpenWifi::DeviceAvailabilityEvent>::Convert(
	const OpenWifi::DeviceAvailabilityEventRecordType &In, OpenWifi::DeviceAvailabilityEvent &Out) {
	Out.id = In.get<0>();
	Out.serialNumber = In.get<1>();
	Out.board_id = In.get<2>();
	Out.event_type = In.get<3>();
	Out.event_time = In.get<4>();
	Out.session_id = In.get<5>();
	Out.event_id = In.get<6>();
	Out.idempotency_key = In.get<7>();
}

template <>
void ORM::DB<OpenWifi::DeviceAvailabilityEventRecordType, OpenWifi::DeviceAvailabilityEvent>::Convert(
	const OpenWifi::DeviceAvailabilityEvent &In, OpenWifi::DeviceAvailabilityEventRecordType &Out) {
	Out.set<0>(In.id);
	Out.set<1>(In.serialNumber);
	Out.set<2>(In.board_id);
	Out.set<3>(In.event_type);
	Out.set<4>(In.event_time);
	Out.set<5>(In.session_id);
	Out.set<6>(In.event_id);
	Out.set<7>(In.idempotency_key);
}

template <>
void ORM::DB<OpenWifi::DeviceAvailabilityStateRecordType, OpenWifi::DeviceAvailabilityState>::Convert(
	const OpenWifi::DeviceAvailabilityStateRecordType &In, OpenWifi::DeviceAvailabilityState &Out) {
	Out.serialNumber = In.get<0>();
	Out.board_id = In.get<1>();
	Out.current_state = In.get<2>();
	Out.last_event_time = In.get<3>();
	Out.updated_at = In.get<4>();
}

template <>
void ORM::DB<OpenWifi::DeviceAvailabilityStateRecordType, OpenWifi::DeviceAvailabilityState>::Convert(
	const OpenWifi::DeviceAvailabilityState &In, OpenWifi::DeviceAvailabilityStateRecordType &Out) {
	Out.set<0>(In.serialNumber);
	Out.set<1>(In.board_id);
	Out.set<2>(In.current_state);
	Out.set<3>(In.last_event_time);
	Out.set<4>(In.updated_at);
}

template <>
void ORM::DB<OpenWifi::SystemPropertyRecordType, OpenWifi::SystemProperty>::Convert(
	const OpenWifi::SystemPropertyRecordType &In, OpenWifi::SystemProperty &Out) {
	Out.k = In.get<0>();
	Out.v = In.get<1>();
}

template <>
void ORM::DB<OpenWifi::SystemPropertyRecordType, OpenWifi::SystemProperty>::Convert(
	const OpenWifi::SystemProperty &In, OpenWifi::SystemPropertyRecordType &Out) {
	Out.set<0>(In.k);
	Out.set<1>(In.v);
}

namespace OpenWifi {

	void DeviceAvailabilityEvent::to_json(Poco::JSON::Object &Obj) const {
		RESTAPI_utils::field_to_json(Obj, "id", id);
		RESTAPI_utils::field_to_json(Obj, "serialNumber", serialNumber);
		RESTAPI_utils::field_to_json(Obj, "board_id", board_id);
		RESTAPI_utils::field_to_json(Obj, "event_type", event_type);
		RESTAPI_utils::field_to_json(Obj, "event_time", event_time);
		RESTAPI_utils::field_to_json(Obj, "session_id", session_id);
		RESTAPI_utils::field_to_json(Obj, "event_id", event_id);
		RESTAPI_utils::field_to_json(Obj, "idempotency_key", idempotency_key);
	}

	bool DeviceAvailabilityEvent::from_json(const Poco::JSON::Object::Ptr &Obj) {
		try {
			RESTAPI_utils::field_from_json(Obj, "id", id);
			RESTAPI_utils::field_from_json(Obj, "serialNumber", serialNumber);
			RESTAPI_utils::field_from_json(Obj, "board_id", board_id);
			RESTAPI_utils::field_from_json(Obj, "event_type", event_type);
			RESTAPI_utils::field_from_json(Obj, "event_time", event_time);
			RESTAPI_utils::field_from_json(Obj, "session_id", session_id);
			RESTAPI_utils::field_from_json(Obj, "event_id", event_id);
			RESTAPI_utils::field_from_json(Obj, "idempotency_key", idempotency_key);
			return true;
		} catch (...) {
		}
		return false;
	}

	void DeviceAvailabilityState::to_json(Poco::JSON::Object &Obj) const {
		RESTAPI_utils::field_to_json(Obj, "serialNumber", serialNumber);
		RESTAPI_utils::field_to_json(Obj, "board_id", board_id);
		RESTAPI_utils::field_to_json(Obj, "current_state", current_state);
		RESTAPI_utils::field_to_json(Obj, "last_event_time", last_event_time);
		RESTAPI_utils::field_to_json(Obj, "updated_at", updated_at);
	}

	bool DeviceAvailabilityState::from_json(const Poco::JSON::Object::Ptr &Obj) {
		try {
			RESTAPI_utils::field_from_json(Obj, "serialNumber", serialNumber);
			RESTAPI_utils::field_from_json(Obj, "board_id", board_id);
			RESTAPI_utils::field_from_json(Obj, "current_state", current_state);
			RESTAPI_utils::field_from_json(Obj, "last_event_time", last_event_time);
			RESTAPI_utils::field_from_json(Obj, "updated_at", updated_at);
			return true;
		} catch (...) {
		}
		return false;
	}

	void SystemProperty::to_json(Poco::JSON::Object &Obj) const {
		RESTAPI_utils::field_to_json(Obj, "k", k);
		RESTAPI_utils::field_to_json(Obj, "v", v);
	}

	bool SystemProperty::from_json(const Poco::JSON::Object::Ptr &Obj) {
		try {
			RESTAPI_utils::field_from_json(Obj, "k", k);
			RESTAPI_utils::field_from_json(Obj, "v", v);
			return true;
		} catch (...) {
		}
		return false;
	}

	static ORM::FieldVec AvailabilityEvents_Fields{
		ORM::Field{"id", 64, true},
		ORM::Field{"serialNumber", ORM::FieldType::FT_TEXT},
		ORM::Field{"board_id", ORM::FieldType::FT_TEXT},
		ORM::Field{"event_type", ORM::FieldType::FT_TEXT},
		ORM::Field{"event_time", ORM::FieldType::FT_BIGINT},
		ORM::Field{"session_id", ORM::FieldType::FT_TEXT},
		ORM::Field{"event_id", ORM::FieldType::FT_TEXT},
		ORM::Field{"idempotency_key", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec AvailabilityEvents_Indexes{
		{std::string("avail_serial_time_idx"),
		 ORM::IndexEntryVec{{std::string("serialNumber"), ORM::Indextype::ASC},
							{std::string("event_time"), ORM::Indextype::ASC}}, false},
		{std::string("avail_idempotency_idx"),
		 ORM::IndexEntryVec{{std::string("idempotency_key"), ORM::Indextype::ASC}}, true}};

	DeviceAvailabilityEventsDB::DeviceAvailabilityEventsDB(OpenWifi::DBType T,
															Poco::Data::SessionPool &P,
															Poco::Logger &L)
		: DB(T, "device_availability_events", AvailabilityEvents_Fields, AvailabilityEvents_Indexes,
			 P, L, "dae") {}

	bool DeviceAvailabilityEventsDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		std::vector<std::string> Statements{};
		Statements.push_back("DROP INDEX IF EXISTS avail_idempotency_idx;");
		Statements.push_back("CREATE UNIQUE INDEX IF NOT EXISTS avail_idempotency_idx ON device_availability_events (idempotency_key);");
		if (RunScript(Statements)) {
			to = 1;
			return true;
		}
		return false;
	}

	uint64_t DeviceAvailabilityEventsDB::GetOfflineEvents(const std::string &boardId,
														  const std::string &serialNumber,
														  uint64_t startTime, uint64_t endTime,
														  std::optional<uint64_t> &earliestTime,
														  std::optional<uint64_t> &latestTime) {
		earliestTime.reset();
		latestTime.reset();
		std::string whereClause = fmt::format(
			"board_id='{}' AND serialNumber='{}' AND event_type='offline' AND event_time >= {} AND event_time < {}",
			ORM::Escape(boardId), ORM::Escape(serialNumber), startTime, endTime);

		std::vector<DeviceAvailabilityEvent> recs;
		GetRecords(0, 0, recs, whereClause, " ORDER BY event_time ASC ");

		if (recs.empty()) {
			return 0;
		}

		earliestTime = recs.front().event_time;
		latestTime = recs.back().event_time;
		return recs.size();
	}

	bool DeviceAvailabilityEventsDB::RecordEvent(const DeviceAvailabilityEvent &event) {
		return CreateRecord(event);
	}

	bool DeviceAvailabilityEventsDB::RecordTransitionAndState(const DeviceAvailabilityEvent &event,
															  const DeviceAvailabilityState &state) {
		try {
			Poco::Data::Session Session = Pool_.get();
			Poco::Data::Transaction Tx(Session);

			// Check if idempotency key already exists (already processed)
			std::string checkSt = "select id from device_availability_events where idempotency_key=? limit 1";
			Poco::Data::Statement Check(Session);
			std::string existingId;
			auto keyCopy = event.idempotency_key;
			Check << ConvertParams(checkSt), Poco::Data::Keywords::into(existingId), Poco::Data::Keywords::use(keyCopy);
			if (Check.execute() == 1) {
				// Already processed! Idempotent no-op success.
				Tx.commit();
				return true;
			}

			// 1. Insert transition event into device_availability_events table
			Poco::Data::Statement Insert(Session);
			DeviceAvailabilityEventRecordType RT;
			Convert(event, RT);
			std::string St = "insert into " + TableName_ + " ( " + SelectFields() + " ) values " + SelectList();
			Insert << ConvertParams(St), Poco::Data::Keywords::use(RT);
			Insert.execute();

			// 2. Upsert current state into device_availability_state table
			Poco::Data::Statement SelectState(Session);
			DeviceAvailabilityStateRecordType StateRT;
			std::string StateSelectSt = "select serialNumber, board_id, current_state, last_event_time, updated_at from device_availability_state where serialNumber=? limit 1";
			auto serialCopy = state.serialNumber;
			SelectState << ConvertParams(StateSelectSt), Poco::Data::Keywords::into(StateRT), Poco::Data::Keywords::use(serialCopy);
			bool exists = (SelectState.execute() == 1);

			DeviceAvailabilityStateRecordType InStateRT;
			InStateRT.set<0>(state.serialNumber);
			InStateRT.set<1>(state.board_id);
			InStateRT.set<2>(state.current_state);
			InStateRT.set<3>(state.last_event_time);
			InStateRT.set<4>(state.updated_at);

			if (exists) {
				Poco::Data::Statement UpdateState(Session);
				std::string StateUpdateSt = "update device_availability_state set serialNumber=?, board_id=?, current_state=?, last_event_time=?, updated_at=? where serialNumber=?";
				UpdateState << ConvertParams(StateUpdateSt), Poco::Data::Keywords::use(InStateRT), Poco::Data::Keywords::use(serialCopy);
				UpdateState.execute();
			} else {
				Poco::Data::Statement CreateState(Session);
				std::string StateCreateSt = "insert into device_availability_state ( serialNumber, board_id, current_state, last_event_time, updated_at ) values (?, ?, ?, ?, ?)";
				CreateState << ConvertParams(StateCreateSt), Poco::Data::Keywords::use(InStateRT);
				CreateState.execute();
			}

			Tx.commit();
			return true;
		} catch (const Poco::Exception &E) {
			Logger_.log(E);
		} catch (const std::exception &E) {
			Logger_.error(fmt::format("RecordTransitionAndState error: {}", E.what()));
		}
		return false;
	}

	static ORM::FieldVec AvailabilityState_Fields{
		ORM::Field{"serialNumber", 64, true},
		ORM::Field{"board_id", ORM::FieldType::FT_TEXT},
		ORM::Field{"current_state", ORM::FieldType::FT_TEXT},
		ORM::Field{"last_event_time", ORM::FieldType::FT_BIGINT},
		ORM::Field{"updated_at", ORM::FieldType::FT_BIGINT}};

	static ORM::IndexVec AvailabilityState_Indexes{};

	DeviceAvailabilityStateDB::DeviceAvailabilityStateDB(OpenWifi::DBType T,
														  Poco::Data::SessionPool &P,
														  Poco::Logger &L)
		: DB(T, "device_availability_state", AvailabilityState_Fields, AvailabilityState_Indexes, P, L,
			 "das") {}

	bool DeviceAvailabilityStateDB::GetState(const std::string &serialNumber,
											 DeviceAvailabilityState &state) {
		return GetRecord("serialNumber", serialNumber, state);
	}

	bool DeviceAvailabilityStateDB::UpdateState(const DeviceAvailabilityState &state) {
		DeviceAvailabilityState existing;
		if (GetRecord("serialNumber", state.serialNumber, existing)) {
			return UpdateRecord("serialNumber", state.serialNumber, state);
		}
		return CreateRecord(state);
	}

	static ORM::FieldVec SystemProperties_Fields{
		ORM::Field{"k", 128, true},
		ORM::Field{"v", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec SystemProperties_Indexes{};

	SystemPropertiesDB::SystemPropertiesDB(OpenWifi::DBType T, Poco::Data::SessionPool &P,
											Poco::Logger &L)
		: DB(T, "system_properties", SystemProperties_Fields, SystemProperties_Indexes, P, L, "sysp") {}

	bool SystemPropertiesDB::GetProperty(const std::string &key, std::string &value) {
		SystemProperty prop;
		if (GetRecord("k", key, prop)) {
			value = prop.v;
			return true;
		}
		return false;
	}

	bool SystemPropertiesDB::SetProperty(const std::string &key, const std::string &value) {
		SystemProperty prop{key, value};
		SystemProperty existing;
		if (GetRecord("k", key, existing)) {
			return UpdateRecord("k", key, prop);
		}
		return CreateRecord(prop);
	}

} // namespace OpenWifi
