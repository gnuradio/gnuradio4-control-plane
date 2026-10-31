#include "gr4cp/runtime/stub_runtime_manager.hpp"

#include <iostream>

namespace gr4cp::runtime {

namespace {

void log_action(const char* action, const domain::Session& session) {
    std::clog << "[stub-runtime] " << action << ' ' << session.id << '\n';
}

}  // namespace

void StubRuntimeManager::prepare(const domain::Session& session) {
    log_action("prepare", session);
    release_streams(session.id);
    if (const auto contract_error = domain::validate_stream_runtime_contract(session); contract_error.has_value()) {
        throw std::runtime_error(*contract_error);
    }
    if (auto plan = domain::derive_stream_runtime_plan(session); plan.has_value()) {
        auto& bindings = stream_bindings_[session.id];
        for (auto stream_plan : plan->streams) {
            auto internal = stream_plan.transport == "websocket" ? stream_allocator_.allocate_websocket()
                                                                  : stream_allocator_.allocate_http();
            stream_plan.ready = true;
            bindings.push_back(domain::RuntimeStreamBinding{
                .plan = stream_plan,
                .internal = internal,
                .browser = domain::to_browser_stream_descriptor(stream_plan),
            });
        }
    }
}

void StubRuntimeManager::start(const domain::Session& session) {
    log_action("start", session);
}

std::optional<std::string> StubRuntimeManager::stop(const domain::Session& session) {
    log_action("stop", session);
    release_streams(session.id);
    return std::nullopt;
}

void StubRuntimeManager::destroy(const domain::Session& session) {
    log_action("destroy", session);
    release_streams(session.id);
    settings_.erase(session.id);
}

std::optional<domain::StreamRuntimePlan> StubRuntimeManager::active_stream_plan(const domain::Session& session) {
    const auto it = stream_bindings_.find(session.id);
    if (it == stream_bindings_.end() || it->second.empty()) {
        return std::nullopt;
    }

    domain::StreamRuntimePlan plan;
    for (const auto& binding : it->second) {
        plan.streams.push_back(binding.plan);
    }
    return plan;
}

HttpStreamResponse StubRuntimeManager::fetch_http_stream(const domain::Session& session, const std::string& stream_id) {
    const auto it = stream_bindings_.find(session.id);
    if (it == stream_bindings_.end()) {
        throw std::runtime_error("session stream resources are not available");
    }
    for (const auto& binding : it->second) {
        if (binding.plan.stream_id == stream_id && binding.plan.transport != "websocket") {
            return HttpStreamResponse{
                .status = 200,
                .body = R"({"stub":true})",
                .content_type = "application/json",
            };
        }
    }
    throw std::runtime_error("stream not found in active session resources: " + stream_id);
}

WebSocketStreamRoute StubRuntimeManager::resolve_websocket_stream(const domain::Session& session, const std::string& stream_id) {
    const auto it = stream_bindings_.find(session.id);
    if (it == stream_bindings_.end()) {
        throw std::runtime_error("session stream resources are not available");
    }
    for (const auto& binding : it->second) {
        if (binding.plan.stream_id == stream_id && binding.plan.transport == "websocket") {
            return WebSocketStreamRoute{.internal = binding.internal};
        }
    }
    throw std::runtime_error("stream not found in active session resources: " + stream_id);
}

void StubRuntimeManager::set_block_settings(const domain::Session& session,
                                            const std::string& block_unique_name,
                                            const gr::property_map& patch,
                                            BlockSettingsMode) {
    log_action("set_block_settings", session);
    auto& block_settings = settings_[session.id][block_unique_name];
    for (const auto& [key, value] : patch) {
        block_settings.insert_or_assign(key, value);
    }
}

gr::property_map StubRuntimeManager::get_block_settings(const domain::Session& session,
                                                        const std::string& block_unique_name) {
    log_action("get_block_settings", session);
    const auto session_it = settings_.find(session.id);
    if (session_it == settings_.end()) {
        return {};
    }
    const auto block_it = session_it->second.find(block_unique_name);
    if (block_it == session_it->second.end()) {
        return {};
    }
    return block_it->second;
}

void StubRuntimeManager::release_streams(const std::string& session_id) {
    const auto it = stream_bindings_.find(session_id);
    if (it == stream_bindings_.end()) {
        return;
    }
    for (const auto& binding : it->second) {
        stream_allocator_.release(binding.internal);
    }
    stream_bindings_.erase(it);
}

}  // namespace gr4cp::runtime
