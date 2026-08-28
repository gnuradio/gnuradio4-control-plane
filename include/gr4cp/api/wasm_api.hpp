#pragma once

#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/scheduler_catalog_service.hpp"
#include "gr4cp/app/session_service.hpp"
#include "gr4cp/app/session_stream_service.hpp"

#include <string>

namespace gr4cp::api {

// Result of a WasmApi dispatch, shaped after an HTTP response so the browser side can keep
// treating the control plane as a REST API even though no socket is involved.
struct WasmResponse {
    int status{200};
    std::string content_type;
    std::string body;
};

// In-process replacement for HttpServer on Emscripten builds, where neither cpp-httplib nor
// Boost.Beast can open a listening socket. gnuradio4-studio calls dispatch() directly through
// the embind bindings instead of going over the network.
class WasmApi {
public:
    WasmApi(app::SessionService& session_service,
            app::SessionStreamService& session_stream_service,
            app::BlockCatalogService& block_catalog_service,
            app::SchedulerCatalogService& scheduler_catalog_service,
            app::BlockSettingsService& block_settings_service);

    // `target` is a request target such as "/sessions/s1/blocks/foo/settings?mode=immediate";
    // the query string is optional. Never throws: service failures come back as an error body
    // with the same envelope the HTTP server produces.
    WasmResponse dispatch(const std::string& method, const std::string& target, const std::string& body) const;

private:
    app::SessionService& session_service_;
    app::SessionStreamService& session_stream_service_;
    app::BlockCatalogService& block_catalog_service_;
    app::SchedulerCatalogService& scheduler_catalog_service_;
    app::BlockSettingsService& block_settings_service_;
};

}  // namespace gr4cp::api
