/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "sepp_n32f_forward.hpp"

#include <sstream>
#include <string_view>

#include "FlatJweJson.h"
#include "N32fContextInfo.h"
#include "N32fReformattedReqMsg.h"
#include "N32fReformattedRspMsg.h"
#include "http_client.hpp"
#include "logger.hpp"
#include "sepp_JOSE.hpp"
#include "sepp_config.hpp"

using namespace oai::config::sepp;
using namespace oai::_3gpp::model;

extern std::shared_ptr<oai::http::http_client> http_client_inst_nbi;
extern std::shared_ptr<oai::http::http_client> http_client_inst;
extern std::unique_ptr<sepp_config> sepp_cfg;

namespace oai::sepp::app {

static std::string url_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      auto hex_to_int = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      int hi = hex_to_int(in[i + 1]);
      int lo = hex_to_int(in[i + 2]);
      if (hi != -1 && lo != -1) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += (in[i] == '+') ? ' ' : in[i];
  }
  return out;
}

static void extract_plmn_from_path(std::string_view path, std::string &mcc,
                                   std::string &mnc) {
  auto pos = path.find('?');
  if (pos == std::string_view::npos)
    return;

  std::string_view query_str = path.substr(pos + 1);
  while (!query_str.empty()) {
    auto amp_pos = query_str.find('&');
    std::string_view param = (amp_pos == std::string_view::npos)
                                 ? query_str
                                 : query_str.substr(0, amp_pos);

    auto eq_pos = param.find('=');
    if (eq_pos != std::string_view::npos) {
      std::string_view key = param.substr(0, eq_pos);
      std::string_view val = param.substr(eq_pos + 1);

      if (key == "plmn-id" || key == "target-plmn-id") {
        std::string decoded_val = url_decode(val);
        try {
          nlohmann::json plmn_json = nlohmann::json::parse(decoded_val);
          mcc = plmn_json.value("mcc", "");
          mnc = plmn_json.value("mnc", "");
        } catch (...) {
          auto mcc_pos = decoded_val.find("\"mcc\":\"");
          if (mcc_pos != std::string::npos)
            mcc = decoded_val.substr(mcc_pos + 7, 3);
          auto mnc_pos = decoded_val.find("\"mnc\":\"");
          if (mnc_pos != std::string::npos) {
            auto end_pos = decoded_val.find('"', mnc_pos + 7);
            if (end_pos != std::string::npos) {
              mnc = decoded_val.substr(mnc_pos + 7, end_pos - (mnc_pos + 7));
            }
          }
        }
        return;
      }
    }
    if (amp_pos == std::string_view::npos)
      break;
    query_str.remove_prefix(amp_pos + 1);
  }
}

bool sepp_n32f_forward::handle_n32f_forward(
    const nlohmann::json &req_data, nlohmann::json &resp_data,
    std::unordered_map<std::string, std::string> &resp_headers) {
  if (!validate_n32f_message(req_data))
    return false;

  if (m_security_capability != "TLS") {
    resp_data = {{"status", 400},
                 {"title", "Bad Request"},
                 {"cause", "INVALID_CAPABILITY_ENDPOINT"},
                 {"detail", "Endpoint requires TLS capability"}};
    resp_headers["content-type"] = "application/problem+json";
    return false;
  }

  const std::string target_authority = req_data.value("authority", "");
  const std::string target_path =
      req_data.value("path", "") + "?" + req_data.value("query", "");

  std::unordered_map<std::string, std::string> forward_headers;
  if (req_data.contains("headers") && req_data["headers"].is_object()) {
    for (const auto &[k, v] : req_data["headers"].items()) {
      if (v.is_string()) {
        forward_headers[k] = v.get<std::string>();
      }
    }
  }

  long status_code = 200;
  if (!send_http_request_to_target(target_authority, target_path, "GET", "",
                                   forward_headers, resp_data, status_code)) {
    return false;
  }

  if (resp_data.empty() && status_code == 200) {
    resp_data = {{"status", "OK"}};
  }

  resp_headers["content-type"] = "application/json";
  return true;
}

