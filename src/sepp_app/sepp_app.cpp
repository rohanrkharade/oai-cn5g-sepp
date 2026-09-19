/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "sepp_app.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "AccessTokenReq.h"
#include "N32fContextInfo.h"
#include "N32fErrorDetail.h"
#include "N32fReformattedRspMsg.h"
#include "SecNegotiateReqData.h"
#include "SecNegotiateRspData.h"
#include "SecParamExchReqData.h"
#include "SecParamExchRspData.h"
#include "conversions.hpp"
#include "http_client.hpp"
#include "logger.hpp"
#include "sepp_JOSE.hpp"
#include "sepp_config.hpp"

using namespace oai::sepp::app;
using namespace oai::config::sepp;
using namespace oai::_3gpp::model;

extern std::unique_ptr<sepp_config> sepp_cfg;
extern std::shared_ptr<oai::http::http_client> http_client_inst;
extern std::shared_ptr<oai::http::http_client> http_client_inst_nbi;

//------------------------------------------------------------------------------
sepp_app::sepp_app(sepp_event &ev) : m_event_sub(ev) {
  Logger::sepp_app().startup("Starting SEPP Application...");

  if (sepp_cfg->register_nrf()) {
    m_sepp_nrf_inst = std::make_unique<sepp_nrf>(ev);
    m_sepp_nrf_inst->register_to_nrf();
    Logger::sepp_app().info("NRF Task Created");

    m_roaming_partners = sepp_cfg->get_roaming_config().get_roaming_partners();

    m_sepp_telescopic_fqdn_mapping =
        std::make_unique<sepp_telescopic_fqdn_mapping>();
    m_sepp_telescopic_fqdn_mapping->set_roaming_partners(m_roaming_partners);

    m_sepp_n32c_handshake_inst = std::make_unique<sepp_n32c_handshake>();
    m_sepp_n32f_forward_inst = std::make_unique<sepp_n32f_forward>();

    const std::string &sec_capability = sepp_cfg->get_security();
    m_sepp_n32c_handshake_inst->m_selected_sec_capability = sec_capability;
    m_sepp_n32f_forward_inst->set_selected_sec_capability(sec_capability);

    if (sepp_cfg->get_role_enum() == sepp_role_e::C_SEPP) {
      Logger::sepp_app().info("SEPP is configured as C-SEPP");
      if (!m_roaming_partners.empty()) {
        const auto &plmn = m_roaming_partners.front();
        bool use_tls = sepp_cfg->is_tls_enabled();
        std::string scheme = use_tls ? "https://" : "http://";

        unsigned int nbi_port = sepp_cfg->local().get_nbi().get_port();
        std::string port_str = std::to_string(nbi_port);

        m_remote_sepp_url = std::make_unique<std::string>(
            scheme + "sepp.5gc.mnc" + plmn.mnc.get_value() + ".mcc" +
            plmn.mcc.get_value() + ".3gppnetwork.org:" + port_str);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        create_exchange_capability();
      }
    }
  }
}

//------------------------------------------------------------------------------
bool sepp_app::handle_exchange_capability_request(
    const nlohmann::json &req_data, nlohmann::json &resp_data,
    std::unordered_map<std::string, std::string> &resp_headers) {
  if (!m_sepp_n32c_handshake_inst)
    return false;

  try {
    SecNegotiateReqData sec_req;
    from_json(req_data, sec_req);
    std::stringstream err_ss;
    if (!sec_req.validate(err_ss)) {
      Logger::sepp_app().warn("SecNegotiateReqData invalid: %s",
                              err_ss.str().c_str());
    }
  } catch (const std::exception &e) {
    Logger::sepp_app().warn("SecNegotiateReqData validation failed: %s",
                            e.what());
  }

  return m_sepp_n32c_handshake_inst->handle_exchange_capability_request(
      req_data, resp_data, resp_headers);
}

