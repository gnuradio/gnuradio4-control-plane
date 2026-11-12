#pragma once

#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/scheduler_catalog_service.hpp"
#include "gr4cp/app/session_service.hpp"

#include <memory>
#include <string>

namespace gr4cp::api {

// Serves proto/gr4cp/v1/control_plane.proto over gRPC, next to the REST API and
// over the same application services.
class GrpcServer {
public:
  GrpcServer(app::SessionService &session_service,
             app::BlockCatalogService &block_catalog_service,
             app::SchedulerCatalogService &scheduler_catalog_service,
             app::BlockSettingsService &block_settings_service);
  ~GrpcServer();

  // Starts serving on `host`:`port` without transport security. Port 0 selects
  // a free port. Returns the port in use, or 0 when the address could not be
  // bound or the server already runs.
  int start(const std::string &host, int port);
  // Ends the calls in progress and stops serving. Does nothing when the server
  // does not run.
  void stop();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gr4cp::api
