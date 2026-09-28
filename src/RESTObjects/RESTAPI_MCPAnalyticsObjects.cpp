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

	void MCPGatewayWifiTemperatureSummaryData::to_json(Poco::JSON::Object &Obj) const {
		nullable_double_to_json(Obj, "min_wifi_temp_2.4G", min_wifi_temp_2_4G);
		nullable_double_to_json(Obj, "max_wifi_temp_2.4G", max_wifi_temp_2_4G);
		nullable_double_to_json(Obj, "avg_wifi_temp_2.4G", avg_wifi_temp_2_4G);
		nullable_double_to_json(Obj, "latest_wifi_temp_2.4G", latest_wifi_temp_2_4G);
		nullable_double_to_json(Obj, "min_wifi_temp_5G", min_wifi_temp_5G);
		nullable_double_to_json(Obj, "max_wifi_temp_5G", max_wifi_temp_5G);
		nullable_double_to_json(Obj, "avg_wifi_temp_5G", avg_wifi_temp_5G);
		nullable_double_to_json(Obj, "latest_wifi_temp_5G", latest_wifi_temp_5G);
	}

	void MCPGatewayWifiTemperatureSummary::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "data", data);
		field_to_json(Obj, "meta", meta);
	}

	void MCPClientUsageItem::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "mac", mac);
		field_to_json(Obj, "rx_bytes", rx_bytes);
		field_to_json(Obj, "tx_bytes", tx_bytes);
		field_to_json(Obj, "total_bytes", total_bytes);
		field_to_json(Obj, "data_consume_rx", data_consume_rx);
		field_to_json(Obj, "data_consume_tx", data_consume_tx);
		field_to_json(Obj, "total_data_usage", total_data_usage);
	}

	void MCPClientUsageSummaryData::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "items", items);
		field_to_json(Obj, "totalClients", totalClients);
		field_to_json(Obj, "truncated", truncated);
	}

	void MCPDeviceBandwidthConsumptionSummary::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "data", data);
		field_to_json(Obj, "meta", meta);
	}

	void MCPClientRssiItem::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "mac", mac);
		field_to_json(Obj, "rssi_excellent_pct", rssi_excellent_pct);
		field_to_json(Obj, "rssi_good_pct", rssi_good_pct);
		field_to_json(Obj, "rssi_fair_pct", rssi_fair_pct);
		field_to_json(Obj, "rssi_poor_pct", rssi_poor_pct);
		field_to_json(Obj, "rssi_total_samples", rssi_total_samples);
	}

	void MCPClientRssiQualitySummary::to_json(Poco::JSON::Object &Obj) const {
		Poco::JSON::Object Meta;
		field_to_json(Meta, "requestedWindow", requestedWindow);
		field_to_json(Meta, "observedWindow", observedWindow);

		Poco::JSON::Object Data;
		field_to_json(Data, "items", items);
		field_to_json(Data, "totalClients", totalClients);
		field_to_json(Data, "truncated", truncated);

		Obj.set("meta", Meta);
		Obj.set("data", Data);
	}

	void MCPGatewayAvailabilityData::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "gw_uuid", gw_uuid);
		field_to_json(Obj, "fetch_status", fetch_status);
		field_to_json(Obj, "offlineEventCount", offlineEventCount);
	}

	void MCPGatewayAvailabilityMeta::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "requestedWindow", requestedWindow);
		field_to_json(Obj, "observedWindow", observedWindow);
	}

	void MCPGatewayAvailabilitySummary::to_json(Poco::JSON::Object &Obj) const {
		field_to_json(Obj, "meta", meta);
		field_to_json(Obj, "data", data);
	}

} // namespace OpenWifi::AnalyticsObjects
