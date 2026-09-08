#include "RESTAPI/RESTAPI_mcp_helpers.h"
#include "RouterIdResolver.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

using namespace OpenWifi;

namespace {

	AnalyticsObjects::DeviceTimePoint Point(uint64_t Timestamp, std::optional<uint64_t> Free,
											std::optional<uint64_t> Total = std::nullopt,
											std::string Id = "") {
		AnalyticsObjects::DeviceTimePoint P;
		P.id = std::move(Id);
		P.timestamp = Timestamp;
		P.resource_data.memory_free = Free;
		P.resource_data.memory_total = Total;
		return P;
	}

	MCP::Window TestWindow() {
		MCP::Window W;
		W.startTime = 1000;
		W.endTime = 2000;
		W.lookbackHours = 1;
		return W;
	}

	Poco::URI::QueryParameters Params(const std::string &Query) {
		Poco::URI URI("http://example.test/?" + Query);
		return URI.getQueryParameters();
	}

	AnalyticsObjects::BoardInfo Board(const std::string &BoardId, const std::string &VenueId) {
		AnalyticsObjects::BoardInfo B;
		B.info.id = BoardId;
		AnalyticsObjects::VenueInfo V;
		V.id = VenueId;
		V.retention = 86400;
		B.venueList.push_back(V);
		return B;
	}

	void TestMultipleValidSamples() {
		auto Summary = MCP::CalculateMemorySummary(
			{Point(100, 100, 1000), Point(300, 300, 1000), Point(200, 200, 1000)},
			TestWindow());
		assert(Summary.data.min_memfree == 100);
		assert(Summary.data.max_memfree == 300);
		assert(Summary.data.avg_memfree == 200);
		assert(Summary.data.latest_memfree == 300);
		assert(Summary.meta.observedWindow.startTime == "1970-01-01T00:01:40Z");
		assert(Summary.meta.observedWindow.endTime == "1970-01-01T00:05:00Z");
	}

	void TestSingleAndNoSampleCases() {
		auto Single = MCP::CalculateMemorySummary({Point(250, 250)}, TestWindow());
		assert(Single.data.min_memfree == 250);
		assert(Single.data.max_memfree == 250);
		assert(Single.data.avg_memfree == 250);
		assert(Single.data.latest_memfree == 250);

		auto Empty = MCP::CalculateMemorySummary({}, TestWindow());
		assert(!Empty.data.min_memfree);
		assert(!Empty.data.max_memfree);
		assert(!Empty.data.avg_memfree);
		assert(!Empty.data.latest_memfree);
		assert(!Empty.meta.observedWindow.startTime);
		assert(!Empty.meta.observedWindow.endTime);
	}

	void TestContractSerialization() {
		auto Summary = MCP::CalculateMemorySummary({Point(250, 250)}, TestWindow());
		Poco::JSON::Object Obj;
		Summary.to_json(Obj);
		assert(Obj.size() == 2);
		assert(Obj.has("data"));
		assert(Obj.has("meta"));
		assert(!Obj.has("min_memfree"));
		assert(!Obj.has("max_memfree"));
		assert(!Obj.has("avg_memfree"));
		assert(!Obj.has("latest_memfree"));
		assert(!Obj.has("requestedWindow"));
		assert(!Obj.has("observedWindow"));

		auto DataObj = Obj.get("data").extract<Poco::JSON::Object>();
		auto MetaObj = Obj.get("meta").extract<Poco::JSON::Object>();
		assert(DataObj.has("min_memfree"));
		assert(DataObj.has("max_memfree"));
		assert(DataObj.has("avg_memfree"));
		assert(DataObj.has("latest_memfree"));
		assert(MetaObj.has("requestedWindow"));
		assert(MetaObj.has("observedWindow"));
	}

	void TestInvalidSamplesAreIgnored() {
		auto Summary = MCP::CalculateMemorySummary(
				{Point(100, std::nullopt, 1000), Point(200, 500, 400), Point(300, 500),
				 Point(400, 600, 1000)},
				TestWindow());
		assert(Summary.data.min_memfree == 500);
		assert(Summary.data.max_memfree == 600);
		assert(Summary.data.avg_memfree == 550);
		assert(Summary.data.latest_memfree == 600);
		assert(Summary.meta.observedWindow.startTime == "1970-01-01T00:05:00Z");
		assert(Summary.meta.observedWindow.endTime == "1970-01-01T00:06:40Z");
	}

