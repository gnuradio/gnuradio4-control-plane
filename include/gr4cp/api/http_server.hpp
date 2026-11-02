#pragma once

#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/scheduler_catalog_service.hpp"
#include "gr4cp/app/session_service.hpp"
#include "gr4cp/app/session_stream_service.hpp"

#include <memory>
#include <string>

namespace httplib {
class Server;
}

namespace gr4cp::api {

void register_routes(httplib::Server& server,
                     app::SessionService& session_service,
                     app::SessionStreamService& session_stream_service,
                     app::BlockCatalogService& block_catalog_service,
                     app::SchedulerCatalogService& scheduler_catalog_service,
                     app::BlockSettingsService& block_settings_service);

class HttpServer {
public:
    HttpServer(app::SessionService& session_service,
               app::SessionStreamService& session_stream_service,
               app::BlockCatalogService& block_catalog_service,
               app::SchedulerCatalogService& scheduler_catalog_service,
               app::BlockSettingsService& block_settings_service);
    ~HttpServer();

    int bind_to_any_port(const std::string& host);
    bool listen_after_bind();
    bool listen(const std::string& host, int port);
    void stop();

private:
    friend struct HttpServerTestAccess;
    std::size_t connection_worker_count() const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gr4cp::api
