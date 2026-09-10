#pragma once

#include "RESTObjects/RESTAPI_AnalyticsObjects.h"
#include "sdks/SDK_prov.h"
#include <Poco/Net/HTTPResponse.h>
#include <string>
#include <vector>

#include "storage/storage_boards.h"

namespace OpenWifi {

	class RESTAPIHandler;

	class RouterIdResolver {
	  public:
		struct Result {
			std::string routerId;
			std::string resolvedBoardId;
			std::string resolvedVenueId;
			uint64_t retention = 0;
			uint64_t interval = 0;
			bool monitorSubVenues = false;
		};

		struct Error {
			Poco::Net::HTTPResponse::HTTPStatus status =
				Poco::Net::HTTPResponse::HTTP_NOT_FOUND;
			std::string error = "not_found";
			std::string message = "Router was not found";
		};

		bool Resolve(RESTAPIHandler &Client, const std::string &routerId, Result &Resolved,
					 Error &E);
		static bool ClassifyDeviceFetchResult(SDK::Prov::Device::FetchResult Result,
											  Poco::Net::HTTPResponse::HTTPStatus Status,
											  Error &E);
		static bool ClassifyProvisioningFailure(Poco::Net::HTTPResponse::HTTPStatus Status,
												Error &E);
		static bool AnalyticsBoardStorageFailure(Error &E);
		static bool InvalidProvisioningResponse(Error &E);
		static bool ResolveBoardForVenue(const std::string &venueId,
										 const std::vector<BoardsDB::BoardVenueRecord> &Boards,
										 Result &Resolved, Error &E);
	};

	inline bool RouterIdResolver::ClassifyProvisioningFailure(
		Poco::Net::HTTPResponse::HTTPStatus Status, Error &E) {
		if (Status == Poco::Net::HTTPResponse::HTTP_NOT_FOUND ||
			Status == Poco::Net::HTTPResponse::HTTP_FORBIDDEN ||
			Status == Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED) {
			E.status = Poco::Net::HTTPResponse::HTTP_NOT_FOUND;
			E.error = "not_found";
			E.message = "Router was not found";
			return false;
		}
		E.status = Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY;
		E.error = "owprov_unavailable";
		E.message = "Upstream provisioning service is unavailable";
		return false;
	}

	inline bool RouterIdResolver::AnalyticsBoardStorageFailure(Error &E) {
		E.status = Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR;
		E.error = "internal_error";
		E.message = "Internal error";
		return false;
	}

	inline bool RouterIdResolver::InvalidProvisioningResponse(Error &E) {
		E.status = Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY;
		E.error = "owprov_invalid_response";
		E.message = "Upstream provisioning service returned an invalid response";
		return false;
	}

	inline bool RouterIdResolver::ClassifyDeviceFetchResult(
		SDK::Prov::Device::FetchResult Result,
		Poco::Net::HTTPResponse::HTTPStatus Status, Error &E) {
		if (Result == SDK::Prov::Device::FetchResult::Success)
			return true;
		if (Result == SDK::Prov::Device::FetchResult::InvalidResponse)
			return InvalidProvisioningResponse(E);
		return ClassifyProvisioningFailure(Status, E);
	}

	inline bool RouterIdResolver::ResolveBoardForVenue(
		const std::string &venueId, const std::vector<BoardsDB::BoardVenueRecord> &Boards,
		Result &Resolved, Error &E) {
		std::vector<BoardsDB::BoardVenueRecord> MatchingBoards;
		for (const auto &Board : Boards) {
			if (!Board.venueId.empty() && Board.venueId == venueId) {
				MatchingBoards.emplace_back(Board);
			}
		}

		if (MatchingBoards.empty()) {
			E.status = Poco::Net::HTTPResponse::HTTP_NOT_FOUND;
			E.error = "not_found";
			E.message = "Router was not found";
			return false;
		}
		if (MatchingBoards.size() > 1) {
			E.status = Poco::Net::HTTPResponse::HTTP_CONFLICT;
			E.error = "multiple_boards";
			E.message = "Router is mapped to multiple current boards";
			return false;
		}

		Resolved.resolvedVenueId = venueId;
		Resolved.resolvedBoardId = MatchingBoards.front().boardId;
		Resolved.retention = MatchingBoards.front().retention;
		Resolved.interval = MatchingBoards.front().interval;
		Resolved.monitorSubVenues = MatchingBoards.front().monitorSubVenues;
		return true;
	}

} // namespace OpenWifi