//------------------------------------------------------------------------------
void sepp_app::create_exchange_capability() {
  if (!m_sepp_n32c_handshake_inst) {
    Logger::sepp_app().warn("N32-c Handshake instance is not initialized");
    return;
  }

  nlohmann::json req_body;
  if (!m_sepp_n32c_handshake_inst->create_exchange_capability_req(req_body)) {
    Logger::sepp_app().warn("Failed to build exchange capability request body");
    return;
  }

  if (!m_remote_sepp_url) {
    Logger::sepp_app().warn("Remote SEPP URL is not set");
    return;
  }

  std::string target_endpoint =
      *m_remote_sepp_url + "/n32c-handshake/v1/exchange-capability";
  Logger::sepp_app().info(
      "Initiating N32-c capability exchange with peer SEPP: %s",
      target_endpoint.c_str());

  auto request = http_client_inst_nbi->prepare_json_request(target_endpoint,
                                                            req_body.dump());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto http_response =
      http_client_inst_nbi->send_http_request(method_e::POST, request);

  Logger::sepp_app().info("Received HTTP response with status code: %ld",
                          http_response.status_code);

  if (http_response.status_code == http_status_code::OK ||
      http_response.status_code == http_status_code::CREATED) {
    try {
      nlohmann::json resp_body = nlohmann::json::parse(http_response.body);

      SecNegotiateRspData sec_rsp;
      from_json(resp_body, sec_rsp);
      std::stringstream err_ss;
      if (!sec_rsp.validate(err_ss)) {
        Logger::sepp_app().warn("SecNegotiateRspData validation warning: %s",
                                err_ss.str().c_str());
      }

      if (m_sepp_n32c_handshake_inst->handle_exchange_capability_response(
              resp_body)) {
        Logger::sepp_app().info("N32-c capability exchange succeeded with %s",
                                target_endpoint.c_str());

        if (m_sepp_n32c_handshake_inst->m_selected_sec_capability == "PRINS") {
          Logger::sepp_app().info(
              "Security capability is PRINS. Performing Parameter Exchange...");
          create_exchange_params();
        }
      }
    } catch (const std::exception &e) {
      Logger::sepp_app().warn(
          "Failed to parse or validate N32-c capability response: %s",
          e.what());
    }
  } else {
    Logger::sepp_app().warn(
        "N32-c capability exchange failed. Response code: %ld",
        http_response.status_code);
  }
}

//------------------------------------------------------------------------------
void sepp_app::create_exchange_params() {
  if (!m_sepp_n32c_handshake_inst || !m_remote_sepp_url) {
    Logger::sepp_app().warn("N32-c Handshake instance or peer URL is not set");
    return;
  }

  nlohmann::json req_body;
  if (!m_sepp_n32c_handshake_inst->create_exchange_params_req(req_body)) {
    Logger::sepp_app().warn("Failed to create exchange params request body");
    return;
  }

  std::string target_endpoint =
      *m_remote_sepp_url + "/n32c-handshake/v1/exchange-params";
  Logger::sepp_app().info("Initiating N32-c parameter exchange with %s",
                          target_endpoint.c_str());

  auto request = http_client_inst_nbi->prepare_json_request(target_endpoint,
                                                            req_body.dump());
  auto http_response =
      http_client_inst_nbi->send_http_request(method_e::POST, request);

  Logger::sepp_app().info("Parameter exchange status code: %ld",
                          http_response.status_code);

  if (http_response.status_code == http_status_code::OK ||
      http_response.status_code == http_status_code::CREATED) {
    try {
      nlohmann::json resp_body = nlohmann::json::parse(http_response.body);

      SecParamExchRspData param_rsp;
      from_json(resp_body, param_rsp);
      std::stringstream err_ss;
      if (!param_rsp.validate(err_ss)) {
        Logger::sepp_app().warn("SecParamExchRspData validation warning: %s",
                                err_ss.str().c_str());
      }

      if (m_sepp_n32c_handshake_inst->handle_exchange_params_response(
              resp_body)) {
        Logger::sepp_app().info(
            "N32-c Parameter Exchange successfully established");

        if (m_sepp_n32f_forward_inst) {
          m_sepp_n32f_forward_inst->m_n32f_context_id =
              m_sepp_n32c_handshake_inst->get_n32f_context_id();
        }
      }
    } catch (const std::exception &e) {
      Logger::sepp_app().warn("Failed to parse SecParamExchRspData: %s",
                              e.what());
    }
  } else {
    Logger::sepp_app().warn("N32-c parameter exchange failed. Status code: %ld",
                            http_response.status_code);
  }
}

