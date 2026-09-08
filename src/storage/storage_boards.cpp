//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "storage_boards.h"
#include "framework/OpenWifiTypes.h"
#include "framework/RESTAPI_utils.h"
#include <set>

namespace OpenWifi {

	static ORM::FieldVec Boards_Fields{// object info
									   ORM::Field{"id", 64, true},
									   ORM::Field{"name", ORM::FieldType::FT_TEXT},
									   ORM::Field{"description", ORM::FieldType::FT_TEXT},
									   ORM::Field{"notes", ORM::FieldType::FT_TEXT},
									   ORM::Field{"created", ORM::FieldType::FT_BIGINT},
									   ORM::Field{"modified", ORM::FieldType::FT_BIGINT},
									   ORM::Field{"venueId", ORM::FieldType::FT_TEXT},
									   ORM::Field{"venueName", ORM::FieldType::FT_TEXT},
									   ORM::Field{"venueDescription", ORM::FieldType::FT_TEXT},
									   ORM::Field{"retention", ORM::FieldType::FT_BIGINT},
									   ORM::Field{"interval", ORM::FieldType::FT_BIGINT},
									   ORM::Field{"monitorSubVenues", ORM::FieldType::FT_BOOLEAN}};

	static ORM::IndexVec BoardsDB_Indexes{
		{std::string("boards_name_index"),
		 ORM::IndexEntryVec{{std::string("name"), ORM::Indextype::ASC}}},
		{std::string("boards_venue_id_index"),
		 ORM::IndexEntryVec{{std::string("venueId"), ORM::Indextype::ASC}}}};

	BoardsDB::BoardsDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L)
		: DB(T, "boards", Boards_Fields, BoardsDB_Indexes, P, L, "bor") {}

	bool BoardsDB::Upgrade([[maybe_unused]] uint32_t from, uint32_t &to) {
		std::vector<std::string> Statements;
		if (Type_ == mysql) {
			Statements = {
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS venueid TEXT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS venuename TEXT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS venuedescription TEXT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS retention BIGINT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS `interval` BIGINT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS monitorsubvenues BOOLEAN",
				"CREATE INDEX IF NOT EXISTS boards_venue_id_index ON boards(venueid)"};
		} else {
			Statements = {
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS venueid TEXT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS venuename TEXT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS venuedescription TEXT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS retention BIGINT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS interval BIGINT",
				"ALTER TABLE boards ADD COLUMN IF NOT EXISTS monitorsubvenues BOOLEAN",
				"CREATE INDEX IF NOT EXISTS boards_venue_id_index ON boards(venueid)"};
		}
		to = 2;
		return RunScript(Statements);
	}

	bool BoardsDB::FindBoardsByVenue(const std::string &venueId,
									 std::vector<AnalyticsObjects::BoardInfo> &boards) {
		boards.clear();
		if (venueId.empty())
			return true;

		try {
			Poco::Data::Session Session = Pool_.get();
			Poco::Data::Statement Select(Session);
			std::vector<BoardDBRecordType> Records;
			auto VenueId = venueId;

			Select << ConvertParams("select id,name,description,notes,created,modified,"
									"venueid,venuename,venuedescription,retention,"
									"interval,monitorsubvenues from boards where venueid=? "
									"limit 100"),
				Poco::Data::Keywords::use(VenueId),
				Poco::Data::Keywords::into(Records);
			Select.execute();

			for (const auto &Record : Records) {
				AnalyticsObjects::BoardInfo Board;
				Board.info.id = Record.get<0>();
				Board.info.name = Record.get<1>();
				Board.info.description = Record.get<2>();
				Board.info.notes =
					RESTAPI_utils::to_object_array<SecurityObjects::NoteInfo>(Record.get<3>());
				Board.info.created = Record.get<4>();
				Board.info.modified = Record.get<5>();
				if (!Record.get<6>().empty()) {
					AnalyticsObjects::VenueInfo Venue;
					Venue.id = Record.get<6>();
					Venue.name = Record.get<7>();
					Venue.description = Record.get<8>();
					Venue.retention = Record.get<9>();
					Venue.interval = Record.get<10>();
					Venue.monitorSubVenues = Record.get<11>();
					Board.venueList.emplace_back(Venue);
				}
				boards.emplace_back(Board);
			}
			return true;
		} catch (const Poco::Exception &E) {
			Logger_.log(E);
		}
		return false;
	}
} // namespace OpenWifi

template <>
void ORM::DB<OpenWifi::BoardDBRecordType, OpenWifi::AnalyticsObjects::BoardInfo>::Convert(
	const OpenWifi::BoardDBRecordType &In, OpenWifi::AnalyticsObjects::BoardInfo &Out) {
	Out.info.id = In.get<0>();
	Out.info.name = In.get<1>();
	Out.info.description = In.get<2>();
	Out.info.notes =
		OpenWifi::RESTAPI_utils::to_object_array<OpenWifi::SecurityObjects::NoteInfo>(In.get<3>());
	Out.info.created = In.get<4>();
	Out.info.modified = In.get<5>();
	Out.venueList.clear();
	if (!In.get<6>().empty()) {
		OpenWifi::AnalyticsObjects::VenueInfo Venue;
		Venue.id = In.get<6>();
		Venue.name = In.get<7>();
		Venue.description = In.get<8>();
		Venue.retention = In.get<9>();
		Venue.interval = In.get<10>();
		Venue.monitorSubVenues = In.get<11>();
		Out.venueList.emplace_back(Venue);
	}
}

template <>
void ORM::DB<OpenWifi::BoardDBRecordType, OpenWifi::AnalyticsObjects::BoardInfo>::Convert(
	const OpenWifi::AnalyticsObjects::BoardInfo &In, OpenWifi::BoardDBRecordType &Out) {
	OpenWifi::AnalyticsObjects::VenueInfo Venue;
	if (!In.venueList.empty()) {
		Venue = In.venueList[0];
	}

	Out.set<0>(In.info.id);
	Out.set<1>(In.info.name);
	Out.set<2>(In.info.description);
	Out.set<3>(OpenWifi::RESTAPI_utils::to_string(In.info.notes));
	Out.set<4>(In.info.created);
	Out.set<5>(In.info.modified);
	Out.set<6>(Venue.id);
	Out.set<7>(Venue.name);
	Out.set<8>(Venue.description);
	Out.set<9>(Venue.retention);
	Out.set<10>(Venue.interval);
	Out.set<11>(Venue.monitorSubVenues);
}
