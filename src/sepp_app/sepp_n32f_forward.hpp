/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_SEPP_N32F_FORWARD_HPP_SEEN
#define FILE_SEPP_N32F_FORWARD_HPP_SEEN

#pragma once

#include "FlatJweJson.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace oai::sepp::app {

// Struct representing the unencapsulated HTTP request to be forwarded
struct nf_http_request {
  std::string method;
  std::string path;
  std::string authority;
  std::string body;
  std::string mcc;
  std::string mnc;
  std::unordered_map<std::string, std::string> headers;
};

// Struct representing the response received from target local NF
struct nf_http_response {
  int status_code{200};
  std::string body;
  std::unordered_map<std::string, std::string> headers;
};

class sepp_n32f_forward {
  friend class sepp_app;

public:
  using nf_forward_callback =
      std::function<bool(const nf_http_request &, nf_http_response &)>;

  sepp_n32f_forward() = default;
  ~sepp_n32f_forward() = default;

  void set_enc_key(std::vector<uint8_t> key) { m_enc_key = std::move(key); }
  void set_sign_key(std::vector<uint8_t> key) { m_sign_key = std::move(key); }

  void set_selected_sec_capability(std::string sec_cap) {
    m_security_capability = std::move(sec_cap);
  }

  [[nodiscard]] std::string get_selected_sec_capability() const {
    return m_security_capability;
  }

  void set_local_nrf_uri(std::string uri) { m_local_nrf_uri = std::move(uri); }

  void register_nf_forward_callback(nf_forward_callback cb) {
    m_nf_forward_cb = std::move(cb);
  }

  bool handle_n32f_forward(
      const nlohmann::json &req_data, nlohmann::json &resp_data,
      std::unordered_map<std::string, std::string> &resp_headers);

  // 3GPP TS 29.573 POST /n32f-forward/v1/n32f-process
  bool handle_n32f_process_post(
      const nlohmann::json &req_data, nlohmann::json &resp_data,
      std::unordered_map<std::string, std::string> &resp_headers);

  // 3GPP TS 29.573 OPTIONS /n32f-forward/v1/n32f-process
  bool handle_n32f_process_options(nlohmann::json &resp_data);

  bool create_n32f_process_request(const std::string &authority,
                                   const std::string &path,
                                   const std::string &method,
                                   const std::string &body,
                                   nlohmann::json &req_body);

private:
  bool validate_n32f_message(const nlohmann::json &req_data) const;

  bool send_http_request_to_target(
      const std::string &authority, const std::string &path,
      const std::string &method_str, const std::string &payload,
      const std::unordered_map<std::string, std::string> &headers,
      nlohmann::json &response_json, long &status_code,
      nf_http_response* raw_response = nullptr);

  bool build_reformatted_data(const std::string &method_or_status,
                              const std::string &path,
                              const std::string &authority,
                              const std::string &body,
                              const std::string &context_id,
                              oai::_3gpp::model::FlatJweJson &out_reformatted,
                              const std::unordered_map<std::string, std::string>& response_headers = {});

  bool parse_aad(const std::string &decoded_aad, std::string &method,
                 std::string &path, std::string &authority,
                 std::unordered_map<std::string, std::string> &headers) const;

  std::string m_n32f_context_id;
  std::string m_peer_sepp_fqdn;
  std::string m_local_nrf_uri{"http://127.0.0.1:8080"};
  std::string m_security_capability{"PRINS"};

  std::vector<uint8_t> m_enc_key;  // AES-128 GCM key
  std::vector<uint8_t> m_sign_key; // HMAC-SHA256 key

  nf_forward_callback m_nf_forward_cb{nullptr};
};

} // namespace oai::sepp::app

#endif /* FILE_SEPP_N32F_FORWARD_HPP_SEEN */