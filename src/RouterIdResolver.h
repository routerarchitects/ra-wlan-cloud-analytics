//
// RouterIdResolver Header
//

#pragma once

#include "RESTObjects/RESTAPI_ProvObjects.h"
#include "framework/RESTAPI_Handler.h"
#include "Poco/Logger.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace OpenWifi {

	enum class RouterIdResolutionStatus {
		Success,
		InvalidRouterId,
		InventoryNotFound,
		EmptyVenue,
		BoardNotConfigured,
		MultipleBoards,
		AccessDenied,
		MonitoringNotConfigured,
		MonitoringDisabled,
		OwprovUnavailable,
		OwprovInvalidResponse
	};

	struct RouterIdResolutionResult {
		RouterIdResolutionStatus status = RouterIdResolutionStatus::OwprovUnavailable;
		std::string routerId;
		std::string venueId;
		std::string resolvedBoardId;
		uint64_t resolvedAt = 0;
		uint64_t ownershipVersion = 0;
		std::string message;
	};

	class RouterIdResolver {
	  public:
		static RouterIdResolver *instance() {
			static RouterIdResolver instance_;
			return &instance_;
		}

		RouterIdResolutionResult ResolveRouterIdContext(RESTAPIHandler *client,
														const std::string &routerId);

		void InvalidateCache(const std::string &routerId = "");
		uint64_t GetCurrentOwnershipVersion() const;
		void IncrementOwnershipVersion();

	  private:
		RouterIdResolver();

		struct CacheEntry {
			RouterIdResolutionResult result;
			uint64_t expiresAt = 0;
			uint64_t accessTime = 0;
		};

		mutable std::mutex Mutex_;
		std::map<std::string, CacheEntry> Cache_;
		uint64_t OwnershipVersion_ = 1;
		static constexpr size_t MAX_CACHE_SIZE = 10000;
		static constexpr uint64_t POSITIVE_TTL_SEC = 300;
		static constexpr uint64_t NEGATIVE_TTL_SEC = 30;

		void CleanExpiredCache(uint64_t nowSec);
		void EnforceMaxCacheSize();
	};

	inline RouterIdResolver *RouterIdResolverService() { return RouterIdResolver::instance(); }

} // namespace OpenWifi
