/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef _LIBINPUT_VELOCITY_TRACKER_H
#define _LIBINPUT_VELOCITY_TRACKER_H

#include <input/Input.h>
#include <utils/BitSet.h>
#include <utils/Timers.h>

namespace android {

class VelocityTrackerStrategy;

/*
 * Calculates the velocity of pointer movements over time.
 */
class VelocityTracker {
public:
    enum class Strategy : int32_t {
        DEFAULT = -1,
        MIN = 0,
        IMPULSE = 0,
        LSQ1 = 1,
        LSQ2 = 2,
        LSQ3 = 3,
        WLSQ2_DELTA = 4,
        WLSQ2_CENTRAL = 5,
        WLSQ2_RECENT = 6,
        INT1 = 7,
        INT2 = 8,
        LEGACY = 9,
        MAX = LEGACY,
    };

    struct Position {
        float x, y;
    };

    struct Estimator {
        static const size_t MAX_DEGREE = 4;

        // Estimator time base.
        nsecs_t time;

        // Polynomial coefficients describing motion in X and Y.
        float xCoeff[MAX_DEGREE + 1], yCoeff[MAX_DEGREE + 1];

        // Polynomial degree (number of coefficients), or zero if no information is
        // available.
        uint32_t degree;

        // Confidence (coefficient of determination), between 0 (no fit) and 1 (perfect fit).
        float confidence;

        inline void clear() {
            time = 0;
            degree = 0;
            confidence = 0;
            for (size_t i = 0; i <= MAX_DEGREE; i++) {
                xCoeff[i] = 0;
                yCoeff[i] = 0;
            }
        }
    };

    // Creates a velocity tracker using the specified strategy.
    // If strategy is not provided, uses the default strategy for the platform.
    VelocityTracker(const Strategy strategy = Strategy::DEFAULT);

    ~VelocityTracker();

    // Resets the velocity tracker state.
    void clear();

    // Resets the velocity tracker state for specific pointers.
    // Call this method when some pointers have changed and may be reusing
    // an id that was assigned to a different pointer earlier.
    void clearPointers(BitSet32 idBits);

    // Adds movement information for a set of pointers.
    // The idBits bitfield specifies the pointer ids of the pointers whose positions
    // are included in the movement.
    // The positions array contains position information for each pointer in order by
    // increasing id.  Its size should be equal to the number of one bits in idBits.
    void addMovement(nsecs_t eventTime, BitSet32 idBits, const std::vector<Position>& positions);

    // Adds movement information for all pointers in a MotionEvent, including historical samples.
    void addMovement(const MotionEvent* event);

    // Gets the velocity of the specified pointer id in position units per second.
    // Returns false and sets the velocity components to zero if there is
    // insufficient movement information for the pointer.
    bool getVelocity(uint32_t id, float* outVx, float* outVy) const;

    // Gets an estimator for the recent movements of the specified pointer id.
    // Returns false and clears the estimator if there is no information available
    // about the pointer.
    bool getEstimator(uint32_t id, Estimator* outEstimator) const;

    // Gets the active pointer id, or -1 if none.
    inline int32_t getActivePointerId() const { return mActivePointerId; }

    // Gets a bitset containing all pointer ids from the most recent movement.
    inline BitSet32 getCurrentPointerIdBits() const { return mCurrentPointerIdBits; }

private:
    // The default velocity tracker strategy.
    // Although other strategies are available for testing and comparison purposes,
    // this is the strategy that applications will actually use.  Be very careful
    // when adjusting the default strategy because it can dramatically affect
    // (often in a bad way) the user experience.
    static const Strategy DEFAULT_STRATEGY = Strategy::LSQ2;

    nsecs_t mLastEventTime;
    BitSet32 mCurrentPointerIdBits;
    int32_t mActivePointerId;
    std::unique_ptr<VelocityTrackerStrategy> mStrategy;

    bool configureStrategy(const Strategy strategy);

    static std::unique_ptr<VelocityTrackerStrategy> createStrategy(const Strategy strategy);
};


/*
 * Implements a particular velocity tracker algorithm.
 */
class VelocityTrackerStrategy {
protected:
    VelocityTrackerStrategy() { }

public:
    virtual ~VelocityTrackerStrategy() { }

    virtual void clear() = 0;
    virtual void clearPointers(BitSet32 idBits) = 0;
    virtual void addMovement(nsecs_t eventTime, BitSet32 idBits,
                             const std::vector<VelocityTracker::Position>& positions) = 0;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const = 0;
};


/*
 * Velocity tracker algorithm based on least-squares linear regression.
 */
class LeastSquaresVelocityTrackerStrategy : public VelocityTrackerStrategy {
public:
    enum Weighting {
        // No weights applied.  All data points are equally reliable.
        WEIGHTING_NONE,

        // Weight by time delta.  Data points clustered together are weighted less.
        WEIGHTING_DELTA,

        // Weight such that points within a certain horizon are weighed more than those
        // outside of that horizon.
        WEIGHTING_CENTRAL,