	void TestAverageRounding() {
		auto Whole = MCP::CalculateMemorySummary({Point(100, 100), Point(200, 200)},
												 TestWindow());
		assert(Whole.data.avg_memfree == 150);

		auto Half = MCP::CalculateMemorySummary({Point(100, 100), Point(101, 101)}, TestWindow());
		assert(Half.data.avg_memfree == 101);

		auto FractionalDown =
			MCP::CalculateMemorySummary({Point(100, 100), Point(101, 100)}, TestWindow());
		assert(FractionalDown.data.avg_memfree == 100);

		auto Single = MCP::CalculateMemorySummary({Point(100, 123)}, TestWindow());
		assert(Single.data.avg_memfree == 123);

		auto Max = std::numeric_limits<uint64_t>::max();
		auto Large = MCP::CalculateMemorySummary(
			{Point(100, Max, Max), Point(101, Max - 2, Max)}, TestWindow());
		assert(Large.data.avg_memfree == Max - 1);
	}

	void TestLatestMemorySelection() {
		auto One = MCP::CalculateMemorySummary({Point(100, 100)}, TestWindow());
		assert(One.data.latest_memfree == 100);

		auto Multiple = MCP::CalculateMemorySummary(
			{Point(100, 100), Point(200, 200), Point(300, 300)}, TestWindow());
		assert(Multiple.data.latest_memfree == 300);

		auto OutOfOrder = MCP::CalculateMemorySummary(
			{Point(300, 300), Point(100, 100), Point(200, 200)}, TestWindow());
		assert(OutOfOrder.data.latest_memfree == 300);
		assert(OutOfOrder.meta.observedWindow.startTime == "1970-01-01T00:01:40Z");
		assert(OutOfOrder.meta.observedWindow.endTime == "1970-01-01T00:05:00Z");

		auto LatestAtRangeEnd =
			MCP::CalculateMemorySummary({Point(1000, 100), Point(1999, 999)}, TestWindow());
		assert(LatestAtRangeEnd.data.latest_memfree == 999);

		std::vector<AnalyticsObjects::DeviceTimePoint> BoundaryRecords{
			Point(1000, 100), Point(1999, 999), Point(2000, 2000)};
		std::vector<AnalyticsObjects::DeviceTimePoint> Filtered;
		for (const auto &Record : BoundaryRecords) {
			if (MCP::TimestampInHalfOpenWindow(Record.timestamp, TestWindow()))
				Filtered.push_back(Record);
		}
		auto EndExcluded = MCP::CalculateMemorySummary(Filtered, TestWindow());
		assert(EndExcluded.data.latest_memfree == 999);

		auto InvalidLatest =
			MCP::CalculateMemorySummary({Point(1900, 900, 800), Point(1800, 700, 1000)},
										TestWindow());
		assert(InvalidLatest.data.latest_memfree == 700);
		assert(InvalidLatest.meta.observedWindow.startTime == "1970-01-01T00:30:00Z");
		assert(InvalidLatest.meta.observedWindow.endTime == "1970-01-01T00:30:00Z");

		auto Tied = MCP::CalculateMemorySummary(
			{Point(500, 100, std::nullopt, "a"), Point(500, 200, std::nullopt, "b")},
			TestWindow());
		assert(Tied.data.latest_memfree == 200);

		auto Empty = MCP::CalculateMemorySummary({Point(100, std::nullopt)}, TestWindow());
		assert(!Empty.data.latest_memfree);
		assert(!Empty.meta.observedWindow.startTime);
		assert(!Empty.meta.observedWindow.endTime);
	}

	void TestRouterValidation() {
		MCP::Error E;
		assert(MCP::ValidateRouterId("dc6279652334", E));
		assert(MCP::ValidateRouterId("gw_1-A", E));
		assert(!MCP::ValidateRouterId("", E));
		assert(E.error == "invalid_router_id");
		assert(!MCP::ValidateRouterId("bad/router", E));
		assert(!MCP::ValidateRouterId(std::string(65, 'a'), E));
	}

