#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "CClientMgr.hpp"

namespace sdk {

// ---- WHAT APPEARED, AND WHAT LEFT, BETWEEN TWO LOOKS AT THE WORLD -----------
//
// `CClientMgr` answers "what objects exist right now". Almost everything a mod
// actually wants to react to is a CHANGE in that set rather than the set
// itself: a projectile spawning, an impact effect landing where a shot went, a
// body dropping, a pickup being taken, a door opening as a world model.
//
// Computing that difference by hand at each call site means every consumer
// re-invents the same identity rule and the same bookkeeping, and gets the
// recycling caveat below wrong in its own private way. So it lives here once.
//
// WHY THIS IS THE FIRST WAY TO SEE WHERE A SHOT LANDS. The engine's fire ray is
// not reachable through any interface this project has mapped -- neither
// `ILTPhysics` nor `ILTCommon` carries a segment-intersect entry, and no trace
// function is named anywhere in the exe. But the shot has an OBSERVABLE
// CONSEQUENCE: firing spawns effects, and a newly-appeared object's position is
// a point the ray reached. That turns an un-findable function into a
// measurement, which is the same move as reading the camera's rotation from the
// object it lands in rather than from the code that computes it.
//
// IDENTITY, AND THE ONE CASE IT GETS WRONG. Two samples are matched on
// (address, handle) together. Address alone is not enough because the allocator
// reuses freed object memory; the handle is the engine's own identifier and is
// re-registered per object, but 335 of 3583 live objects carry `kNoHandle`
// (they are client-created, see LTObject.handle in the .genny) so it is not
// sufficient alone either.
//
// The pair still aliases in exactly one case: an object freed and a DIFFERENT
// object allocated at the same address AND registered under the same handle,
// between two samples. That reads as continuity -- no appearance, no
// disappearance. This is not defended against, because defending would mean
// holding a pointer across the gap and dereferencing it later, which is the
// lifetime hazard `snapshot_objects` exists to avoid. Sample often enough that
// the world cannot turn over between looks, and treat a single frame's
// difference as evidence rather than proof.
//
// THREAD AFFINITY: this copies POD out through `CClientMgr::snapshot_objects`
// and holds no engine pointers, so it is safe to drive from a diagnostic thread
// -- the same reason that method exists (AGENTS.md rule 6). What it is NOT is
// internally synchronised: one `ObjectWatch` belongs to one caller.
class ObjectWatch {
public:
    using Snapshot = CClientMgr::ObjectSnapshot;

    // `capacity` bounds how many objects one sample will copy. A bucket larger
    // than this is reported through `truncated()` rather than silently clipped,
    // because a clipped sample would manufacture fake disappearances for every
    // object past the bound.
    explicit ObjectWatch(ObjectType type, size_t capacity = 4096);

    // Re-read the bucket and recompute the difference against the previous
    // sample. Returns the number of objects now present, or nullopt when the
    // walk fails -- same conditions as `CClientMgr::snapshot_objects` (bad
    // type, faulted read, unterminated list, bucket/type disagreement).
    //
    // A failed sample leaves the previous one intact and clears the reported
    // difference, so a consumer polling through a level transition sees "no
    // changes" rather than "everything vanished".
    std::optional<size_t> sample();

    // Objects in the newest sample that were not in the one before it. Empty
    // until the second successful sample -- the first has nothing to compare
    // against, and reporting the entire world as "appeared" would be a lie that
    // every consumer would then have to special-case.
    const std::vector<Snapshot>& appeared() const { return m_appeared; }

    // Objects in the previous sample that are not in the newest one.
    const std::vector<Snapshot>& vanished() const { return m_vanished; }

    // The newest sample itself.
    const std::vector<Snapshot>& current() const { return m_current; }

    // False until the first successful sample. `appeared()` is meaningful only
    // after the second, which `samples()` distinguishes.
    bool primed() const { return m_primed; }

    // Successful samples taken since construction or the last `reset()`.
    uint64_t samples() const { return m_samples; }

    // True when the last sample hit `capacity` and so may be incomplete. The
    // difference is suppressed while this holds, for the reason in the
    // constructor comment.
    bool truncated() const { return m_truncated; }

    ObjectType type() const { return m_type; }
    size_t capacity() const { return m_capacity; }

    // ---- WHICH WAY DID THAT BURST OF THINGS APPEAR? ----------------------
    //
    // Effects arrive in groups, and the group has a DIRECTION: a shot's impacts
    // land along the ray, an explosion throws debris from one point, gunfire
    // elsewhere in the level spawns muzzle effects at the shooter. A consumer
    // asking "what just happened, and where" wants that one bearing, not the
    // fifteen positions it is spread across.
    //
    // Returns the bearing of the LARGEST CLUSTER of newly-appeared objects as
    // seen from `origin`, in the horizontal plane: `atan2(dz, dx)`, radians.
    //
    // WHY A CLUSTER AND NOT AN AVERAGE OR AN EXTREME. Both of those were tried
    // against the live game and both are wrong. The world spawns ambient
    // effects continuously and they are not related to the event you asked
    // about -- a distant emitter measured 7060 units away at a bearing 114
    // degrees off the shot, and it appeared in every trial. Taking the farthest
    // object returns that emitter; taking the mean lets it drag the answer by
    // tens of degrees. The impacts themselves agreed to within 5 degrees across
    // 15 objects, so the majority direction is both the robust statistic and
    // the one that means something.
    //
    // `tolerance` is the half-width of a cluster in radians (default 15
    // degrees, comfortably wider than the ~5 degree spread real impacts show
    // and far narrower than the separation from unrelated ambient spawns).
    struct Bearing {
        float radians;        // atan2(dz, dx) of the cluster
        size_t count;         // objects agreeing, out of appeared().size()
        float mean_distance;  // their mean horizontal distance from `origin`
    };

    // nullopt when nothing appeared. `origin` is 3 floats (x, y, z); only x and
    // z are read, because a bearing is a horizontal quantity and the vertical
    // spread of an impact spray is not part of the direction it came from.
    std::optional<Bearing> dominant_bearing(const float origin[3], float tolerance = 0.2618f) const;

    // Forget everything. The next sample primes again and reports no changes.
    void reset();

private:
    static bool same_object(const Snapshot& a, const Snapshot& b) {
        return a.address == b.address && a.handle == b.handle;
    }

    // Strict weak ordering over the SAME pair `same_object` compares, so a
    // sorted search and a direct comparison can never disagree about identity.
    static bool order_by_identity(const Snapshot& a, const Snapshot& b) {
        if (a.address != b.address) {
            return a.address < b.address;
        }

        return a.handle < b.handle;
    }

    ObjectType m_type;
    size_t m_capacity;
    bool m_primed{false};
    bool m_truncated{false};
    uint64_t m_samples{0};
    std::vector<Snapshot> m_current;
    std::vector<Snapshot> m_previous;
    std::vector<Snapshot> m_appeared;
    std::vector<Snapshot> m_vanished;
};

} // namespace sdk
