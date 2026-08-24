//
// RESTAPI_mcp_handlers Header
//

#pragma once

#include "StorageService.h"
#include "framework/RESTAPI_Handler.h"

namespace OpenWifi {

	class RESTAPI_device_memory_summary_handler : public RESTAPIHandler {
	  public:
		RESTAPI_device_memory_summary_handler(const RESTAPIHandler::BindingMap &bindings, Poco::Logger &L,
											 RESTAPI_GenericServerAccounting &Server,
											 uint64_t TransactionId, bool Internal)
			: RESTAPIHandler(bindings, L,
							 std::vector<std::string>{Poco::Net::HTTPRequest::HTTP_GET,
													  Poco::Net::HTTPRequest::HTTP_OPTIONS},
							 Server, TransactionId, Internal) {}

		static auto PathName() {
			return std::list<std::string>{"/api/v1/devices/{routerId}/memory-summary"};
		}

	  private:
		void DoGet() final;
		void DoPost() final {}
		void DoPut() final {}
		void DoDelete() final {}
	};

	class RESTAPI_device_radio_temp_summary_handler : public RESTAPIHandler {
	  public:
		RESTAPI_device_radio_temp_summary_handler(const RESTAPIHandler::BindingMap &bindings,
												 Poco::Logger &L,
												 RESTAPI_GenericServerAccounting &Server,
												 uint64_t TransactionId, bool Internal)
			: RESTAPIHandler(bindings, L,
							 std::vector<std::string>{Poco::Net::HTTPRequest::HTTP_GET,
													  Poco::Net::HTTPRequest::HTTP_OPTIONS},
							 Server, TransactionId, Internal) {}

		static auto PathName() {
			return std::list<std::string>{"/api/v1/devices/{routerId}/radio-temperature-summary"};
		}

	  private:
		void DoGet() final;
		void DoPost() final {}
		void DoPut() final {}
		void DoDelete() final {}
	};

	class RESTAPI_device_wifi_client_usage_summary_handler : public RESTAPIHandler {
	  public:
		RESTAPI_device_wifi_client_usage_summary_handler(const RESTAPIHandler::BindingMap &bindings,
														Poco::Logger &L,
														RESTAPI_GenericServerAccounting &Server,
														uint64_t TransactionId, bool Internal)
			: RESTAPIHandler(bindings, L,
							 std::vector<std::string>{Poco::Net::HTTPRequest::HTTP_GET,
													  Poco::Net::HTTPRequest::HTTP_OPTIONS},
							 Server, TransactionId, Internal) {}

		static auto PathName() {
			return std::list<std::string>{"/api/v1/devices/{routerId}/wifi-clients/usage-summary"};
		}

	  private:
		void DoGet() final;
		void DoPost() final {}
		void DoPut() final {}
		void DoDelete() final {}
	};

	class RESTAPI_device_wifi_client_rssi_summary_handler : public RESTAPIHandler {
	  public:
		RESTAPI_device_wifi_client_rssi_summary_handler(const RESTAPIHandler::BindingMap &bindings,
													   Poco::Logger &L,
													   RESTAPI_GenericServerAccounting &Server,
													   uint64_t TransactionId, bool Internal)
			: RESTAPIHandler(bindings, L,
							 std::vector<std::string>{Poco::Net::HTTPRequest::HTTP_GET,
													  Poco::Net::HTTPRequest::HTTP_OPTIONS},
							 Server, TransactionId, Internal) {}

		static auto PathName() {
			return std::list<std::string>{"/api/v1/devices/{routerId}/wifi-clients/rssi-summary"};
		}

	  private:
		void DoGet() final;
		void DoPost() final {}
		void DoPut() final {}
		void DoDelete() final {}
	};

	class RESTAPI_device_availability_summary_handler : public RESTAPIHandler {
	  public:
		RESTAPI_device_availability_summary_handler(const RESTAPIHandler::BindingMap &bindings,
												   Poco::Logger &L,
												   RESTAPI_GenericServerAccounting &Server,
												   uint64_t TransactionId, bool Internal)
			: RESTAPIHandler(bindings, L,
							 std::vector<std::string>{Poco::Net::HTTPRequest::HTTP_GET,
													  Poco::Net::HTTPRequest::HTTP_OPTIONS},
							 Server, TransactionId, Internal) {}

		static auto PathName() {
			return std::list<std::string>{"/api/v1/devices/{routerId}/availability-summary"};
		}

	  private:
		void DoGet() final;
		void DoPost() final {}
		void DoPut() final {}
		void DoDelete() final {}
	};

} // namespace OpenWifi