        // Weight such that points older than a certain amount are weighed less.
        WEIGHTING_RECENT,
    };

    // Degree must be no greater than Estimator::MAX_DEGREE.
    LeastSquaresVelocityTrackerStrategy(uint32_t degree, Weighting weighting = WEIGHTING_NONE);
    virtual ~LeastSquaresVelocityTrackerStrategy();

    virtual void clear();
    virtual void clearPointers(BitSet32 idBits);
    void addMovement(nsecs_t eventTime, BitSet32 idBits,
                     const std::vector<VelocityTracker::Position>& positions) override;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const;

private:
    // Sample horizon.
    // We don't use too much history by default since we want to react to quick
    // changes in direction.
    static const nsecs_t HORIZON = 100 * 1000000; // 100 ms

    // Number of samples to keep.
    static const uint32_t HISTORY_SIZE = 20;

    struct Movement {
        nsecs_t eventTime;
        BitSet32 idBits;
        VelocityTracker::Position positions[MAX_POINTERS];

        inline const VelocityTracker::Position& getPosition(uint32_t id) const {
            return positions[idBits.getIndexOfBit(id)];
        }
    };

    float chooseWeight(uint32_t index) const;

    const uint32_t mDegree;
    const Weighting mWeighting;
    uint32_t mIndex;
    Movement mMovements[HISTORY_SIZE];
};


/*
 * Velocity tracker algorithm that uses an IIR filter.
 */
class IntegratingVelocityTrackerStrategy : public VelocityTrackerStrategy {
public:
    // Degree must be 1 or 2.
    IntegratingVelocityTrackerStrategy(uint32_t degree);
    ~IntegratingVelocityTrackerStrategy();

    virtual void clear();
    virtual void clearPointers(BitSet32 idBits);
    void addMovement(nsecs_t eventTime, BitSet32 idBits,
                     const std::vector<VelocityTracker::Position>& positions) override;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const;

private:
    // Current state estimate for a particular pointer.
    struct State {
        nsecs_t updateTime;
        uint32_t degree;

        float xpos, xvel, xaccel;
        float ypos, yvel, yaccel;
    };

    const uint32_t mDegree;
    BitSet32 mPointerIdBits;
    State mPointerState[MAX_POINTER_ID + 1];

    void initState(State& state, nsecs_t eventTime, float xpos, float ypos) const;
    void updateState(State& state, nsecs_t eventTime, float xpos, float ypos) const;
    void populateEstimator(const State& state, VelocityTracker::Estimator* outEstimator) const;
};


/*
 * Velocity tracker strategy used prior to ICS.
 */
class LegacyVelocityTrackerStrategy : public VelocityTrackerStrategy {
public:
    LegacyVelocityTrackerStrategy();
    virtual ~LegacyVelocityTrackerStrategy();

    virtual void clear();
    virtual void clearPointers(BitSet32 idBits);
    void addMovement(nsecs_t eventTime, BitSet32 idBits,
                     const std::vector<VelocityTracker::Position>& positions) override;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const;

private:
    // Oldest sample to consider when calculating the velocity.
    static const nsecs_t HORIZON = 200 * 1000000; // 100 ms

    // Number of samples to keep.
    static const uint32_t HISTORY_SIZE = 20;

    // The minimum duration between samples when estimating velocity.
    static const nsecs_t MIN_DURATION = 10 * 1000000; // 10 ms

    struct Movement {
        nsecs_t eventTime;
        BitSet32 idBits;
        VelocityTracker::Position positions[MAX_POINTERS];

        inline const VelocityTracker::Position& getPosition(uint32_t id) const {
            return positions[idBits.getIndexOfBit(id)];
        }
    };

    uint32_t mIndex;
    Movement mMovements[HISTORY_SIZE];
};

class ImpulseVelocityTrackerStrategy : public VelocityTrackerStrategy {
public:
    ImpulseVelocityTrackerStrategy();
    virtual ~ImpulseVelocityTrackerStrategy();

    virtual void clear();
    virtual void clearPointers(BitSet32 idBits);
    void addMovement(nsecs_t eventTime, BitSet32 idBits,
                     const std::vector<VelocityTracker::Position>& positions) override;
    virtual bool getEstimator(uint32_t id, VelocityTracker::Estimator* outEstimator) const;

private:
    // Sample horizon.
    // We don't use too much history by default since we want to react to quick
    // changes in direction.
    static constexpr nsecs_t HORIZON = 100 * 1000000; // 100 ms

    // Number of samples to keep.
    static constexpr size_t HISTORY_SIZE = 20;

    struct Movement {
        nsecs_t eventTime;
        BitSet32 idBits;
        VelocityTracker::Position positions[MAX_POINTERS];

        inline const VelocityTracker::Position& getPosition(uint32_t id) const {
            return positions[idBits.getIndexOfBit(id)];
        }
    };

    size_t mIndex;
    Movement mMovements[HISTORY_SIZE];
};

} // namespace android

#endif // _LIBINPUT_VELOCITY_TRACKER_H
