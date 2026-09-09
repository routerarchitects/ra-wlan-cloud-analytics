//
// Created for MCP gateway Wi-Fi temperature summary.
//

#include "RESTAPI_gateway_wifi_temp_handler.h"

#include "framework/MicroServiceFuncs.h"
#include "sdks/SDK_prov.h"

#include <Poco/DateTime.h>
#include <Poco/DateTimeFormat.h>
#include <Poco/DateTimeParser.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Stringifier.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <optional>
#include <vector>

namespace OpenWifi {
	namespace {
		struct Window {
			uint64_t startTime = 0;
			uint64_t endTime = 0;
		};

		struct BandStats {
			bool hasSamples = false;
			int64_t min = 0;
			int64_t max = 0;
			int64_t sum = 0;
			uint64_t count = 0;

			void Add(int64_t Value) {
				if (!hasSamples) {
					min = max = Value;
					hasSamples = true;
				} else {
					min = std::min(min, Value);
					max = std::max(max, Value);
				}
				sum += Value;
				count++;
			}

			double Avg() const { return count == 0 ? 0.0 : static_cast<double>(sum) / count; }
		};

		uint64_t CountQueryParameter(const Poco::URI::QueryParameters &Parameters,
									 const std::string &Name) {
			return static_cast<uint64_t>(std::count_if(
				Parameters.begin(), Parameters.end(),
				[&](const auto &Entry) { return Entry.first == Name; }));
		}

		bool IsRouterIdValid(const std::string &RouterId) {
			if (RouterId.empty() || RouterId.size() > 64)
				return false;
			return std::all_of(RouterId.begin(), RouterId.end(), [](unsigned char C) {
				return std::isalnum(C) || C == '-' || C == '_';
			});
		}

		bool ParseUtcTimestamp(const std::string &Value, uint64_t &Epoch) {
			if (Value.size() != 20 || Value[4] != '-' || Value[7] != '-' || Value[10] != 'T' ||
				Value[13] != ':' || Value[16] != ':' || Value[19] != 'Z')
				return false;

			try {
				Poco::DateTime DateTime;
				int TimeZone = 0;
				Poco::DateTimeParser::parse(Poco::DateTimeFormat::ISO8601_FORMAT, Value,
											DateTime, TimeZone);
				if (TimeZone != 0)
					return false;
				Epoch = DateTime.timestamp().epochTime();
				return true;
			} catch (...) {
			}
			return false;
		}

		bool ParseIsoTimestamp(const std::string &Value, uint64_t &Epoch) {
			try {
				Poco::DateTime DateTime;
				int TimeZone = 0;
				Poco::DateTimeParser::parse(Poco::DateTimeFormat::ISO8601_FORMAT, Value,
											DateTime, TimeZone);
				Epoch = DateTime.timestamp().epochTime();
				return true;
			} catch (...) {
			}
			return false;
		}

		std::string ToUtcString(uint64_t Epoch) {
			std::time_t RawTime = static_cast<std::time_t>(Epoch);
			std::tm Tm{};
#if defined(_WIN32)
			gmtime_s(&Tm, &RawTime);
#else
			gmtime_r(&RawTime, &Tm);
#endif
			char Buffer[sizeof("YYYY-MM-DDTHH:MM:SSZ")] = {};
			std::strftime(Buffer, sizeof(Buffer), "%Y-%m-%dT%H:%M:%SZ", &Tm);
			return Buffer;
		}

		bool ParseLookbackHours(const std::string &Value, uint64_t &LookbackHours) {
			if (Value.empty() || !std::all_of(Value.begin(), Value.end(), [](unsigned char C) {
					return std::isdigit(C);
				}))
				return false;
			try {
				size_t Position = 0;
				LookbackHours = std::stoull(Value, &Position, 10);
				return Position == Value.size() && LookbackHours > 0;
			} catch (...) {
			}
			return false;
		}

