#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/scheduler_catalog_service.hpp"
#include "gr4cp/app/session_service.hpp"
#include "gr4cp/app/session_stream_service.hpp"
#include "gr4cp/api/http_server.hpp"
#include "gr4cp/catalog/gr4_block_catalog_provider.hpp"
#include "gr4cp/catalog/gr4_scheduler_catalog_provider.hpp"
#include "gr4cp/runtime/gr4_runtime_manager.hpp"
#include "gr4cp/runtime/plugin_loader.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <httplib.h>

namespace {

void configure_managed_stdio() {
    const char* stream_stdio = std::getenv("GR4CP_STREAM_STDIO");
    if (stream_stdio == nullptr || std::string_view(stream_stdio) != "1") {
        return;
    }

    (void)std::setvbuf(stdout, nullptr, _IOLBF, 0);
    (void)std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
}

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

void print_usage(std::ostream& output) {
    output << "Usage: gr4cp_server [--help]\n"
              "\n"
              "Serves the GNU Radio 4 control plane's REST API over HTTP.\n"
              "\n"
              "Environment:\n"
              "  GR4CP_PORT                    port to listen on, 8080 by default; 0 selects a free\n"
              "                                loopback port\n"
              "  GR4CP_PORT_FILE               file that receives the port GR4CP_PORT=0 selected\n"
              "  GNURADIO4_PLUGIN_DIRECTORIES  colon-separated plugin directories, searched first\n"
              "  GR4CP_STREAM_STDIO            1 for line-buffered standard output\n"
              "  GR4CP_STREAM_DEBUG            nonzero to log stream routing to standard error\n"
              "  GR4CP_PROXY_DEBUG             nonzero to log each proxied request to standard error\n";
#if defined(GR4CP_HAVE_GNURADIO4)
    output << "\nPlugin directories, in search order:\n";
    for (const auto& directory : gr4cp::runtime::plugin_directories()) {
        output << "  " << directory << '\n';
    }
#endif
}

}  // namespace

int main(int argc, char** argv) {
    configure_managed_stdio();
    std::set_terminate(report_terminate);

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            print_usage(std::cout);
            return 0;
        }
        std::cerr << "gr4cp_server: unknown argument: " << argument << '\n';
        print_usage(std::cerr);
        return 2;
    }

    try {
#if !defined(GR4CP_HAVE_GNURADIO4)
        std::cerr << "GNU Radio 4 catalog support is not built. Configure with GR4CP_ENABLE_GR4_CATALOG=ON "
                     "and a working gnuradio4 installation.\n";
        return 1;
#else
        std::shared_ptr<gr::PluginLoader> loader;
        try {
            loader = gr4cp::runtime::load_plugins();
        } catch (const gr4cp::runtime::PluginLoadError& error) {
            std::cerr << "gr4cp_server: " << error.what() << '\n';
            return 1;
        }

        gr4cp::storage::InMemorySessionRepository repository;
        gr4cp::runtime::Gr4RuntimeManager runtime_manager(loader);
        gr4cp::app::SessionService session_service(repository, runtime_manager);
        gr4cp::app::SessionStreamService session_stream_service;
        gr4cp::app::BlockSettingsService block_settings_service(session_service, runtime_manager);
        gr4cp::catalog::Gr4BlockCatalogProvider block_catalog_provider(loader);
        gr4cp::app::BlockCatalogService block_catalog_service(block_catalog_provider);
        gr4cp::catalog::Gr4SchedulerCatalogProvider scheduler_catalog_provider(loader);
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
