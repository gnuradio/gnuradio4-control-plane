#pragma once

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gr4cp::test {

// Produces `n_samples` zeros and ends.
struct FiniteSource : gr::Block<FiniteSource> {
    gr::PortOut<float> out;
    gr::Size_t n_samples = 4096U;

    GR_MAKE_REFLECTABLE(FiniteSource, out, n_samples);

    explicit FiniteSource(gr::property_map init = {}) : gr::Block<FiniteSource>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const auto n = std::min<std::size_t>(outSpan.size(), n_samples - produced_);
        std::fill_n(outSpan.begin(), n, 0.0F);
        outSpan.publish(n);
        produced_ += static_cast<gr::Size_t>(n);
        return produced_ >= n_samples ? gr::work::Status::DONE : gr::work::Status::OK;
    }

private:
    gr::Size_t produced_ = 0U;
};

// Reports an error from its first work call and goes on producing zeros.
struct FaultReportingSource : gr::Block<FaultReportingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FaultReportingSource, out);

    explicit FaultReportingSource(gr::property_map init = {}) : gr::Block<FaultReportingSource>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        if (!reported_) {
            this->emitErrorMessage("processBulk", "the source reports a fault");
            reported_ = true;
        }
        std::ranges::fill(outSpan, 0.0F);
        outSpan.publish(outSpan.size());
        return gr::work::Status::OK;
    }

private:
    bool reported_ = false;
};

// Throws from its first work call, which ends the run with an error.
struct FailingSource : gr::Block<FailingSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(FailingSource, out);

    explicit FailingSource(gr::property_map init = {}) : gr::Block<FailingSource>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto&) { throw std::runtime_error("the source fails"); }
};

// Consumes its input and declares the settings of a block that serves a managed stream.
struct StreamEndpointSink : gr::Block<StreamEndpointSink> {
    gr::PortIn<float> in;
    std::string transport;
    std::string payload_format;
    std::string endpoint;

    GR_MAKE_REFLECTABLE(StreamEndpointSink, in, transport, payload_format, endpoint);

    explicit StreamEndpointSink(gr::property_map init = {}) : gr::Block<StreamEndpointSink>(std::move(init)) {}

    void processOne(float) const noexcept {}
};

// Counts the live instances of the block that holds it as a member.
class LiveInstances {
public:
    LiveInstances() noexcept { count_.fetch_add(1); }
    LiveInstances(const LiveInstances&) noexcept { count_.fetch_add(1); }
    LiveInstances& operator=(const LiveInstances&) noexcept = default;
    ~LiveInstances() { count_.fetch_sub(1); }

    static std::int64_t count() noexcept { return count_.load(); }

private:
    static inline std::atomic<std::int64_t> count_{0};
};

// Produces zeros until the run stops, and counts its live instances.
struct CountedSource : gr::Block<CountedSource> {
    gr::PortOut<float> out;

    GR_MAKE_REFLECTABLE(CountedSource, out);

    explicit CountedSource(gr::property_map init = {}) : gr::Block<CountedSource>(std::move(init)) {}

    static std::int64_t alive() noexcept { return LiveInstances::count(); }

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        std::ranges::fill(outSpan, 0.0F);
        outSpan.publish(outSpan.size());
        return gr::work::Status::OK;
    }

private:
    LiveInstances live_;
};

// Produces zeros until the run stops, and holds settings of vector and complex type.
struct VectorSettingsSource : gr::Block<VectorSettingsSource> {
    gr::PortOut<float> out;
    std::vector<float> gains{1.0F, 2.0F};
    std::vector<std::string> antennas{"RX", "TX"};
    std::complex<float> correction{1.0F, -0.5F};
    std::vector<std::complex<float>> taps{{1.0F, 0.0F}};

    GR_MAKE_REFLECTABLE(VectorSettingsSource, out, gains, antennas, correction, taps);

    explicit VectorSettingsSource(gr::property_map init = {}) : gr::Block<VectorSettingsSource>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        std::ranges::fill(outSpan, 0.0F);
        outSpan.publish(outSpan.size());
        return gr::work::Status::OK;
    }
};

// Registers the blocks above into the global registry once, each under `gr4cp::test::<name>`.
inline void register_test_blocks() {
    static const bool registered = [] {
        auto& registry = gr::globalBlockRegistry();
        return registry.insert<FiniteSource>("=gr4cp::test::FiniteSource") &&
               registry.insert<FaultReportingSource>("=gr4cp::test::FaultReportingSource") &&
               registry.insert<FailingSource>("=gr4cp::test::FailingSource") &&
               registry.insert<StreamEndpointSink>("=gr4cp::test::StreamEndpointSink") &&
               registry.insert<CountedSource>("=gr4cp::test::CountedSource") &&
               registry.insert<VectorSettingsSource>("=gr4cp::test::VectorSettingsSource");
    }();
    ASSERT_TRUE(registered);
}

}  // namespace gr4cp::test
