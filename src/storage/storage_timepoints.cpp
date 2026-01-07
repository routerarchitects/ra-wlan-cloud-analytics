//
// Created by stephane bourque on 2022-03-21.
//

#include "storage_timepoints.h"
#include "fmt/format.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"
#include "VenueCoordinator.h"

template <>
void ORM::DB<OpenWifi::TimePointDBRecordType, OpenWifi::AnalyticsObjects::DeviceTimePoint>::Convert(
	const OpenWifi::TimePointDBRecordType &In, OpenWifi::AnalyticsObjects::DeviceTimePoint &Out);

template <>
void ORM::DB<OpenWifi::TimePointDBRecordType, OpenWifi::AnalyticsObjects::DeviceTimePoint>::Convert(
	const OpenWifi::AnalyticsObjects::DeviceTimePoint &In, OpenWifi::TimePointDBRecordType &Out);

namespace OpenWifi {

	static ORM::FieldVec TimePoint_Fields{// object info
										  ORM::Field{"id", 64, true},
										  ORM::Field{"boardId", ORM::FieldType::FT_TEXT},
										  ORM::Field{"timestamp", ORM::FieldType::FT_BIGINT},
										  ORM::Field{"ap_data", ORM::FieldType::FT_TEXT},
										  ORM::Field{"ssid_data", ORM::FieldType::FT_TEXT},
										  ORM::Field{"radio_data", ORM::FieldType::FT_TEXT},
										  ORM::Field{"device_info", ORM::FieldType::FT_TEXT},
										  ORM::Field{"serialNumber", ORM::FieldType::FT_TEXT}};

	static ORM::IndexVec TimePointDB_Indexes{
		{std::string("timepoint_board_index"),
		 ORM::IndexEntryVec{{std::string("boardId"), ORM::Indextype::ASC},
							{std::string("timestamp"), ORM::Indextype::ASC}}},
		{std::string("timepoint_serial_time_index"),
		 ORM::IndexEntryVec{{std::string("serialNumber"), ORM::Indextype::ASC},
							{std::string("timestamp"), ORM::Indextype::ASC}}}};

