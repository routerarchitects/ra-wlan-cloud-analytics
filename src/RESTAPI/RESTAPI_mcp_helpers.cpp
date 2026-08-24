//
// RESTAPI_mcp_helpers Implementation
//

#include "RESTAPI_mcp_helpers.h"
#include "StorageService.h"
#include "fmt/format.h"
#include "framework/AuthClient.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/utils.h"
#include "Poco/DateTimeFormat.h"
#include "Poco/DateTimeFormatter.h"
#include "Poco/DateTimeParser.h"
#include "Poco/JSON/Object.h"
#include "Poco/JSON/Stringifier.h"
#include "Poco/URI.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <regex>

namespace OpenWifi::MCP {

	void SendMCPError(RESTAPIHandler *handler, Poco::Net::HTTPResponse::HTTPStatus status,
					  const std::string &errorCode, const std::string &message) {
		handler->PrepareResponse(status);
		Poco::JSON::Object errorObj;
		errorObj.set("error", errorCode);
		errorObj.set("message", message);
		std::ostream &answer = handler->Response->send();
		Poco::JSON::Stringifier::stringify(errorObj, answer);
	}

	std::string FormatRFC3339UTC(uint64_t epochSeconds) {
		std::time_t t = static_cast<std::time_t>(epochSeconds);
		std::tm tm_buf;
		gmtime_r(&t, &tm_buf);
		char buf[64];
		std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
		return std::string(buf);
	}

	bool ParseRFC3339UTC(const std::string &str, uint64_t &epochSeconds) {
		if (str.empty() || str.back() != 'Z')
			return false;

		// Check for forbidden numeric offsets like +05:30 or -08:00
		if (str.find('+') != std::string::npos || (str.find('-') != std::string::npos && str.rfind('-') > 7)) {
			// Hyphens in YYYY-MM-DD are at positions 4 and 7. Any subsequent hyphen or plus is a numeric timezone offset.
			size_t lastHyphen = str.rfind('-');
			if (lastHyphen > 7) return false;
		}

		static const std::regex pattern(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$)");
		if (!std::regex_match(str, pattern))
			return false;

		std::tm tm_buf{};
		int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
		if (sscanf(str.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &month, &day, &hour, &min, &sec) != 6) {
			return false;
		}

		if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 ||
			min > 59 || sec < 0 || sec > 59) {
			return false;
		}

		tm_buf.tm_year = year - 1900;
		tm_buf.tm_mon = month - 1;
		tm_buf.tm_mday = day;
		tm_buf.tm_hour = hour;
		tm_buf.tm_min = min;
		tm_buf.tm_sec = sec;
		tm_buf.tm_isdst = 0;

		std::time_t t = timegm(&tm_buf);
		if (t == -1)
			return false;

		// Calendar round-trip validation to reject invalid dates normalized by timegm (e.g. 2026-02-31 or 2025-02-29)
		if (tm_buf.tm_year != (year - 1900) || tm_buf.tm_mon != (month - 1) ||
			tm_buf.tm_mday != day || tm_buf.tm_hour != hour || tm_buf.tm_min != min ||
			tm_buf.tm_sec != sec) {
			return false;
		}

