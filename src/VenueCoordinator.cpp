//
// Created by stephane bourque on 2022-03-10.
//

#include "VenueCoordinator.h"
#include "StorageService.h"
#include "VenueWatcher.h"
#include "fmt/format.h"
#include "framework/MicroServiceFuncs.h"
#include "framework/utils.h"
#include "sdks/SDK_prov.h"

namespace OpenWifi {

	int VenueCoordinator::Start() {
		poco_notice(Logger(), "Starting...");
		GetBoardList();
		Worker_.start(*this);

		ReconcileTimerCallback_ = std::make_unique<Poco::TimerCallback<VenueCoordinator>>(
			*this, &VenueCoordinator::onReconcileTimer);
		ReconcileTimerTimer_.setStartInterval(3 * 60 * 1000);
		ReconcileTimerTimer_.setPeriodicInterval(3 * 60 * 1000); // 1 hours
		ReconcileTimerTimer_.start(*ReconcileTimerCallback_, MicroServiceTimerPool());

		return 0;
	}

	void VenueCoordinator::onReconcileTimer([[maybe_unused]] Poco::Timer &timer) {
		std::lock_guard G(Mutex_);
		Utils::SetThreadName("brd-refresh");

		poco_information(Logger(), "Starting to reconcile board information.");
		for (const auto &[board_id, watcher] : Watchers_) {
			poco_information(Logger(), fmt::format("Updating: {}", board_id));
			UpdateBoard(board_id);
		}
		poco_information(Logger(), "Finished reconciling board information.");
	}

	void VenueCoordinator::GetBoardList() {
		BoardsToWatch_.clear();
		auto F = [&](const AnalyticsObjects::BoardInfo &B) -> bool {
			BoardsToWatch_.insert(B);
			// poco_information(Logger(),fmt::format("Starting watch for {}.", B.info.name));
			return true;
		};
		StorageService()->BoardsDB().Iterate(F);
	}

	void VenueCoordinator::Stop() {
		poco_notice(Logger(), "Stopping...");
		Running_ = false;
		Worker_.wakeUp();
		Worker_.wakeUp();
		Worker_.join();
		poco_notice(Logger(), "Stopped...");
	}

	void VenueCoordinator::run() {
		Utils::SetThreadName("venue-coord");
		Running_ = true;
		while (Running_) {
			Poco::Thread::trySleep(60000);
			if (!Running_)
				break;

			std::lock_guard G(Mutex_);
			GetBoardList();

			if (!BoardsToWatch_.empty()) {
				for (const auto &board_to_start : BoardsToWatch_) {
					bool VenueExists = true;
					if (!Watching(board_to_start.info.id)) {
						StartBoard(board_to_start);
					} else if (SDK::Prov::Venue::Exists(nullptr, board_to_start.venueList[0].id,
														VenueExists) &&
							   !VenueExists) {
						RetireBoard(board_to_start);
					}
				}
			}
		}
	}

	void VenueCoordinator::RetireBoard(const AnalyticsObjects::BoardInfo &B) {
		Logger().error(fmt::format(
			"Venue board '{}' is no longer in the system. Retiring its associated board.",
			B.venueList[0].name));
		StopBoard(B.info.id);
		StorageService()->BoardsDB().DeleteRecord("id", B.info.id);
		StorageService()->TimePointsDB().DeleteRecords(fmt::format(" boardId='{}' ", B.info.id));
	}

	bool VenueCoordinator::GetDevicesForBoard(const AnalyticsObjects::BoardInfo &B,
											  std::vector<uint64_t> &Devices, bool &VenueExists) {
		ProvObjects::VenueDeviceList VDL;

		if (SDK::Prov::Venue::GetDevices(nullptr, B.venueList[0].id,
										 B.venueList[0].monitorSubVenues, VDL, VenueExists)) {
			Devices.clear();
			for (const auto &device : VDL.devices) {
				Devices.push_back(Utils::SerialNumberToInt(device));
			}
			std::sort(Devices.begin(), Devices.end());
			auto LastDevice = std::unique(Devices.begin(), Devices.end());
			Devices.erase(LastDevice, Devices.end());
			return true;
		}

		if (!VenueExists) {
			RetireBoard(B);
		}

		return false;
	}

	bool VenueCoordinator::StartBoard(const AnalyticsObjects::BoardInfo &B) {
		if (B.venueList.empty())
			return true;

		bool VenueExists = true;
		std::vector<uint64_t> Devices;
		if (GetDevicesForBoard(B, Devices, VenueExists)) {
			ApplyDeviceUpdate(B.info.id, Devices, ExistingVersions_[B.info.id]);
			return true;
		}

		if (!VenueExists) {
			RetireBoard(B);
			return false;
		}

		poco_information(Logger(), fmt::format("Could not start board {}", B.info.name));
		return false;
	}

