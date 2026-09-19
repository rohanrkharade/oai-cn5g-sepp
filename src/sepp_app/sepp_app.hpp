/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_SEPP_APP_HPP_SEEN
#define FILE_SEPP_APP_HPP_SEEN

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "3gpp_29.500.h"
#include "TelescopicMapping.h"
#include "sepp_event.hpp"
#include "sepp_n32c_handshake.hpp"
#include "sepp_n32f_forward.hpp"
#include "sepp_nrf.hpp"
#include "sepp_profile.hpp"
#include "sepp_telescopic_fqdn_mapping.hpp"

namespace oai::sepp::app {

class sepp_app {
public:
  explicit sepp_app(sepp_event &ev);
  sepp_app(const sepp_app &) = delete;
  sepp_app &operator=(const sepp_app &) = delete;
  sepp_app(sepp_app &&) = delete;
  sepp_app &operator=(sepp_app &&) = delete;

  virtual ~sepp_app();

  /**
   * Stop all ongoing processes and procedures, deregistering from NRF.
   */
  void stop();

  /* Telescopic mapping */
  bool get_telescopic_mapping(const std::string &foreignFqdn,
                              const std::string &telescopicLabel,
                              std::string &telescopicMapping);

  bool
  get_telescopic_mapping(const std::string &foreignFqdn,
                         const std::string &telescopicLabel,
                         oai::_3gpp::model::TelescopicMapping &mapping_model);

  /* N32-c Handshake Handlers */
  bool handle_exchange_capability_request(
      const nlohmann::json &req_data, nlohmann::json &resp_data,
      std::unordered_map<std::string, std::string> &resp_headers);

  void create_exchange_capability();
  void create_exchange_params();

  bool handle_exchange_params(
      const nlohmann::json &req_data, nlohmann::json &resp_data,
      std::unordered_map<std::string, std::string> &resp_headers);

  bool handle_n32f_terminate_req(
      const nlohmann::json &req_data, nlohmann::json &resp_data,
      std::unordered_map<std::string, std::string> &resp_headers);

  bool
  handle_n32f_error(const nlohmann::json &req_data, nlohmann::json &resp_data,
                    std::unordered_map<std::string, std::string> &resp_headers);

  /* N32-f Service & Forwarding Handlers */
  bool handle_nf_service_request(const std::string &authority,
                                 const std::string &path,
                                 const std::string &method,
                                 const std::string &body,
                                 oai::sepp::app::nf_http_response &resp_data);

  bool handle_n32f_forward(
      const nlohmann::json &req_data, nlohmann::json &resp_data,
      std::unordered_map<std::string, std::string> &resp_headers);

  bool handle_n32f_process(
      const nlohmann::json &req_data, nlohmann::json &resp_data,
      std::unordered_map<std::string, std::string> &resp_headers);

private:
  bool handle_nf_service_request_prins(const std::string &authority,
                                       const std::string &path,
                                       const std::string &method,
                                       const std::string &body,
                                       oai::sepp::app::nf_http_response &resp_data);

  bool handle_nf_service_request_tls(const std::string &authority,
                                     const std::string &path,
                                     const std::string &method,
                                     const std::string &body,
                                     oai::sepp::app::nf_http_response &resp_data);

  sepp_profile m_nf_instance_profile;
  std::string m_sepp_instance_id;

  // Event Handling
  sepp_event &m_event_sub;
  bs2::connection m_task_connection;

  std::unique_ptr<oai::sepp::app::sepp_nrf> m_sepp_nrf_inst;
  std::unique_ptr<sepp_telescopic_fqdn_mapping> m_sepp_telescopic_fqdn_mapping;
  std::unique_ptr<sepp_n32c_handshake> m_sepp_n32c_handshake_inst;
  std::unique_ptr<sepp_n32f_forward> m_sepp_n32f_forward_inst;
  std::unique_ptr<std::string> m_remote_sepp_url;

  // Roaming partner PLMNs
  std::vector<oai::config::plmn_config> m_roaming_partners;

  void send_n32f_terminate();
};

} // namespace oai::sepp::app

#endif /* FILE_SEPP_APP_HPP_SEEN */