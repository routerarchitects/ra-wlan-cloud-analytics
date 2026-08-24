//
// RESTAPI_mcp_helpers Header
//

#pragma once

#include "RESTObjects/RESTAPI_AnalyticsObjects.h"
#include "framework/RESTAPI_Handler.h"
#include "RouterIdResolver.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace OpenWifi::MCP {

	struct MCPValidatedRequest {
		std::string routerId;
		std::string timestampTillStr;
		uint64_t lookbackHours = 0;
		uint64_t startTimeEpoch = 0;
		uint64_t endTimeEpoch = 0;
		uint64_t currentServerTime = 0;

		std::string resolvedBoardId;
		std::string resolvedVenueId;

		uint64_t allowedClockSkewSeconds = 300;
		uint64_t monitoringDuration = 365 * 24 * 3600;
		uint64_t monitoringEnabledAt = 0;
		uint64_t monitoringConfigurationExpiry = 0xFFFFFFFFFFFFFFFFULL;
	};

	enum class MCPDomainCutoverType {
		None,
		RadioTemperature,
		Availability
	};

	// Helper to send standardized JSON error response: {"error": "...", "message": "..."}
	void SendMCPError(RESTAPIHandler *handler, Poco::Net::HTTPResponse::HTTPStatus status,
					  const std::string &errorCode, const std::string &message);

	// Unified validation function for Phase 0, Phase 1, Phase 2, Phase 3
	bool ValidateMCPRequest(RESTAPIHandler *handler, MCPDomainCutoverType cutoverType,
							MCPValidatedRequest &outReq);

	// Helpers for RFC3339 UTC time formatting & parsing
	std::string FormatRFC3339UTC(uint64_t epochSeconds);
	bool ParseRFC3339UTC(const std::string &str, uint64_t &epochSeconds);

	// Configuration helpers for cutover timestamps
	uint64_t GetTemperatureMigrationCutoverTime();
	uint64_t GetAvailabilityValidFromTime();

} // namespace OpenWifi::MCP
