#include "gr4cp/api/wasm_api.hpp"

#include <string>

#if defined(GR4CP_HAVE_GNURADIO4)
#include "gr4cp/catalog/gr4_block_catalog_provider.hpp"
#include "gr4cp/catalog/gr4_scheduler_catalog_provider.hpp"
#include "gr4cp/runtime/gr4_runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

// Generated from GR4CP_WASM_BLOCK_LIBRARIES; see cmake/wasm_block_registration.hpp.in.
#include "gr4cp/wasm_block_registration.hpp"
#endif

#include <emscripten/bind.h>

namespace {

using gr4cp::api::WasmResponse;

#if defined(GR4CP_HAVE_GNURADIO4)

// Same wiring src/main.cpp performs before handing the services to HttpServer. Declaration order
// matters: every service below borrows the members declared above it.
struct ControlPlane {
    gr4cp::storage::InMemorySessionRepository repository;
    gr4cp::runtime::Gr4RuntimeManager runtime_manager;
    gr4cp::app::SessionService session_service{repository, runtime_manager};
    gr4cp::app::SessionStreamService session_stream_service;
    gr4cp::app::BlockSettingsService block_settings_service{repository, runtime_manager};
    gr4cp::catalog::Gr4BlockCatalogProvider block_catalog_provider;
    gr4cp::app::BlockCatalogService block_catalog_service{block_catalog_provider};
    gr4cp::catalog::Gr4SchedulerCatalogProvider scheduler_catalog_provider;
    gr4cp::app::SchedulerCatalogService scheduler_catalog_service{scheduler_catalog_provider};
    gr4cp::api::WasmApi api{session_service,
                            session_stream_service,
                            block_catalog_service,
                            scheduler_catalog_service,
                            block_settings_service};
};

ControlPlane& control_plane() {
    [[maybe_unused]] static const bool blocks_registered = [] {
        gr4cp::wasm::register_linked_blocks();
        return true;
    }();
    static ControlPlane instance;
    return instance;
}

WasmResponse handle_request(const std::string& method, const std::string& target, const std::string& body) {
    return control_plane().api.dispatch(method, target, body);
}

#else

WasmResponse handle_request(const std::string&, const std::string&, const std::string&) {
    return WasmResponse{
        501,
        "application/json",
        R"({"error":{"code":"not_implemented","message":"built without GNU Radio 4 catalog support"}})"};
}

#endif

WasmResponse initialize() {
    return handle_request("GET", "/healthz", "");
}

}  // namespace

EMSCRIPTEN_BINDINGS(gr4cp_control_plane) {
    emscripten::value_object<WasmResponse>("Gr4cpResponse")
        .field("status", &WasmResponse::status)
        .field("contentType", &WasmResponse::content_type)
        .field("body", &WasmResponse::body);

    emscripten::function("handleRequest", &handle_request);
    emscripten::function("initialize", &initialize);
}

// This exists only to give the generated module a conventional entry point
int main() {
    return 0;
}
