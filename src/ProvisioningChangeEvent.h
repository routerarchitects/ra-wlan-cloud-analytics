#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "Poco/Logger.h"

namespace OpenWifi {

    struct ProvisioningChangeEventDevice {
        std::string serial;
    };

    struct ProvisioningChangeEventBoard {
        std::string id;
        std::string name;
        std::string venueId;
        bool monitorSubVenues = false;
        uint64_t version = 0;
        std::vector<std::string> devices;
    };

    struct ProvisioningChangeEvent {
        std::string eventId;
        std::string eventType;
        std::string occurredAt;
        std::string correlationId;
        ProvisioningChangeEventBoard board;

        bool Valid() const {
            return !eventType.empty() && !board.id.empty();
        }
    };

    inline bool ParseProvisioningChangeEvent(const std::string &payload,
                                             ProvisioningChangeEvent &event,
                                             Poco::Logger &logger) {
        try {
            auto json = nlohmann::json::parse(payload);
            if (json.contains("eventId")) event.eventId = json["eventId"].get<std::string>();
            if (json.contains("eventType")) event.eventType = json["eventType"].get<std::string>();
            if (json.contains("occurredAt")) event.occurredAt = json["occurredAt"].get<std::string>();
            if (json.contains("correlationId")) event.correlationId = json["correlationId"].get<std::string>();
            event.board.devices.clear();
            if (json.contains("board")) {
                auto &bj = json["board"];
                if (bj.contains("id")) event.board.id = bj["id"].get<std::string>();
                if (bj.contains("name")) event.board.name = bj["name"].get<std::string>();
                if (bj.contains("venueId")) event.board.venueId = bj["venueId"].get<std::string>();
                if (bj.contains("monitorSubVenues")) event.board.monitorSubVenues = bj["monitorSubVenues"].get<bool>();
                if (bj.contains("version")) {
                    try {
                        event.board.version = bj["version"].get<uint64_t>();
                    } catch (...) {
                        event.board.version = 0;
                    }
                }
                if (bj.contains("devices") && bj["devices"].is_array()) {
                    for (const auto &d : bj["devices"]) {
                        event.board.devices.push_back(d.get<std::string>());
                    }
                }
            }
            return event.Valid();
        } catch (const std::exception &ex) {
            logger.error("Failed to parse provisioning change event: {}", std::string(ex.what()));
        } catch (...) {
            logger.error("Failed to parse provisioning change event: unknown error");
        }
        return false;
    }

} // namespace OpenWifi
