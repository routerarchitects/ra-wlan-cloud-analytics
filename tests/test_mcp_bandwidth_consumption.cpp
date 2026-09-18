#include "RESTAPI/RESTAPI_mcp_helpers.h"

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <cassert>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace OpenWifi {
	const std::string &MicroServiceDataDirectory() {
		static const std::string DataDirectory = "/tmp";
		return DataDirectory;
	}

	std::string MicroServiceCreateUUID() {
		return "test-uuid";
	}
} // namespace OpenWifi

using namespace OpenWifi;

namespace {

	MCP::Window TestWindow() {
		MCP::Window W;
		W.startTime = 1000;
		W.endTime = 2000;
		W.lookbackHours = 1;
		return W;
	}

	AnalyticsObjects::UETimePoint Assoc(const std::string &Station, std::optional<uint64_t> Rx,
										std::optional<uint64_t> Tx) {
		AnalyticsObjects::UETimePoint UE;
		UE.station = Station;
		UE.rx_bytes_present = Rx.has_value();
		UE.rx_bytes = Rx.value_or(0);
		UE.tx_bytes_present = Tx.has_value();
		UE.tx_bytes = Tx.value_or(0);
		return UE;
	}

	AnalyticsObjects::UETimePoint Assoc(const std::string &Station, uint64_t Rx,
										uint64_t Tx) {
		return Assoc(Station, std::optional<uint64_t>(Rx), std::optional<uint64_t>(Tx));
	}

	AnalyticsObjects::DeviceTimePoint Point(
		uint64_t Timestamp, std::initializer_list<AnalyticsObjects::UETimePoint> Assocs,
		const std::string &BSSID = "aa:bb:cc:dd:ee:ff", const std::string &SSID = "main",
		uint64_t Band = 5, const std::string &Id = "") {
		AnalyticsObjects::DeviceTimePoint P;
		P.id = Id;
		P.timestamp = Timestamp;
		AnalyticsObjects::SSIDTimePoint S;
		S.bssid = BSSID;
		S.ssid = SSID;
		S.band = Band;
		S.associations = Assocs;
		P.ssid_data.push_back(std::move(S));
		return P;
	}

	const AnalyticsObjects::MCPClientUsageItem *FindItem(
		const AnalyticsObjects::MCPDeviceBandwidthConsumptionSummary &Summary,
		const std::string &Mac) {
		for (const auto &Item : Summary.data.items) {
			if (Item.mac == Mac)
				return &Item;
		}
		return nullptr;
	}

