//
// Created by stephane bourque on 2022-03-11.
//

#pragma once

#include "Poco/String.h"
#include "Poco/StringTokenizer.h"
#include "Poco/Logger.h"
#include "RESTObjects/RESTAPI_AnalyticsObjects.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/utils.h"
#include "nlohmann/json.hpp"
#include <mutex>
#include <optional>

namespace OpenWifi {

	namespace APStats {
		inline std::optional<double> GetOptionalDoubleJSON(const char *field,
														   const nlohmann::json &doc) {
			try {
				if (!doc.contains(field) || doc[field].is_null())
					return std::nullopt;
				if (doc[field].is_number())
					return doc[field].get<double>();
			} catch (...) {
			}
			return std::nullopt;
		}

		inline bool GetOptionalBoolJSON(const char *field, const nlohmann::json &doc, bool &value) {
			try {
				if (!doc.contains(field) || doc[field].is_null() || !doc[field].is_boolean())
					return false;
				value = doc[field].get<bool>();
				return true;
			} catch (...) {
			}
			return false;
		}

		inline bool ConfigListContains(const std::string &ConfigKey, const std::string &Value) {
			if (Value.empty())
				return false;

			Poco::StringTokenizer Tokens(MicroServiceConfigGetString(ConfigKey, ""), ",",
										 Poco::StringTokenizer::TOK_TRIM |
											 Poco::StringTokenizer::TOK_IGNORE_EMPTY);
			for (const auto &Token : Tokens) {
				if (!Poco::icompare(Token, Value))
					return true;
			}
			return false;
		}

		inline bool ConfigListContainsPrefix(const std::string &ConfigKey,
											 const std::string &Value) {
			if (Value.empty())
				return false;

			Poco::StringTokenizer Tokens(MicroServiceConfigGetString(ConfigKey, ""), ",",
										 Poco::StringTokenizer::TOK_TRIM |
											 Poco::StringTokenizer::TOK_IGNORE_EMPTY);
			for (const auto &Token : Tokens) {
				if (Value.size() >= Token.size() &&
					!Poco::icompare(Value.substr(0, Token.size()), Token))
					return true;
			}
			return false;
		}

		inline bool ResolveTemperatureZeroIsUnavailableContract(
			const nlohmann::json &radio, const AnalyticsObjects::DeviceInfo &Device) {
			bool ExplicitContract = false;
			if (GetOptionalBoolJSON("temperature_zero_is_unavailable", radio, ExplicitContract) ||
				GetOptionalBoolJSON("temperatureZeroIsUnavailable", radio, ExplicitContract))
				return ExplicitContract;

			return ConfigListContains("temperature.zero_unavailable_device_types",
									  Device.deviceType) ||
				   ConfigListContains("temperature.zero_unavailable_platforms",
									  Device.platform) ||
				   ConfigListContainsPrefix(
					   "temperature.zero_unavailable_firmware_prefixes",
					   Device.lastFirmware);
		}

		inline bool ParseRadioTimePoint(const nlohmann::json &radio,
										const AnalyticsObjects::DeviceInfo &Device,
										AnalyticsObjects::RadioTimePoint &RTP) {
			RTP.temperature = GetOptionalDoubleJSON("temperature", radio);
			RTP.temperature_zero_is_unavailable =
				ResolveTemperatureZeroIsUnavailableContract(radio, Device);
			return true;
		}
	} // namespace APStats

	struct InterfaceClientEntry {
		std::vector<std::string> ipv4_addresses;
		std::vector<std::string> ipv6_addresses;
	};

	using InterfaceClientEntryMap_t = std::map<std::string, InterfaceClientEntry>;

	class AP {
	  public:
		explicit AP(uint64_t mac, const std::string &venue_id, const std::string &BoardId,
					Poco::Logger &L)
			: venue_id_(venue_id), boardId_(BoardId), Logger_(L) {
			DI_.serialNumber = Utils::IntToSerialNumber(mac);
		}

		void UpdateStats(const std::shared_ptr<nlohmann::json> &State);
		void UpdateConnection(const std::shared_ptr<nlohmann::json> &Connection);
		void UpdateHealth(const std::shared_ptr<nlohmann::json> &Health);

		[[nodiscard]] const AnalyticsObjects::DeviceInfo &Info() const { return DI_; }

	  private:
		std::string venue_id_;
		std::string boardId_;
		AnalyticsObjects::DeviceInfo DI_;
		AnalyticsObjects::DeviceTimePoint tp_base_;
		bool got_health = false, got_connection = false, got_base = false;
		Poco::Logger &Logger_;
		inline Poco::Logger &Logger() { return Logger_; }
	};
} // namespace OpenWifi
