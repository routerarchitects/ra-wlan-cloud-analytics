//
// Created by stephane bourque on 2022-03-10.
//

#pragma once

#include "VenueWatcher.h"
#include "framework/SubSystemServer.h"
#include "ProvisioningChangeEvent.h"

#include "Poco/Timer.h"

namespace OpenWifi {

	class VenueCoordinator : public SubSystemServer, Poco::Runnable {
	  public:
		static auto instance() {
			static auto instance_ = new VenueCoordinator;
			return instance_;
		}

		int Start() override;
		void Stop() override;
		void run() override;

		void StopBoard(const std::string &id);
		void UpdateBoard(const std::string &id);
		void AddBoard(const std::string &id);
		void HandleProvisioningEvent(const ProvisioningChangeEvent &event);

		bool GetDevicesForBoard(const AnalyticsObjects::BoardInfo &B,
								std::vector<uint64_t> &Devices, bool &VenueExists);
		void GetDevices(std::string &id, AnalyticsObjects::DeviceInfoList &DIL);
		void GetBoardList();
		bool Watching(const std::string &id);
		void RetireBoard(const AnalyticsObjects::BoardInfo &B);

		void onReconcileTimer(Poco::Timer &timer);

	  private:
		Poco::Thread Worker_;
		std::atomic_bool Running_ = false;
		std::set<AnalyticsObjects::BoardInfo> BoardsToWatch_;
		std::map<std::string, std::shared_ptr<VenueWatcher>> Watchers_;
		std::unique_ptr<Poco::TimerCallback<VenueCoordinator>> ReconcileTimerCallback_;
		Poco::Timer ReconcileTimerTimer_;

		std::map<std::string, std::vector<uint64_t>> ExistingBoards_;
		std::map<std::string, uint64_t> ExistingVersions_;

		VenueCoordinator() noexcept
			: SubSystemServer("VenueCoordinator", "VENUE-COORD", "venue.coordinator") {}

		bool StartBoard(const AnalyticsObjects::BoardInfo &B);
		bool StartBoard(const ProvisioningChangeEvent &event);
		void ApplyDeviceUpdate(const std::string &boardId,
							 const std::vector<uint64_t> &devices,
							 uint64_t version);
	};
	inline auto VenueCoordinator() { return VenueCoordinator::instance(); }

} // namespace OpenWifi