		std::optional<uint64_t> GetTemperatureCutoverTime() {
			std::string Cutover;
			if (const auto *EnvCutover = std::getenv("TEMPERATURE_MIGRATION_CUTOVER_TIME")) {
				if (*EnvCutover != '\0')
					Cutover = EnvCutover;
			}
			if (Cutover.empty())
				Cutover = MicroServiceConfigGetString("temperature.migration_cutover_time", "");
			if (Cutover.empty())
				return std::nullopt;

			uint64_t Parsed = 0;
			if (!ParseIsoTimestamp(Cutover, Parsed))
				return std::nullopt;
			return Parsed;
		}

		void SendMcpError(RESTAPIHandler &Handler, Poco::Net::HTTPResponse::HTTPStatus Status,
						  const std::string &Error, const std::string &Message) {
			Handler.PrepareResponse(Status);
			Poco::JSON::Object ErrorObject;
			ErrorObject.set("error", Error);
			ErrorObject.set("message", Message);
			std::ostream &Answer = Handler.Response->send();
			Poco::JSON::Stringifier::stringify(ErrorObject, Answer);
		}

		bool ValidateQuery(RESTAPI_gateway_wifi_temp_handler &Handler, Window &Output) {
			if (Handler.QueryParameters().size() != 2)
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
									"invalid_query_parameter",
									"Only timestampTill and lookbackHours are supported"),
					   false;
			if (CountQueryParameter(Handler.QueryParameters(), "timestampTill") != 1)
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
									"invalid_timestamp", "timestampTill is required exactly once"),
					   false;
			if (CountQueryParameter(Handler.QueryParameters(), "lookbackHours") != 1)
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
									"invalid_lookback_hours",
									"lookbackHours is required exactly once"),
					   false;

			uint64_t EndTime = 0;
			if (!ParseUtcTimestamp(Handler.GetParameter("timestampTill", ""), EndTime))
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
									"invalid_timestamp",
									"timestampTill must be a UTC RFC3339 timestamp"),
					   false;

			uint64_t LookbackHours = 0;
			if (!ParseLookbackHours(Handler.GetParameter("lookbackHours", ""), LookbackHours))
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
									"invalid_lookback_hours",
									"lookbackHours must be a positive integer"),
					   false;

			auto AllowedClockSkew =
				MicroServiceConfigGetInt("allowed.clock_skew.seconds", 300);
			auto Now = Utils::Now();
			if (EndTime > Now + AllowedClockSkew)
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
									"invalid_timestamp",
									"timestampTill is too far in the future"),
					   false;
			if (LookbackHours >
				std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(3600))
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
									"invalid_lookback_hours", "lookbackHours is too large"),
					   false;

			auto LookbackSeconds = LookbackHours * static_cast<uint64_t>(3600);
			if (EndTime < LookbackSeconds)
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
									"invalid_timestamp",
									"requested window starts before Unix epoch"),
					   false;

			Output.startTime = EndTime - LookbackSeconds;
			Output.endTime = EndTime;

			auto TemperatureCutoverTime = GetTemperatureCutoverTime();
			if (!TemperatureCutoverTime)
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
									"internal_error",
									"Temperature migration cutover time is not configured"),
					   false;
			if (Output.startTime < *TemperatureCutoverTime)
				return SendMcpError(Handler, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
									"temperature_range_before_cutover",
									"The requested summary interval starts before the "
									"temperature migration cutover timestamp."),
					   false;

			return true;
		}

		bool ResolveBoardByVenue(RESTAPIHandler *Handler, const std::string &RouterId,
								 const std::string &VenueId,
								 AnalyticsObjects::BoardInfo &Board, bool &MultipleBoards,
								 bool &ProvisioningUnavailable) {
			MultipleBoards = false;
			ProvisioningUnavailable = false;
			std::vector<AnalyticsObjects::BoardInfo> Matches;
			auto AddMatch = [&](const AnalyticsObjects::BoardInfo &CurrentBoard) {
				auto Existing = std::find_if(
					Matches.begin(), Matches.end(), [&](const auto &ExistingBoard) {
						return ExistingBoard.info.id == CurrentBoard.info.id;
					});
				if (Existing == Matches.end())
					Matches.emplace_back(CurrentBoard);
			};

			auto Visitor = [&](const AnalyticsObjects::BoardInfo &CurrentBoard) {
				for (const auto &Venue : CurrentBoard.venueList) {
					if (!Venue.id.empty() && Venue.id == VenueId) {
						AddMatch(CurrentBoard);
						break;
					}
				}
				return true;
			};
			if (!StorageService()->BoardsDB().Iterate(Visitor))
				return false;

			if (Matches.empty()) {
				auto ChildVenueVisitor = [&](const AnalyticsObjects::BoardInfo &CurrentBoard) {
					for (const auto &Venue : CurrentBoard.venueList) {
						if (Venue.id.empty() || !Venue.monitorSubVenues)
							continue;

						ProvObjects::VenueDeviceList Devices;
						bool VenueExists = true;
						if (!SDK::Prov::Venue::GetDevices(Handler, Venue.id, true, Devices,
														  VenueExists)) {
							if (VenueExists)
								ProvisioningUnavailable = true;
							continue;
						}
						if (VenueExists &&
							std::find(Devices.devices.begin(), Devices.devices.end(), RouterId) !=
								Devices.devices.end()) {
							AddMatch(CurrentBoard);
							break;
						}
					}
					return !ProvisioningUnavailable;
				};
				if (!StorageService()->BoardsDB().Iterate(ChildVenueVisitor))
					return false;
				if (ProvisioningUnavailable)
					return false;
			}

			if (Matches.empty())
				return true;
			if (Matches.size() > 1) {
				MultipleBoards = true;
				return true;
			}
			Board = Matches.front();
			return true;
		}

		void SetNullableStat(Poco::JSON::Object &Object, const std::string &Key,
							 const std::optional<double> &Value) {
			if (Value)
				Object.set(Key, *Value);
			else
				Object.set(Key, Poco::Dynamic::Var());
		}

		void AddBand(Poco::JSON::Object &Object, const std::string &Suffix,
					 const BandStats &Stats) {
			if (!Stats.hasSamples) {
				SetNullableStat(Object, "min_wifi_temp_" + Suffix, std::nullopt);
				SetNullableStat(Object, "max_wifi_temp_" + Suffix, std::nullopt);
				SetNullableStat(Object, "avg_wifi_temp_" + Suffix, std::nullopt);
				return;
			}

			Object.set("min_wifi_temp_" + Suffix, Stats.min);
			Object.set("max_wifi_temp_" + Suffix, Stats.max);
			Object.set("avg_wifi_temp_" + Suffix, Stats.Avg());
		}

		std::optional<int64_t> GetRadioTemperature(const AnalyticsObjects::RadioTimePoint &Radio) {
			if (!Radio.wifi_temp)
				return std::nullopt;
			auto Temperature = *Radio.wifi_temp;
			if (Temperature < -40 || Temperature > 125)
				return std::nullopt;
			if (Temperature == 255)
				return std::nullopt;
			if (Temperature == 0 && Radio.wifi_temp_zero_is_unavailable)
				return std::nullopt;
			return Temperature;
		}
	} // namespace

	void RESTAPI_gateway_wifi_temp_handler::DoGet() {
		auto RouterId = GetBinding("routerId", "");
		if (!IsRouterIdValid(RouterId)) {
			return SendMcpError(*this, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
								"invalid_router_id", "routerId is invalid");
		}

		Window RequestedWindow;
		if (!ValidateQuery(*this, RequestedWindow))
			return;

		ProvObjects::InventoryTag Device;
		Poco::Net::HTTPResponse::HTTPStatus ProvisioningStatus =
			Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR;
		if (!SDK::Prov::Device::GetWithStatus(this, RouterId, Device, ProvisioningStatus)) {
			if (ProvisioningStatus == Poco::Net::HTTPResponse::HTTP_NOT_FOUND ||
				ProvisioningStatus == Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED ||
				ProvisioningStatus == Poco::Net::HTTPResponse::HTTP_FORBIDDEN) {
				return SendMcpError(*this, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "not_found",
									"Router was not found");
			}
			return SendMcpError(*this, Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY,
								"owprov_unavailable",
								"Unable to resolve router through provisioning service");
		}
		if (Device.venue.empty()) {
			return SendMcpError(*this, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "not_found",
								"Router was not found");
		}

		AnalyticsObjects::BoardInfo Board;
		bool MultipleBoards = false;
		bool ProvisioningUnavailable = false;
		if (!ResolveBoardByVenue(this, RouterId, Device.venue, Board, MultipleBoards,
								 ProvisioningUnavailable)) {
			if (ProvisioningUnavailable) {
				return SendMcpError(*this, Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY,
									"owprov_unavailable",
									"Unable to resolve router through provisioning service");
			}
			return SendMcpError(*this, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
								"analytics_board_storage_failed",
								"Unable to resolve Analytics board for router");
		}
		if (MultipleBoards) {
			return SendMcpError(*this, Poco::Net::HTTPResponse::HTTP_CONFLICT, "multiple_boards",
								"Router is mapped to multiple current boards");
		}
		if (Board.info.id.empty()) {
			return SendMcpError(*this, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "not_found",
								"Router was not found");
		}

		auto RetentionSeconds = Board.venueList[0].retention;
		if (RetentionSeconds > 0) {
			auto Now = Utils::Now();
			auto RetentionStart = Now > RetentionSeconds ? Now - RetentionSeconds : 0;
			if (RequestedWindow.endTime - RequestedWindow.startTime > RetentionSeconds ||
				RequestedWindow.startTime < RetentionStart) {
				return SendMcpError(
					*this, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
					"lookback_outside_retention",
					"Requested range is outside the configured monitoring retention window");
			}
		}

		TimePointDB::RecordVec Records;
		if (!StorageService()->TimePointsDB().SelectRecordsBySerial(
				Board.info.id, RouterId, RequestedWindow.startTime, RequestedWindow.endTime,
				Records)) {
			return SendMcpError(*this, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
								"radio_temperature_summary_query_failed",
								"Unable to retrieve gateway Wi-Fi temperature history");
		}

		BandStats Band2G;
		BandStats Band5G;
		uint64_t FirstObserved = 0;
		uint64_t LastObserved = 0;
		for (const auto &Record : Records) {
			bool UsedRecord = false;
			for (const auto &Radio : Record.radio_data) {
				auto Temperature = GetRadioTemperature(Radio);
				if (!Temperature)
					continue;
				if (Radio.band == 2) {
					Band2G.Add(*Temperature);
					UsedRecord = true;
				} else if (Radio.band == 5) {
					Band5G.Add(*Temperature);
					UsedRecord = true;
				}
			}

			if (UsedRecord) {
				if (FirstObserved == 0)
					FirstObserved = Record.timestamp;
				LastObserved = Record.timestamp;
			}
		}

		Poco::JSON::Object Answer;
		Poco::JSON::Object Requested;
		Requested.set("startTime", ToUtcString(RequestedWindow.startTime));
		Requested.set("endTime", ToUtcString(RequestedWindow.endTime));
		Answer.set("requestedWindow", Requested);

		Poco::JSON::Object Observed;
		if (FirstObserved == 0) {
			Observed.set("startTime", Poco::Dynamic::Var());
			Observed.set("endTime", Poco::Dynamic::Var());
		} else {
			Observed.set("startTime", ToUtcString(FirstObserved));
			Observed.set("endTime", ToUtcString(LastObserved));
		}
		Answer.set("observedWindow", Observed);

		AddBand(Answer, "2.4G", Band2G);
		AddBand(Answer, "5G", Band5G);
		return ReturnObject(Answer);
	}

} // namespace OpenWifi
