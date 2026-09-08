#include "RESTAPI_device_memory_summary_handler.h"

#include "RESTAPI_mcp_helpers.h"
#include "RouterIdResolver.h"
#include "StorageService.h"
#include "framework/MicroServiceFuncs.h"

namespace OpenWifi {

	namespace {
		bool FindVenueRetention(const AnalyticsObjects::BoardInfo &Board,
								const std::string &VenueId, uint64_t &RetentionSeconds) {
			for (const auto &Venue : Board.venueList) {
				if (Venue.id == VenueId) {
					RetentionSeconds = Venue.retention;
					return RetentionSeconds > 0;
				}
			}
			return false;
		}

		MCP::Error ConvertResolverError(const RouterIdResolver::Error &ResolverError) {
			MCP::Error E;
			E.status = ResolverError.status;
			E.error = ResolverError.error;
			E.message = ResolverError.message;
			return E;
		}
	} // namespace

	void RESTAPI_device_memory_summary_handler::DoGet() {
		MCP::Error Error;
		if (!MCP::AuthenticateBearerToken(*this, Error))
			return MCP::SendError(*this, Error);

		auto routerId = GetBinding("routerId", "");
		if (!MCP::ValidateRouterId(routerId, Error))
			return MCP::SendError(*this, Error);

		MCP::Window Window;
		auto ClockSkewSeconds = MicroServiceConfigGetInt("allowed.clock_skew.seconds", 300);
		if (!MCP::ValidateWindowQuery(Parameters_, Utils::Now(), ClockSkewSeconds, Window, Error))
			return MCP::SendError(*this, Error);

		RouterIdResolver Resolver;
		RouterIdResolver::Result Resolved;
		RouterIdResolver::Error ResolverError;
		if (!Resolver.Resolve(*this, routerId, Resolved, ResolverError)) {
			if (ResolverError.status == Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY) {
				poco_warning(Logger(), "Failed to resolve routerId through OWPROV");
			}
			return MCP::SendError(*this, ConvertResolverError(ResolverError));
		}

		uint64_t RetentionSeconds = 0;
		if (!FindVenueRetention(Resolved.board, Resolved.resolvedVenueId, RetentionSeconds)) {
			MCP::SetError(Error, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "not_found",
						  "Router was not found");
			return MCP::SendError(*this, Error);
		}
		if (!MCP::ValidateRetention(Window, RetentionSeconds, Utils::Now(), ClockSkewSeconds, Error))
			return MCP::SendError(*this, Error);

		std::vector<AnalyticsObjects::DeviceTimePoint> Records;
		if (!StorageService()->TimePointsDB().SelectResourceRecordsBySerial(
				Resolved.resolvedBoardId, routerId, Window.startTime, Window.endTime, Records)) {
			poco_error(Logger(), "Failed to query timepoints for memory summary");
			MCP::SetError(Error, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
						  "memory_summary_query_failed",
						  "Unable to retrieve gateway memory history");
			return MCP::SendError(*this, Error);
		}

		auto Summary = MCP::CalculateMemorySummary(Records, Window);
		return Object(Summary);
	}

} // namespace OpenWifi
