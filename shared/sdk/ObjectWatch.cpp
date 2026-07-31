#include "ObjectWatch.hpp"

#include <algorithm>
#include <cmath>

namespace sdk {

ObjectWatch::ObjectWatch(ObjectType type, size_t capacity) : m_type(type), m_capacity(capacity) {
    m_current.reserve(capacity);
    m_previous.reserve(capacity);
}

void ObjectWatch::reset() {
    m_primed = false;
    m_truncated = false;
    m_samples = 0;
    m_current.clear();
    m_previous.clear();
    m_appeared.clear();
    m_vanished.clear();
}

std::optional<size_t> ObjectWatch::sample() {
    auto* mgr = CClientMgr::get();

    if (mgr == nullptr) {
        m_appeared.clear();
        m_vanished.clear();
        return std::nullopt;
    }

    // Copy into scratch first. A failed walk must not disturb the state a
    // consumer is already holding, so nothing is committed until it succeeds.
    std::vector<Snapshot> fresh(m_capacity);
    const auto taken = mgr->snapshot_objects(m_type, fresh.data(), fresh.size());

    if (!taken.has_value()) {
        m_appeared.clear();
        m_vanished.clear();
        return std::nullopt;
    }

    fresh.resize(*taken);

    const bool had_previous = m_primed;
    m_previous.swap(m_current);
    m_current.swap(fresh);
    m_truncated = m_current.size() >= m_capacity;
    m_primed = true;
    ++m_samples;

    m_appeared.clear();
    m_vanished.clear();

    // Sorted by the same (address, handle) pair that defines identity, so the
    // ordering and the comparison agree by construction. Sort-and-search
    // rather than a nested scan because the buckets are big -- OT_NORMAL alone
    // runs to ~1900 objects live -- and the pairwise form would cost millions
    // of comparisons per sample, ruling this class out of the per-frame use it
    // exists for.
    std::sort(m_current.begin(), m_current.end(), order_by_identity);

    // No comparison is possible on the first sample, and a truncated one would
    // manufacture differences out of the bound rather than out of the world.
    if (!had_previous || m_truncated) {
        return m_current.size();
    }


    for (const auto& now : m_current) {
        if (!std::binary_search(m_previous.begin(), m_previous.end(), now, order_by_identity)) {
            m_appeared.push_back(now);
        }
    }

    for (const auto& before : m_previous) {
        if (!std::binary_search(m_current.begin(), m_current.end(), before, order_by_identity)) {
            m_vanished.push_back(before);
        }
    }

    return m_current.size();
}


std::optional<ObjectWatch::Bearing> ObjectWatch::dominant_bearing(const float origin[3],
                                                                  float tolerance) const {
    if (origin == nullptr || m_appeared.empty()) {
        return std::nullopt;
    }

    // Each candidate cluster is centred on one object's own bearing, so the
    // answer is always a direction something actually lies in rather than an
    // average of a bimodal set (see the header's note on why the mean is wrong).
    const size_t n = m_appeared.size();
    size_t best_count = 0;
    float best_radians = 0.0f;
    float best_distance = 0.0f;

    for (size_t i = 0; i < n; ++i) {
        const float cx = m_appeared[i].position[0] - origin[0];
        const float cz = m_appeared[i].position[2] - origin[2];
        const float centre = std::atan2(cz, cx);

        size_t count = 0;
        float sum_sin = 0.0f;
        float sum_cos = 0.0f;
        float sum_distance = 0.0f;

        for (size_t j = 0; j < n; ++j) {
            const float dx = m_appeared[j].position[0] - origin[0];
            const float dz = m_appeared[j].position[2] - origin[2];
            const float b = std::atan2(dz, dx);

            // Wrapped difference: two bearings either side of the +/-pi seam
            // are neighbours, and a naive subtraction would call them opposite.
            float delta = b - centre;
            while (delta > 3.14159265f) {
                delta -= 6.28318531f;
            }
            while (delta < -3.14159265f) {
                delta += 6.28318531f;
            }

            if (std::fabs(delta) <= tolerance) {
                ++count;
                // Summed as unit vectors, again because of the seam: the mean
                // of 179 and -179 degrees is 180, not zero.
                sum_sin += std::sin(b);
                sum_cos += std::cos(b);
                sum_distance += std::sqrt(dx * dx + dz * dz);
            }
        }

        if (count > best_count) {
            best_count = count;
            best_radians = std::atan2(sum_sin, sum_cos);
            best_distance = sum_distance / static_cast<float>(count);
        }
    }

    if (best_count == 0) {
        return std::nullopt;
    }

    return Bearing{best_radians, best_count, best_distance};
}

} // namespace sdk