	void VenueCoordinator::StopBoard(const std::string &id) {
		std::lock_guard G(Mutex_);

		auto it = Watchers_.find(id);
		if (it != Watchers_.end()) {
			it->second->Stop();
			Watchers_.erase(it);
		}
		ExistingBoards_.erase(id);
		ExistingVersions_.erase(id);
	}

	void VenueCoordinator::UpdateBoard(const std::string &id) {
		AnalyticsObjects::BoardInfo B;
		if (StorageService()->BoardsDB().GetRecord("id", id, B)) {
			std::vector<uint64_t> Devices;
			bool VenueExists = true;
			if (GetDevicesForBoard(B, Devices, VenueExists)) {
				ApplyDeviceUpdate(id, Devices, ExistingVersions_[id]);
				return;
			}

			if (!VenueExists) {
				RetireBoard(B);
				return;
			}

			poco_information(Logger(), fmt::format("Could not modify board {}", B.info.name));
		}
	}

	bool VenueCoordinator::Watching(const std::string &id) {
		std::lock_guard G(Mutex_);
		return (ExistingBoards_.find(id) != ExistingBoards_.end());
	}

	void VenueCoordinator::AddBoard(const std::string &id) {
		std::lock_guard G(Mutex_);

		AnalyticsObjects::BoardInfo B;
		if (StorageService()->BoardsDB().GetRecord("id", id, B))
			BoardsToWatch_.insert(B);
		else
			poco_information(Logger(), fmt::format("Board {} does not seem to exist", id));
	}

	void VenueCoordinator::GetDevices(std::string &id, AnalyticsObjects::DeviceInfoList &DIL) {
		std::lock_guard G(Mutex_);

		auto it = Watchers_.find(id);
		if (it != end(Watchers_)) {
			it->second->GetDevices(DIL.devices);
		}
	}

	bool VenueCoordinator::StartBoard(const ProvisioningChangeEvent &event) {
		std::vector<uint64_t> devices;
		devices.reserve(event.board.devices.size());
		for (const auto &serial : event.board.devices) {
			devices.push_back(Utils::SerialNumberToInt(serial));
		}
		ApplyDeviceUpdate(event.board.id, devices, event.board.version);
		return true;
	}

	void VenueCoordinator::ApplyDeviceUpdate(const std::string &boardId,
							 const std::vector<uint64_t> &devices,
							 uint64_t version) {
		std::lock_guard G(Mutex_);

		uint64_t currentVersion = ExistingVersions_[boardId];
		if (version != 0 && currentVersion != 0 && version < currentVersion) {
			poco_debug(Logger(), fmt::format("Ignoring stale provisioning event for board {} version {} (< {}).",
							boardId, version, currentVersion));
			return;
		}

		auto watcherIt = Watchers_.find(boardId);
		if (watcherIt == Watchers_.end()) {
			if (devices.empty()) {
				ExistingBoards_.erase(boardId);
				ExistingVersions_.erase(boardId);
				return;
			}
			AnalyticsObjects::BoardInfo boardInfo;
			std::string venueId;
			if (StorageService()->BoardsDB().GetRecord("id", boardId, boardInfo) && !boardInfo.venueList.empty()) {
				venueId = boardInfo.venueList[0].id;
			}
			auto watcher = std::make_shared<VenueWatcher>(boardId, venueId, Logger(), devices);
			watcher->Start();
			Watchers_[boardId] = watcher;
			poco_information(Logger(), fmt::format("Started board {} with {} devices.", boardId, devices.size()));
		} else {
			if (ExistingBoards_[boardId] != devices) {
				watcherIt->second->ModifySerialNumbers(devices);
				poco_information(Logger(), fmt::format("Updated board {} device list ({} devices).",
							boardId, devices.size()));
			}
		}

		ExistingBoards_[boardId] = devices;
		if (version != 0) {
			ExistingVersions_[boardId] = version;
		}
	}

	void VenueCoordinator::HandleProvisioningEvent(const ProvisioningChangeEvent &event) {
		if (event.eventType == "board.deleted") {
			poco_information(Logger(), fmt::format("Provisioning event delete board {}", event.board.id));
			StopBoard(event.board.id);
			StorageService()->BoardsDB().DeleteRecord("id", event.board.id);
			StorageService()->TimePointsDB().DeleteRecords(fmt::format(" boardId='{}' ", event.board.id));
			return;
		}

		if (!event.board.id.empty()) {
			AnalyticsObjects::BoardInfo boardInfo;
			if (!StorageService()->BoardsDB().GetRecord("id", event.board.id, boardInfo)) {
				boardInfo.info.id = event.board.id;
				boardInfo.info.name = event.board.name;
				if (!event.board.venueId.empty()) {
					AnalyticsObjects::VenueInfo venue;
					venue.id = event.board.venueId;
					venue.monitorSubVenues = event.board.monitorSubVenues;
					boardInfo.venueList.push_back(venue);
				}
				StorageService()->BoardsDB().CreateRecord(boardInfo);
			}
		}

		StartBoard(event);
	}
} // namespace OpenWifi