//------------------------------------------------------------------------------
bool sepp_app::handle_exchange_params(
    const nlohmann::json &req_data, nlohmann::json &resp_data,
    std::unordered_map<std::string, std::string> &resp_headers) {
  if (!m_sepp_n32c_handshake_inst)
    return false;
  return m_sepp_n32c_handshake_inst->handle_exchange_params(req_data, resp_data,
                                                            resp_headers);
}

//------------------------------------------------------------------------------
bool sepp_app::handle_n32f_terminate_req(
    const nlohmann::json &req_data, nlohmann::json &resp_data,
    std::unordered_map<std::string, std::string> &resp_headers) {
  if (!m_sepp_n32c_handshake_inst)
    return false;

  try {
    N32fContextInfo term_info;
    from_json(req_data, term_info);
    Logger::sepp_app().info("Processing N32-F Termination for Context ID: %s",
                            term_info.getN32fContextId().c_str());
  } catch (const std::exception &e) {
    Logger::sepp_app().warn("Invalid N32fContextInfo in terminate request: %s",
                            e.what());
  }

  return m_sepp_n32c_handshake_inst->handle_n32f_terminate_req(
      req_data, resp_data, resp_headers);
}

//------------------------------------------------------------------------------
void sepp_app::send_n32f_terminate() {
  if (!m_sepp_n32c_handshake_inst || !m_remote_sepp_url) {
    return;
  }

  nlohmann::json req_body;
  if (!m_sepp_n32c_handshake_inst->create_n32f_terminate_req(req_body)) {
    return;
  }

  std::string target_endpoint =
      *m_remote_sepp_url + "/n32c-handshake/v1/n32f-terminate";
  Logger::sepp_app().info("Initiating N32-f termination towards: %s",
                          target_endpoint.c_str());

  auto request = http_client_inst_nbi->prepare_json_request(target_endpoint,
                                                            req_body.dump());
  auto http_response =
      http_client_inst_nbi->send_http_request(method_e::POST, request);

  if (http_response.status_code == http_status_code::OK ||
      http_response.status_code == http_status_code::NO_CONTENT) {
    nlohmann::json resp_body;
    if (!http_response.body.empty()) {
      try {
        resp_body = nlohmann::json::parse(http_response.body);
      } catch (...) {
        resp_body = nlohmann::json::object();
      }
    }
    m_sepp_n32c_handshake_inst->handle_n32f_terminate_response(resp_body);
  } else {
    Logger::sepp_app().warn("N32-f termination request failed with status: %ld",
                            http_response.status_code);
  }
}

//------------------------------------------------------------------------------
bool sepp_app::handle_n32f_error(
    const nlohmann::json &req_data, nlohmann::json &resp_data,
    std::unordered_map<std::string, std::string> &resp_headers) {
  if (!m_sepp_n32c_handshake_inst)
    return false;

  try {
    if (req_data.contains("n32fErrorDetail")) {
      N32fErrorDetail error_detail;
      from_json(req_data["n32fErrorDetail"], error_detail);
      std::stringstream err_ss;
      if (!error_detail.validate(err_ss)) {
        Logger::sepp_app().warn("N32fErrorDetail validation warn: %s",
                                err_ss.str().c_str());
      }
    }
  } catch (const std::exception &e) {
    Logger::sepp_app().warn("N32fErrorDetail model parsing failed: %s",
                            e.what());
  }

  return m_sepp_n32c_handshake_inst->handle_n32f_error(req_data, resp_data,
                                                       resp_headers);
}