	void TestCumulativeDeltasAreNotSummed() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1000, {Assoc("E25195ED0F28", 1000, 500)}),
			 Point(1300, {Assoc("e2:51:95:ed:0f:28", 1600, 700)}),
			 Point(1600, {Assoc("e2-51-95-ed-0f-28", 2200, 900)})},
			TestWindow());

		assert(Summary.data.totalClients == 1);
		assert(!Summary.data.truncated);
		assert(Summary.data.items.size() == 1);
		const auto &Item = Summary.data.items[0];
		assert(Item.mac == "e2:51:95:ed:0f:28");
		assert(Item.rx_bytes == 1200);
		assert(Item.tx_bytes == 400);
		assert(Item.total_bytes == 1600);
		assert(Item.data_consume_rx == "0.00 MB");
		assert(Item.total_data_usage == "0.00 MB");
		assert(Summary.meta.observedWindow.startTime == "1970-01-01T00:16:40Z");
		assert(Summary.meta.observedWindow.endTime == "1970-01-01T00:26:40Z");
	}

	void TestObservedWindowIncludesBoundarySamples() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(900, {Assoc("28:39:26:a1:7c:a5", 100, 50)}),
			 Point(1200, {Assoc("28:39:26:a1:7c:a5", 400, 70)}),
			 Point(2100, {Assoc("28:39:26:a1:7c:a5", 900, 120)})},
			TestWindow());

		const auto *Item = FindItem(Summary, "28:39:26:a1:7c:a5");
		assert(Item != nullptr);
		assert(Item->rx_bytes == 800);
		assert(Item->tx_bytes == 70);
		assert(Summary.meta.observedWindow.startTime == "1970-01-01T00:15:00Z");
		assert(Summary.meta.observedWindow.endTime == "1970-01-01T00:35:00Z");
	}

	void TestAmbiguousCounterDecreaseBecomesNewBaseline() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1100, {Assoc("54:6c:0e:44:11:09", 1000, 1000)}),
			 Point(1200, {Assoc("54:6c:0e:44:11:09", 1500, 1200)}),
			 Point(1300, {Assoc("54:6c:0e:44:11:09", 100, 1300)}),
			 Point(1400, {Assoc("54:6c:0e:44:11:09", 300, 1500)})},
			TestWindow());

		const auto *Item = FindItem(Summary, "54:6c:0e:44:11:09");
		assert(Item != nullptr);
		assert(Item->rx_bytes == 700);
		assert(Item->tx_bytes == 500);
		assert(Item->total_bytes == 1200);
	}

	void TestSingleSampleReturnsClientWithZeroBytesAndNoObservedWindow() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1200, {Assoc("aa:bb:cc:dd:ee:01", 9999, 1000)})}, TestWindow());

		assert(Summary.data.totalClients == 1);
		assert(Summary.data.items.size() == 1);
		assert(Summary.data.items[0].rx_bytes == 0);
		assert(Summary.data.items[0].tx_bytes == 0);
		assert(!Summary.meta.observedWindow.startTime);
		assert(!Summary.meta.observedWindow.endTime);
	}

	void TestOutsideWindowOnlyClientIsExcluded() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(900, {Assoc("aa:bb:cc:dd:ee:02", 100, 100)}),
			 Point(2100, {Assoc("aa:bb:cc:dd:ee:02", 200, 200)})},
			TestWindow());

		assert(Summary.data.totalClients == 0);
		assert(Summary.data.items.empty());
	}

	void TestStreamChangesAreCalculatedIndependentlyAndAggregatedByMac() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1100, {Assoc("aa:bb:cc:dd:ee:03", 100, 100)}, "bssid-1", "main", 5),
			 Point(1200, {Assoc("aa:bb:cc:dd:ee:03", 250, 125)}, "bssid-1", "main", 5),
			 Point(1300, {Assoc("aa:bb:cc:dd:ee:03", 50, 20)}, "bssid-2", "main", 5),
			 Point(1400, {Assoc("aa:bb:cc:dd:ee:03", 90, 40)}, "bssid-2", "main", 5)},
			TestWindow());

		const auto *Item = FindItem(Summary, "aa:bb:cc:dd:ee:03");
		assert(Item != nullptr);
		assert(Item->rx_bytes == 190);
		assert(Item->tx_bytes == 45);
	}

	void TestSortingAndInvalidMacFiltering() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1100,
				   {Assoc("not-a-mac", 1, 1), Assoc("aa:bb:cc:dd:ee:04", 100, 100),
					Assoc("aa:bb:cc:dd:ee:05", 100, 100)}),
			 Point(1200,
				   {Assoc("aa:bb:cc:dd:ee:04", 150, 150),
					Assoc("aa:bb:cc:dd:ee:05", 300, 100)})},
			TestWindow());

		assert(Summary.data.totalClients == 2);
		assert(Summary.data.items.size() == 2);
		assert(Summary.data.items[0].mac == "aa:bb:cc:dd:ee:05");
		assert(Summary.data.items[1].mac == "aa:bb:cc:dd:ee:04");
	}

	void TestPreviousMissingDirectionDoesNotCreateTraffic() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1100,
				   {Assoc("aa:bb:cc:dd:ee:07", std::optional<uint64_t>(100),
						  std::nullopt)}),
			 Point(1200,
				   {Assoc("aa:bb:cc:dd:ee:07", std::optional<uint64_t>(300),
						  std::optional<uint64_t>(500))})},
			TestWindow());

		const auto *Item = FindItem(Summary, "aa:bb:cc:dd:ee:07");
		assert(Item != nullptr);
		assert(Item->rx_bytes == 200);
		assert(Item->tx_bytes == 0);
		assert(Item->total_bytes == 200);
	}

	void TestPreviousMissingRxDoesNotCreateTraffic() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1100,
				   {Assoc("aa:bb:cc:dd:ee:08", std::nullopt,
						  std::optional<uint64_t>(100))}),
			 Point(1200,
				   {Assoc("aa:bb:cc:dd:ee:08", std::optional<uint64_t>(500),
						  std::optional<uint64_t>(300))})},
			TestWindow());

		const auto *Item = FindItem(Summary, "aa:bb:cc:dd:ee:08");
		assert(Item != nullptr);
		assert(Item->rx_bytes == 0);
		assert(Item->tx_bytes == 200);
		assert(Item->total_bytes == 200);
	}

	void TestMissingCounterBetweenValidSamplesIsSkippedForThatDirection() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1100,
				   {Assoc("aa:bb:cc:dd:ee:09", std::optional<uint64_t>(100),
						  std::optional<uint64_t>(100))}),
			 Point(1200,
				   {Assoc("aa:bb:cc:dd:ee:09", std::nullopt,
						  std::optional<uint64_t>(200))}),
			 Point(1300,
				   {Assoc("aa:bb:cc:dd:ee:09", std::optional<uint64_t>(400),
						  std::optional<uint64_t>(300))})},
			TestWindow());

		const auto *Item = FindItem(Summary, "aa:bb:cc:dd:ee:09");
		assert(Item != nullptr);
		assert(Item->rx_bytes == 300);
		assert(Item->tx_bytes == 200);
		assert(Item->total_bytes == 500);
	}

	void TestSingleDirectionCountersAreCalculatedIndependently() {
		auto RxOnly = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1100,
				   {Assoc("aa:bb:cc:dd:ee:0a", std::optional<uint64_t>(100),
						  std::nullopt)}),
			 Point(1200,
				   {Assoc("aa:bb:cc:dd:ee:0a", std::optional<uint64_t>(600),
						  std::nullopt)})},
			TestWindow());
		const auto *RxOnlyItem = FindItem(RxOnly, "aa:bb:cc:dd:ee:0a");
		assert(RxOnlyItem != nullptr);
		assert(RxOnlyItem->rx_bytes == 500);
		assert(RxOnlyItem->tx_bytes == 0);

		auto TxOnly = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1100,
				   {Assoc("aa:bb:cc:dd:ee:0b", std::nullopt,
						  std::optional<uint64_t>(100))}),
			 Point(1200,
				   {Assoc("aa:bb:cc:dd:ee:0b", std::nullopt,
						  std::optional<uint64_t>(600))})},
			TestWindow());
		const auto *TxOnlyItem = FindItem(TxOnly, "aa:bb:cc:dd:ee:0b");
		assert(TxOnlyItem != nullptr);
		assert(TxOnlyItem->rx_bytes == 0);
		assert(TxOnlyItem->tx_bytes == 500);
	}

	void TestExplicitZeroCounterIsValid() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1100, {Assoc("aa:bb:cc:dd:ee:0c", 0, 0)}),
			 Point(1200, {Assoc("aa:bb:cc:dd:ee:0c", 500, 250)})},
			TestWindow());

		const auto *Item = FindItem(Summary, "aa:bb:cc:dd:ee:0c");
		assert(Item != nullptr);
		assert(Item->rx_bytes == 500);
		assert(Item->tx_bytes == 250);
		assert(Item->total_bytes == 750);
	}

	void TestSerializationShape() {
		auto Summary = MCP::CalculateBandwidthConsumptionSummary(
			{Point(1000, {Assoc("aa:bb:cc:dd:ee:06", 100, 100)}),
			 Point(1100, {Assoc("aa:bb:cc:dd:ee:06", 200, 150)})},
			TestWindow());

		Poco::JSON::Object Obj;
		Summary.to_json(Obj);

		std::stringstream ss;
		Obj.stringify(ss);
		Poco::JSON::Parser parser;
		auto ParsedObj = parser.parse(ss).extract<Poco::JSON::Object::Ptr>();

		assert(ParsedObj->has("data"));
		assert(ParsedObj->has("meta"));
		assert(!ParsedObj->has("items"));

		auto DataObj = ParsedObj->getObject("data");
		assert(DataObj->has("items"));
		assert(DataObj->has("totalClients"));
		assert(DataObj->has("truncated"));

		auto Items = DataObj->getArray("items");
		assert(Items->size() == 1);
		auto Item = Items->getObject(0);
		assert(Item->has("mac"));
		assert(Item->has("rx_bytes"));
		assert(Item->has("tx_bytes"));
		assert(Item->has("total_bytes"));
		assert(Item->has("data_consume_rx"));
		assert(Item->has("data_consume_tx"));
		assert(Item->has("total_data_usage"));
	}

} // namespace

int main() {
	TestCumulativeDeltasAreNotSummed();
	TestObservedWindowIncludesBoundarySamples();
	TestAmbiguousCounterDecreaseBecomesNewBaseline();
	TestSingleSampleReturnsClientWithZeroBytesAndNoObservedWindow();
	TestOutsideWindowOnlyClientIsExcluded();
	TestStreamChangesAreCalculatedIndependentlyAndAggregatedByMac();
	TestSortingAndInvalidMacFiltering();
	TestPreviousMissingDirectionDoesNotCreateTraffic();
	TestPreviousMissingRxDoesNotCreateTraffic();
	TestMissingCounterBetweenValidSamplesIsSkippedForThatDirection();
	TestSingleDirectionCountersAreCalculatedIndependently();
	TestExplicitZeroCounterIsValid();
	TestSerializationShape();

	std::cout << "test_mcp_bandwidth_consumption passed\n";
	return 0;
}
