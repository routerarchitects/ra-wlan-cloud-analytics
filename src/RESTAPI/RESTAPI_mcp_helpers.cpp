#include "RESTAPI_mcp_helpers.h"

#include "framework/AuthClient.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/RESTAPI_Handler.h"
#include <Poco/Environment.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Stringifier.h>

namespace OpenWifi::MCP {

	void SendError(RESTAPIHandler &Handler, const Error &E) {
		Handler.PrepareResponse(E.status);
		Poco::JSON::Object Body;
		Body.set("error", E.error);
		Body.set("message", E.message);
		auto &Answer = Handler.Response->send();
		Poco::JSON::Stringifier::stringify(Body, Answer);
	}

	bool AuthenticateBearerToken(RESTAPIHandler &Handler, Error &E) {
		auto Request = Handler.Request;
		std::optional<std::string> Authorization;
		if (Request != nullptr && Request->has("Authorization"))
			Authorization = Request->get("Authorization", "");

		std::string Token;
		if (!ExtractBearerToken(Authorization, Token, E))
			return false;

		bool Expired = false;
		bool Contacted = false;
		if (!AuthClient()->IsAuthorized(Token, Handler.UserInfo_, 0, Expired, Contacted, false)) {
			SetError(E, Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED, "unauthorized",
					 UnauthorizedMessage);
			return false;
		}
		return true;
	}

	bool GetTemperatureMigrationCutoverTime(uint64_t &CutoverTime, Error &E) {
		auto Value = Poco::Environment::get("TEMPERATURE_MIGRATION_CUTOVER_TIME", "");
		if (Value.empty())
			Value = MicroServiceConfigGetString("temperature.migration_cutover_time", "");

		if (Value.empty()) {
			SetError(E, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
					 "temperature_cutover_not_configured",
					 "Missing required configuration 'temperature.migration_cutover_time'");
			return false;
		}
		if (!ParseRFC3339Timestamp(Value, CutoverTime)) {
			SetError(E, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR,
					 "temperature_cutover_invalid",
					 "Unparseable configuration 'temperature.migration_cutover_time'");
			return false;
		}
		return true;
	}

} // namespace OpenWifi::MCP