//------------------------------------------------------------------------------
bool sepp_app::handle_nf_service_request(const std::string &authority,
                                         const std::string &path,
                                         const std::string &method,
                                         const std::string &body,
                                         oai::sepp::app::nf_http_response &resp_data) {
  if (!m_sepp_n32f_forward_inst) {
    Logger::sepp_app().warn("N32-f forward instance is not initialized");
    return false;
  }

  if (!m_remote_sepp_url) {
    Logger::sepp_app().error("Remote SEPP URL is not configured");
    return false;
  }

  Logger::sepp_app().info(
      "Forwarding NF service request: authority=%s, path=%s, method=%s",
      authority.c_str(), path.c_str(), method.c_str());

  if (m_sepp_n32c_handshake_inst &&
      m_sepp_n32c_handshake_inst->m_selected_sec_capability == "PRINS") {
    return handle_nf_service_request_prins(authority, path, method, body,
                                           resp_data);
  }

  return handle_nf_service_request_tls(authority, path, method, body,
                                       resp_data);
}

//------------------------------------------------------------------------------
bool sepp_app::handle_nf_service_request_prins(const std::string &authority,
                                               const std::string &path,
                                               const std::string &method,
                                               const std::string &body,
                                               oai::sepp::app::nf_http_response &resp_data) {
  nlohmann::json req_body;
  if (!m_sepp_n32f_forward_inst->create_n32f_process_request(
          authority, path, method, body, req_body)) {
    Logger::sepp_app().warn(
        "PRINS: Failed to build N32-f process request body");
    return false;
  }

  const std::string target_endpoint =
      *m_remote_sepp_url + "/n32f-forward/v1/n32f-process";

  auto request = http_client_inst_nbi->prepare_json_request(target_endpoint,
                                                            req_body.dump());
  auto http_response =
      http_client_inst_nbi->send_http_request(method_e::POST, request);

  Logger::sepp_app().info("PRINS: Received HTTP response status: %ld",
                          http_response.status_code);

  if (http_response.status_code != http_status_code::OK &&
      http_response.status_code != http_status_code::CREATED) {
    Logger::sepp_app().error(
        "PRINS: N32-f process endpoint returned error status: %ld",
        http_response.status_code);
    return false;
  }

  try {
    nlohmann::json http_resp_json = nlohmann::json::parse(http_response.body);

    N32fReformattedRspMsg rsp_msg;
    from_json(http_resp_json, rsp_msg);
    const auto &reformatted_data = rsp_msg.getReformattedData();

    const std::string &ciphertext_b64 = reformatted_data.getCiphertext();
    const std::string &aad_b64 = reformatted_data.getAad();
    const std::string &iv_b64 = reformatted_data.getIv();
    const std::string &tag_b64 = reformatted_data.getTag();

    std::string decrypted_body;
    if (!ciphertext_b64.empty()) {
      const auto &enc_key = m_sepp_n32f_forward_inst->m_enc_key;
      if (!enc_key.empty() && !iv_b64.empty() && !tag_b64.empty()) {
        if (!jose::decrypt_jwe_payload(ciphertext_b64, aad_b64, iv_b64, tag_b64,
                                       enc_key, decrypted_body)) {
          Logger::sepp_app().error("PRINS: Failed to decrypt JOSE payload");
          return false;
        }
      } else {
        decrypted_body = jose::base64url_decode(ciphertext_b64);
      }
      Logger::sepp_app().debug("PRINS: Decrypted JOSE response: %s",
                               decrypted_body.c_str());
    }

    std::string status, response_path, response_authority;
    if (!m_sepp_n32f_forward_inst->parse_aad(jose::base64url_decode(aad_b64),
            status, response_path, response_authority, resp_data.headers)) return false;
    resp_data.status_code = std::stoi(status);
    if (resp_data.status_code < 100 || resp_data.status_code > 599) return false;
    resp_data.body = decrypted_body;
    return true;
  } catch (const std::exception &e) {
    Logger::sepp_app().warn(
        "PRINS: Failed to parse/decode N32-f process response: %s", e.what());
    return false;
  }
}