bool sepp_n32f_forward::handle_n32f_process_post(
    const nlohmann::json &req_data, nlohmann::json &resp_data,
    std::unordered_map<std::string, std::string> &resp_headers) {
  if (!validate_n32f_message(req_data))
    return false;

  if (m_security_capability != "PRINS") {
    resp_data = {
        {"status", 400},
        {"title", "Bad Request"},
        {"cause", "INVALID_CAPABILITY_ENDPOINT"},
        {"detail", "Endpoint /n32f-process requires PRINS capability"}};
    resp_headers["content-type"] = "application/problem+json";
    return false;
  }

  N32fReformattedReqMsg req_msg;
  try {
    from_json(req_data, req_msg);
  } catch (const std::exception &e) {
    Logger::sepp_app().error("Failed to parse N32fReformattedReqMsg: %s",
                             e.what());
    return false;
  }

  const FlatJweJson &reformatted_data = req_msg.getReformattedData();

  std::string context_id;
  const nlohmann::json &unprotected = reformatted_data.getUnprotected();
  if (unprotected.is_object()) {
    try {
      N32fContextInfo ctx_info;
      from_json(unprotected, ctx_info);
      context_id = ctx_info.getN32fContextId();
    } catch (...) {
      context_id = unprotected.value("n32fContextId", "");
    }
  }
  if (context_id.empty() && req_data.contains("n32fContextId")) {
    context_id = req_data.value("n32fContextId", "");
  }

  if (context_id.empty())
    return false;

  std::string aad_b64 = reformatted_data.getAad();
  std::string decoded_aad = jose::base64url_decode(aad_b64);

  nf_http_request target_req;
  if (!parse_aad(decoded_aad, target_req.method, target_req.path,
                 target_req.authority, target_req.headers)) {
    return false;
  }

  extract_plmn_from_path(target_req.path, target_req.mcc, target_req.mnc);

  std::string ciphertext_b64 = reformatted_data.getCiphertext();
  if (!ciphertext_b64.empty()) {
    std::string iv_b64 = reformatted_data.getIv();
    std::string tag_b64 = reformatted_data.getTag();

    if (!m_enc_key.empty() && !iv_b64.empty() && !tag_b64.empty()) {
      if (!jose::decrypt_jwe_payload(ciphertext_b64, aad_b64, iv_b64, tag_b64,
                                     m_enc_key, target_req.body)) {
        return false;
      }
    } else {
      target_req.body = jose::base64url_decode(ciphertext_b64);
    }
    Logger::sepp_app().info("Decrypted JOSE incoming payload: %s",
                            target_req.body.c_str());
  }

  nlohmann::json nf_resp_json;
  nf_http_response producer_response;
  long status_code = 200;
  if (!send_http_request_to_target(
          target_req.authority, target_req.path, target_req.method,
          target_req.body, target_req.headers, nf_resp_json, status_code, &producer_response)) {
    return false;
  }

  FlatJweJson resp_reformatted;
  if (!build_reformatted_data(std::to_string(status_code), target_req.path,
                              target_req.authority, producer_response.body,
                              context_id, resp_reformatted, producer_response.headers)) {
    return false;
  }

  N32fReformattedRspMsg resp_msg;
  resp_msg.setReformattedData(resp_reformatted);
  to_json(resp_data, resp_msg);

  resp_headers["content-type"] = "application/json";
  return true;
}

bool sepp_n32f_forward::handle_n32f_process_options(nlohmann::json &resp_data) {
  resp_data["supportedFeatures"] = "1";
  resp_data["supportedEncAlgs"] = nlohmann::json::array({"A128GCM", "A256GCM"});
  resp_data["supportedIpxs"] = nlohmann::json::array();
  return true;
}

bool sepp_n32f_forward::create_n32f_process_request(
    const std::string &authority, const std::string &path,
    const std::string &method, const std::string &body,
    nlohmann::json &req_body) {
  if (m_security_capability == "TLS")
    return true;

  const std::string &ctx_id =
      m_n32f_context_id.empty() ? "dummy_context_id" : m_n32f_context_id;

  FlatJweJson reformatted_data;
  if (!build_reformatted_data(method, path, authority, body, ctx_id,
                              reformatted_data)) {
    return false;
  }

  N32fReformattedReqMsg req_msg;
  req_msg.setReformattedData(reformatted_data);
  to_json(req_body, req_msg);

  return true;
}

bool sepp_n32f_forward::build_reformatted_data(
    const std::string &method_or_status, const std::string &path,
    const std::string &authority, const std::string &body,
    const std::string &context_id, FlatJweJson &out_reformatted,
    const std::unordered_map<std::string, std::string>& response_headers) {

  std::string aad_raw = method_or_status + " " + path + " HTTP/2.0\r\n";
  if (!authority.empty()) {
    aad_raw += "authority: " + authority + "\r\n";
    std::string api_root = authority;
    if (api_root.rfind("https://", 0) == 0) {
      api_root.replace(0, 8, "http://");
    } else if (api_root.rfind("http://", 0) != 0) {
      api_root = "http://" + api_root;
    }
    aad_raw += "3gpp-sbi-target-apiroot: " + api_root + "\r\n";
  }

  for (const auto& [key, value] : response_headers) {
    if (key.find_first_of("\r\n") == std::string::npos && value.find_first_of("\r\n") == std::string::npos)
      aad_raw += key + ": " + value + "\r\n";
  }
  std::string aad_b64 = jose::base64url_encode(aad_raw);

  nlohmann::json protected_hdr = {
      {"alg", m_enc_key.empty() ? "dir" : "A128GCMKW"},
      {"enc", "A128GCM"},
      {"n32fMsgId", "1"}};

  N32fContextInfo ctx_info;
  ctx_info.setN32fContextId(context_id);
  nlohmann::json unprotected_json;
  to_json(unprotected_json, ctx_info);

  out_reformatted.setRProtected(jose::base64url_encode(protected_hdr.dump()));
  out_reformatted.setAad(aad_b64);
  out_reformatted.setUnprotected(unprotected_json);
  out_reformatted.setCiphertext("");

  if (!body.empty()) {
    if (!m_enc_key.empty()) {
      std::string iv_b64, ciphertext_b64, tag_b64;
      if (jose::apply_jwe_encryption(body, aad_b64, m_enc_key, iv_b64,
                                     ciphertext_b64, tag_b64)) {
        out_reformatted.setIv(iv_b64);
        out_reformatted.setCiphertext(ciphertext_b64);
        out_reformatted.setTag(tag_b64);
      } else {
        return false;
      }
    } else {
      out_reformatted.setCiphertext(jose::base64url_encode(body));
    }
  }

  return true;
}

