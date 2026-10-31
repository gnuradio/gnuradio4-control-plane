#pragma once

#include <gnuradio-4.0/Block.hpp>
#include <gnuradio-4.0/BlockRegistry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
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

// Produces `n_samples` zeros and ends. When it ends, `lowest` and `highest` hold the least and the greatest value of
// their 64-bit types.
struct ExtremeValueSource : gr::Block<ExtremeValueSource> {
    gr::PortOut<float> out;
    gr::Size_t n_samples = 64U;
    std::int64_t lowest = 0;
    std::uint64_t highest = 0U;

    GR_MAKE_REFLECTABLE(ExtremeValueSource, out, n_samples, lowest, highest);

    explicit ExtremeValueSource(gr::property_map init = {}) : gr::Block<ExtremeValueSource>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        const auto n = std::min<std::size_t>(outSpan.size(), n_samples - produced_);
        std::fill_n(outSpan.begin(), n, 0.0F);
        outSpan.publish(n);
        produced_ += static_cast<gr::Size_t>(n);
        if (produced_ < n_samples) {
            return gr::work::Status::OK;
        }
        lowest = std::numeric_limits<std::int64_t>::min();
        highest = std::numeric_limits<std::uint64_t>::max();
        return gr::work::Status::DONE;
    }

private:
    gr::Size_t produced_ = 0U;
};

// Produces zeros until the run stops, and holds the 64-bit settings `offset` (signed), `limit` (unsigned) and `marks`
// (a signed list). Each starts beyond the integers a double holds exactly.
struct WideIntegerSource : gr::Block<WideIntegerSource> {
    gr::PortOut<float> out;
    std::int64_t offset = std::numeric_limits<std::int64_t>::min() + 1;
    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max() - 1U;
    std::vector<std::int64_t> marks{std::numeric_limits<std::int64_t>::max() - 1, -1};

    GR_MAKE_REFLECTABLE(WideIntegerSource, out, offset, limit, marks);

    explicit WideIntegerSource(gr::property_map init = {}) : gr::Block<WideIntegerSource>(std::move(init)) {}

    gr::work::Status processBulk(gr::OutputSpanLike auto& outSpan) {
        std::ranges::fill(outSpan, 0.0F);
        outSpan.publish(outSpan.size());
        return gr::work::Status::OK;
    }
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

// Consumes float samples and declares `transport`, `payload_format` and `endpoint`, the settings of a managed stream
// block. A stream binding writes `endpoint` and `transport`.
struct StreamEndpointSink : gr::Block<StreamEndpointSink> {
    gr::PortIn<float> in;
    std::string transport;
    std::string payload_format;
    std::string endpoint;

    GR_MAKE_REFLECTABLE(StreamEndpointSink, in, transport, payload_format, endpoint);

    explicit StreamEndpointSink(gr::property_map init = {}) : gr::Block<StreamEndpointSink>(std::move(init)) {}

    void processOne(float) const noexcept {}
};

// Consumes float samples and declares `transport` and `endpoint`, the two settings a stream binding writes, and no
// other stream key.
struct BoundStreamSink : gr::Block<BoundStreamSink> {
    gr::PortIn<float> in;
    std::string transport;
    std::string endpoint;

    GR_MAKE_REFLECTABLE(BoundStreamSink, in, transport, endpoint);

    explicit BoundStreamSink(gr::property_map init = {}) : gr::Block<BoundStreamSink>(std::move(init)) {}

    void processOne(float) const noexcept {}
};

// A hold on the destructor of HeldTeardownSource. Once armed, the next destructor waits until release() or ten seconds
// have passed.
class TeardownHold {
public:
    static void arm() {
        std::lock_guard lock(mutex_);
        armed_ = true;
        entered_ = false;
        released_ = false;
    }

    // Returns whether a destructor entered the hold within `timeout`.
    static bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [] { return entered_; });
    }

    // Returns whether a destructor waits in the hold.
    static bool holding() {
        std::lock_guard lock(mutex_);
        return entered_ && !released_;
    }

    static void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

    static void enter() {
        std::unique_lock lock(mutex_);
        if (!std::exchange(armed_, false)) {
            return;
        }
        entered_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, std::chrono::seconds(10), [] { return released_; });
        released_ = true;
    }

private:
    static inline std::mutex mutex_;
    static inline std::condition_variable changed_;
    static inline bool armed_ = false;
    static inline bool entered_ = false;
    static inline bool released_ = false;
};

// Produces `n_samples` zeros and ends. Its destructor enters the TeardownHold.
struct HeldTeardownSource : gr::Block<HeldTeardownSource> {
    gr::PortOut<float> out;
    gr::Size_t n_samples = 64U;

    GR_MAKE_REFLECTABLE(HeldTeardownSource, out, n_samples);

    explicit HeldTeardownSource(gr::property_map init = {}) : gr::Block<HeldTeardownSource>(std::move(init)) {}
    HeldTeardownSource(const HeldTeardownSource&) = delete;
    HeldTeardownSource& operator=(const HeldTeardownSource&) = delete;
    ~HeldTeardownSource() { TeardownHold::enter(); }

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

// Counts the live objects that hold it as a member. Every holder shares the one count.
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

// Produces zeros until the run stops, and holds the settings `gains` (float list), `antennas` (string list),
// `correction` (complex) and `taps` (complex list).
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
               registry.insert<ExtremeValueSource>("=gr4cp::test::ExtremeValueSource") &&
               registry.insert<WideIntegerSource>("=gr4cp::test::WideIntegerSource") &&
               registry.insert<FaultReportingSource>("=gr4cp::test::FaultReportingSource") &&
               registry.insert<FailingSource>("=gr4cp::test::FailingSource") &&
               registry.insert<StreamEndpointSink>("=gr4cp::test::StreamEndpointSink") &&
               registry.insert<BoundStreamSink>("=gr4cp::test::BoundStreamSink") &&
               registry.insert<CountedSource>("=gr4cp::test::CountedSource") &&
               registry.insert<HeldTeardownSource>("=gr4cp::test::HeldTeardownSource") &&
               registry.insert<VectorSettingsSource>("=gr4cp::test::VectorSettingsSource");
    }();
    ASSERT_TRUE(registered);
}

}  // namespace gr4cp::test