	TimePointDB::TimePointDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "timepoints", TimePoint_Fields, TimePointDB_Indexes, P, L, "tpo") {}

	bool TimePointDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		std::vector<std::string> Statements{};
		RunScript(Statements);
		to = 2;
		return true;
	}

	bool TimePointDB::GetStats(const std::string &id, AnalyticsObjects::DeviceTimePointStats &S) {
		S.count = S.firstPoint = S.lastPoint = 0;
		auto F = [&](const DB::RecordName &R) -> bool {
			S.count++;
			if (S.firstPoint == 0)
				S.firstPoint = R.timestamp;
			S.lastPoint = R.timestamp;
			return true;
		};
		Iterate(F, " boardId='" + id + "'");
		return true;
	}

	bool TimePointDB::SelectRecords(const std::string &boardId, uint64_t FromDate,
									uint64_t LastDate, uint64_t MaxRecords, bool LatestPerDevice,
									std::vector<AnalyticsObjects::DeviceTimePoint> &Recs) {

		if (LatestPerDevice) {
			std::vector<AnalyticsObjects::DeviceTimePoint> tmp;
			if (!GetRecordsPerDevice(boardId, FromDate, LastDate, MaxRecords, tmp))
				return false;

			std::sort(tmp.begin(), tmp.end(),
			[](auto const &a, auto const &b) {
				return a.timestamp > b.timestamp;
			});

			Recs.swap(tmp);
			return true;
		} else {
			std::string WhereClause;

			if (FromDate && LastDate) {
				WhereClause =
					fmt::format(" boardId='{}' and (timestamp >= {} ) and ( timestamp <= {} ) ",
						boardId, FromDate, LastDate);
			} else if (FromDate) {
				WhereClause = fmt::format(" boardId='{}' and (timestamp >= {}) ", boardId, FromDate);
			} else if (LastDate) {
				WhereClause = fmt::format(" boardId='{}' and (timestamp <= {}) ", boardId, LastDate);
			}
			GetRecords(0, MaxRecords, Recs, WhereClause, " order by timestamp, serialNumber ASC ");
			return true;
		}
	}

	bool TimePointDB::DeleteBoard(const std::string &boardId) {
		return DeleteRecords(fmt::format(" boardId='{}' ", boardId));
	}

	bool TimePointDB::DeleteTimeLine(const std::string &boardId, uint64_t FromDate,
									 uint64_t LastDate) {
		std::string WhereClause;

		if (FromDate && LastDate) {
			WhereClause =
				fmt::format(" boardId='{}' and (timestamp >= {} ) and ( timestamp <= {} ) ",
							boardId, FromDate, LastDate);
		} else if (FromDate) {
			WhereClause = fmt::format(" boardId='{}' and (timestamp >= {}) ", boardId, FromDate);
		} else if (LastDate) {
			WhereClause = fmt::format(" boardId='{}' and (timestamp <= {}) ", boardId, LastDate);
		}
		DeleteRecords(WhereClause);
		return true;
	}

	bool TimePointDB::GetRecordsPerDevice(const std::string &boardId, uint64_t FromDate, uint64_t LastDate,
						uint64_t MaxRecords, std::vector<AnalyticsObjects::DeviceTimePoint> &Recs) {

		Recs.clear();

		if (MaxRecords == 0) {
			return true;
		}

		std::string whereClause = fmt::format("boardId='{}'", ORM::Escape(boardId));

		if (FromDate || LastDate) {
			whereClause += " and ";

			if (FromDate && LastDate) {
				whereClause += fmt::format("timestamp >= {} and timestamp <= {}", FromDate, LastDate);
			} else if (FromDate) {
				whereClause += fmt::format("timestamp >= {}", FromDate);
			} else {
				whereClause += fmt::format("timestamp <= {}", LastDate);
			}
		}

		const std::string rangeClause = ComputeRange(0, MaxRecords);

		const std::string sql = fmt::format(
			"select distinct on (serialNumber) {fields} "
			"from {table} "
			"where {where} "
			"order by serialNumber, timestamp DESC, id DESC{range}",
			fmt::arg("fields", SelectFields()),   // all columns for TimePointDBRecordType
			fmt::arg("table",  TableName_),       // timepoints table
			fmt::arg("where",  whereClause),
			fmt::arg("range",  rangeClause)
		);

		std::vector<TimePointDBRecordType> rawRecords;
		if (!Join(sql, rawRecords)) {
			return false;
		}
		auto currentSerials = GetCurrentDeviceFromBoard(boardId);
		if (currentSerials.empty()) {
			return true;
		}

		Recs.reserve(rawRecords.size());
		for (const auto &row : rawRecords) {
			AnalyticsObjects::DeviceTimePoint point;
			Convert(row, point);
			
			if (currentSerials.count(point.serialNumber) != 0) {
				Recs.emplace_back(std::move(point));
			}
		}

		return true;
	}

	std::set<std::string> TimePointDB::GetCurrentDeviceFromBoard(const std::string &boardId) {
		AnalyticsObjects::DeviceInfoList DIL;
		std::set<std::string> serials;

		std::string boardIdCopy(boardId);
		VenueCoordinator()->GetDevices(boardIdCopy, DIL);

		for (const auto &dev : DIL.devices) {
			serials.insert(dev.serialNumber);
		}

		return serials;
	}

} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::TimePointDBRecordType, OpenWifi::AnalyticsObjects::DeviceTimePoint>::Convert(
	const OpenWifi::TimePointDBRecordType &In, OpenWifi::AnalyticsObjects::DeviceTimePoint &Out) {
	Out.id = In.get<0>();
	Out.boardId = In.get<1>();
	Out.timestamp = In.get<2>();
	Out.ap_data =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::AnalyticsObjects::APTimePoint>(In.get<3>());
	Out.ssid_data =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::AnalyticsObjects::SSIDTimePoint>(
			In.get<4>());
	Out.radio_data =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::AnalyticsObjects::RadioTimePoint>(
			In.get<5>());
	Out.device_info =
		OpenWifi::RESTAPI_utils::to_object<OpenWifi::AnalyticsObjects::DeviceInfo>(In.get<6>());
	Out.serialNumber = In.get<7>();
}

template <>
void ORM::DB<OpenWifi::TimePointDBRecordType, OpenWifi::AnalyticsObjects::DeviceTimePoint>::Convert(
	const OpenWifi::AnalyticsObjects::DeviceTimePoint &In, OpenWifi::TimePointDBRecordType &Out) {
	Out.set<0>(In.id);
	Out.set<1>(In.boardId);
	Out.set<2>(In.timestamp);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.ap_data));
	Out.set<4>(OpenWifi::RESTAPI_utils::to_string(In.ssid_data));
	Out.set<5>(OpenWifi::RESTAPI_utils::to_string(In.radio_data));
	Out.set<6>(OpenWifi::RESTAPI_utils::to_string(In.device_info));
	Out.set<7>(In.serialNumber);
}