		epochSeconds = static_cast<uint64_t>(t);
		return true;
	}

	uint64_t GetTemperatureMigrationCutoverTime() {
		static uint64_t cachedTime = 0;
		if (cachedTime != 0)
			return cachedTime;

		std::string strTime =
			MicroServiceConfigGetString("temperature.migration_cutover_time", "2026-07-01T00:00:00Z");
		uint64_t epoch = 0;
		if (ParseRFC3339UTC(strTime, epoch)) {
			cachedTime = epoch;
			return cachedTime;
		}
		// Default to epoch 0 if unconfigured
		return 0;
	}

	uint64_t GetAvailabilityValidFromTime() {
		std::string dbVal;
		if (StorageService()->SystemPropertiesDB().GetProperty("availability_valid_from", dbVal)) {
			uint64_t epoch = 0;
			if (ParseRFC3339UTC(dbVal, epoch)) {
				return epoch;
			}
			try {
				return std::stoull(dbVal);
			} catch (...) {
			}
		}

		std::string configVal =
			MicroServiceConfigGetString("availability.valid_from", "2026-07-01T00:00:00Z");
		uint64_t epoch = 0;
		if (ParseRFC3339UTC(configVal, epoch)) {
			StorageService()->SystemPropertiesDB().SetProperty("availability_valid_from", configVal);
			return epoch;
		}
		return 0;
	}

	bool ValidateMCPRequest(RESTAPIHandler *handler, MCPDomainCutoverType cutoverType,
							MCPValidatedRequest &outReq) {
		// Phase 0 — Authentication
		// Must use Authorization: Bearer <token>. Do not accept X-API-KEY.
		if (!handler->Request->has("Authorization")) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED, "unauthorized",
						 "Missing, invalid, or expired bearer token");
			return false;
		}

		std::string authHeader = handler->Request->get("Authorization", "");
		if (authHeader.find("Bearer ") != 0) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED, "unauthorized",
						 "Missing, invalid, or expired bearer token");
			return false;
		}

		std::string token = authHeader.substr(7);
		if (token.empty()) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED, "unauthorized",
						 "Missing, invalid, or expired bearer token");
			return false;
		}

		bool expired = false, contacted = false;
		if (!AuthClient()->IsAuthorized(token, handler->UserInfo_, handler->TransactionId(), expired,
										contacted)) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED, "unauthorized",
						 "Missing, invalid, or expired bearer token");
			return false;
		}

		// Phase 1 — Pure Input Validation
		std::string routerId = handler->GetBinding("routerId", "");
		if (routerId.empty()) {
			routerId = handler->GetBinding("routerid", "");
		}

		static const std::regex routerIdPattern("^[a-zA-Z0-9_-]+$");
		if (routerId.empty() || routerId.size() > 64 || !std::regex_match(routerId, routerIdPattern)) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_router_id",
						 "routerId must be a valid path-safe OWPROV gateway serial number (1 to 64 alphanumeric characters, hyphens, or underscores)");
			return false;
		}
		outReq.routerId = routerId;

		// Inspect raw query collection
		Poco::URI uri(handler->Request->getURI());
		auto queryParams = uri.getQueryParameters();

		int timestampTillCount = 0;
		int lookbackHoursCount = 0;
		std::string timestampTillVal;
		std::string lookbackHoursVal;

		for (const auto &[key, val] : queryParams) {
			if (key == "timestampTill") {
				timestampTillCount++;
				timestampTillVal = val;
			} else if (key == "lookbackHours") {
				lookbackHoursCount++;
				lookbackHoursVal = val;
			} else {
				SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
							 "invalid_query_parameter",
							 fmt::format("Unsupported query parameter: {}", key));
				return false;
			}
		}

		if (timestampTillCount != 1) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_timestamp",
						 "timestampTill must be specified exactly once");
			return false;
		}

		if (lookbackHoursCount != 1) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
						 "invalid_lookback_hours",
						 "lookbackHours must be specified exactly once");
			return false;
		}

		// Validate timestampTill format & skew allowance
		uint64_t endTimeEpoch = 0;
		if (!ParseRFC3339UTC(timestampTillVal, endTimeEpoch)) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_timestamp",
						 "timestampTill must be a valid UTC timestamp ending with 'Z'");
			return false;
		}

		uint64_t currentServerTime = Utils::Now();
		outReq.currentServerTime = currentServerTime;
		outReq.allowedClockSkewSeconds =
			MicroServiceConfigGetInt("allowed.clock_skew.seconds", 300);

		if (endTimeEpoch > currentServerTime + outReq.allowedClockSkewSeconds) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_timestamp",
						 "timestampTill is beyond the server clock skew tolerance");
			return false;
		}

		outReq.timestampTillStr = timestampTillVal;
		outReq.endTimeEpoch = endTimeEpoch;

		// Validate lookbackHours as strict positive whole decimal integer
		if (lookbackHoursVal.empty() || !std::all_of(lookbackHoursVal.begin(), lookbackHoursVal.end(), ::isdigit)) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
						 "invalid_lookback_hours",
						 "lookbackHours must be a strict positive whole decimal integer");
			return false;
		}

		uint64_t lookbackHours = 0;
		try {
			lookbackHours = std::stoull(lookbackHoursVal);
		} catch (...) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
						 "invalid_lookback_hours",
						 "lookbackHours integer overflow");
			return false;
		}

		if (lookbackHours == 0) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
						 "invalid_lookback_hours", "lookbackHours must be greater than 0");
			return false;
		}
		outReq.lookbackHours = lookbackHours;

		// Checked epoch calculation
		if (lookbackHours > UINT64_MAX / 3600) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
						 "invalid_lookback_hours",
						 "lookbackHours value causes integer overflow");
			return false;
		}
		uint64_t secondsToLookback = lookbackHours * 3600;
		if (endTimeEpoch < secondsToLookback) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_timestamp",
						 "Calculated startTime precedes supported Unix epoch minimum");
			return false;
		}

		outReq.startTimeEpoch = endTimeEpoch - secondsToLookback;

		// Phase 2 — Router Ownership & Serial Resolution
		auto resolution = RouterIdResolverService()->ResolveRouterIdContext(handler, routerId);
		if (resolution.status != RouterIdResolutionStatus::Success) {
			switch (resolution.status) {
			case RouterIdResolutionStatus::InvalidRouterId:
				SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
							 "invalid_router_id", resolution.message);
				return false;
			case RouterIdResolutionStatus::InventoryNotFound:
			case RouterIdResolutionStatus::EmptyVenue:
			case RouterIdResolutionStatus::BoardNotConfigured:
			case RouterIdResolutionStatus::AccessDenied:
				SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "not_found",
							 "Router was not found");
				return false;
			case RouterIdResolutionStatus::MultipleBoards:
				SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_CONFLICT, "multiple_boards",
							 resolution.message);
				return false;
			case RouterIdResolutionStatus::OwprovUnavailable:
			case RouterIdResolutionStatus::OwprovInvalidResponse:
				SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY,
							 "owprov_unavailable", resolution.message);
				return false;
			default:
				SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "not_found",
							 "Router was not found");
				return false;
			}
		}

		outReq.resolvedBoardId = resolution.resolvedBoardId;
		outReq.resolvedVenueId = resolution.venueId;

		// Phase 3 — Monitoring Duration, Retention & Cutover Validation
		AnalyticsObjects::BoardInfo boardInfo;
		if (StorageService()->BoardsDB().GetRecord("id", outReq.resolvedBoardId, boardInfo)) {
			const AnalyticsObjects::VenueInfo *matchedVenue = nullptr;
			for (const auto &venue : boardInfo.venueList) {
				if (venue.id == outReq.resolvedVenueId) {
					matchedVenue = &venue;
					break;
				}
			}

			if (matchedVenue != nullptr && matchedVenue->retention > 0) {
				outReq.monitoringDuration = matchedVenue->retention;
			} else {
				outReq.monitoringDuration = MicroServiceConfigGetInt("monitoring.duration", 365 * 24 * 3600);
			}

			// Effective monitoring start is the board creation timestamp (or 0 if unpopulated).
			outReq.monitoringEnabledAt = boardInfo.info.created;
			outReq.monitoringConfigurationExpiry = UINT64_MAX;
		} else {
			outReq.monitoringDuration = MicroServiceConfigGetInt("monitoring.duration", 365 * 24 * 3600);
			outReq.monitoringEnabledAt = 0;
			outReq.monitoringConfigurationExpiry = UINT64_MAX;
		}

		uint64_t maxLookbackHours = outReq.monitoringDuration / 3600;
		if (lookbackHours > maxLookbackHours) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
						 "invalid_lookback_hours",
						 "lookbackHours exceeds configured maximum lookback");
			return false;
		}

		uint64_t retentionDataEnd = std::min(currentServerTime, outReq.monitoringConfigurationExpiry);
		uint64_t retentionStart = retentionDataEnd > outReq.monitoringDuration
									  ? retentionDataEnd - outReq.monitoringDuration
									  : 0;

		if (outReq.monitoringEnabledAt > 0 && outReq.monitoringEnabledAt > retentionStart) {
			retentionStart = outReq.monitoringEnabledAt;
		}

		uint64_t requestEndLimit = std::min(currentServerTime + outReq.allowedClockSkewSeconds,
											outReq.monitoringConfigurationExpiry);

		if (outReq.startTimeEpoch < retentionStart || outReq.endTimeEpoch > requestEndLimit) {
			SendMCPError(handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
						 "lookback_outside_retention",
						 "Requested range is outside the configured monitoring retention window");
			return false;
		}

		// Domain Cutover Validation
		if (cutoverType == MCPDomainCutoverType::RadioTemperature) {
			uint64_t tempCutover = GetTemperatureMigrationCutoverTime();
			if (outReq.startTimeEpoch < tempCutover) {
				SendMCPError(
					handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
					"temperature_range_before_cutover",
					"The requested summary interval starts before the temperature migration cutover timestamp.");
				return false;
			}
		} else if (cutoverType == MCPDomainCutoverType::Availability) {
			uint64_t availCutover = GetAvailabilityValidFromTime();
			if (outReq.startTimeEpoch < availCutover) {
				SendMCPError(
					handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
					"availability_range_before_cutover",
					"Availability history is only available for ranges starting at or after availabilityValidFrom");
				return false;
			}
		}

		return true;
	}

} // namespace OpenWifi::MCP
