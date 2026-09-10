#pragma once

#include "RESTObjects/RESTAPI_AnalyticsObjects.h"
#include <Poco/Net/HTTPResponse.h>
#include <Poco/URI.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace OpenWifi {

	class RESTAPIHandler;

	namespace MCP {

		constexpr const char *InvalidRouterIdMessage =
			"routerId must be a valid path-safe OWPROV gateway serial number (1 to 64 "
			"alphanumeric characters, hyphens, or underscores)";
		constexpr const char *UnauthorizedMessage =
			"Missing, invalid, or expired bearer token";

		struct Error {
			Poco::Net::HTTPResponse::HTTPStatus status =
				Poco::Net::HTTPResponse::HTTP_BAD_REQUEST;
			std::string error;
			std::string message;
		};

		struct Window {
			uint64_t startTime = 0;
			uint64_t endTime = 0;
			uint64_t lookbackHours = 0;
		};

		inline void SetError(Error &E, Poco::Net::HTTPResponse::HTTPStatus Status,
							 std::string ErrorCode, std::string Message) {
			E.status = Status;
			E.error = std::move(ErrorCode);
			E.message = std::move(Message);
		}

		inline bool ValidateRouterId(const std::string &routerId, Error &E) {
			if (routerId.empty() || routerId.size() > 64) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_router_id",
						 InvalidRouterIdMessage);
				return false;
			}
			for (auto c : routerId) {
				if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
					SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_router_id",
							 InvalidRouterIdMessage);
					return false;
				}
			}
			return true;
		}

		inline bool ExtractBearerToken(const std::optional<std::string> &Authorization,
									   std::string &Token, Error &E) {
			if (!Authorization) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED, "unauthorized",
						 UnauthorizedMessage);
				return false;
			}

			const std::string Prefix = "Bearer ";
			if (Authorization->rfind(Prefix, 0) != 0 || Authorization->size() == Prefix.size()) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED, "unauthorized",
						 UnauthorizedMessage);
				return false;
			}

			Token = Authorization->substr(Prefix.size());
			return true;
		}

		inline bool ValidateBearerAuthorization(
			const std::optional<std::string> &Authorization,
			const std::function<bool(const std::string &)> &TokenValidator, Error &E) {
			std::string Token;
			if (!ExtractBearerToken(Authorization, Token, E))
				return false;
			if (!TokenValidator(Token)) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED, "unauthorized",
						 UnauthorizedMessage);
				return false;
			}
			return true;
		}

		inline bool ParseDecimalUint64(const std::string &Value, uint64_t &Parsed) {
			if (Value.empty())
				return false;
			Parsed = 0;
			for (auto c : Value) {
				if (!std::isdigit(static_cast<unsigned char>(c)))
					return false;
				auto Digit = static_cast<uint64_t>(c - '0');
				if (Parsed > (std::numeric_limits<uint64_t>::max() - Digit) / 10)
					return false;
				Parsed = Parsed * 10 + Digit;
			}
			return true;
		}

		inline bool IsLeapYear(int Year) {
			return (Year % 4 == 0 && Year % 100 != 0) || (Year % 400 == 0);
		}

		inline int DaysInMonth(int Year, int Month) {
			static constexpr int Days[] = {31, 28, 31, 30, 31, 30,
										   31, 31, 30, 31, 30, 31};
			if (Month == 2 && IsLeapYear(Year))
				return 29;
			if (Month < 1 || Month > 12)
				return 0;
			return Days[Month - 1];
		}

		inline bool DigitsAt(const std::string &Value, size_t Offset, size_t Count) {
			return Value.size() >= Offset + Count &&
				   std::all_of(Value.begin() + Offset, Value.begin() + Offset + Count,
							   [](unsigned char c) { return std::isdigit(c); });
		}

		inline int ToInt(const std::string &Value, size_t Offset, size_t Count) {
			int Parsed = 0;
			for (size_t i = Offset; i < Offset + Count; ++i)
				Parsed = Parsed * 10 + (Value[i] - '0');
			return Parsed;
		}

		inline bool ParseTimestampTill(const std::string &Value, uint64_t &EpochSeconds) {
			if (Value.size() != 20 || Value[4] != '-' || Value[7] != '-' ||
				Value[10] != 'T' || Value[13] != ':' || Value[16] != ':' || Value[19] != 'Z' ||
				!DigitsAt(Value, 0, 4) || !DigitsAt(Value, 5, 2) ||
				!DigitsAt(Value, 8, 2) || !DigitsAt(Value, 11, 2) ||
				!DigitsAt(Value, 14, 2) || !DigitsAt(Value, 17, 2)) {
				return false;
			}

			auto Year = ToInt(Value, 0, 4);
			auto Month = ToInt(Value, 5, 2);
			auto Day = ToInt(Value, 8, 2);
			auto Hour = ToInt(Value, 11, 2);
			auto Minute = ToInt(Value, 14, 2);
			auto Second = ToInt(Value, 17, 2);
			if (Month < 1 || Month > 12 || Day < 1 || Day > DaysInMonth(Year, Month) ||
				Hour > 23 || Minute > 59 || Second > 59) {
				return false;
			}

			std::tm Tm{};
			Tm.tm_year = Year - 1900;
			Tm.tm_mon = Month - 1;
			Tm.tm_mday = Day;
			Tm.tm_hour = Hour;
			Tm.tm_min = Minute;
			Tm.tm_sec = Second;
			Tm.tm_isdst = 0;
			auto Epoch = timegm(&Tm);
			if (Epoch < 0)
				return false;

			std::tm RoundTrip{};
			gmtime_r(&Epoch, &RoundTrip);
			if (RoundTrip.tm_year != Tm.tm_year || RoundTrip.tm_mon != Tm.tm_mon ||
				RoundTrip.tm_mday != Tm.tm_mday || RoundTrip.tm_hour != Tm.tm_hour ||
				RoundTrip.tm_min != Tm.tm_min || RoundTrip.tm_sec != Tm.tm_sec) {
				return false;
			}
			EpochSeconds = static_cast<uint64_t>(Epoch);
			return true;
		}

		inline bool ParseRFC3339Timestamp(const std::string &Value, uint64_t &EpochSeconds) {
			if (Value.size() != 20 && Value.size() != 25)
				return false;
			if (Value[4] != '-' || Value[7] != '-' || Value[10] != 'T' ||
				Value[13] != ':' || Value[16] != ':' ||
				!DigitsAt(Value, 0, 4) || !DigitsAt(Value, 5, 2) ||
				!DigitsAt(Value, 8, 2) || !DigitsAt(Value, 11, 2) ||
				!DigitsAt(Value, 14, 2) || !DigitsAt(Value, 17, 2)) {
				return false;
			}

			int OffsetSeconds = 0;
			if (Value.size() == 20) {
				if (Value[19] != 'Z')
					return false;
			} else {
				if ((Value[19] != '+' && Value[19] != '-') || Value[22] != ':' ||
					!DigitsAt(Value, 20, 2) || !DigitsAt(Value, 23, 2)) {
					return false;
				}
				auto OffsetHours = ToInt(Value, 20, 2);
				auto OffsetMinutes = ToInt(Value, 23, 2);
				if (OffsetHours > 23 || OffsetMinutes > 59)
					return false;
				OffsetSeconds = (OffsetHours * 3600) + (OffsetMinutes * 60);
				if (Value[19] == '-')
					OffsetSeconds = -OffsetSeconds;
			}

			auto Year = ToInt(Value, 0, 4);
			auto Month = ToInt(Value, 5, 2);
			auto Day = ToInt(Value, 8, 2);
			auto Hour = ToInt(Value, 11, 2);
			auto Minute = ToInt(Value, 14, 2);
			auto Second = ToInt(Value, 17, 2);
			if (Month < 1 || Month > 12 || Day < 1 || Day > DaysInMonth(Year, Month) ||
				Hour > 23 || Minute > 59 || Second > 59) {
				return false;
			}

			std::tm Tm{};
			Tm.tm_year = Year - 1900;
			Tm.tm_mon = Month - 1;
			Tm.tm_mday = Day;
			Tm.tm_hour = Hour;
			Tm.tm_min = Minute;
			Tm.tm_sec = Second;
			Tm.tm_isdst = 0;
			auto LocalEpoch = timegm(&Tm);
			if (LocalEpoch < 0)
				return false;
			auto UtcEpoch = static_cast<int64_t>(LocalEpoch) - OffsetSeconds;
			if (UtcEpoch < 0)
				return false;
			EpochSeconds = static_cast<uint64_t>(UtcEpoch);
			return true;
		}

		inline std::string FormatTimestamp(uint64_t EpochSeconds) {
			std::time_t Time = static_cast<std::time_t>(EpochSeconds);
			std::tm Tm{};
			gmtime_r(&Time, &Tm);
			char Buffer[21]{};
			std::strftime(Buffer, sizeof(Buffer), "%Y-%m-%dT%H:%M:%SZ", &Tm);
			return Buffer;
		}

		inline bool TimestampInHalfOpenWindow(uint64_t Timestamp, const Window &Requested) {
			return Timestamp >= Requested.startTime && Timestamp < Requested.endTime;
		}

		inline bool ValidateWindowQuery(const Poco::URI::QueryParameters &Params, uint64_t Now,
										uint64_t ClockSkewSeconds, Window &Parsed, Error &E) {
			std::set<std::string> Allowed{"timestampTill", "lookbackHours"};
			size_t TimestampCount = 0;
			size_t LookbackCount = 0;
			std::string TimestampValue;
			std::string LookbackValue;

			for (const auto &[Name, Value] : Params) {
				if (Allowed.find(Name) == Allowed.end()) {
					SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
							 "invalid_query_parameter",
							 "Unsupported query parameter: " + Name);
					return false;
				}
				if (Name == "timestampTill") {
					++TimestampCount;
					TimestampValue = Value;
				} else if (Name == "lookbackHours") {
					++LookbackCount;
					LookbackValue = Value;
				}
			}

			if (TimestampCount != 1) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_timestamp",
						 "timestampTill must be present exactly once");
				return false;
			}
			if (!ParseTimestampTill(TimestampValue, Parsed.endTime)) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_timestamp",
						 "timestampTill must be a valid UTC timestamp in YYYY-MM-DDTHH:mm:ssZ "
						 "format");
				return false;
			}
			if (Parsed.endTime > Now &&
				Parsed.endTime - Now > ClockSkewSeconds) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_timestamp",
						 "timestampTill is beyond the server clock skew tolerance");
				return false;
			}

			if (LookbackCount != 1) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_lookback_hours",
						 "lookbackHours must be present exactly once");
				return false;
			}
			if (!ParseDecimalUint64(LookbackValue, Parsed.lookbackHours) ||
				Parsed.lookbackHours == 0) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_lookback_hours",
						 "lookbackHours must be a positive whole number");
				return false;
			}
			constexpr uint64_t MaxApiLookbackHours = 87600; // 10 years maximum API limit
			if (Parsed.lookbackHours > MaxApiLookbackHours) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_lookback_hours",
						 "lookbackHours exceeds maximum API limit of 87600 hours");
				return false;
			}
			auto LookbackSeconds = Parsed.lookbackHours * static_cast<uint64_t>(3600);
			if (LookbackSeconds > Parsed.endTime) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_lookback_hours",
						 "lookbackHours places the requested start before the Unix epoch");
				return false;
			}
			Parsed.startTime = Parsed.endTime - LookbackSeconds;
			return true;
		}

		inline bool ValidateRetention(const Window &Requested, uint64_t RetentionSeconds,
									  uint64_t Now, uint64_t ClockSkewSeconds, Error &E) {
			auto MaxLookbackHours = RetentionSeconds / static_cast<uint64_t>(3600);
			if (MaxLookbackHours == 0 || Requested.lookbackHours > MaxLookbackHours) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "invalid_lookback_hours",
						 "lookbackHours exceeds configured maximum lookback");
				return false;
			}

			uint64_t RequestEndLimit = Now;
			if (std::numeric_limits<uint64_t>::max() - RequestEndLimit >= ClockSkewSeconds)
				RequestEndLimit += ClockSkewSeconds;
			else
				RequestEndLimit = std::numeric_limits<uint64_t>::max();

			auto RetentionStart = Now > RetentionSeconds ? Now - RetentionSeconds : 0;
			if (Requested.startTime < RetentionStart || Requested.endTime > RequestEndLimit) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
						 "lookback_outside_retention",
						 "Requested range is outside the configured monitoring retention window");
				return false;
			}
			return true;
		}

		inline AnalyticsObjects::MCPGatewayMemorySummary CalculateMemorySummary(
			const std::vector<AnalyticsObjects::DeviceTimePoint> &Records,
			const Window &Requested) {
			AnalyticsObjects::MCPGatewayMemorySummary Summary;
			Summary.meta.requestedWindow.startTime = FormatTimestamp(Requested.startTime);
			Summary.meta.requestedWindow.endTime = FormatTimestamp(Requested.endTime);

			uint64_t Count = 0;
			long double Sum = 0;
			uint64_t LatestTimestamp = 0;
			uint64_t ObservedStartTimestamp = 0;
			uint64_t ObservedEndTimestamp = 0;
			std::string LatestId;

			for (const auto &Record : Records) {
				const auto &Resource = Record.resource_data;
				if (!Resource.memory_free)
					continue;
				if (Resource.memory_total && *Resource.memory_free > *Resource.memory_total)
					continue;

				auto Free = *Resource.memory_free;
				if (Count == 0) {
					Summary.data.min_memfree = Free;
					Summary.data.max_memfree = Free;
					Summary.data.latest_memfree = Free;
					Summary.meta.observedWindow.startTime = FormatTimestamp(Record.timestamp);
					Summary.meta.observedWindow.endTime = FormatTimestamp(Record.timestamp);
					LatestTimestamp = Record.timestamp;
					ObservedStartTimestamp = Record.timestamp;
					ObservedEndTimestamp = Record.timestamp;
					LatestId = Record.id;
				} else {
					Summary.data.min_memfree = std::min(*Summary.data.min_memfree, Free);
					Summary.data.max_memfree = std::max(*Summary.data.max_memfree, Free);
					if (Record.timestamp < ObservedStartTimestamp) {
						Summary.meta.observedWindow.startTime = FormatTimestamp(Record.timestamp);
						ObservedStartTimestamp = Record.timestamp;
					}
					if (Record.timestamp > ObservedEndTimestamp) {
						Summary.meta.observedWindow.endTime = FormatTimestamp(Record.timestamp);
						ObservedEndTimestamp = Record.timestamp;
					}
					if (Record.timestamp > LatestTimestamp ||
						(Record.timestamp == LatestTimestamp &&
						 (Record.id > LatestId ||
						  (Record.id.empty() && LatestId.empty() &&
						   Free > *Summary.data.latest_memfree)))) {
						Summary.data.latest_memfree = Free;
						LatestTimestamp = Record.timestamp;
						LatestId = Record.id;
					}
				}

				Sum += static_cast<long double>(Free);
				++Count;
			}

			if (Count > 0) {
				auto Average = std::floor(Sum / static_cast<long double>(Count) + 0.5L);
				if (Average > static_cast<long double>(std::numeric_limits<uint64_t>::max()))
					Summary.data.avg_memfree = std::numeric_limits<uint64_t>::max();
				else
					Summary.data.avg_memfree = static_cast<uint64_t>(Average);
			}
			return Summary;
		}

		inline bool ValidateTemperatureCutover(const Window &Requested, uint64_t CutoverTime,
											   Error &E) {
			if (Requested.startTime < CutoverTime) {
				SetError(E, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
						 "temperature_range_before_cutover",
						 "The requested summary interval starts before the temperature migration "
						 "cutover timestamp.");
				return false;
			}
			return true;
		}

		inline AnalyticsObjects::MCPGatewayWifiTemperatureSummary
		CalculateRadioTemperatureSummary(
			const std::vector<AnalyticsObjects::DeviceTimePoint> &Records,
			const Window &Requested, uint64_t CutoverTime) {
			struct BandStats {
				std::optional<double> min;
				std::optional<double> max;
				std::optional<double> latest;
				uint64_t latestTimestamp = 0;
				long double sum = 0;
				uint64_t count = 0;
			};

			auto AddSample = [](BandStats &Stats, double Value, uint64_t Timestamp) {
				if (!Stats.min || Value < *Stats.min)
					Stats.min = Value;
				if (!Stats.max || Value > *Stats.max)
					Stats.max = Value;
				if (!Stats.latest || Timestamp >= Stats.latestTimestamp) {
					Stats.latest = Value;
					Stats.latestTimestamp = Timestamp;
				}
				Stats.sum += static_cast<long double>(Value);
				++Stats.count;
			};

			AnalyticsObjects::MCPGatewayWifiTemperatureSummary Summary;
			Summary.requestedWindow.startTime = FormatTimestamp(Requested.startTime);
			Summary.requestedWindow.endTime = FormatTimestamp(Requested.endTime);

			BandStats Band2G;
			BandStats Band5G;
			bool AnyValidSample = false;
			uint64_t ObservedStartTimestamp = 0;
			uint64_t ObservedEndTimestamp = 0;

			for (const auto &Record : Records) {
				if (Record.timestamp < CutoverTime ||
					!TimestampInHalfOpenWindow(Record.timestamp, Requested)) {
					continue;
				}

				bool RecordContributed = false;
				for (const auto &Radio : Record.radio_data) {
					if (Radio.band != 2 && Radio.band != 5)
						continue;
					if (!Radio.wifi_temp || !std::isfinite(*Radio.wifi_temp))
						continue;
					auto Value = *Radio.wifi_temp;
					if (Value < -40.0 || Value > 125.0)
						continue;
					if (Value == 0.0 && Radio.wifi_temp_zero_is_unavailable)
						continue;

					if (Radio.band == 2)
						AddSample(Band2G, Value, Record.timestamp);
					else
						AddSample(Band5G, Value, Record.timestamp);
					RecordContributed = true;
				}

				if (RecordContributed) {
					if (!AnyValidSample) {
						ObservedStartTimestamp = Record.timestamp;
						ObservedEndTimestamp = Record.timestamp;
						AnyValidSample = true;
					} else {
						ObservedStartTimestamp = std::min(ObservedStartTimestamp, Record.timestamp);
						ObservedEndTimestamp = std::max(ObservedEndTimestamp, Record.timestamp);
					}
				}
			}

			if (AnyValidSample) {
				Summary.observedWindow.startTime = FormatTimestamp(ObservedStartTimestamp);
				Summary.observedWindow.endTime = FormatTimestamp(ObservedEndTimestamp);
			}

			if (Band2G.count > 0) {
				Summary.min_wifi_temp_2_4G = Band2G.min;
				Summary.max_wifi_temp_2_4G = Band2G.max;
				Summary.avg_wifi_temp_2_4G =
					static_cast<double>(Band2G.sum / static_cast<long double>(Band2G.count));
				Summary.latest_wifi_temp_2_4G = Band2G.latest;
			}
			if (Band5G.count > 0) {
				Summary.min_wifi_temp_5G = Band5G.min;
				Summary.max_wifi_temp_5G = Band5G.max;
				Summary.avg_wifi_temp_5G =
					static_cast<double>(Band5G.sum / static_cast<long double>(Band5G.count));
				Summary.latest_wifi_temp_5G = Band5G.latest;
			}

			return Summary;
		}

		void SendError(RESTAPIHandler &Handler, const Error &E);
		bool AuthenticateBearerToken(RESTAPIHandler &Handler, Error &E);
		bool GetTemperatureMigrationCutoverTime(uint64_t &CutoverTime, Error &E);

	} // namespace MCP
} // namespace OpenWifi