	void TestBearerHeaderValidation() {
		MCP::Error E;
		std::string Token;
		assert(MCP::ExtractBearerToken(std::string("Bearer abc123"), Token, E));
		assert(Token == "abc123");
		assert(!MCP::ExtractBearerToken(std::nullopt, Token, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED);
		assert(E.error == "unauthorized");
		assert(!MCP::ExtractBearerToken(std::string("Basic abc123"), Token, E));
		assert(!MCP::ExtractBearerToken(std::string("Bearer "), Token, E));
		assert(!MCP::ExtractBearerToken(std::string("bearer abc123"), Token, E));
		assert(MCP::ValidateBearerAuthorization(
			std::string("Bearer valid"), [](const std::string &T) { return T == "valid"; }, E));
		assert(!MCP::ValidateBearerAuthorization(
			std::string("Bearer invalid"), [](const std::string &) { return false; }, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED);
		assert(E.error == "unauthorized");
		assert(!MCP::ValidateBearerAuthorization(
			std::nullopt, [](const std::string &) { return true; }, E));
	}

	void TestRouterResolutionHelpers() {
		RouterIdResolver::Result R;
		RouterIdResolver::Error E;
		assert(RouterIdResolver::ResolveBoardForVenue("venue-a", {Board("board-a", "venue-a")}, R,
													  E));
		assert(R.resolvedBoardId == "board-a");
		assert(R.resolvedVenueId == "venue-a");

		assert(!RouterIdResolver::ResolveBoardForVenue("venue-b", {Board("board-a", "venue-a")},
													   R, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
		assert(E.error == "not_found");

		assert(!RouterIdResolver::ResolveBoardForVenue(
			"venue-a", {Board("board-a", "venue-a"), Board("board-b", "venue-a")}, R, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_CONFLICT);
		assert(E.error == "multiple_boards");

		AnalyticsObjects::BoardInfo MultiVenueBoard = Board("board-c", "venue-a");
		AnalyticsObjects::VenueInfo ExtraVenue;
		ExtraVenue.id = "venue-extra";
		MultiVenueBoard.venueList.push_back(ExtraVenue);
		assert(!RouterIdResolver::ResolveBoardForVenue("venue-a", {MultiVenueBoard}, R, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_NOT_FOUND);

		assert(!RouterIdResolver::ClassifyProvisioningFailure(
			Poco::Net::HTTPResponse::HTTP_FORBIDDEN, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
		assert(E.error == "not_found");
		assert(E.message == "Router was not found");
		assert(!RouterIdResolver::ClassifyProvisioningFailure(
			Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
		assert(E.error == "not_found");
		assert(!RouterIdResolver::ClassifyProvisioningFailure(
			Poco::Net::HTTPResponse::HTTP_NOT_FOUND, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
		assert(E.error == "not_found");
		assert(!RouterIdResolver::ClassifyProvisioningFailure(
			Poco::Net::HTTPResponse::HTTP_GATEWAY_TIMEOUT, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
		assert(E.error == "owprov_unavailable");
		assert(E.message == "Upstream provisioning service is unavailable");
		assert(!RouterIdResolver::ClassifyProvisioningFailure(
			Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
		assert(E.error == "owprov_unavailable");

		assert(!RouterIdResolver::AnalyticsBoardStorageFailure(E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
		assert(E.error == "internal_error");
		assert(E.message == "Internal error");
	}

	void TestDeviceFetchResultClassification() {
		RouterIdResolver::Error E;
		assert(RouterIdResolver::ClassifyDeviceFetchResult(
			SDK::Prov::Device::FetchResult::Success, Poco::Net::HTTPResponse::HTTP_OK, E));

		assert(!RouterIdResolver::ClassifyDeviceFetchResult(
			SDK::Prov::Device::FetchResult::HttpFailure,
			Poco::Net::HTTPResponse::HTTP_FORBIDDEN, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
		assert(E.error == "not_found");

		assert(!RouterIdResolver::ClassifyDeviceFetchResult(
			SDK::Prov::Device::FetchResult::HttpFailure,
			Poco::Net::HTTPResponse::HTTP_GATEWAY_TIMEOUT, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
		assert(E.error == "owprov_unavailable");

		assert(!RouterIdResolver::ClassifyDeviceFetchResult(
			SDK::Prov::Device::FetchResult::InvalidResponse,
			Poco::Net::HTTPResponse::HTTP_OK, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
		assert(E.error == "owprov_invalid_response");
		assert(E.message == "Upstream provisioning service returned an invalid response");
	}

	void TestDeviceFetchOutcomeClassification() {
		assert(SDK::Prov::Device::ClassifyFetchOutcome(
				   Poco::Net::HTTPResponse::HTTP_OK, true) ==
			   SDK::Prov::Device::FetchResult::Success);
		assert(SDK::Prov::Device::ClassifyFetchOutcome(
				   Poco::Net::HTTPResponse::HTTP_OK, false) ==
			   SDK::Prov::Device::FetchResult::InvalidResponse);
		assert(SDK::Prov::Device::ClassifyFetchOutcome(
				   Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, false) ==
			   SDK::Prov::Device::FetchResult::HttpFailure);

		RouterIdResolver::Error E;
		auto Invalid200 = SDK::Prov::Device::ClassifyFetchOutcome(
			Poco::Net::HTTPResponse::HTTP_OK, false);
		assert(!RouterIdResolver::ClassifyDeviceFetchResult(
			Invalid200, Poco::Net::HTTPResponse::HTTP_OK, E));
		assert(E.status == Poco::Net::HTTPResponse::HTTP_BAD_GATEWAY);
		assert(E.error == "owprov_invalid_response");
		assert(E.message == "Upstream provisioning service returned an invalid response");
	}

	void TestTimestampValidation() {
		uint64_t Epoch = 0;
		assert(MCP::ParseTimestampTill("2026-08-21T06:25:00Z", Epoch));
		assert(!MCP::ParseTimestampTill("2026-08-21T06:25:00+05:30", Epoch));
		assert(!MCP::ParseTimestampTill("2026-08-21 06:25:00", Epoch));
		assert(!MCP::ParseTimestampTill("2026-02-31T10:00:00Z", Epoch));
		assert(!MCP::ParseTimestampTill("2025-02-29T10:00:00Z", Epoch));
	}

	void TestQueryValidation() {
		MCP::Error E;
		MCP::Window W;
		auto Now = static_cast<uint64_t>(2000000000);
		assert(MCP::ValidateWindowQuery(
			Params("timestampTill=2026-08-21T06:25:00Z&lookbackHours=24"), Now, 300, W, E));
		assert(W.endTime == 1787293500);
		assert(W.startTime == 1787207100);

		assert(!MCP::ValidateWindowQuery(Params("lookbackHours=24"), Now, 300, W, E));
		assert(E.error == "invalid_timestamp");
		assert(!MCP::ValidateWindowQuery(
			Params("timestampTill=2026-08-21T06:25:00Z&lookbackHours=24&foo=1"), Now, 300, W,
			E));
		assert(E.error == "invalid_query_parameter");
		assert(!MCP::ValidateWindowQuery(
			Params("timestampTill=2026-08-21T06:25:00Z&timestampTill=2026-08-21T06:25:00Z&lookbackHours=24"),
			Now, 300, W, E));
		assert(E.error == "invalid_timestamp");
		assert(!MCP::ValidateWindowQuery(
			Params("timestampTill=2026-08-21T06:25:00Z&lookbackHours=0"), Now, 300, W, E));
		assert(E.error == "invalid_lookback_hours");
		assert(!MCP::ValidateWindowQuery(
			Params("timestampTill=2026-08-21T06:25:00Z&lookbackHours=-1"), Now, 300, W, E));
		assert(E.error == "invalid_lookback_hours");
		assert(!MCP::ValidateWindowQuery(
			Params("timestampTill=2026-08-21T06:25:00Z&lookbackHours=1.5"), Now, 300, W, E));
		assert(E.error == "invalid_lookback_hours");
		assert(!MCP::ValidateWindowQuery(
			Params("timestampTill=2026-08-21T06:25:00Z&lookbackHours=999999999999999999999"),
			Now, 300, W, E));
		assert(E.error == "invalid_lookback_hours");
	}

	void TestFutureAndRetentionValidation() {
		MCP::Error E;
		MCP::Window W;
		assert(MCP::ValidateWindowQuery(
			Params("timestampTill=2033-05-18T03:33:50Z&lookbackHours=1"), 2000000000, 300, W,
			E));
		assert(!MCP::ValidateWindowQuery(
			Params("timestampTill=2033-05-18T03:40:01Z&lookbackHours=1"), 2000000000, 300, W,
			E));
		assert(E.error == "invalid_timestamp");

		MCP::Window Retained;
		Retained.startTime = 9000;
		Retained.endTime = 12600;
		Retained.lookbackHours = 1;
		assert(MCP::ValidateRetention(Retained, 7200, 10000, 3000, E));
		MCP::Window TooLong;
		TooLong.startTime = 0;
		TooLong.endTime = 10800;
		TooLong.lookbackHours = 3;
		assert(!MCP::ValidateRetention(TooLong, 7200, 10000, 300, E));
		assert(E.error == "invalid_lookback_hours");
		MCP::Window BeforeStart;
		BeforeStart.startTime = 2000;
		BeforeStart.endTime = 10600;
		BeforeStart.lookbackHours = 1;
		assert(!MCP::ValidateRetention(BeforeStart, 7200, 10000, 300, E));
		assert(E.error == "lookback_outside_retention");
	}

	void TestHalfOpenWindowBoundaries() {
		MCP::Window W;
		W.startTime = 100;
		W.endTime = 200;
		assert(MCP::TimestampInHalfOpenWindow(100, W));
		assert(!MCP::TimestampInHalfOpenWindow(99, W));
		assert(MCP::TimestampInHalfOpenWindow(199, W));
		assert(!MCP::TimestampInHalfOpenWindow(200, W));
	}

} // namespace

int main() {
	TestMultipleValidSamples();
	TestSingleAndNoSampleCases();
	TestContractSerialization();
	TestInvalidSamplesAreIgnored();
	TestAverageRounding();
	TestLatestMemorySelection();
	TestRouterValidation();
	TestBearerHeaderValidation();
	TestRouterResolutionHelpers();
	TestDeviceFetchResultClassification();
	TestDeviceFetchOutcomeClassification();
	TestTimestampValidation();
	TestQueryValidation();
	TestFutureAndRetentionValidation();
	TestHalfOpenWindowBoundaries();
	std::cout << "test_mcp_memory_summary passed\n";
	return 0;
}
