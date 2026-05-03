// test_config_bus.cc — Unit tests for lib/config/config_bus.h
//
// Tests ConfigBus pub/sub functionality

#include "config_bus.h"

#include <gtest/gtest.h>
#include <vector>

namespace atsc3 {
namespace config {
namespace {

//==============================================================================
// Test Observer Implementation
//==============================================================================

class TestObserver : public ConfigObserver {
public:
    void on_config_update(const Atsc3Config& config) override {
        update_count_++;
        last_config_ = config;
    }

    int update_count() const {
        return update_count_;
    }

    const Atsc3Config& last_config() const {
        return last_config_;
    }

    void reset() {
        update_count_ = 0;
        last_config_ = Atsc3Config();
    }

private:
    int update_count_ = 0;
    Atsc3Config last_config_;
};

//==============================================================================
// Construction Tests
//==============================================================================

TEST(ConfigBusTest, DefaultConstruction) {
    ConfigBus bus;

    EXPECT_EQ(bus.num_observers(), 0u);
    EXPECT_FALSE(bus.has_valid_config());
    EXPECT_EQ(bus.get_publish_count(), 0u);
}

TEST(ConfigBusTest, MaxObserversConstant) {
    EXPECT_EQ(ConfigBus::MAX_OBSERVERS, 16u);
}

//==============================================================================
// Registration Tests
//==============================================================================

TEST(ConfigBusTest, RegisterSingleObserver) {
    ConfigBus bus;
    TestObserver observer;

    bool result = bus.register_observer(&observer);

    EXPECT_TRUE(result);
    EXPECT_EQ(bus.num_observers(), 1u);
    EXPECT_TRUE(bus.is_registered(&observer));
}

TEST(ConfigBusTest, RegisterMultipleObservers) {
    ConfigBus bus;
    TestObserver obs1, obs2, obs3;

    EXPECT_TRUE(bus.register_observer(&obs1));
    EXPECT_TRUE(bus.register_observer(&obs2));
    EXPECT_TRUE(bus.register_observer(&obs3));

    EXPECT_EQ(bus.num_observers(), 3u);
    EXPECT_TRUE(bus.is_registered(&obs1));
    EXPECT_TRUE(bus.is_registered(&obs2));
    EXPECT_TRUE(bus.is_registered(&obs3));
}

TEST(ConfigBusTest, RegisterNullObserver) {
    ConfigBus bus;

    bool result = bus.register_observer(nullptr);

    EXPECT_FALSE(result);
    EXPECT_EQ(bus.num_observers(), 0u);
}

TEST(ConfigBusTest, RegisterDuplicateObserver) {
    ConfigBus bus;
    TestObserver observer;

    EXPECT_TRUE(bus.register_observer(&observer));
    EXPECT_TRUE(bus.register_observer(&observer));  // Already registered

    EXPECT_EQ(bus.num_observers(), 1u);  // Should not add duplicate
}

TEST(ConfigBusTest, RegisterMaxObservers) {
    ConfigBus bus;
    std::vector<TestObserver> observers(ConfigBus::MAX_OBSERVERS);

    // Register up to max
    for (size_t i = 0; i < ConfigBus::MAX_OBSERVERS; i++) {
        EXPECT_TRUE(bus.register_observer(&observers[i]));
    }

    EXPECT_EQ(bus.num_observers(), ConfigBus::MAX_OBSERVERS);

    // Try to register one more
    TestObserver extra;
    EXPECT_FALSE(bus.register_observer(&extra));
    EXPECT_EQ(bus.num_observers(), ConfigBus::MAX_OBSERVERS);
}

//==============================================================================
// Unregistration Tests
//==============================================================================

TEST(ConfigBusTest, UnregisterObserver) {
    ConfigBus bus;
    TestObserver observer;

    bus.register_observer(&observer);
    EXPECT_EQ(bus.num_observers(), 1u);

    bus.unregister_observer(&observer);
    EXPECT_EQ(bus.num_observers(), 0u);
    EXPECT_FALSE(bus.is_registered(&observer));
}

TEST(ConfigBusTest, UnregisterMiddleObserver) {
    ConfigBus bus;
    TestObserver obs1, obs2, obs3;

    bus.register_observer(&obs1);
    bus.register_observer(&obs2);
    bus.register_observer(&obs3);

    bus.unregister_observer(&obs2);

    EXPECT_EQ(bus.num_observers(), 2u);
    EXPECT_TRUE(bus.is_registered(&obs1));
    EXPECT_FALSE(bus.is_registered(&obs2));
    EXPECT_TRUE(bus.is_registered(&obs3));
}

TEST(ConfigBusTest, UnregisterNullObserver) {
    ConfigBus bus;
    TestObserver observer;

    bus.register_observer(&observer);

    // Should not crash or change state
    bus.unregister_observer(nullptr);

    EXPECT_EQ(bus.num_observers(), 1u);
}

TEST(ConfigBusTest, UnregisterNonexistentObserver) {
    ConfigBus bus;
    TestObserver obs1, obs2;

    bus.register_observer(&obs1);

    // Should not crash or change state
    bus.unregister_observer(&obs2);

    EXPECT_EQ(bus.num_observers(), 1u);
    EXPECT_TRUE(bus.is_registered(&obs1));
}

//==============================================================================
// Publish Tests
//==============================================================================

TEST(ConfigBusTest, PublishToSingleObserver) {
    ConfigBus bus;
    TestObserver observer;

    bus.register_observer(&observer);

    Atsc3Config config;
    config.fft_size = FftSize::FFT_16K;
    config.valid = true;

    bus.publish(config);

    EXPECT_EQ(observer.update_count(), 1);
    EXPECT_EQ(observer.last_config().fft_size, FftSize::FFT_16K);
    EXPECT_TRUE(observer.last_config().valid);
}

TEST(ConfigBusTest, PublishToMultipleObservers) {
    ConfigBus bus;
    TestObserver obs1, obs2, obs3;

    bus.register_observer(&obs1);
    bus.register_observer(&obs2);
    bus.register_observer(&obs3);

    Atsc3Config config;
    config.num_plps = 2;
    config.valid = true;

    bus.publish(config);

    EXPECT_EQ(obs1.update_count(), 1);
    EXPECT_EQ(obs2.update_count(), 1);
    EXPECT_EQ(obs3.update_count(), 1);

    EXPECT_EQ(obs1.last_config().num_plps, 2u);
    EXPECT_EQ(obs2.last_config().num_plps, 2u);
    EXPECT_EQ(obs3.last_config().num_plps, 2u);
}

TEST(ConfigBusTest, PublishToNoObservers) {
    ConfigBus bus;

    Atsc3Config config;
    config.valid = true;

    // Should not crash
    bus.publish(config);

    EXPECT_TRUE(bus.has_valid_config());
    EXPECT_EQ(bus.get_publish_count(), 1u);
}

TEST(ConfigBusTest, PublishMultipleTimes) {
    ConfigBus bus;
    TestObserver observer;

    bus.register_observer(&observer);

    Atsc3Config config1;
    config1.fft_size = FftSize::FFT_8K;
    bus.publish(config1);

    Atsc3Config config2;
    config2.fft_size = FftSize::FFT_16K;
    bus.publish(config2);

    Atsc3Config config3;
    config3.fft_size = FftSize::FFT_32K;
    bus.publish(config3);

    EXPECT_EQ(observer.update_count(), 3);
    EXPECT_EQ(observer.last_config().fft_size, FftSize::FFT_32K);
    EXPECT_EQ(bus.get_publish_count(), 3u);
}

TEST(ConfigBusTest, PublishUpdatesCurrentConfig) {
    ConfigBus bus;

    Atsc3Config config;
    config.pilot_pattern = PilotPattern::PP5;
    config.num_plps = 3;
    config.valid = true;

    bus.publish(config);

    EXPECT_EQ(bus.get_config().pilot_pattern, PilotPattern::PP5);
    EXPECT_EQ(bus.get_config().num_plps, 3u);
    EXPECT_TRUE(bus.has_valid_config());
}

//==============================================================================
// Clear Tests
//==============================================================================

TEST(ConfigBusTest, ClearObservers) {
    ConfigBus bus;
    TestObserver obs1, obs2, obs3;

    bus.register_observer(&obs1);
    bus.register_observer(&obs2);
    bus.register_observer(&obs3);

    bus.clear_observers();

    EXPECT_EQ(bus.num_observers(), 0u);
    EXPECT_FALSE(bus.is_registered(&obs1));
    EXPECT_FALSE(bus.is_registered(&obs2));
    EXPECT_FALSE(bus.is_registered(&obs3));
}

TEST(ConfigBusTest, ClearDoesNotAffectConfig) {
    ConfigBus bus;
    TestObserver observer;

    bus.register_observer(&observer);

    Atsc3Config config;
    config.valid = true;
    bus.publish(config);

    bus.clear_observers();

    EXPECT_TRUE(bus.has_valid_config());
    EXPECT_EQ(bus.get_publish_count(), 1u);
}

TEST(ConfigBusTest, PublishAfterClear) {
    ConfigBus bus;
    TestObserver obs1, obs2;

    bus.register_observer(&obs1);
    bus.publish(Atsc3Config());

    bus.clear_observers();
    bus.register_observer(&obs2);

    Atsc3Config config;
    config.fft_size = FftSize::FFT_32K;
    bus.publish(config);

    EXPECT_EQ(obs1.update_count(), 1);  // Only first publish
    EXPECT_EQ(obs2.update_count(), 1);  // Only second publish
    EXPECT_EQ(obs2.last_config().fft_size, FftSize::FFT_32K);
}

//==============================================================================
// Config Content Tests
//==============================================================================

TEST(ConfigBusTest, PublishFullConfig) {
    ConfigBus bus;
    TestObserver observer;

    bus.register_observer(&observer);

    Atsc3Config config;
    config.version = 1;
    config.fft_size = FftSize::FFT_16K;
    config.cp_length = CpLength::CP_1536_8192;
    config.pilot_pattern = PilotPattern::PP4;
    config.num_plps = 2;
    config.plps[0].plp_id = 0;
    config.plps[0].modulation = Modulation::QAM64;
    config.plps[0].code_rate = CodeRate::RATE_7_15;
    config.plps[1].plp_id = 1;
    config.plps[1].modulation = Modulation::QAM256;
    config.plps[1].code_rate = CodeRate::RATE_9_15;
    config.valid = true;

    bus.publish(config);

    const auto& received = observer.last_config();
    EXPECT_EQ(received.version, 1u);
    EXPECT_EQ(received.fft_size, FftSize::FFT_16K);
    EXPECT_EQ(received.cp_length, CpLength::CP_1536_8192);
    EXPECT_EQ(received.pilot_pattern, PilotPattern::PP4);
    EXPECT_EQ(received.num_plps, 2u);
    EXPECT_EQ(received.plps[0].modulation, Modulation::QAM64);
    EXPECT_EQ(received.plps[1].modulation, Modulation::QAM256);
    EXPECT_TRUE(received.valid);
}

//==============================================================================
// Global Instance Tests
//==============================================================================

TEST(ConfigBusTest, GlobalInstance) {
    ConfigBus& bus1 = get_global_config_bus();
    ConfigBus& bus2 = get_global_config_bus();

    // Should be same instance
    EXPECT_EQ(&bus1, &bus2);
}

TEST(ConfigBusTest, GlobalInstancePersists) {
    ConfigBus& bus = get_global_config_bus();

    // Clear any previous state
    bus.clear_observers();

    TestObserver observer;
    bus.register_observer(&observer);

    Atsc3Config config;
    config.valid = true;
    bus.publish(config);

    // Get instance again
    ConfigBus& bus2 = get_global_config_bus();

    EXPECT_TRUE(bus2.has_valid_config());
    EXPECT_EQ(bus2.num_observers(), 1u);

    // Cleanup
    bus.clear_observers();
}

//==============================================================================
// Edge Cases
//==============================================================================

TEST(ConfigBusTest, RegisterUnregisterRegister) {
    ConfigBus bus;
    TestObserver observer;

    bus.register_observer(&observer);
    bus.unregister_observer(&observer);
    bus.register_observer(&observer);

    EXPECT_EQ(bus.num_observers(), 1u);
    EXPECT_TRUE(bus.is_registered(&observer));

    Atsc3Config config;
    bus.publish(config);

    EXPECT_EQ(observer.update_count(), 1);
}

TEST(ConfigBusTest, ObserverOrderPreserved) {
    ConfigBus bus;
    std::vector<int> call_order;

    class OrderedObserver : public ConfigObserver {
    public:
        OrderedObserver(int id, std::vector<int>& order) : id_(id), order_(order) {}

        void on_config_update(const Atsc3Config&) override {
            order_.push_back(id_);
        }

    private:
        int id_;
        std::vector<int>& order_;
    };

    OrderedObserver obs1(1, call_order);
    OrderedObserver obs2(2, call_order);
    OrderedObserver obs3(3, call_order);

    bus.register_observer(&obs1);
    bus.register_observer(&obs2);
    bus.register_observer(&obs3);

    bus.publish(Atsc3Config());

    ASSERT_EQ(call_order.size(), 3u);
    EXPECT_EQ(call_order[0], 1);
    EXPECT_EQ(call_order[1], 2);
    EXPECT_EQ(call_order[2], 3);
}

}  // namespace
}  // namespace config
}  // namespace atsc3
