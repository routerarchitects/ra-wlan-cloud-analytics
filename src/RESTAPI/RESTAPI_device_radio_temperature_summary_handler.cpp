#include "RESTAPI_device_radio_temperature_summary_handler.h"

#include "RESTAPI_mcp_helpers.h"
#include "RouterIdResolver.h"
#include "StorageService.h"
#include "framework/MicroServiceFuncs.h"

namespace OpenWifi {

	namespace {
		MCP::Error ConvertResolverError(const RouterIdResolver::Error &ResolverError) {
			MCP::Error E;
			E.status = ResolverError.status;
			E.error = ResolverError.error;
			E.message = ResolverError.message;
			return E;
		}
	} // namespace

	void RESTAPI_device_radio_temperature_summary_handler::DoGet() {
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

		if (Resolved.retention == 0) {
			MCP::SetError(Error, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "not_found",
						  "Router was not found");
			return MCP::SendError(*this, Error);
		}
		if (!MCP::ValidateRetention(Window, Resolved.retention, Utils::Now(), ClockSkewSeconds, Error))
			return MCP::SendError(*this, Error);

		uint64_t CutoverTime = 0;
		if (!MCP::GetTemperatureMigrationCutoverTime(CutoverTime, Error)) {
			poco_error(Logger(), Error.message);
			MCP::SetError(Error, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
						  "radio_temperature_summary_query_failed",
						  "Unable to retrieve gateway radio temperature history");
			return MCP::SendError(*this, Error);
		}
		if (!MCP::ValidateTemperatureCutover(Window, CutoverTime, Error))
			return MCP::SendError(*this, Error);

		std::vector<AnalyticsObjects::DeviceTimePoint> Records;
		if (!StorageService()->TimePointsDB().SelectRadioRecordsBySerial(
				Resolved.resolvedBoardId, routerId, Window.startTime, Window.endTime, Records)) {
			poco_error(Logger(), "Failed to query timepoints for radio temperature summary");
			MCP::SetError(Error, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
						  "radio_temperature_summary_query_failed",
						  "Unable to retrieve gateway radio temperature history");
			return MCP::SendError(*this, Error);
		}

		auto Summary = MCP::CalculateRadioTemperatureSummary(Records, Window, CutoverTime);
		return Object(Summary);
	}

} // namespace OpenWifi