//------------------------------------------------------------------------------
bool sepp_app::handle_nf_service_request_tls(const std::string &authority,
                                             const std::string &path,
                                             const std::string &method,
                                             const std::string &body,
                                             oai::sepp::app::nf_http_response &resp_data) {
  const std::string target_uri = *m_remote_sepp_url + path;

  method_e http_method = method_e::GET;
  if (method == "POST")
    http_method = method_e::POST;
  else if (method == "PUT")
    http_method = method_e::PUT;
  else if (method == "DELETE")
    http_method = method_e::DELETE;
  else if (method == "PATCH")
    http_method = method_e::PATCH;

  oai::http::request req;
  if (!body.empty()) {
    req = http_client_inst_nbi->prepare_json_request(target_uri, body);
  } else {
    req.uri = target_uri;
  }

  req.headers["Content-Type"] = "application/json";
  req.headers["Accept"] = "application/json";
  if (!authority.empty()) {
    req.headers["Authority"] = authority;
    std::string default_scheme = sepp_cfg->is_tls_enabled() ? "https://" : "http://";
    std::string api_root = (authority.rfind("http://", 0) != 0 &&
                            authority.rfind("https://", 0) != 0)
                               ? default_scheme + authority
                               : authority;
    req.headers["3gpp-Sbi-Target-apiRoot"] = api_root;
  }

  auto http_response =
      http_client_inst_nbi->send_http_request(http_method, req);

  Logger::sepp_app().info("Direct HTTP response status: %ld",
                          http_response.status_code);

  if (http_response.status_code == 0) return false;
  resp_data.status_code = http_response.status_code;
  resp_data.body = http_response.body;
  for (const auto& header : http_response.headers) resp_data.headers[header.first] = header.second;
  return true;
}

//------------------------------------------------------------------------------
bool sepp_app::handle_n32f_forward(
    const nlohmann::json &req_data, nlohmann::json &resp_data,
    std::unordered_map<std::string, std::string> &resp_headers) {
  if (!m_sepp_n32f_forward_inst)
    return false;
  return m_sepp_n32f_forward_inst->handle_n32f_forward(req_data, resp_data,
                                                       resp_headers);
}

//------------------------------------------------------------------------------
bool sepp_app::handle_n32f_process(
    const nlohmann::json &req_data, nlohmann::json &resp_data,
    std::unordered_map<std::string, std::string> &resp_headers) {
  if (!m_sepp_n32f_forward_inst)
    return false;
  return m_sepp_n32f_forward_inst->handle_n32f_process_post(req_data, resp_data,
                                                            resp_headers);
}

//------------------------------------------------------------------------------
void sepp_app::stop() {
  if (sepp_cfg && sepp_cfg->get_role_enum() == sepp_role_e::C_SEPP) {
    if (m_sepp_n32c_handshake_inst &&
        !m_sepp_n32c_handshake_inst->get_n32f_context_id().empty()) {
      Logger::sepp_app().info(
          "Stopping C-SEPP: Initiating N32-f context termination");
      send_n32f_terminate();
    }
  }

  if (m_sepp_nrf_inst) {
    m_sepp_nrf_inst->deregister_to_nrf();
  }
}

//------------------------------------------------------------------------------
bool sepp_app::get_telescopic_mapping(
    const std::string &foreignFqdn, const std::string &telescopicLabel,
    oai::_3gpp::model::TelescopicMapping &mapping_model) {
  if (!m_sepp_telescopic_fqdn_mapping)
    return false;
  return m_sepp_telescopic_fqdn_mapping->get_telescopic_mapping(
      foreignFqdn, telescopicLabel, mapping_model);
}

//------------------------------------------------------------------------------
bool sepp_app::get_telescopic_mapping(const std::string &foreignFqdn,
                                      const std::string &telescopicLabel,
                                      std::string &telescopicMapping) {
  if (!m_sepp_telescopic_fqdn_mapping)
    return false;
  return m_sepp_telescopic_fqdn_mapping->get_telescopic_mapping(
      foreignFqdn, telescopicLabel, telescopicMapping);
}

//------------------------------------------------------------------------------
sepp_app::~sepp_app() {
  Logger::sepp_app().debug("Delete SEPP_APP instance...");
}