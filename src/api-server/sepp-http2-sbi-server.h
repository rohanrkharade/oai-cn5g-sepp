/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_SEPP_HTTP2_SBI_SERVER_SEEN
#define FILE_SEPP_HTTP2_SBI_SERVER_SEEN

#include <nghttp2/asio_http2_server.h>

// #include "TelescopicMappingDocumentApi.h"
// #include "SecurityCapabilityNegotiationApi.h"
// #include "ParameterExchangeApi.h"
// #include "N32FForwardApi.h"
// #include "N32FErrorReportApi.h"
#include "N32FContextTerminateApi.h"
// #include "AccessTokenReq.h"
#include "TelescopicMapping.h"
#include "conversions.hpp"
// #include "sepp.h"
#include "sepp_app.hpp"
#include "string.hpp"
#include "uint_generator.hpp"

using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;
using namespace oai::_3gpp::model;

class sepp_http2_sbi_server {
public:
  sepp_http2_sbi_server(
      const std::string addr, uint32_t port,
      const std::unique_ptr<oai::sepp::app::sepp_app> &sepp_app_inst)
      : m_address(addr), m_port(port), server() {}
  void start();
  void init(size_t thr) {}

  void get_api_list(const response &response);

  void stop();

private:
  oai::utils::uint_generator<uint32_t> m_promise_id_generator;
  std::string m_address;
  uint32_t m_port;
  http2 server;
  // sepp::sepp_app* m_sepp_app;
  bool handle_telescopic_mapping(
      const std::string &foreignFqdn, const std::string &telescopicLabel,
      oai::_3gpp::model::TelescopicMapping &mapping_model);

  bool handle_nf_service_request(const std::string &authority,
                                 const std::string &path,
                                 const std::string &method,
                                 const std::string &body,
                                 oai::sepp::app::nf_http_response &resp_data);
  // std::unique_ptr<oai::sepp::app::sepp_app> m_sepp_app_inst;
  // std::shared_ptr<oai::http::http_client> m_http_client_inst;
  // std::shared_ptr<oai::config::sepp::sepp_config> m_sepp_cfg_inst;
  // std::shared_ptr<oai::sepp::app::task_manager> m_task_manager_inst;
  // std::shared_ptr<oai::sepp::app::sepp_event> m_sepp_event_inst;
  // std::shared_ptr<oai::sepp::app::sepp_nrf> m_sepp_nrf_inst;
  bool running_server;
};

#endif
