//
// Created by stephane bourque on 2022-03-11.
//

#pragma once

#include "RESTObjects/RESTAPI_AnalyticsObjects.h"
#include "framework/orm.h"

namespace OpenWifi {
	typedef Poco::Tuple<std::string, std::string, std::string, std::string, uint64_t, uint64_t,
						std::string, std::string, std::string, uint64_t, uint64_t, bool>
		BoardDBRecordType;

	class BoardsDB : public ORM::DB<BoardDBRecordType, AnalyticsObjects::BoardInfo> {
	  public:
		BoardsDB(OpenWifi::DBType T, Poco::Data::SessionPool &P, Poco::Logger &L);
		virtual ~BoardsDB(){};

		bool FindBoardsByVenue(const std::string &venueId,
							   std::vector<AnalyticsObjects::BoardInfo> &boards);

	  private:
		bool Upgrade(uint32_t from, uint32_t &to) override;
	};
} // namespace OpenWifi
