/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <android-base/stringprintf.h>

#include <algorithm>
#include <numeric>
#include <optional>
#include <type_traits>

#include "DisplayHardware/HWComposer.h"
#include "HwcStrongTypes.h"
#include "Scheduler/SchedulerUtils.h"
#include "Scheduler/StrongTyping.h"

namespace android::scheduler {
using namespace std::chrono_literals;

enum class RefreshRateConfigEvent : unsigned { None = 0b0, Changed = 0b1 };

inline RefreshRateConfigEvent operator|(RefreshRateConfigEvent lhs, RefreshRateConfigEvent rhs) {
    using T = std::underlying_type_t<RefreshRateConfigEvent>;
    return static_cast<RefreshRateConfigEvent>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

/**
 * This class is used to encapsulate configuration for refresh rates. It holds information
 * about available refresh rates on the device, and the mapping between the numbers and human
 * readable names.
 */
class RefreshRateConfigs {
public:
    // Margin used when matching refresh rates to the content desired ones.
    static constexpr nsecs_t MARGIN_FOR_PERIOD_CALCULATION =
        std::chrono::nanoseconds(800us).count();

    struct RefreshRate {
        // The tolerance within which we consider FPS approximately equals.
        static constexpr float FPS_EPSILON = 0.001f;

        RefreshRate(HwcConfigIndexType configId, nsecs_t vsyncPeriod,
                    HwcConfigGroupType configGroup, std::string name, float fps)
              : configId(configId),
                vsyncPeriod(vsyncPeriod),
                configGroup(configGroup),
                name(std::move(name)),
                fps(fps) {}

        RefreshRate(const RefreshRate&) = delete;
        // This config ID corresponds to the position of the config in the vector that is stored
        // on the device.
        const HwcConfigIndexType configId;
        // Vsync period in nanoseconds.
        const nsecs_t vsyncPeriod;
        // This configGroup for the config.
        const HwcConfigGroupType configGroup;
        // Human readable name of the refresh rate.
        const std::string name;
        // Refresh rate in frames per second
        const float fps = 0;

        // Checks whether the fps of this RefreshRate struct is within a given min and max refresh
        // rate passed in. FPS_EPSILON is applied to the boundaries for approximation.
        bool inPolicy(float minRefreshRate, float maxRefreshRate) const {
            return (fps >= (minRefreshRate - FPS_EPSILON) && fps <= (maxRefreshRate + FPS_EPSILON));
        }

        bool operator!=(const RefreshRate& other) const {
            return configId != other.configId || vsyncPeriod != other.vsyncPeriod ||
                    configGroup != other.configGroup;
        }

        bool operator==(const RefreshRate& other) const { return !(*this != other); }
    };

    using AllRefreshRatesMapType =
            std::unordered_map<HwcConfigIndexType, std::unique_ptr<const RefreshRate>>;

    struct Policy {
        // The default config, used to ensure we only initiate display config switches within the
        // same config group as defaultConfigId's group.
        HwcConfigIndexType defaultConfig;
        // The min and max FPS allowed by the policy.
        float minRefreshRate = 0;
        float maxRefreshRate = std::numeric_limits<float>::max();
        // Whether or not we switch config groups to get the best frame rate. Only used by tests.
        bool allowGroupSwitching = false;

        bool operator==(const Policy& other) const {
            return defaultConfig == other.defaultConfig && minRefreshRate == other.minRefreshRate &&
                    maxRefreshRate == other.maxRefreshRate &&
                    allowGroupSwitching == other.allowGroupSwitching;
        }

        bool operator!=(const Policy& other) const { return !(*this == other); }
    };

    // Return code set*Policy() to indicate the current policy is unchanged.
    static constexpr int CURRENT_POLICY_UNCHANGED = 1;

    // We maintain the display manager policy and the override policy separately. The override
    // policy is used by CTS tests to get a consistent device state for testing. While the override
    // policy is set, it takes precedence over the display manager policy. Once the override policy
    // is cleared, we revert to using the display manager policy.

    // Sets the display manager policy to choose refresh rates. The return value will be:
    //   - A negative value if the policy is invalid or another error occurred.
    //   - NO_ERROR if the policy was successfully updated, and the current policy is different from
    //     what it was before the call.
    //   - CURRENT_POLICY_UNCHANGED if the policy was successfully updated, but the current policy
    //     is the same as it was before the call.
    status_t setDisplayManagerPolicy(const Policy& policy) EXCLUDES(mLock);
    // Sets the override policy. See setDisplayManagerPolicy() for the meaning of the return value.
    status_t setOverridePolicy(const std::optional<Policy>& policy) EXCLUDES(mLock);
    // Gets the current policy, which will be the override policy if active, and the display manager
    // policy otherwise.
    Policy getCurrentPolicy() const EXCLUDES(mLock);
    // Gets the display manager policy, regardless of whether an override policy is active.
    Policy getDisplayManagerPolicy() const EXCLUDES(mLock);

    // Returns true if config is allowed by the current policy.
    bool isConfigAllowed(HwcConfigIndexType config) const EXCLUDES(mLock);

    // Describes the different options the layer voted for refresh rate
    enum class LayerVoteType {
        NoVote,          // Doesn't care about the refresh rate
        Min,             // Minimal refresh rate available
        Max,             // Maximal refresh rate available
        Heuristic,       // Specific refresh rate that was calculated by platform using a heuristic
        ExplicitDefault, // Specific refresh rate that was provided by the app with Default
                         // compatibility
        ExplicitExactOrMultiple // Specific refresh rate that was provided by the app with
                                // ExactOrMultiple compatibility
    };

    // Captures the layer requirements for a refresh rate. This will be used to determine the
    // display refresh rate.
    struct LayerRequirement {
        std::string name;         // Layer's name. Used for debugging purposes.
        LayerVoteType vote;       // Layer vote type.
        float desiredRefreshRate; // Layer's desired refresh rate, if applicable.
        float weight; // Layer's weight in the range of [0, 1]. The higher the weight the more
                      // impact this layer would have on choosing the refresh rate.

        bool operator==(const LayerRequirement& other) const {
            return name == other.name && vote == other.vote &&
                    desiredRefreshRate == other.desiredRefreshRate && weight == other.weight;
        }

        bool operator!=(const LayerRequirement& other) const { return !(*this == other); }
    };

    // Returns the refresh rate that fits best to the given layers.
    const RefreshRate& getRefreshRateForContent(const std::vector<LayerRequirement>& layers) const
            EXCLUDES(mLock);

    // Returns the refresh rate that fits best to the given layers. This function also gets a
    // boolean flag that indicates whether user touched the screen recently to be factored in when
    // choosing the refresh rate and returns whether the refresh rate was chosen as a result of
    // a touch event.
    const RefreshRate& getRefreshRateForContentV2(const std::vector<LayerRequirement>& layers,
                                                  bool touchActive, bool* touchConsidered) const
            EXCLUDES(mLock);

    // Returns all the refresh rates supported by the device. This won't change at runtime.
    const AllRefreshRatesMapType& getAllRefreshRates() const EXCLUDES(mLock);

    // Returns the lowest refresh rate supported by the device. This won't change at runtime.
    const RefreshRate& getMinRefreshRate() const { return *mMinSupportedRefreshRate; }

    // Returns the lowest refresh rate according to the current policy. May change in runtime.
    const RefreshRate& getMinRefreshRateByPolicy() const EXCLUDES(mLock);

    // Returns the highest refresh rate supported by the device. This won't change at runtime.
    const RefreshRate& getMaxRefreshRate() const { return *mMaxSupportedRefreshRate; }

    // Returns the highest refresh rate according to the current policy. May change in runtime.
    const RefreshRate& getMaxRefreshRateByPolicy() const EXCLUDES(mLock);

    // Returns the current refresh rate
    const RefreshRate& getCurrentRefreshRate() const EXCLUDES(mLock);

    // Returns the current refresh rate, if allowed. Otherwise the default that is allowed by
    // the policy.
    const RefreshRate& getCurrentRefreshRateByPolicy() const;

    // Returns the refresh rate that corresponds to a HwcConfigIndexType. This won't change at
    // runtime.
    const RefreshRate& getRefreshRateFromConfigId(HwcConfigIndexType configId) const {
        return *mRefreshRates.at(configId);
    };

    // Stores the current configId the device operates at
    void setCurrentConfigId(HwcConfigIndexType configId) EXCLUDES(mLock);

    struct InputConfig {
        HwcConfigIndexType configId = HwcConfigIndexType(0);
        HwcConfigGroupType configGroup = HwcConfigGroupType(0);
        nsecs_t vsyncPeriod = 0;
    };

    RefreshRateConfigs(const std::vector<InputConfig>& configs,
                       HwcConfigIndexType currentHwcConfig);
    RefreshRateConfigs(const std::vector<std::shared_ptr<const HWC2::Display::Config>>& configs,
                       HwcConfigIndexType currentConfigId);

private:
    void init(const std::vector<InputConfig>& configs, HwcConfigIndexType currentHwcConfig);

    void constructAvailableRefreshRates() REQUIRES(mLock);

    void getSortedRefreshRateList(
            const std::function<bool(const RefreshRate&)>& shouldAddRefreshRate,
            std::vector<const RefreshRate*>* outRefreshRates);

    // Returns the refresh rate with the highest score in the collection specified from begin
    // to end. If there are more than one with the same highest refresh rate, the first one is
    // returned.
    template <typename Iter>
    const RefreshRate* getBestRefreshRate(Iter begin, Iter end) const;

    // Returns number of display frames and remainder when dividing the layer refresh period by
    // display refresh period.
    std::pair<nsecs_t, nsecs_t> getDisplayFrames(nsecs_t layerPeriod, nsecs_t displayPeriod) const;

    // Returns the current refresh rate, if allowed. Otherwise the default that is allowed by
    // the policy.
    const RefreshRate& getCurrentRefreshRateByPolicyLocked() const REQUIRES(mLock);

    const Policy* getCurrentPolicyLocked() const REQUIRES(mLock);
    bool isPolicyValid(const Policy& policy);

    // The list of refresh rates, indexed by display config ID. This must not change after this
    // object is initialized.
    AllRefreshRatesMapType mRefreshRates;

    // The list of refresh rates which are available in the current policy, ordered by vsyncPeriod
    // (the first element is the lowest refresh rate)
    std::vector<const RefreshRate*> mAvailableRefreshRates GUARDED_BY(mLock);

    // The current config. This will change at runtime. This is set by SurfaceFlinger on
    // the main thread, and read by the Scheduler (and other objects) on other threads.
    const RefreshRate* mCurrentRefreshRate GUARDED_BY(mLock);

    // The policy values will change at runtime. They're set by SurfaceFlinger on the main thread,
    // and read by the Scheduler (and other objects) on other threads.
    Policy mDisplayManagerPolicy GUARDED_BY(mLock);
    std::optional<Policy> mOverridePolicy GUARDED_BY(mLock);

    // The min and max refresh rates supported by the device.
    // This will not change at runtime.
    const RefreshRate* mMinSupportedRefreshRate;
    const RefreshRate* mMaxSupportedRefreshRate;

    mutable std::mutex mLock;
};

} // namespace android::scheduler