/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "sepp-http2-sbi-server.h"
#include "sepp_config.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/thread.hpp>
#include <boost/thread/future.hpp>
#include <nlohmann/json.hpp>
#include <string>

#include "3gpp_29.500.h"
#include "ServiceName_anyOf.h"
#include "TelescopicMapping.h"
#include "logger.hpp"
#include "sbi_helper.hpp"
#include "sepp_app.hpp"

using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;
using namespace oai::_3gpp::model;
using namespace oai::sepp::api;
using namespace oai::common::sbi;
using namespace oai::sepp::app;

extern std::unique_ptr<oai::config::sepp::sepp_config> sepp_cfg;
extern std::unique_ptr<oai::sepp::app::sepp_app> sepp_app_inst;

namespace {
static const header_map JSON_HEADERS{
    {"content-type", header_value{"application/json"}}};
static const header_map PROBLEM_HEADERS{
    {"content-type", header_value{"application/problem+json"}}};
} // namespace

//------------------------------------------------------------------------------
void sepp_http2_sbi_server::start() {
  boost::system::error_code ec;

  Logger::sepp_sbi().info("+++++++ SBI HTTP2 TLS server being started +++++++");

  // ------------------------------------------------------------
  // Telescopic FQDN Mapping API
  // ------------------------------------------------------------
  server.handle("/nsepp-telescopic/v1/mapping", [&](const request &request,
                                                    const response &response) {
    if (request.method() != "GET") {
      nlohmann::json problemDetails = {
          {"status", oai::common::sbi::http_status_code::METHOD_NOT_ALLOWED},
          {"title", "Method Not Allowed"},
          {"cause", "ONLY_GET_SUPPORTED"},
          {"detail", "Only HTTP GET method is supported for this endpoint"}};
      response.write_head(
          oai::common::sbi::http_status_code::METHOD_NOT_ALLOWED,
          PROBLEM_HEADERS);
      response.end(problemDetails.dump());
      return;
    }

    request.on_data([&](const uint8_t *data, std::size_t len) {
      if (len > 0)
        return;

      const std::string &qs = request.uri().raw_query;
      std::string foreignFqdn = oai::utils::get_query_param(qs, "foreign-fqdn");
      std::string telescopicLabel =
          oai::utils::get_query_param(qs, "telescopic-label");

      if (!foreignFqdn.empty() && !telescopicLabel.empty()) {
        nlohmann::json problemDetails = {
            {"status", oai::common::sbi::http_status_code::BAD_REQUEST},
            {"title", "400: Bad Request"},
            {"cause", "INVALID_QUERY_PARAM"},
            {"detail", "foreign-fqdn and telescopic-label shall not be "
                       "supplied together"}};
        response.write_head(oai::common::sbi::http_status_code::BAD_REQUEST,
                            PROBLEM_HEADERS);
        response.end(problemDetails.dump());
        return;
      }

      if (foreignFqdn.empty() && telescopicLabel.empty()) {
        nlohmann::json problemDetails = {
            {"status", oai::common::sbi::http_status_code::BAD_REQUEST},
            {"title", "400: Bad Request"},
            {"cause", "MISSING_QUERY_PARAM"},
            {"detail",
             "Either foreign-fqdn or telescopic-label shall be supplied"}};
        response.write_head(oai::common::sbi::http_status_code::BAD_REQUEST,
                            PROBLEM_HEADERS);
        response.end(problemDetails.dump());
        return;
      }

      if (!sepp_app_inst) {
        nlohmann::json problemDetails = {
            {"status", oai::common::sbi::http_status_code::SERVICE_UNAVAILABLE},
            {"title", "503: Service Unavailable"},
            {"cause", "SYSTEM_UNAVAILABLE"},
            {"detail", "SEPP Application instance is not available"}};
        response.write_head(
            oai::common::sbi::http_status_code::SERVICE_UNAVAILABLE,
            PROBLEM_HEADERS);
        response.end(problemDetails.dump());
        return;
      }

      try {
        oai::_3gpp::model::TelescopicMapping mapping_model;
        bool result = handle_telescopic_mapping(foreignFqdn, telescopicLabel,
                                                mapping_model);
        if (result) {
          nlohmann::json resp_json;
          to_json(resp_json, mapping_model);
          response.write_head(oai::common::sbi::http_status_code::OK,
                              JSON_HEADERS);
          response.end(resp_json.dump());
        } else {
          nlohmann::json problemDetails = {
              {"status", oai::common::sbi::http_status_code::NOT_FOUND},
              {"title", "404: Not Found"},
              {"cause", "MAPPING_NOT_FOUND"},
              {"detail", "Telescopic Mapping NOT found"}};
          response.write_head(oai::common::sbi::http_status_code::NOT_FOUND,
                              PROBLEM_HEADERS);
          response.end(problemDetails.dump());
        }
      } catch (const std::exception &e) {
        nlohmann::json problemDetails = {
            {"status",
             oai::common::sbi::http_status_code::INTERNAL_SERVER_ERROR},
            {"title", "500: Internal Server Error"},
            {"cause", "UNEXPECTED_SYSTEM_ERROR"},
            {"detail", e.what()}};
        response.write_head(
            oai::common::sbi::http_status_code::INTERNAL_SERVER_ERROR,
            PROBLEM_HEADERS);
        response.end(problemDetails.dump());
      }
    });
  });

  // ============================================================
  // GENERIC NF SERVICE HANDLER
  // ============================================================
  server.handle("/", [&](const request &req, const response &response) {
    auto body = std::make_shared<std::string>();
    req.on_data([this, body, &req, &response](const uint8_t *data,
                                              std::size_t len) {
      if (data && len > 0) {
        body->append(reinterpret_cast<const char *>(data), len);
      }

      if (len == 0) {
        std::string authority;

        auto target_api_root_it = req.header().find("3gpp-sbi-target-apiroot");
        if (target_api_root_it != req.header().end() &&
            !target_api_root_it->second.value.empty()) {
          std::string api_root = target_api_root_it->second.value;
          if (api_root.rfind("http://", 0) == 0) {
            authority = api_root.substr(7);
          } else if (api_root.rfind("https://", 0) == 0) {
            authority = api_root.substr(8);
          } else {
            authority = api_root;
          }
        }

        if (authority.empty()) {
          auto authority_it = req.header().find("authority");
          if (authority_it != req.header().end()) {
            authority = authority_it->second.value;
          } else {
            auto host_it = req.header().find("host");
            if (host_it != req.header().end()) {
              authority = host_it->second.value;
            }
          }
        }

        if (authority.empty()) {
          response.write_head(oai::common::sbi::http_status_code::BAD_REQUEST);
          response.end("Missing authority header");
          return;
        }

        std::string full_path = req.uri().path;
        if (!req.uri().raw_query.empty()) {
          full_path.reserve(full_path.size() + 1 + req.uri().raw_query.size());
          full_path += "?";
          full_path += req.uri().raw_query;
        }

        nf_http_response resp_data;
        if (handle_nf_service_request(authority, full_path, req.method(), *body, resp_data)) {
          header_map headers;
          for (const auto& [key, value] : resp_data.headers) {
            if (key == "content-type" || key == "location" || key == "retry-after")
              headers.emplace(key, header_value{value});
          }
          response.write_head(resp_data.status_code, headers);
          response.end(resp_data.status_code == 204 ? "" : resp_data.body);
        } else {
          response.write_head(
              oai::common::sbi::http_status_code::BAD_GATEWAY);
          response.end("NF service forwarding failed");
        }
      }
    });
  });

  running_server = true;
  if (server.listen_and_serve(ec, m_address, std::to_string(m_port))) {
    Logger::sepp_sbi().error("SBI HTTPS/HTTP2 server error: %s",
                             ec.message().c_str());
  }

  running_server = false;
  Logger::sepp_sbi().info("SBI HTTP2 TLS server fully stopped");
}
//------------------------------------------------------------------------------
bool sepp_http2_sbi_server::handle_telescopic_mapping(
    const std::string &foreignFqdn, const std::string &telescopicLabel,
    oai::_3gpp::model::TelescopicMapping &mapping_model) {
  if (!sepp_app_inst)
    return false;

  std::string telescopicMapping;
  if (!sepp_app_inst->get_telescopic_mapping(foreignFqdn, telescopicLabel,
                                             telescopicMapping)) {
    return false;
  }

  mapping_model.setTelescopicLabel(telescopicLabel);
  mapping_model.setForeignFqdn(foreignFqdn);

  return true;
}
//------------------------------------------------------------------------------
bool sepp_http2_sbi_server::handle_nf_service_request(
    const std::string &authority, const std::string &path,
    const std::string &method, const std::string &body,
    oai::sepp::app::nf_http_response &resp_data) {
  return sepp_app_inst->handle_nf_service_request(authority, path, method, body,
                                                  resp_data);
}

//------------------------------------------------------------------------------
void sepp_http2_sbi_server::get_api_list(const response &response) {
  nlohmann::json json_data;
  if (sepp_cfg->get_api_list(json_data)) {
    response.write_head(oai::common::sbi::http_status_code::OK, JSON_HEADERS);
    response.end(json_data.dump());
  } else {
    response.write_head(oai::common::sbi::http_status_code::SERVICE_UNAVAILABLE,
                        JSON_HEADERS);
    response.end();
  }
}

//------------------------------------------------------------------------------
void sepp_http2_sbi_server::stop() {
  server.stop();
  while (running_server) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Logger::sepp_sbi().info("HTTP2 server should be fully stopped");
}