bool sepp_n32f_forward::parse_aad(
    const std::string &decoded_aad, std::string &method, std::string &path,
    std::string &authority,
    std::unordered_map<std::string, std::string> &headers) const {
  std::istringstream stream(decoded_aad);
  std::string line;

  if (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::istringstream line_stream(line);
    line_stream >> method >> path;
  } else {
    return false;
  }

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto pos = line.find(':');
    if (pos != std::string::npos) {
      std::string header_name = line.substr(0, pos);
      std::string header_value = line.substr(pos + 1);
      if (!header_value.empty() && header_value.front() == ' ') {
        header_value.erase(0, 1);
      }
      if (header_name == "authority")
        authority = header_value;
      headers[std::move(header_name)] = std::move(header_value);
    }
  }

  return true;
}

bool sepp_n32f_forward::send_http_request_to_target(
    const std::string &authority, const std::string &path,
    const std::string &method_str, const std::string &payload,
    const std::unordered_map<std::string, std::string> &headers,
    nlohmann::json &response_json, long &status_code,
    nf_http_response* raw_response) {

  std::string target_endpoint;
  auto target_it = headers.find("3gpp-sbi-target-apiroot");
  if (target_it != headers.end() && !target_it->second.empty()) {
    target_endpoint = target_it->second;
  } else if (!authority.empty()) {
    target_endpoint = authority;
  } else {
    target_endpoint =
        sepp_cfg->get_nf(config::NRF_CONFIG_NAME)->get_sbi().get_url();
  }

  // Force plain HTTP for local NFs (strip https:// if present)
  if (target_endpoint.rfind("https://", 0) == 0) {
    target_endpoint.replace(0, 8, "http://");
  } else if (target_endpoint.rfind("http://", 0) != 0) {
    target_endpoint = "http://" + target_endpoint;
  }

  Logger::sepp_app().info("Sending local HTTP request to target NF: %s%s",
                          target_endpoint.c_str(), path.c_str());

  oai::http::request req;
  req.uri = target_endpoint + path;

  method_e method = method_e::GET;
  if (method_str == "POST")
    method = method_e::POST;
  else if (method_str == "PUT")
    method = method_e::PUT;
  else if (method_str == "DELETE")
    method = method_e::DELETE;
  else if (method_str == "PATCH")
    method = method_e::PATCH;

  auto &client_inst = http_client_inst;

  if (!payload.empty()) {
    req = client_inst->prepare_json_request(req.uri, payload);
  }

  for (const auto &[hdr_name, hdr_val] : headers) {
    if (hdr_name != "authority" && hdr_name != "3gpp-sbi-target-apiroot") {
      req.headers[hdr_name] = hdr_val;
    }
  }

  Logger::sepp_app().info("Prepared local NF HTTP request: %s", req.to_string().c_str());

  auto http_response = client_inst->send_http_request(method, req);
  status_code = http_response.status_code;
  if (status_code == 0) return false;
  if (raw_response) {
    raw_response->status_code = status_code;
    raw_response->body = http_response.body;
    for (const auto& [key, value] : http_response.headers)
      if (key == "location" || key == "content-type" || key == "retry-after")
        raw_response->headers[key] = value;
  }

  Logger::sepp_app().info("Received local NF HTTP response with status code: %ld",
                          status_code);

  if (!http_response.body.empty()) {
    try {
      response_json = http_response.get_json();
    } catch (...) {
      response_json = {{"body", http_response.body}};
    }
  } else {
    response_json = nlohmann::json::object();
  }
  return true;
}

bool sepp_n32f_forward::validate_n32f_message(
    const nlohmann::json &req_data) const {
  return !req_data.is_null() && req_data.is_object();
}

} // namespace oai::sepp::app