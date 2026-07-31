#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/scheduler_catalog_service.hpp"
#include "gr4cp/app/session_service.hpp"
#include "gr4cp/app/session_stream_service.hpp"
#include "gr4cp/api/http_server.hpp"
#include "gr4cp/catalog/gr4_block_catalog_provider.hpp"
#include "gr4cp/catalog/gr4_scheduler_catalog_provider.hpp"
#include "gr4cp/runtime/gr4_runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <httplib.h>

namespace {

[[noreturn]] void report_terminate() noexcept {
    std::cerr << "gr4cp_server: fatal: std::terminate called";
    const auto exception = std::current_exception();
    if (exception) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            std::cerr << " with active exception: " << error.what();
        } catch (...) {
            std::cerr << " with an unknown active exception";
        }
    } else {
        std::cerr << " without an active exception";
    }
    std::cerr << '\n';
    std::cerr.flush();
    std::abort();
}

void write_port_file(const char* path, int port) {
    if (path == nullptr || path[0] == '\0') {
        return;
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error(std::string("failed to open GR4CP_PORT_FILE: ") + path);
    }
    output << port << '\n';
    output.close();
    if (!output) {
        throw std::runtime_error(std::string("failed to write GR4CP_PORT_FILE: ") + path);
    }
}

}  // namespace

int main() {
    std::set_terminate(report_terminate);

    try {
#if !defined(GR4CP_HAVE_GNURADIO4)
        std::cerr << "GNU Radio 4 catalog support is not built. Configure with GR4CP_ENABLE_GR4_CATALOG=ON "
                     "and a working gnuradio4 installation.\n";
        return 1;
#else
        gr4cp::storage::InMemorySessionRepository repository;
        gr4cp::runtime::Gr4RuntimeManager runtime_manager;
        gr4cp::app::SessionService session_service(repository, runtime_manager);
        gr4cp::app::SessionStreamService session_stream_service;
        gr4cp::app::BlockSettingsService block_settings_service(repository, runtime_manager);
        gr4cp::catalog::Gr4BlockCatalogProvider block_catalog_provider;
        gr4cp::app::BlockCatalogService block_catalog_service(block_catalog_provider);
        gr4cp::catalog::Gr4SchedulerCatalogProvider scheduler_catalog_provider;
        gr4cp::app::SchedulerCatalogService scheduler_catalog_service(scheduler_catalog_provider);

        gr4cp::api::HttpServer server(session_service,
                                      session_stream_service,
                                      block_catalog_service,
                                      scheduler_catalog_service,
                                      block_settings_service);

        const char* port_env = std::getenv("GR4CP_PORT");
        const int requested_port = port_env != nullptr ? std::stoi(port_env) : 8080;
        int listen_port = requested_port;

        if (requested_port == 0) {
            listen_port = server.bind_to_any_port("127.0.0.1");
            if (listen_port <= 0) {
                std::cerr << "gr4cp_server: failed to reserve an available loopback port\n";
                return 1;
            }
            write_port_file(std::getenv("GR4CP_PORT_FILE"), listen_port);
            std::cout << "Reserved 127.0.0.1:" << listen_port << '\n';
        }

        try {
            (void)block_catalog_service.list();
            (void)scheduler_catalog_service.list();
            std::cout << "Using GNU Radio 4-backed block catalog\n";
            std::cout << "Using GNU Radio 4-backed scheduler catalog\n";
        } catch (const gr4cp::catalog::CatalogLoadError& error) {
            std::cerr << "GNU Radio 4 catalog initialization failed: " << error.what() << '\n';
            return 1;
        } catch (const gr4cp::catalog::SchedulerCatalogLoadError& error) {
            std::cerr << "GNU Radio 4 scheduler catalog initialization failed: " << error.what() << '\n';
            return 1;
        }

        const std::string listen_host = requested_port == 0 ? "127.0.0.1" : "0.0.0.0";
        std::cout << "Listening on " << listen_host << ':' << listen_port << '\n';
        const bool listened =
            requested_port == 0 ? server.listen_after_bind() : server.listen(listen_host, listen_port);
        if (!listened) {
            std::cerr << "gr4cp_server: HTTP server stopped because listen failed on " << listen_host << ':'
                      << listen_port << '\n';
            return 1;
        }
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << "gr4cp_server: fatal unhandled exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "gr4cp_server: fatal unknown exception\n";
        return 1;
    }
}
