#include "RouterIdResolver.h"

#include "StorageService.h"
#include "RESTAPI/RESTAPI_mcp_helpers.h"
#include "sdks/SDK_prov.h"

namespace OpenWifi {

	namespace {
		void NotFound(RouterIdResolver::Error &E) {
			E.status = Poco::Net::HTTPResponse::HTTP_NOT_FOUND;
			E.error = "not_found";
			E.message = "Router was not found";
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

		std::vector<BoardsDB::BoardVenueRecord> Matches;
		if (!StorageService()->BoardsDB().FindBoardVenueRecordsByVenue(Device.venue, Matches)) {
			poco_error(Client.Logger(),
					   "Failed to read Analytics boards while resolving routerId=" +
						   routerId);
			AnalyticsBoardStorageFailure(E);
			return false;
		}

		Resolved.routerId = routerId;
		return ResolveBoardForVenue(Device.venue, Matches, Resolved, E);
	}

} // namespace OpenWifi
