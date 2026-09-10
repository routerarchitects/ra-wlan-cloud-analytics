//
// Created for MCP analytics response objects.
//

#include "RESTAPI_AnalyticsObjects.h"
#include "framework/RESTAPI_utils.h"
#include <Poco/Dynamic/Var.h>

using OpenWifi::RESTAPI_utils::field_to_json;

namespace OpenWifi::AnalyticsObjects {

	void MCPRequestedWindow::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "startTime", startTime);
		field_to_json(Obj, "endTime", endTime);
	}

	void MCPObservedWindow::to_json(Poco::JSON::Object &Obj) const {
		if (startTime)
			field_to_json(Obj, "startTime", *startTime);
		else
			Obj.set("startTime", Poco::Dynamic::Var());
		if (endTime)
			field_to_json(Obj, "endTime", *endTime);
		else
			Obj.set("endTime", Poco::Dynamic::Var());
	}

	namespace {
		void nullable_uint_to_json(Poco::JSON::Object &Obj, const char *Field,
								   const std::optional<uint64_t> &Value) {
			if (Value)
				field_to_json(Obj, Field, *Value);
			else
				Obj.set(Field, Poco::Dynamic::Var());
		}

		void nullable_double_to_json(Poco::JSON::Object &Obj, const char *Field,
									 const std::optional<double> &Value) {
			if (Value)
				field_to_json(Obj, Field, *Value);
			else
				Obj.set(Field, Poco::Dynamic::Var());
		}
	} // namespace

	void MCPMemorySummaryData::to_json(Poco::JSON::Object &Obj) const {
		nullable_uint_to_json(Obj, "min_memfree", min_memfree);
		nullable_uint_to_json(Obj, "max_memfree", max_memfree);
		nullable_uint_to_json(Obj, "avg_memfree", avg_memfree);
		nullable_uint_to_json(Obj, "latest_memfree", latest_memfree);
	}

	void MCPMemorySummaryMeta::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "requestedWindow", requestedWindow);
		field_to_json(Obj, "observedWindow", observedWindow);
	}

	void MCPGatewayMemorySummary::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "data", data);
		field_to_json(Obj, "meta", meta);
	}

	void MCPGatewayWifiTemperatureSummary::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "requestedWindow", requestedWindow);
		field_to_json(Obj, "observedWindow", observedWindow);
		nullable_double_to_json(Obj, "min_wifi_temp_2.4G", min_wifi_temp_2_4G);
		nullable_double_to_json(Obj, "max_wifi_temp_2.4G", max_wifi_temp_2_4G);
		nullable_double_to_json(Obj, "avg_wifi_temp_2.4G", avg_wifi_temp_2_4G);
		nullable_double_to_json(Obj, "latest_wifi_temp_2.4G", latest_wifi_temp_2_4G);
		nullable_double_to_json(Obj, "min_wifi_temp_5G", min_wifi_temp_5G);
		nullable_double_to_json(Obj, "max_wifi_temp_5G", max_wifi_temp_5G);
		nullable_double_to_json(Obj, "avg_wifi_temp_5G", avg_wifi_temp_5G);
		nullable_double_to_json(Obj, "latest_wifi_temp_5G", latest_wifi_temp_5G);
	}

} // namespace OpenWifi::AnalyticsObjects
