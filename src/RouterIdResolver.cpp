#include "RouterIdResolver.h"

#include "StorageService.h"
#include "RESTAPI/RESTAPI_mcp_helpers.h"
#include "VenueCoordinator.h"
#include "sdks/SDK_prov.h"
#include <algorithm>

namespace OpenWifi {

	namespace {
		void NotFound(RouterIdResolver::Error &E) {
			E.status = Poco::Net::HTTPResponse::HTTP_NOT_FOUND;
			E.error = "not_found";
			E.message = "Router was not found";
		}

		bool ResolveFromCurrentVenueMap(const std::string &routerId,
										const std::string &authorizedVenueId,
										RouterIdResolver::Result &Resolved,
										RouterIdResolver::Error &E,
										bool &StorageFailure) {
			std::vector<AnalyticsObjects::BoardInfo> MatchingBoards;
			auto VisitBoard = [&](const AnalyticsObjects::BoardInfo &Board) -> bool {
				auto BoardId = Board.info.id;
				AnalyticsObjects::DeviceInfoList Devices;
				VenueCoordinator()->GetDevices(BoardId, Devices);
				for (const auto &Device : Devices.devices) {
					if (Device.serialNumber == routerId) {
						MatchingBoards.emplace_back(Board);
						break;
					}
				}
				return true;
			};

			StorageFailure = !StorageService()->BoardsDB().Iterate(VisitBoard);
			if (StorageFailure) {
				RouterIdResolver::AnalyticsBoardStorageFailure(E);
				return false;
			}
			if (MatchingBoards.empty())
				return false;

			if (MatchingBoards.size() > 1) {
				E.status = Poco::Net::HTTPResponse::HTTP_CONFLICT;
				E.error = "multiple_boards";
				E.message = "Router is mapped to multiple current boards";
				return false;
			}

			const auto &Board = MatchingBoards.front();
			Resolved.routerId = routerId;
			Resolved.board = Board;
			Resolved.resolvedBoardId = Board.info.id;
			Resolved.resolvedVenueId = Board.venueList.empty() ? authorizedVenueId : Board.venueList[0].id;
			return true;
		}

		bool ResolveFromMonitoredSubVenues(RESTAPIHandler &Client, const std::string &routerId,
										   RouterIdResolver::Result &Resolved,
										   RouterIdResolver::Error &E) {
			std::vector<AnalyticsObjects::BoardInfo> MatchingBoards;
			auto VisitBoard = [&](const AnalyticsObjects::BoardInfo &Board) -> bool {
				if (Board.venueList.empty() || !Board.venueList[0].monitorSubVenues)
					return true;

				ProvObjects::VenueDeviceList Devices;
				bool VenueExists = true;
				if (!SDK::Prov::Venue::GetDevices(&Client, Board.venueList[0].id, true, Devices,
												   VenueExists)) {
					return true;
				}
				if (!VenueExists)
					return true;
				if (std::find(Devices.devices.begin(), Devices.devices.end(), routerId) !=
					Devices.devices.end()) {
					MatchingBoards.emplace_back(Board);
				}
				return true;
			};

			if (!StorageService()->BoardsDB().Iterate(VisitBoard))
				return RouterIdResolver::AnalyticsBoardStorageFailure(E);
			if (MatchingBoards.empty())
				return false;
			if (MatchingBoards.size() > 1) {
				E.status = Poco::Net::HTTPResponse::HTTP_CONFLICT;
				E.error = "multiple_boards";
				E.message = "Router is mapped to multiple current boards";
				return false;
			}

			const auto &Board = MatchingBoards.front();
			Resolved.routerId = routerId;
			Resolved.board = Board;
			Resolved.resolvedBoardId = Board.info.id;
			Resolved.resolvedVenueId = Board.venueList[0].id;
			return true;
		}
	} // namespace

	bool RouterIdResolver::Resolve(RESTAPIHandler &Client, const std::string &routerId,
								   Result &Resolved, Error &E) {
		MCP::Error ValidationError;
		if (!MCP::ValidateRouterId(routerId, ValidationError)) {
			E.status = ValidationError.status;
			E.error = ValidationError.error;
			E.message = ValidationError.message;
			return false;
		}

		ProvObjects::InventoryTag Device;
		Poco::Net::HTTPResponse::HTTPStatus ProvisioningStatus =
			Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR;
		auto FetchResult =
			SDK::Prov::Device::GetWithStatus(&Client, routerId, Device, ProvisioningStatus);
		if (!ClassifyDeviceFetchResult(FetchResult, ProvisioningStatus, E)) {
			if (FetchResult == SDK::Prov::Device::FetchResult::InvalidResponse) {
				poco_warning(Client.Logger(),
							 "Failed to parse OWPROV inventory response for routerId=" + routerId);
			}
			return false;
		}

		if (Device.venue.empty()) {
			NotFound(E);
			return false;
		}

		bool MapStorageFailure = false;
		if (ResolveFromCurrentVenueMap(routerId, Device.venue, Resolved, E, MapStorageFailure))
			return true;
		if (MapStorageFailure || E.status == Poco::Net::HTTPResponse::HTTP_CONFLICT)
			return false;

		std::vector<AnalyticsObjects::BoardInfo> Matches;
		if (!StorageService()->BoardsDB().FindBoardsByVenue(Device.venue, Matches)) {
			poco_error(Client.Logger(),
					   "Failed to read Analytics boards while resolving routerId=" +
						   routerId);
			AnalyticsBoardStorageFailure(E);
			return false;
		}

		if (Matches.empty()) {
			return ResolveFromMonitoredSubVenues(Client, routerId, Resolved, E);
		}

		if (Matches.size() > 1) {
			E.status = Poco::Net::HTTPResponse::HTTP_CONFLICT;
			E.error = "multiple_boards";
			E.message = "Router is mapped to multiple current boards";
			return false;
		}

		Resolved.routerId = routerId;
		Resolved.board = Matches.front();
		Resolved.resolvedBoardId = Matches.front().info.id;
		Resolved.resolvedVenueId = Device.venue;
		return true;
	}

} // namespace OpenWifi
