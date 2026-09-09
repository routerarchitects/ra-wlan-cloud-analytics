//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "Daemon.h"

#include "Poco/Environment.h"
#include "Poco/DateTime.h"
#include "Poco/DateTimeFormat.h"
#include "Poco/DateTimeParser.h"
#include "Poco/Exception.h"
#include "Poco/Net/SSLManager.h"
#include "Poco/Util/Application.h"
#include "Poco/Util/Option.h"
#include "framework/OpenWifiTypes.h"

#include "DeviceStatusReceiver.h"
#include "HealthReceiver.h"
#include "StateReceiver.h"
#include "StorageService.h"
#include "VenueCoordinator.h"
#include "WifiClientCache.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/UI_WebSocketClientServer.h"

#include <cstdlib>

namespace OpenWifi {
	class Daemon *Daemon::instance_ = nullptr;

	namespace {
		std::string GetTemperatureCutoverConfigValue() {
			if (const auto *EnvCutover = std::getenv("TEMPERATURE_MIGRATION_CUTOVER_TIME")) {
				if (*EnvCutover != '\0')
					return EnvCutover;
			}
			return MicroServiceConfigGetString("temperature.migration_cutover_time", "");
		}

		bool ParseTemperatureCutoverTime(const std::string &Value) {
			try {
				Poco::DateTime DateTime;
				int TimeZone = 0;
				Poco::DateTimeParser::parse(Poco::DateTimeFormat::ISO8601_FORMAT, Value,
											DateTime, TimeZone);
				(void)DateTime.timestamp().epochTime();
				return true;
			} catch (...) {
			}
			return false;
		}
	}

	class Daemon *Daemon::instance() {
		if (instance_ == nullptr) {
			instance_ = new Daemon(vDAEMON_PROPERTIES_FILENAME, vDAEMON_ROOT_ENV_VAR,
								   vDAEMON_CONFIG_ENV_VAR, vDAEMON_APP_NAME, vDAEMON_BUS_TIMER,
								   SubSystemVec{OpenWifi::StorageService(), StateReceiver(),
												DeviceStatusReceiver(), HealthReceiver(),
												VenueCoordinator(), WifiClientCache(),
												UI_WebSocketClientServer()});
		}
		return instance_;
	}

	void Daemon::PostInitialization([[maybe_unused]] Poco::Util::Application &self) {
		const auto Cutover = GetTemperatureCutoverConfigValue();
		if (Cutover.empty()) {
			Log().fatal("FATAL: Missing required configuration "
						"'temperature.migration_cutover_time'");
			throw Poco::InvalidArgumentException(
				"Missing required configuration 'temperature.migration_cutover_time'");
		}
		if (!ParseTemperatureCutoverTime(Cutover)) {
			Log().fatal("FATAL: Unparseable configuration "
						"'temperature.migration_cutover_time'");
			throw Poco::InvalidArgumentException(
				"Unparseable configuration 'temperature.migration_cutover_time'");
		}
	}

	void DaemonPostInitialization(Poco::Util::Application &self) {
		Daemon()->PostInitialization(self);
	}
} // namespace OpenWifi

int main(int argc, char **argv) {
	int ExitCode;
	try {
		Poco::Net::SSLManager::instance().initializeServer(nullptr, nullptr, nullptr);
		auto App = OpenWifi::Daemon::instance();
		ExitCode = App->run(argc, argv);
		Poco::Net::SSLManager::instance().shutdown();
	} catch (Poco::Exception &exc) {
		ExitCode = Poco::Util::Application::EXIT_SOFTWARE;
		std::cout << exc.displayText() << std::endl;
	} catch (std::exception &exc) {
		ExitCode = Poco::Util::Application::EXIT_TEMPFAIL;
		std::cout << exc.what() << std::endl;
	} catch (...) {
		ExitCode = Poco::Util::Application::EXIT_TEMPFAIL;
		std::cout << "Exception on closure" << std::endl;
	}

	std::cout << "Exitcode: " << ExitCode << std::endl;
	return ExitCode;
}

// end of namespace
