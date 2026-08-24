//
// RESTAPI_mcp_handlers Implementation
//

#include "RESTAPI_mcp_handlers.h"
#include "RESTAPI_mcp_helpers.h"
#include "StorageService.h"
#include "fmt/format.h"
#include "framework/utils.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

namespace OpenWifi {

	static std::string FormatMB(uint64_t bytes) {
		double mb = static_cast<double>(bytes) / 1000000.0;
		std::ostringstream os;
		os << std::fixed << std::setprecision(2) << mb << " MB";
		return os.str();
	}

	static std::string NormalizeMac(const std::string &rawMac) {
		std::string clean;
		for (char c : rawMac) {
			if (std::isxdigit(static_cast<unsigned char>(c))) {
				clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
		}
		if (clean.size() != 12)
			return rawMac;
		std::string formatted;
		for (size_t i = 0; i < 12; i += 2) {
			if (i > 0)
				formatted += ":";
			formatted += clean.substr(i, 2);
		}
		return formatted;
	}

	// 1. Memory Summary Handler
	void RESTAPI_device_memory_summary_handler::DoGet() {
		MCP::MCPValidatedRequest req;
		if (!MCP::ValidateMCPRequest(this, MCP::MCPDomainCutoverType::None, req)) {
			return;
		}

		std::vector<AnalyticsObjects::DeviceTimePoint> recs;
		StorageService()->TimePointsDB().SelectRecordsBySerial(req.resolvedBoardId, req.routerId, req.startTimeEpoch,
															   req.endTimeEpoch, 0, recs);

		std::vector<uint64_t> memoryFreeSamples;
		std::optional<uint64_t> earliestTime;
		std::optional<uint64_t> latestTime;
		std::optional<uint64_t> latestSampleTime;
		std::optional<uint64_t> latestMemfreeVal;

		for (const auto &rec : recs) {
			if (rec.resource_data.memory_free.has_value()) {
				uint64_t freeVal = rec.resource_data.memory_free.value();
				if (rec.resource_data.memory_total.has_value()) {
					uint64_t totalVal = rec.resource_data.memory_total.value();
					if (freeVal > totalVal) {
						continue; // Exclude invalid sample where free > total
					}
				}
				memoryFreeSamples.push_back(freeVal);
				if (!earliestTime.has_value() || rec.timestamp < earliestTime.value()) {
					earliestTime = rec.timestamp;
				}
				if (!latestTime.has_value() || rec.timestamp > latestTime.value()) {
					latestTime = rec.timestamp;
				}
				if (!latestSampleTime.has_value() || rec.timestamp >= latestSampleTime.value()) {
					latestSampleTime = rec.timestamp;
					latestMemfreeVal = freeVal;
				}
			}
		}

		AnalyticsObjects::MCPGatewayMemorySummary resp;
		resp.meta.requestedWindow.startTime = MCP::FormatRFC3339UTC(req.startTimeEpoch);
		resp.meta.requestedWindow.endTime = MCP::FormatRFC3339UTC(req.endTimeEpoch);

		if (!memoryFreeSamples.empty()) {
			uint64_t minVal = *std::min_element(memoryFreeSamples.begin(), memoryFreeSamples.end());
			uint64_t maxVal = *std::max_element(memoryFreeSamples.begin(), memoryFreeSamples.end());
			double sumVal = 0.0;
			for (auto v : memoryFreeSamples)
				sumVal += static_cast<double>(v);
			uint64_t avgVal = static_cast<uint64_t>(std::llround(sumVal / static_cast<double>(memoryFreeSamples.size())));

			resp.data.min_memfree = minVal;
			resp.data.max_memfree = maxVal;
			resp.data.avg_memfree = avgVal;
			resp.data.latest_memfree = latestMemfreeVal;
			resp.meta.observedWindow.startTime = MCP::FormatRFC3339UTC(earliestTime.value());
			resp.meta.observedWindow.endTime = MCP::FormatRFC3339UTC(latestTime.value());
		}

		ReturnObject(resp);
	}

	// 2. Radio Temperature Summary Handler
	void RESTAPI_device_radio_temp_summary_handler::DoGet() {
		MCP::MCPValidatedRequest req;
		if (!MCP::ValidateMCPRequest(this, MCP::MCPDomainCutoverType::RadioTemperature, req)) {
			return;
		}

		std::vector<AnalyticsObjects::DeviceTimePoint> recs;
		StorageService()->TimePointsDB().SelectRecordsBySerial(req.resolvedBoardId, req.routerId, req.startTimeEpoch,
															   req.endTimeEpoch, 0, recs);

		std::vector<double> temps24G;
		std::vector<double> temps5G;
		std::optional<uint64_t> earliestTime;
		std::optional<uint64_t> latestTime;

		std::optional<uint64_t> latest24GTime;
		std::optional<double> latest24GVal;
		std::optional<uint64_t> latest5GTime;
		std::optional<double> latest5GVal;

		for (const auto &rec : recs) {
			for (const auto &radio : rec.radio_data) {
				std::optional<int64_t> tempOpt = radio.wifi_temp;
				if (!tempOpt.has_value()) {
					// Fallback to radio.temperature if valid
					if (radio.temperature >= -40 && radio.temperature <= 125 && radio.temperature != 255) {
						tempOpt = radio.temperature;
					}
				}

				if (!tempOpt.has_value())
					continue;
				int64_t tempVal = tempOpt.value();

				// Temperature validation rules
				if (tempVal == 255 || tempVal < -40 || tempVal > 125)
					continue;

				double dTemp = static_cast<double>(tempVal);
				bool sampleUsed = false;

				if (radio.band == 2) {
					temps24G.push_back(dTemp);
					if (!latest24GTime.has_value() || rec.timestamp >= latest24GTime.value()) {
						latest24GTime = rec.timestamp;
						latest24GVal = dTemp;
					}
					sampleUsed = true;
				} else if (radio.band == 5) {
					temps5G.push_back(dTemp);
					if (!latest5GTime.has_value() || rec.timestamp >= latest5GTime.value()) {
						latest5GTime = rec.timestamp;
						latest5GVal = dTemp;
					}
					sampleUsed = true;
				}

				if (sampleUsed) {
					if (!earliestTime.has_value() || rec.timestamp < earliestTime.value()) {
						earliestTime = rec.timestamp;
					}
					if (!latestTime.has_value() || rec.timestamp > latestTime.value()) {
						latestTime = rec.timestamp;
					}
				}
			}
		}

		AnalyticsObjects::MCPRadioTempSummary resp;
		resp.meta.requestedWindow.startTime = MCP::FormatRFC3339UTC(req.startTimeEpoch);
		resp.meta.requestedWindow.endTime = MCP::FormatRFC3339UTC(req.endTimeEpoch);

		if (!temps24G.empty()) {
			resp.data.min_wifi_temp_2_4G = *std::min_element(temps24G.begin(), temps24G.end());
			resp.data.max_wifi_temp_2_4G = *std::max_element(temps24G.begin(), temps24G.end());
			double sum = 0.0;
			for (double t : temps24G)
				sum += t;
			double avg = sum / static_cast<double>(temps24G.size());
			resp.data.avg_wifi_temp_2_4G = std::round(avg * 100.0) / 100.0;
			resp.data.latest_wifi_temp_2_4G = latest24GVal;
		}

		if (!temps5G.empty()) {
			resp.data.min_wifi_temp_5G = *std::min_element(temps5G.begin(), temps5G.end());
			resp.data.max_wifi_temp_5G = *std::max_element(temps5G.begin(), temps5G.end());
			double sum = 0.0;
			for (double t : temps5G)
				sum += t;
			double avg = sum / static_cast<double>(temps5G.size());
			resp.data.avg_wifi_temp_5G = std::round(avg * 100.0) / 100.0;
			resp.data.latest_wifi_temp_5G = latest5GVal;
		}

		if (earliestTime.has_value()) {
			resp.meta.observedWindow.startTime = MCP::FormatRFC3339UTC(earliestTime.value());
			resp.meta.observedWindow.endTime = MCP::FormatRFC3339UTC(latestTime.value());
		}

		ReturnObject(resp);
	}

	// 3. Wi-Fi Client Bandwidth Usage Summary Handler
	void RESTAPI_device_wifi_client_usage_summary_handler::DoGet() {
		MCP::MCPValidatedRequest req;
		if (!MCP::ValidateMCPRequest(this, MCP::MCPDomainCutoverType::None, req)) {
			return;
		}

		std::vector<AnalyticsObjects::DeviceTimePoint> recs;
		StorageService()->TimePointsDB().SelectRecordsBySerial(req.resolvedBoardId, req.routerId, req.startTimeEpoch,
															   req.endTimeEpoch, 0, recs);

		struct ClientSample {
			uint64_t timestamp;
			uint64_t rx_bytes;
			uint64_t tx_bytes;
		};

		std::map<std::string, std::vector<ClientSample>> clientSamples;

		for (const auto &rec : recs) {
			for (const auto &ssid : rec.ssid_data) {
				for (const auto &assoc : ssid.associations) {
					if (assoc.station.empty())
						continue;
					std::string mac = NormalizeMac(assoc.station);
					clientSamples[mac].push_back(
						ClientSample{rec.timestamp, assoc.rx_bytes, assoc.tx_bytes});
				}
			}
		}

		std::vector<AnalyticsObjects::MCPClientUsageItem> items;

		for (auto &[mac, samples] : clientSamples) {
			std::sort(samples.begin(), samples.end(),
					  [](const ClientSample &a, const ClientSample &b) {
						  return a.timestamp < b.timestamp;
					  });

			uint64_t totalRx = 0;
			uint64_t totalTx = 0;

			if (samples.size() >= 2) {
				for (size_t i = 1; i < samples.size(); ++i) {
					const auto &prev = samples[i - 1];
					const auto &curr = samples[i];

					// RX delta
					if (curr.rx_bytes >= prev.rx_bytes) {
						totalRx += (curr.rx_bytes - prev.rx_bytes);
					} else {
						// Only apply rollover if prev counter was near 32-bit or 64-bit upper boundary
						uint64_t max32 = 0xFFFFFFFFULL;
						uint64_t max64 = 0xFFFFFFFFFFFFFFFFULL;
						uint64_t thresh32 = 0xF0000000ULL;
						uint64_t thresh64 = 0xF000000000000000ULL;

						if (prev.rx_bytes >= thresh32 && prev.rx_bytes <= max32) {
							totalRx += ((max32 - prev.rx_bytes) + curr.rx_bytes + 1);
						} else if (prev.rx_bytes >= thresh64) {
							totalRx += ((max64 - prev.rx_bytes) + curr.rx_bytes + 1);
						} else {
							// Counter reset / AP restart / reconnect baseline
							totalRx += curr.rx_bytes;
						}
					}

					// TX delta
					if (curr.tx_bytes >= prev.tx_bytes) {
						totalTx += (curr.tx_bytes - prev.tx_bytes);
					} else {
						uint64_t max32 = 0xFFFFFFFFULL;
						uint64_t max64 = 0xFFFFFFFFFFFFFFFFULL;
						uint64_t thresh32 = 0xF0000000ULL;
						uint64_t thresh64 = 0xF000000000000000ULL;

						if (prev.tx_bytes >= thresh32 && prev.tx_bytes <= max32) {
							totalTx += ((max32 - prev.tx_bytes) + curr.tx_bytes + 1);
						} else if (prev.tx_bytes >= thresh64) {
							totalTx += ((max64 - prev.tx_bytes) + curr.tx_bytes + 1);
						} else {
							totalTx += curr.tx_bytes;
						}
					}
				}
			}

			AnalyticsObjects::MCPClientUsageItem item;
			item.mac = mac;
			item.rx_bytes = totalRx;
			item.tx_bytes = totalTx;
			item.total_bytes = totalRx + totalTx;
			item.data_consume_rx = FormatMB(totalRx);
			item.data_consume_tx = FormatMB(totalTx);
			item.total_data_usage = FormatMB(item.total_bytes);

			items.push_back(item);
		}

		// Deterministic sort: total_bytes DESC, mac ASC
		std::sort(items.begin(), items.end(),
				  [](const AnalyticsObjects::MCPClientUsageItem &a,
					 const AnalyticsObjects::MCPClientUsageItem &b) {
					  if (a.total_bytes != b.total_bytes)
						  return a.total_bytes > b.total_bytes;
					  return a.mac < b.mac;
				  });

		AnalyticsObjects::MCPClientUsageSummary resp;
		resp.meta.requestedWindow.startTime = MCP::FormatRFC3339UTC(req.startTimeEpoch);
		resp.meta.requestedWindow.endTime = MCP::FormatRFC3339UTC(req.endTimeEpoch);

		resp.data.totalClients = items.size();
		resp.data.truncated = resp.data.totalClients > 500;

		if (items.size() > 500) {
			items.resize(500);
		}
		resp.data.items = items;

		// Calculate observedWindow strictly from samples contributing to truncated items
		std::optional<uint64_t> earliestTime;
		std::optional<uint64_t> latestTime;
		for (const auto &item : resp.data.items) {
			auto it = clientSamples.find(item.mac);
			if (it != clientSamples.end()) {
				for (const auto &s : it->second) {
					if (!earliestTime.has_value() || s.timestamp < earliestTime.value()) {
						earliestTime = s.timestamp;
					}
					if (!latestTime.has_value() || s.timestamp > latestTime.value()) {
						latestTime = s.timestamp;
					}
				}
			}
		}

		if (earliestTime.has_value()) {
			resp.meta.observedWindow.startTime = MCP::FormatRFC3339UTC(earliestTime.value());
			resp.meta.observedWindow.endTime = MCP::FormatRFC3339UTC(latestTime.value());
		}

		ReturnObject(resp);
	}

	// 4. Client RSSI Summary Handler
	void RESTAPI_device_wifi_client_rssi_summary_handler::DoGet() {
		MCP::MCPValidatedRequest req;
		if (!MCP::ValidateMCPRequest(this, MCP::MCPDomainCutoverType::None, req)) {
			return;
		}

		std::vector<AnalyticsObjects::DeviceTimePoint> recs;
		StorageService()->TimePointsDB().SelectRecordsBySerial(req.resolvedBoardId, req.routerId, req.startTimeEpoch,
															   req.endTimeEpoch, 0, recs);

		struct RSSISample {
			uint64_t timestamp;
			int64_t rssi;
		};

		std::map<std::string, std::vector<RSSISample>> clientRssi;

		for (const auto &rec : recs) {
			for (const auto &ssid : rec.ssid_data) {
				for (const auto &assoc : ssid.associations) {
					if (assoc.station.empty())
						continue;
					// Exclude invalid RSSI samples (0, positive, or below -127)
					if (assoc.rssi == 0 || assoc.rssi > 0 || assoc.rssi < -127)
						continue;
					std::string mac = NormalizeMac(assoc.station);
					clientRssi[mac].push_back(RSSISample{rec.timestamp, assoc.rssi});
				}
			}
		}

		std::vector<AnalyticsObjects::MCPClientRssiItem> items;

		for (const auto &[mac, samples] : clientRssi) {
			if (samples.empty())
				continue;
			uint64_t excellent = 0, good = 0, fair = 0, poor = 0;
			for (const auto &s : samples) {
				if (s.rssi >= -55)
					excellent++;
				else if (s.rssi >= -67)
					good++;
				else if (s.rssi >= -75)
					fair++;
				else
					poor++;
			}

			double total = static_cast<double>(samples.size());
			AnalyticsObjects::MCPClientRssiItem item;
			item.mac = mac;
			item.rssi_excellent_pct = std::round((static_cast<double>(excellent) / total * 100.0) * 100.0) / 100.0;
			item.rssi_good_pct = std::round((static_cast<double>(good) / total * 100.0) * 100.0) / 100.0;
			item.rssi_fair_pct = std::round((static_cast<double>(fair) / total * 100.0) * 100.0) / 100.0;
			item.rssi_poor_pct = std::round((static_cast<double>(poor) / total * 100.0) * 100.0) / 100.0;
			item.rssi_total_samples = samples.size();

			items.push_back(item);
		}

		std::sort(items.begin(), items.end(),
				  [](const AnalyticsObjects::MCPClientRssiItem &a,
					 const AnalyticsObjects::MCPClientRssiItem &b) { return a.mac < b.mac; });

		AnalyticsObjects::MCPClientRssiSummary resp;
		resp.meta.requestedWindow.startTime = MCP::FormatRFC3339UTC(req.startTimeEpoch);
		resp.meta.requestedWindow.endTime = MCP::FormatRFC3339UTC(req.endTimeEpoch);

		resp.data.totalClients = items.size();
		resp.data.truncated = resp.data.totalClients > 500;

		if (items.size() > 500) {
			items.resize(500);
		}
		resp.data.items = items;

		// Calculate observedWindow strictly from samples contributing to truncated items
		std::optional<uint64_t> earliestTime;
		std::optional<uint64_t> latestTime;
		for (const auto &item : resp.data.items) {
			auto it = clientRssi.find(item.mac);
			if (it != clientRssi.end()) {
				for (const auto &s : it->second) {
					if (!earliestTime.has_value() || s.timestamp < earliestTime.value()) {
						earliestTime = s.timestamp;
					}
					if (!latestTime.has_value() || s.timestamp > latestTime.value()) {
						latestTime = s.timestamp;
					}
				}
			}
		}

		if (earliestTime.has_value()) {
			resp.meta.observedWindow.startTime = MCP::FormatRFC3339UTC(earliestTime.value());
			resp.meta.observedWindow.endTime = MCP::FormatRFC3339UTC(latestTime.value());
		}

		ReturnObject(resp);
	}

	// 5. Availability Summary Handler
	void RESTAPI_device_availability_summary_handler::DoGet() {
		MCP::MCPValidatedRequest req;
		if (!MCP::ValidateMCPRequest(this, MCP::MCPDomainCutoverType::Availability, req)) {
			return;
		}

		std::optional<uint64_t> earliestTime;
		std::optional<uint64_t> latestTime;

		uint64_t offlineCount = StorageService()->AvailabilityEventsDB().GetOfflineEvents(
			req.resolvedBoardId, req.routerId, req.startTimeEpoch, req.endTimeEpoch, earliestTime, latestTime);

		AnalyticsObjects::MCPAvailabilitySummary resp;
		resp.meta.requestedWindow.startTime = MCP::FormatRFC3339UTC(req.startTimeEpoch);
		resp.meta.requestedWindow.endTime = MCP::FormatRFC3339UTC(req.endTimeEpoch);

		if (offlineCount > 0 && earliestTime.has_value()) {
			resp.meta.observedWindow.startTime = MCP::FormatRFC3339UTC(earliestTime.value());
			resp.meta.observedWindow.endTime = MCP::FormatRFC3339UTC(latestTime.value());
		}

		resp.meta.offlineEventCount = offlineCount;
		resp.data.fetch_status = "success";
		resp.data.gw_uuid = req.routerId;
		resp.data.offline_count = offlineCount;

		ReturnObject(resp);
	}

} // namespace OpenWifi
