#include "RESTAPI_device_bandwidth_consumption_handler.h"

#include "RESTAPI_mcp_helpers.h"
#include "RouterIdResolver.h"
#include "StorageService.h"
#include "framework/MicroServiceFuncs.h"

#include <optional>
#include <vector>

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

	void RESTAPI_device_bandwidth_consumption_handler::DoGet() {
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

		uint64_t MaxSamples = 0;
		auto ConfiguredMaxSamples =
			MicroServiceConfigGetInt("mcp.max_samples", MCP::DefaultMaxSamples);
		if (!MCP::ValidateConfiguredMaxSamples(ConfiguredMaxSamples, MaxSamples, Error))
			return MCP::SendError(*this, Error);

		if (!MCP::ValidateExpectedSampleCount(Window, Resolved.interval, MaxSamples, Error))
			return MCP::SendError(*this, Error);

		std::vector<AnalyticsObjects::DeviceTimePoint> Records;
		bool LimitExceeded = false;
		if (!StorageService()->TimePointsDB().SelectRecordsBySerial(
				Resolved.resolvedBoardId, routerId, Window.startTime, Window.endTime, Records,
				MaxSamples, &LimitExceeded)) {
			poco_error(Logger(), "Failed to query timepoints for bandwidth consumption summary");
			MCP::SetError(Error, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
						  "wifi_client_usage_query_failed",
						  "Unable to retrieve Wi-Fi client usage history");
			return MCP::SendError(*this, Error);
		}
		if (LimitExceeded) {
			MCP::SetError(Error, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "exceeds_max_samples",
						  "Requested query window exceeds maximum allowed telemetry sample count");
			return MCP::SendError(*this, Error);
		}

		auto Now = Utils::Now();
		auto RetentionStart = Now > Resolved.retention ? Now - Resolved.retention : 0;
		std::optional<AnalyticsObjects::DeviceTimePoint> StartBoundary;
		if (!StorageService()->TimePointsDB().SelectLatestRecordAtOrBeforeBySerial(
				Resolved.resolvedBoardId, routerId, RetentionStart, Window.startTime,
				StartBoundary)) {
			poco_error(Logger(), "Failed to query start boundary for bandwidth consumption summary");
			MCP::SetError(Error, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
						  "wifi_client_usage_query_failed",
						  "Unable to retrieve Wi-Fi client usage history");
			return MCP::SendError(*this, Error);
		}
		if (StartBoundary)
			Records.push_back(std::move(*StartBoundary));

		std::optional<AnalyticsObjects::DeviceTimePoint> EndBoundary;
		if (!StorageService()->TimePointsDB().SelectEarliestRecordAtOrAfterBySerial(
				Resolved.resolvedBoardId, routerId, Window.endTime, EndBoundary)) {
			poco_error(Logger(), "Failed to query end boundary for bandwidth consumption summary");
			MCP::SetError(Error, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
						  "wifi_client_usage_query_failed",
						  "Unable to retrieve Wi-Fi client usage history");
			return MCP::SendError(*this, Error);
		}
		if (EndBoundary)
			Records.push_back(std::move(*EndBoundary));

		auto Summary = MCP::CalculateBandwidthConsumptionSummary(Records, Window);
		return Object(Summary);
	}

} // namespace OpenWifi
