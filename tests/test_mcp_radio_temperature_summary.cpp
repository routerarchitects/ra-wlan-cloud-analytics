#include "RESTAPI/RESTAPI_mcp_helpers.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <optional>
#include <vector>

using namespace OpenWifi;

namespace {

	MCP::Window TestWindow() {
		MCP::Window W;
		W.startTime = 1000;
		W.endTime = 2000;
		W.lookbackHours = 1;
		return W;
	}

	AnalyticsObjects::RadioTimePoint Radio(uint64_t Band, std::optional<double> Temperature,
										   bool ZeroUnavailable = false) {
		AnalyticsObjects::RadioTimePoint R;
		R.band = Band;
		R.wifi_temp = Temperature;
		R.wifi_temp_zero_is_unavailable = ZeroUnavailable;
		return R;
	}

	AnalyticsObjects::DeviceTimePoint Point(
		uint64_t Timestamp, std::initializer_list<AnalyticsObjects::RadioTimePoint> Radios) {
		AnalyticsObjects::DeviceTimePoint P;
		P.timestamp = Timestamp;
		P.radio_data = Radios;
		return P;
	}

	bool NearlyEqual(double Left, double Right) {
		return std::fabs(Left - Right) < 0.000001;
	}

	void TestAggregatesByBand() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, 62), Radio(5, 56)}),
			 Point(1300, {Radio(2, 70), Radio(5, 65)}),
			 Point(1200, {Radio(2, 68), Radio(5, 60)})},
			TestWindow(), 1000);

		assert(Summary.requestedWindow.startTime == "1970-01-01T00:16:40Z");
		assert(Summary.requestedWindow.endTime == "1970-01-01T00:33:20Z");
		assert(Summary.observedWindow.startTime == "1970-01-01T00:18:20Z");
		assert(Summary.observedWindow.endTime == "1970-01-01T00:21:40Z");
		assert(Summary.min_wifi_temp_2_4G == 62);
		assert(Summary.max_wifi_temp_2_4G == 70);
		assert(NearlyEqual(*Summary.avg_wifi_temp_2_4G, 200.0 / 3.0));
		assert(Summary.latest_wifi_temp_2_4G == 70);
		assert(Summary.min_wifi_temp_5G == 56);
		assert(Summary.max_wifi_temp_5G == 65);
		assert(NearlyEqual(*Summary.avg_wifi_temp_5G, 181.0 / 3.0));
		assert(Summary.latest_wifi_temp_5G == 65);
	}

	void TestFiltersInvalidSamples() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(900, {Radio(2, 50)}),
			 Point(1000, {Radio(2, std::nullopt), Radio(5, 255)}),
			 Point(1100, {Radio(2, -41), Radio(5, 126)}),
			 Point(1200, {Radio(2, 0, true), Radio(5, 0, false)}),
			 Point(1300, {Radio(6, 44), Radio(5, 10)}),
			 Point(2000, {Radio(2, 55)})},
			TestWindow(), 1000);

		assert(!Summary.min_wifi_temp_2_4G);
		assert(!Summary.max_wifi_temp_2_4G);
		assert(!Summary.avg_wifi_temp_2_4G);
		assert(!Summary.latest_wifi_temp_2_4G);
		assert(Summary.min_wifi_temp_5G == 0);
		assert(Summary.max_wifi_temp_5G == 10);
		assert(Summary.avg_wifi_temp_5G == 5);
		assert(Summary.latest_wifi_temp_5G == 10);
		assert(Summary.observedWindow.startTime == "1970-01-01T00:20:00Z");
		assert(Summary.observedWindow.endTime == "1970-01-01T00:21:40Z");
	}

	void TestNoSamplesReturnsNulls() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, std::nullopt), Radio(5, 255)})}, TestWindow(), 1000);

		assert(!Summary.min_wifi_temp_2_4G);
		assert(!Summary.max_wifi_temp_2_4G);
		assert(!Summary.avg_wifi_temp_2_4G);
		assert(!Summary.latest_wifi_temp_2_4G);
		assert(!Summary.min_wifi_temp_5G);
		assert(!Summary.max_wifi_temp_5G);
		assert(!Summary.avg_wifi_temp_5G);
		assert(!Summary.latest_wifi_temp_5G);
		assert(!Summary.observedWindow.startTime);
		assert(!Summary.observedWindow.endTime);
	}

	void TestSerializationShape() {
		auto Summary =
			MCP::CalculateRadioTemperatureSummary({Point(1100, {Radio(2, 62)})}, TestWindow(),
												  1000);

		Poco::JSON::Object Obj;
		Summary.to_json(Obj);
		assert(Obj.has("requestedWindow"));
		assert(Obj.has("observedWindow"));
		assert(Obj.has("min_wifi_temp_2.4G"));
		assert(Obj.has("max_wifi_temp_2.4G"));
		assert(Obj.has("avg_wifi_temp_2.4G"));
		assert(Obj.has("latest_wifi_temp_2.4G"));
		assert(Obj.has("min_wifi_temp_5G"));
		assert(Obj.has("max_wifi_temp_5G"));
		assert(Obj.has("avg_wifi_temp_5G"));
		assert(Obj.has("latest_wifi_temp_5G"));
		assert(!Obj.has("data"));
		assert(!Obj.has("meta"));
	}

	void TestCutoverValidation() {
		MCP::Error E;
		auto W = TestWindow();
		assert(MCP::ValidateTemperatureCutover(W, 1000, E));
		assert(!MCP::ValidateTemperatureCutover(W, 1001, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
		assert(E.error == "temperature_range_before_cutover");
	}

	void TestRFC3339CutoverParsing() {
		uint64_t Epoch = 0;
		assert(MCP::ParseRFC3339Timestamp("2026-07-01T00:00:00Z", Epoch));
		assert(Epoch == 1782864000);
		assert(MCP::ParseRFC3339Timestamp("2026-07-01T05:30:00+05:30", Epoch));
		assert(Epoch == 1782864000);
		assert(MCP::ParseRFC3339Timestamp("2026-06-30T20:00:00-04:00", Epoch));
		assert(Epoch == 1782864000);
		assert(!MCP::ParseRFC3339Timestamp("invalid-date-string", Epoch));
		assert(!MCP::ParseRFC3339Timestamp("2026-07-01T00:00:00+25:00", Epoch));
	}

	void TestOnlyOneBand() {
		auto Summary2G = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, 62)})}, TestWindow(), 1000);
		assert(Summary2G.min_wifi_temp_2_4G == 62);
		assert(Summary2G.max_wifi_temp_2_4G == 62);
		assert(Summary2G.avg_wifi_temp_2_4G == 62);
		assert(Summary2G.latest_wifi_temp_2_4G == 62);
		assert(!Summary2G.min_wifi_temp_5G);
		assert(!Summary2G.max_wifi_temp_5G);
		assert(!Summary2G.avg_wifi_temp_5G);
		assert(!Summary2G.latest_wifi_temp_5G);

		auto Summary5G = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(5, 55)})}, TestWindow(), 1000);
		assert(!Summary5G.min_wifi_temp_2_4G);
		assert(!Summary5G.max_wifi_temp_2_4G);
		assert(!Summary5G.avg_wifi_temp_2_4G);
		assert(!Summary5G.latest_wifi_temp_2_4G);
		assert(Summary5G.min_wifi_temp_5G == 55);
		assert(Summary5G.max_wifi_temp_5G == 55);
		assert(Summary5G.avg_wifi_temp_5G == 55);
		assert(Summary5G.latest_wifi_temp_5G == 55);
	}

	void TestHalfOpenWindowAndCutoverBoundary() {
		auto W = TestWindow(); // startTime=1000, endTime=2000
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1000, {Radio(2, 40)}), // exactly on startTime boundary & cutover boundary
			 Point(1500, {Radio(2, 50)}),
			 Point(2000, {Radio(2, 60)})}, // on endTime boundary (exclusive, should be excluded)
			W, 1000); // CutoverTime=1000

		assert(Summary.min_wifi_temp_2_4G == 40);
		assert(Summary.max_wifi_temp_2_4G == 50);
		assert(Summary.avg_wifi_temp_2_4G == 45);
		assert(Summary.latest_wifi_temp_2_4G == 50);
		assert(Summary.observedWindow.startTime == "1970-01-01T00:16:40Z");
		assert(Summary.observedWindow.endTime == "1970-01-01T00:25:00Z");
	}

	void TestZeroSentinelFlag() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, 0, false)}), // 0 is valid measurement when flag is false
			 Point(1200, {Radio(5, 0, true)})}, // 0 is sentinel when flag is true
			TestWindow(), 1000);

		assert(Summary.min_wifi_temp_2_4G == 0);
		assert(Summary.max_wifi_temp_2_4G == 0);
		assert(Summary.avg_wifi_temp_2_4G == 0);
		assert(Summary.latest_wifi_temp_2_4G == 0);
		assert(!Summary.min_wifi_temp_5G);
		assert(!Summary.max_wifi_temp_5G);
		assert(!Summary.avg_wifi_temp_5G);
		assert(!Summary.latest_wifi_temp_5G);
	}

	void TestTelemetryJsonParsingToRadioTimePoint() {
		nlohmann::json RadioDoc = nlohmann::json::parse(R"({
			"band": ["5G"],
			"channel": 36,
			"temperature": 54.5,
			"wifi_temp_zero_is_unavailable": true
		})");

		AnalyticsObjects::RadioTimePoint RTP;
		if (RadioDoc.contains("temperature") && !RadioDoc["temperature"].is_null()) {
			RTP.wifi_temp = RadioDoc["temperature"].get<double>();
		}
		if (RadioDoc.contains("wifi_temp_zero_is_unavailable")) {
			RTP.wifi_temp_zero_is_unavailable = RadioDoc["wifi_temp_zero_is_unavailable"].get<bool>();
		}

		assert(RTP.wifi_temp.has_value());
		assert(*RTP.wifi_temp == 54.5);
		assert(RTP.wifi_temp_zero_is_unavailable == true);
	}

} // namespace

int main() {
	TestAggregatesByBand();
	TestFiltersInvalidSamples();
	TestNoSamplesReturnsNulls();
	TestSerializationShape();
	TestCutoverValidation();
	TestRFC3339CutoverParsing();
	TestOnlyOneBand();
	TestHalfOpenWindowAndCutoverBoundary();
	TestZeroSentinelFlag();
	TestTelemetryJsonParsingToRadioTimePoint();
	std::cout << "test_mcp_radio_temperature_summary passed\n";
	return 0;
}
