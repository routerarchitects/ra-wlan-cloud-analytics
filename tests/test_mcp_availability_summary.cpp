#include "RESTAPI/RESTAPI_mcp_helpers.h"

#include <cassert>
#include <iostream>
#include <optional>

namespace OpenWifi {
	const std::string &MicroServiceDataDirectory() {
		static const std::string DataDirectory = "/tmp";
		return DataDirectory;
	}
} // namespace OpenWifi

using namespace OpenWifi;

namespace {
	MCP::Window TestWindow() {
		MCP::Window W;
		W.startTime = 1000;
		W.endTime = 4600;
		W.lookbackHours = 1;
		return W;
	}

	void TestAvailabilitySummaryWithOfflineEvents() {
		auto Summary = MCP::CalculateGatewayAvailabilitySummary(
			"60cf84f22290", TestWindow(), 2, std::optional<uint64_t>{1600},
			std::optional<uint64_t>{4300});

		assert(Summary.meta.requestedWindow.startTime == "1970-01-01T00:16:40Z");
		assert(Summary.meta.requestedWindow.endTime == "1970-01-01T01:16:40Z");
		assert(Summary.meta.observedWindow.startTime == "1970-01-01T00:26:40Z");
		assert(Summary.meta.observedWindow.endTime == "1970-01-01T01:11:40Z");
		assert(Summary.data.gw_uuid == "60cf84f22290");
		assert(Summary.data.fetch_status == "success");
		assert(Summary.data.offlineEventCount == 2);
	}

	void TestAvailabilitySummaryWithNoEvents() {
		auto Summary = MCP::CalculateGatewayAvailabilitySummary(
			"60cf84f22290", TestWindow(), 0, std::nullopt, std::nullopt);

		assert(!Summary.meta.observedWindow.startTime);
		assert(!Summary.meta.observedWindow.endTime);
		assert(Summary.data.offlineEventCount == 0);
	}

	void TestContractSerialization() {
		auto Summary = MCP::CalculateGatewayAvailabilitySummary(
			"60cf84f22290", TestWindow(), 1, std::optional<uint64_t>{1600},
			std::optional<uint64_t>{1600});
		Poco::JSON::Object Obj;
		Summary.to_json(Obj);

		assert(Obj.size() == 2);
		assert(Obj.has("meta"));
		assert(Obj.has("data"));
		assert(!Obj.has("offline_count"));
		assert(!Obj.has("offlineEventCount"));

		auto MetaObj = Obj.get("meta").extract<Poco::JSON::Object>();
		auto DataObj = Obj.get("data").extract<Poco::JSON::Object>();
		assert(MetaObj.has("requestedWindow"));
		assert(MetaObj.has("observedWindow"));
		assert(MetaObj.size() == 2);
		assert(DataObj.size() == 3);
		assert(!MetaObj.has("offlineEventCount"));
		assert(DataObj.getValue<std::string>("gw_uuid") == "60cf84f22290");
		assert(DataObj.getValue<std::string>("fetch_status") == "success");
		assert(!DataObj.has("offline_count"));
		assert(DataObj.getValue<uint64_t>("offlineEventCount") == 1);
	}

	void TestNullObservedWindowSerialization() {
		auto Summary = MCP::CalculateGatewayAvailabilitySummary(
			"60cf84f22290", TestWindow(), 0, std::nullopt, std::nullopt);
		Poco::JSON::Object Obj;
		Summary.to_json(Obj);

		auto MetaObj = Obj.get("meta").extract<Poco::JSON::Object>();
		auto ObservedObj = MetaObj.get("observedWindow").extract<Poco::JSON::Object>();
		assert(ObservedObj.isNull("startTime"));
		assert(ObservedObj.isNull("endTime"));
	}
} // namespace

int main() {
	TestAvailabilitySummaryWithOfflineEvents();
	TestAvailabilitySummaryWithNoEvents();
	TestContractSerialization();
	TestNullObservedWindowSerialization();
	std::cout << "test_mcp_availability_summary passed\n";
	return 0;
}
