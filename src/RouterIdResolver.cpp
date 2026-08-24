//
// RouterIdResolver Implementation
//

#include "RouterIdResolver.h"
#include "StorageService.h"
#include "VenueCoordinator.h"
#include "fmt/format.h"
#include "framework/utils.h"
#include "sdks/SDK_prov.h"
#include <algorithm>
#include <regex>

namespace OpenWifi {

	static bool ValidateRouterIdSyntax(const std::string &routerId) {
		if (routerId.empty() || routerId.size() > 64)
			return false;
		static const std::regex pattern("^[a-zA-Z0-9_-]+$");
		return std::regex_match(routerId, pattern);
	}

	RouterIdResolver::RouterIdResolver() {}

	uint64_t RouterIdResolver::GetCurrentOwnershipVersion() const {
		std::lock_guard G(Mutex_);
		return OwnershipVersion_;
	}

	void RouterIdResolver::IncrementOwnershipVersion() {
		std::lock_guard G(Mutex_);
		OwnershipVersion_++;
		Cache_.clear();
	}

	void RouterIdResolver::InvalidateCache(const std::string &routerId) {
		std::lock_guard G(Mutex_);
		if (routerId.empty()) {
			Cache_.clear();
		} else {
			std::string suffix = ":" + routerId;
			for (auto it = Cache_.begin(); it != Cache_.end();) {
				if (it->first == routerId ||
					(it->first.size() >= suffix.size() &&
					 it->first.compare(it->first.size() - suffix.size(), suffix.size(), suffix) == 0)) {
					it = Cache_.erase(it);
				} else {
					++it;
				}
			}
		}
	}

	void RouterIdResolver::CleanExpiredCache(uint64_t nowSec) {
		for (auto it = Cache_.begin(); it != Cache_.end();) {
			if (it->second.expiresAt <= nowSec) {
				it = Cache_.erase(it);
			} else {
				++it;
			}
		}
	}

	void RouterIdResolver::EnforceMaxCacheSize() {
		if (Cache_.size() <= MAX_CACHE_SIZE)
			return;
		// Evict least recently used
		auto lruIt = Cache_.begin();
		for (auto it = Cache_.begin(); it != Cache_.end(); ++it) {
			if (it->second.accessTime < lruIt->second.accessTime) {
				lruIt = it;
			}
		}
		if (lruIt != Cache_.end()) {
			Cache_.erase(lruIt);
		}
	}

