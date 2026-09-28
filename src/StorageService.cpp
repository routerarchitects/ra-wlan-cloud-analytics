//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//
//	Created by Stephane Bourque on 2021-03-04.
//	Arilia Wireless Inc.
//

#include "StorageService.h"
#include "fmt/format.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/utils.h"

namespace OpenWifi {
	namespace {
		uint64_t EffectiveBoardRetention(const AnalyticsObjects::BoardInfo &Board) {
			uint64_t Retention = 0;
			for (const auto &Venue : Board.venueList) {
				if (Venue.retention > Retention)
					Retention = Venue.retention;
			}
			return Retention;
		}

		uint64_t RetentionCutoff(uint64_t Now, uint64_t Retention) {
			if (Retention == 0 || Retention >= Now)
				return 0;
			return Now - Retention;
		}
	} // namespace

	int Storage::Start() {
		poco_notice(Logger(), "Starting...");
		std::lock_guard Guard(Mutex_);

		StorageClass::Start();

		BoardsDB_ = std::make_unique<OpenWifi::BoardsDB>(dbType_, *Pool_, Logger());
		TimePointsDB_ = std::make_unique<OpenWifi::TimePointDB>(dbType_, *Pool_, Logger());
		WifiClientHistoryDB_ =
			std::make_unique<OpenWifi::WifiClientHistoryDB>(dbType_, *Pool_, Logger());
		DeviceAvailabilityEventsDB_ =
			std::make_unique<OpenWifi::DeviceAvailabilityEventsDB>(dbType_, *Pool_, Logger());

		TimePointsDB_->Create();
		BoardsDB_->Create();
		WifiClientHistoryDB_->Create();
		DeviceAvailabilityEventsDB_->Create();

		PeriodicCleanup_ = MicroServiceConfigGetInt("storage.cleanup.interval", 6 * 60 * 60);
		if (PeriodicCleanup_ < 1 * 60 * 60)
			PeriodicCleanup_ = 1 * 60 * 60;

		Updater_.start(*this);

		TimerCallback_ = std::make_unique<Poco::TimerCallback<Storage>>(*this, &Storage::onTimer);
		Timer_.setStartInterval(60 * 1000);						   // first run in 20 seconds
		Timer_.setPeriodicInterval((long)PeriodicCleanup_ * 1000); // 1 hours
		Timer_.start(*TimerCallback_);

		return 0;
	}

	void Storage::onTimer([[maybe_unused]] Poco::Timer &timer) {
		uint64_t Start = 0;
		const uint64_t Batch = 100;
		poco_information(Logger(), "Starting cleanup of TimePoint and availability databases");
		while (true) {
			BoardsDB::RecordVec BoardList;
			if (!BoardsDB().GetRecords(Start, Batch, BoardList))
				break;

			const auto Now = Utils::Now();
			for (const auto &Board : BoardList) {
				const auto Retention = EffectiveBoardRetention(Board);
				if (Retention == 0) {
					poco_warning(
						Logger(),
						fmt::format("Skipping cleanup for board '{}' because retention is zero",
									Board.info.name));
					continue;
				}

				const auto Cutoff = RetentionCutoff(Now, Retention);
				poco_information(
					Logger(), fmt::format("Removing old records for board '{}'", Board.info.name));
				if (!TimePointsDB().DeleteRecords(fmt::format(
						" boardId='{}' and timestamp<{} ", ORM::Escape(Board.info.id), Cutoff))) {
					poco_error(
						Logger(),
						fmt::format("Failed to remove old records for board '{}'", Board.info.name));
				}

				poco_information(
					Logger(),
					fmt::format("Removing expired availability events for board '{}'",
								Board.info.name));
				if (!DeviceAvailabilityEventsDB().DeleteExpiredEventsForBoard(
						Board.info.id, Cutoff)) {
					poco_error(
						Logger(),
						fmt::format("Failed to remove expired availability events for board '{}'",
									Board.info.name));
				}
			}

			if (BoardList.size() < Batch)
				break;
			Start += BoardList.size();
		}

		auto MaxDays = MicroServiceConfigGetInt("wificlient.age.limit", 14);
		auto LowerDate = Utils::Now() - (MaxDays * 60 * 60 * 24);
		poco_information(Logger(),
						 fmt::format("Removing WiFi Clients history older than {} days.", MaxDays));
		StorageService()->WifiClientHistoryDB().DeleteRecords(
			fmt::format(" timestamp<{} ", LowerDate));
		poco_information(Logger(), fmt::format("Done cleanup of databases. Next run in {} seconds.",
											   PeriodicCleanup_));
	}

	void Storage::run() {
		Utils::SetThreadName("strg-updtr");
		Running_ = true;
		bool FirstRun = true;
		long Retry = 2000;
		while (Running_) {
			if (!FirstRun)
				Poco::Thread::trySleep(Retry);
			if (!Running_)
				break;
			FirstRun = false;
			Retry = 2000;
		}
	}

	void Storage::Stop() {
		poco_notice(Logger(), "Stopping...");
		Running_ = false;
		Timer_.stop();
		Updater_.wakeUp();
		Updater_.join();
		poco_notice(Logger(), "Stopped...");
	}
} // namespace OpenWifi

// namespace
