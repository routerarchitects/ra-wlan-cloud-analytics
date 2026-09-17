#include "APStats.h"
#include "RESTAPI/RESTAPI_mcp_helpers.h"

#include <Poco/JSON/Parser.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
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

	AnalyticsObjects::RadioTimePoint Radio(uint64_t Band, std::optional<double> Temperature,
										   uint64_t Channel = 0) {
		AnalyticsObjects::RadioTimePoint R;
		R.band = Band;
		R.temperature = Temperature;
		R.channel = Channel;
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

	AnalyticsObjects::RadioTimePoint PersistedRadioFromJson(Poco::JSON::Parser &Parser,
															const std::string &Json) {
		auto Obj = Parser.parse(Json).extract<Poco::JSON::Object::Ptr>();
		AnalyticsObjects::RadioTimePoint RTP;
		assert(RTP.from_json(Obj));
		return RTP;
	}

	void TestAggregatesByBand() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, 62), Radio(5, 56)}),
			 Point(1300, {Radio(2, 70), Radio(5, 65)}),
			 Point(1200, {Radio(2, 68), Radio(5, 60)})},
			TestWindow());

		assert(Summary.meta.requestedWindow.startTime == "1970-01-01T00:16:40Z");
		assert(Summary.meta.requestedWindow.endTime == "1970-01-01T00:33:20Z");
		assert(Summary.meta.observedWindow.startTime == "1970-01-01T00:18:20Z");
		assert(Summary.meta.observedWindow.endTime == "1970-01-01T00:21:40Z");
		assert(Summary.data.min_wifi_temp_2_4G == 62);
		assert(Summary.data.max_wifi_temp_2_4G == 70);
		assert(NearlyEqual(*Summary.data.avg_wifi_temp_2_4G, 200.0 / 3.0));
		assert(Summary.data.latest_wifi_temp_2_4G == 70);
		assert(Summary.data.min_wifi_temp_5G == 56);
		assert(Summary.data.max_wifi_temp_5G == 65);
		assert(NearlyEqual(*Summary.data.avg_wifi_temp_5G, 181.0 / 3.0));
		assert(Summary.data.latest_wifi_temp_5G == 65);
	}

	void TestFiltersInvalidSamples() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(900, {Radio(2, 50)}),
			 Point(1000, {Radio(2, std::nullopt), Radio(5, 255)}),
			 Point(1100, {Radio(2, -41), Radio(5, 126)}),
			 Point(1200, {Radio(2, 0), Radio(5, 0)}),
			 Point(1300, {Radio(6, 44), Radio(5, 10)}),
			 Point(2000, {Radio(2, 55)})},
			TestWindow());

		assert(Summary.data.min_wifi_temp_2_4G == 0);
		assert(Summary.data.max_wifi_temp_2_4G == 0);
		assert(Summary.data.avg_wifi_temp_2_4G == 0);
		assert(Summary.data.latest_wifi_temp_2_4G == 0);
		assert(Summary.data.min_wifi_temp_5G == 0);
		assert(Summary.data.max_wifi_temp_5G == 10);
		assert(Summary.data.avg_wifi_temp_5G == 5);
		assert(Summary.data.latest_wifi_temp_5G == 10);
		assert(Summary.meta.observedWindow.startTime == "1970-01-01T00:20:00Z");
		assert(Summary.meta.observedWindow.endTime == "1970-01-01T00:21:40Z");
	}

	void TestNoSamplesReturnsNulls() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, std::nullopt), Radio(5, 255)})}, TestWindow());

		assert(!Summary.data.min_wifi_temp_2_4G);
		assert(!Summary.data.max_wifi_temp_2_4G);
		assert(!Summary.data.avg_wifi_temp_2_4G);
		assert(!Summary.data.latest_wifi_temp_2_4G);
		assert(!Summary.data.min_wifi_temp_5G);
		assert(!Summary.data.max_wifi_temp_5G);
		assert(!Summary.data.avg_wifi_temp_5G);
		assert(!Summary.data.latest_wifi_temp_5G);
		assert(!Summary.meta.observedWindow.startTime);
		assert(!Summary.meta.observedWindow.endTime);
	}

	void TestSerializationShape() {
		auto Summary =
			MCP::CalculateRadioTemperatureSummary({Point(1100, {Radio(2, 62)})}, TestWindow());

		Poco::JSON::Object Obj;
		Summary.to_json(Obj);
		assert(Obj.has("data"));
		assert(Obj.has("meta"));
		assert(!Obj.has("requestedWindow"));
		assert(!Obj.has("observedWindow"));
		assert(!Obj.has("min_wifi_temp_2.4G"));

		auto DataObj = Obj.get("data").extract<Poco::JSON::Object>();
		assert(DataObj.has("min_wifi_temp_2.4G"));
		assert(DataObj.has("max_wifi_temp_2.4G"));
		assert(DataObj.has("avg_wifi_temp_2.4G"));
		assert(DataObj.has("latest_wifi_temp_2.4G"));
		assert(DataObj.has("min_wifi_temp_5G"));
		assert(DataObj.has("max_wifi_temp_5G"));
		assert(DataObj.has("avg_wifi_temp_5G"));
		assert(DataObj.has("latest_wifi_temp_5G"));

		auto MetaObj = Obj.get("meta").extract<Poco::JSON::Object>();
		assert(MetaObj.has("requestedWindow"));
		assert(MetaObj.has("observedWindow"));
	}

	void TestOnlyOneBand() {
		auto Summary2G = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, 62)})}, TestWindow());
		assert(Summary2G.data.min_wifi_temp_2_4G == 62);
		assert(Summary2G.data.max_wifi_temp_2_4G == 62);
		assert(Summary2G.data.avg_wifi_temp_2_4G == 62);
		assert(Summary2G.data.latest_wifi_temp_2_4G == 62);
		assert(!Summary2G.data.min_wifi_temp_5G);
		assert(!Summary2G.data.max_wifi_temp_5G);
		assert(!Summary2G.data.avg_wifi_temp_5G);
		assert(!Summary2G.data.latest_wifi_temp_5G);

		auto Summary5G = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(5, 55)})}, TestWindow());
		assert(!Summary5G.data.min_wifi_temp_2_4G);
		assert(!Summary5G.data.max_wifi_temp_2_4G);
		assert(!Summary5G.data.avg_wifi_temp_2_4G);
		assert(!Summary5G.data.latest_wifi_temp_2_4G);
		assert(Summary5G.data.min_wifi_temp_5G == 55);
		assert(Summary5G.data.max_wifi_temp_5G == 55);
		assert(Summary5G.data.avg_wifi_temp_5G == 55);
		assert(Summary5G.data.latest_wifi_temp_5G == 55);
	}

	void TestHalfOpenWindowBoundary() {
		auto W = TestWindow(); // startTime=1000, endTime=2000
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1000, {Radio(2, 40)}), // exactly on startTime boundary
			 Point(1500, {Radio(2, 50)}),
			 Point(2000, {Radio(2, 60)})}, // on endTime boundary (exclusive, should be excluded)
			W);

		assert(Summary.data.min_wifi_temp_2_4G == 40);
		assert(Summary.data.max_wifi_temp_2_4G == 50);
		assert(Summary.data.avg_wifi_temp_2_4G == 45);
		assert(Summary.data.latest_wifi_temp_2_4G == 50);
		assert(Summary.meta.observedWindow.startTime == "1970-01-01T00:16:40Z");
		assert(Summary.meta.observedWindow.endTime == "1970-01-01T00:25:00Z");
	}

	void TestZeroTemperatureIsValid() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, 0), Radio(5, 0)}),
			 Point(1200, {Radio(2, 10), Radio(5, 10)}),
			 Point(1300, {Radio(2, 20), Radio(5, 20)})},
			TestWindow());

		assert(Summary.data.min_wifi_temp_2_4G == 0);
		assert(Summary.data.max_wifi_temp_2_4G == 20);
		assert(Summary.data.avg_wifi_temp_2_4G == 10);
		assert(Summary.data.latest_wifi_temp_2_4G == 20);
		assert(Summary.data.min_wifi_temp_5G == 0);
		assert(Summary.data.max_wifi_temp_5G == 20);
		assert(Summary.data.avg_wifi_temp_5G == 10);
		assert(Summary.data.latest_wifi_temp_5G == 20);
	}

	void TestLatestTemperatureCanBeZero() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, 10)}),
			 Point(1200, {Radio(2, 20)}),
			 Point(1300, {Radio(2, 0)})},
			TestWindow());

		assert(Summary.data.min_wifi_temp_2_4G == 0);
		assert(Summary.data.max_wifi_temp_2_4G == 20);
		assert(Summary.data.avg_wifi_temp_2_4G == 10);
		assert(Summary.data.latest_wifi_temp_2_4G == 0);
	}

	void TestTelemetryJsonParsingToRadioTimePoint() {
		AnalyticsObjects::DeviceInfo Device;
		Device.deviceType = "ap-model-x";
		Device.platform = "platform-y";
		Device.lastFirmware = "v2.1.0-beta";

		nlohmann::json Doc1 = nlohmann::json::parse(R"({
			"band": ["5G"],
			"channel": 36,
			"temperature": 0
		})");
		AnalyticsObjects::RadioTimePoint RTP1;
		APStats::ParseRadioTimePoint(Doc1, Device, RTP1);
		assert(RTP1.temperature.has_value());
		assert(NearlyEqual(*RTP1.temperature, 0.0));

		nlohmann::json Doc2 = nlohmann::json::parse(R"({
			"band": ["2G"],
			"channel": 6,
			"temperature": 42.0
		})");
		AnalyticsObjects::RadioTimePoint RTP2;
		APStats::ParseRadioTimePoint(Doc2, Device, RTP2);
		assert(RTP2.temperature.has_value());
		assert(*RTP2.temperature == 42.0);

		nlohmann::json Doc3 = nlohmann::json::parse(R"({
			"band": ["5G"],
			"channel": 149,
			"temperature": null
		})");
		AnalyticsObjects::RadioTimePoint RTP3;
		APStats::ParseRadioTimePoint(Doc3, Device, RTP3);
		assert(!RTP3.temperature.has_value());

		nlohmann::json Doc4 = nlohmann::json::parse(R"({
			"band": ["5G"],
			"channel": 149
		})");
		AnalyticsObjects::RadioTimePoint RTP4;
		APStats::ParseRadioTimePoint(Doc4, Device, RTP4);
		assert(!RTP4.temperature.has_value());
	}

	void TestPersistedRadioJsonNullableTemperature() {
		Poco::JSON::Parser Parser;

		auto IntegerRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":20})");
		assert(IntegerRTP.temperature.has_value());
		assert(NearlyEqual(*IntegerRTP.temperature, 20.0));

		auto FloatingPointRTP =
			PersistedRadioFromJson(Parser, R"({"band":2,"temperature":20.5})");
		assert(FloatingPointRTP.temperature.has_value());
		assert(NearlyEqual(*FloatingPointRTP.temperature, 20.5));

		auto ZeroRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":0})");
		assert(ZeroRTP.temperature.has_value());
		assert(NearlyEqual(*ZeroRTP.temperature, 0.0));

		auto NegativeRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":-10})");
		assert(NegativeRTP.temperature.has_value());
		assert(NearlyEqual(*NegativeRTP.temperature, -10.0));

		auto StringRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":"20"})");
		assert(!StringRTP.temperature.has_value());

		auto StringZeroRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":"0"})");
		assert(!StringZeroRTP.temperature.has_value());

		auto TrueRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":true})");
		assert(!TrueRTP.temperature.has_value());

		auto FalseRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":false})");
		assert(!FalseRTP.temperature.has_value());

		auto NullRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":null})");
		assert(!NullRTP.temperature.has_value());

		auto MissingRTP = PersistedRadioFromJson(Parser, R"({"band":5})");
		assert(!MissingRTP.temperature.has_value());

		auto ObjectRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":{}})");
		assert(!ObjectRTP.temperature.has_value());

		auto ArrayRTP = PersistedRadioFromJson(Parser, R"({"band":2,"temperature":[]})");
		assert(!ArrayRTP.temperature.has_value());

		Poco::JSON::Object Serialized;
		ZeroRTP.to_json(Serialized);
		assert(Serialized.has("temperature"));
		assert(Serialized.getValue<double>("temperature") == 0);
	}

	void TestMalformedPersistedTemperatureTypesDoNotAggregate() {
		Poco::JSON::Parser Parser;
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {PersistedRadioFromJson(Parser, R"({"band":2,"temperature":10})")}),
			 Point(1200, {PersistedRadioFromJson(Parser, R"({"band":2,"temperature":"100"})")}),
			 Point(1300, {PersistedRadioFromJson(Parser, R"({"band":2,"temperature":true})")}),
			 Point(1400, {PersistedRadioFromJson(Parser, R"({"band":2,"temperature":20})")}),
			 Point(1500, {PersistedRadioFromJson(Parser, R"({"band":2,"temperature":0})")}),
			 Point(1600, {PersistedRadioFromJson(Parser, R"({"band":2,"temperature":"50"})")})},
			TestWindow());

		assert(Summary.data.min_wifi_temp_2_4G == 0);
		assert(Summary.data.max_wifi_temp_2_4G == 20);
		assert(Summary.data.avg_wifi_temp_2_4G == 10);
		assert(Summary.data.latest_wifi_temp_2_4G == 0);
		assert(Summary.meta.observedWindow.startTime == "1970-01-01T00:18:20Z");
		assert(Summary.meta.observedWindow.endTime == "1970-01-01T00:25:00Z");
	}

	void TestValidTwentyIsPreserved() {
		auto Summary = MCP::CalculateRadioTemperatureSummary(
			{Point(1100, {Radio(2, 20), Radio(5, 20)})}, TestWindow());

		assert(Summary.data.min_wifi_temp_2_4G == 20);
		assert(Summary.data.max_wifi_temp_2_4G == 20);
		assert(Summary.data.avg_wifi_temp_2_4G == 20);
		assert(Summary.data.latest_wifi_temp_2_4G == 20);
		assert(Summary.data.min_wifi_temp_5G == 20);
		assert(Summary.data.max_wifi_temp_5G == 20);
		assert(Summary.data.avg_wifi_temp_5G == 20);
		assert(Summary.data.latest_wifi_temp_5G == 20);
	}

	void TestValidateExpectedSampleCount() {
		MCP::Error E;
		auto WindowWithDuration = [](uint64_t Duration) {
			MCP::Window W;
			W.startTime = 1000;
			W.endTime = W.startTime + Duration;
			W.lookbackHours = 1;
			return W;
		};
		auto AssertSampleLimit = [&](uint64_t Duration, uint64_t Interval,
									 uint64_t ExpectedSamples) {
			auto W = WindowWithDuration(Duration);
			assert(MCP::EstimateExpectedSampleCount(W, Interval) == ExpectedSamples);
			assert(MCP::ValidateExpectedSampleCount(W, Interval, ExpectedSamples, E));
			if (ExpectedSamples > 1) {
				assert(!MCP::ValidateExpectedSampleCount(W, Interval, ExpectedSamples - 1, E));
				assert(E.status == Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
				assert(E.error == "exceeds_max_samples");
			}
		};

		AssertSampleLimit(3600, 60, 60);
		AssertSampleLimit(3599, 60, 60);
		AssertSampleLimit(3601, 60, 61);
		AssertSampleLimit(60, 60, 1);
		AssertSampleLimit(61, 60, 2);
		AssertSampleLimit(59, 60, 1);
		AssertSampleLimit(1, 60, 1);

		auto EmptyWindow = WindowWithDuration(0);
		assert(MCP::EstimateExpectedSampleCount(EmptyWindow, 60) == 0);
		assert(MCP::ValidateExpectedSampleCount(EmptyWindow, 60, 1, E));

		// Interval 0 falls back to the 60s effective interval.
		auto OneHourWindow = WindowWithDuration(3600);
		assert(MCP::ValidateExpectedSampleCount(OneHourWindow, 0, 60, E));
		assert(!MCP::ValidateExpectedSampleCount(OneHourWindow, 0, 59, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_BAD_REQUEST);
		assert(E.error == "exceeds_max_samples");
	}

	void TestValidateConfiguredMaxSamples() {
		MCP::Error E;
		uint64_t MaxSamples = 0;

		assert(MCP::ValidateConfiguredMaxSamples(1, MaxSamples, E));
		assert(MaxSamples == 1);
		assert(MCP::ValidateConfiguredMaxSamples(100000, MaxSamples, E));
		assert(MaxSamples == 100000);

		assert(!MCP::ValidateConfiguredMaxSamples(0, MaxSamples, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
		assert(E.error == "invalid_configuration");

		assert(!MCP::ValidateConfiguredMaxSamples(100001, MaxSamples, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
		assert(E.error == "invalid_configuration");

		assert(!MCP::ValidateConfiguredMaxSamples(std::numeric_limits<uint64_t>::max(),
												  MaxSamples, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
		assert(E.error == "invalid_configuration");
	}

} // namespace

int main() {
	TestAggregatesByBand();
	TestFiltersInvalidSamples();
	TestNoSamplesReturnsNulls();
	TestSerializationShape();
	TestOnlyOneBand();
	TestHalfOpenWindowBoundary();
	TestZeroTemperatureIsValid();
	TestLatestTemperatureCanBeZero();
	TestTelemetryJsonParsingToRadioTimePoint();
	TestPersistedRadioJsonNullableTemperature();
	TestMalformedPersistedTemperatureTypesDoNotAggregate();
	TestValidTwentyIsPreserved();
	TestValidateExpectedSampleCount();
	TestValidateConfiguredMaxSamples();
	std::cout << "test_mcp_radio_temperature_summary passed\n";
	return 0;
}