	RouterIdResolutionResult RouterIdResolver::ResolveRouterIdContext(RESTAPIHandler *client,
																	 const std::string &routerId) {
		RouterIdResolutionResult res;
		res.routerId = routerId;

		if (!ValidateRouterIdSyntax(routerId)) {
			res.status = RouterIdResolutionStatus::InvalidRouterId;
			res.message = "Invalid router ID syntax";
			return res;
		}

		uint64_t nowSec = Utils::Now();
		uint64_t currentVersion = 0;

		std::string userScope = "__system__";
		if (client != nullptr) {
			if (!client->UserInfo_.userinfo.id.empty()) {
				userScope = client->UserInfo_.userinfo.id;
			} else if (!client->UserInfo_.webtoken.username_.empty()) {
				userScope = client->UserInfo_.webtoken.username_;
			} else if (!client->UserInfo_.webtoken.access_token_.empty()) {
				userScope = client->UserInfo_.webtoken.access_token_;
			}
		}

		std::string cacheKey = userScope + ":" + routerId;

		{
			std::lock_guard G(Mutex_);
			currentVersion = OwnershipVersion_;
			CleanExpiredCache(nowSec);

			auto it = Cache_.find(cacheKey);
			if (it != Cache_.end() && it->second.expiresAt > nowSec &&
				it->second.result.ownershipVersion == currentVersion) {
				it->second.accessTime = nowSec;
				return it->second.result;
			}
		}

		// Search local boards DB
		struct BoardMatch {
			AnalyticsObjects::BoardInfo board;
			std::string matchedVenueId;
		};
		std::vector<BoardMatch> rawMatches;
		uint64_t routerInt = Utils::SerialNumberToInt(routerId);

		auto FindInBoard = [&](const AnalyticsObjects::BoardInfo &B) -> bool {
			for (const auto &venue : B.venueList) {
				ProvObjects::VenueDeviceList VDL;
				bool venueExists = true;
				if (SDK::Prov::Venue::GetDevices(client, venue.id, venue.monitorSubVenues, VDL,
												 venueExists)) {
					for (const auto &dev : VDL.devices) {
						if (Utils::SerialNumberToInt(dev) == routerInt || dev == routerId) {
							rawMatches.push_back({B, venue.id});
							break;
						}
					}
				}
			}
			return true;
		};

		StorageService()->BoardsDB().Iterate(FindInBoard);

		// Deduplicate matches by board.info.id
		std::map<std::string, BoardMatch> uniqueBoardMatches;
		for (const auto &m : rawMatches) {
			if (uniqueBoardMatches.find(m.board.info.id) == uniqueBoardMatches.end()) {
				uniqueBoardMatches[m.board.info.id] = m;
			}
		}

		if (uniqueBoardMatches.size() == 1) {
			auto match = uniqueBoardMatches.begin()->second;
			res.status = RouterIdResolutionStatus::Success;
			res.resolvedBoardId = match.board.info.id;
			res.venueId = match.matchedVenueId;
			res.resolvedAt = nowSec;
			res.ownershipVersion = currentVersion;

			std::lock_guard G(Mutex_);
			Cache_[cacheKey] = CacheEntry{res, nowSec + POSITIVE_TTL_SEC, nowSec};
			EnforceMaxCacheSize();
			return res;
		} else if (uniqueBoardMatches.size() > 1) {
			res.status = RouterIdResolutionStatus::MultipleBoards;
			res.message = "Router is mapped to multiple current boards";
			res.resolvedAt = nowSec;
			res.ownershipVersion = currentVersion;

			std::lock_guard G(Mutex_);
			Cache_[cacheKey] = CacheEntry{res, nowSec + NEGATIVE_TTL_SEC, nowSec};
			EnforceMaxCacheSize();
			return res;
		}

		// Fallback to OWPROV inventory lookup
		ProvObjects::InventoryTag invTag;
		auto httpStatus = SDK::Prov::Device::GetWithStatus(client, routerId, invTag);

		if (httpStatus == Poco::Net::HTTPResponse::HTTP_NOT_FOUND) {
			res.status = RouterIdResolutionStatus::InventoryNotFound;
			res.message = "Router was not found";
			res.resolvedAt = nowSec;
			res.ownershipVersion = currentVersion;

			std::lock_guard G(Mutex_);
			Cache_[cacheKey] = CacheEntry{res, nowSec + NEGATIVE_TTL_SEC, nowSec};
			EnforceMaxCacheSize();
			return res;
		} else if (httpStatus != Poco::Net::HTTPResponse::HTTP_OK) {
			res.status = RouterIdResolutionStatus::OwprovUnavailable;
			res.message = "Upstream provisioning service is unavailable";
			return res;
		}

		if (invTag.venue.empty()) {
			res.status = RouterIdResolutionStatus::EmptyVenue;
			res.message = "Router inventory venue is empty";
			res.resolvedAt = nowSec;
			res.ownershipVersion = currentVersion;

			std::lock_guard G(Mutex_);
			Cache_[cacheKey] = CacheEntry{res, nowSec + NEGATIVE_TTL_SEC, nowSec};
			EnforceMaxCacheSize();
			return res;
		}

		res.venueId = invTag.venue;

		// Find boards that monitor invTag.venue
		std::vector<AnalyticsObjects::BoardInfo> venueBoards;
		auto CheckBoardVenue = [&](const AnalyticsObjects::BoardInfo &B) -> bool {
			for (const auto &v : B.venueList) {
				if (v.id == invTag.venue) {
					venueBoards.push_back(B);
					break;
				}
			}
			return true;
		};
		StorageService()->BoardsDB().Iterate(CheckBoardVenue);

		if (venueBoards.size() == 1) {
			res.status = RouterIdResolutionStatus::Success;
			res.resolvedBoardId = venueBoards[0].info.id;
			res.resolvedAt = nowSec;
			res.ownershipVersion = currentVersion;

			std::lock_guard G(Mutex_);
			Cache_[cacheKey] = CacheEntry{res, nowSec + POSITIVE_TTL_SEC, nowSec};
			EnforceMaxCacheSize();
			return res;
		} else if (venueBoards.size() > 1) {
			res.status = RouterIdResolutionStatus::MultipleBoards;
			res.message = "Router venue is monitored by multiple boards";
			res.resolvedAt = nowSec;
			res.ownershipVersion = currentVersion;

			std::lock_guard G(Mutex_);
			Cache_[cacheKey] = CacheEntry{res, nowSec + NEGATIVE_TTL_SEC, nowSec};
			EnforceMaxCacheSize();
			return res;
		}

		res.status = RouterIdResolutionStatus::BoardNotConfigured;
		res.message = "No Analytics board configured for venue";
		res.resolvedAt = nowSec;
		res.ownershipVersion = currentVersion;

		std::lock_guard G(Mutex_);
		Cache_[cacheKey] = CacheEntry{res, nowSec + NEGATIVE_TTL_SEC, nowSec};
		EnforceMaxCacheSize();
		return res;
	}

} // namespace OpenWifi
