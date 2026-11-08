/**
 * @file posehistory.h
 * @brief Pose history for the poser MCP: snapshots of the whole pose before
 *        every change, undo / redo, named marks, differences between two poses
 *        and the files that keep the history across restarts. No viewer
 *        dependencies (standard library only), tested standalone by
 *        tools/poser-mcp/ik/history_test.cpp in the sl-viewer repository.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// The history is a time-ordered list of poses. Each entry is the pose BEFORE a
// change and says which change followed it (source "mcp:aim", "manual", ...);
// entries with an empty source are saved states with no change after them (the
// newest pose saved by an undo, or a mark on the newest pose).
//
// undo / redo only move the position in the list and never add a change; a
// change made after an undo is appended at the end and nothing is thrown away
// (the states left by the undo stay reachable by id, by mark and by undoing
// further back). The list has a capacity; the oldest unmarked entries go first.
//
// Not thread-safe: the viewer uses it on the main thread only.

#ifndef PLUS_POSERMCP_POSEHISTORY_H
#define PLUS_POSERMCP_POSEHISTORY_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

namespace plus::posermcp::history
{
    struct Quat
    {
        float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
    };

    struct Vec3
    {
        float x = 0.f, y = 0.f, z = 0.f;
    };

    // One joint of the poser, as the poser holds it.
    struct JointState
    {
        std::string name;
        Quat        target;          // whole local rotation as drawn (= get_pose rotation)
        Quat        own;             // the poser's own part of it (target = own * base)
        Vec3        position;        // offset from the starting position, m (= get_pose position)
        Vec3        scale;           // offset added to the starting scale (= get_pose scale)
        bool        enabled  = true; // the poser drives this joint
        bool        rotation = true; // false for collision volumes (position/scale only)
    };

    struct Pose
    {
        std::vector<JointState> joints;
        const JointState* find(const std::string& name) const;
    };

    // Rotation angle between two rotations, degrees (q and -q are the same rotation).
    double angleDeg(const Quat& a, const Quat& b);

    struct Tolerance
    {
        double angleDeg  = 0.01;  // below this two rotations count as equal
        double positionM = 1e-5;  // position offsets
        double scale     = 1e-5;  // scale offsets (largest component)
    };

    struct JointDiff
    {
        std::string joint;
        double      angleDeg       = 0.0; // rotation difference of the targets
        double      positionM      = 0.0; // distance between the position offsets
        double      scale          = 0.0; // largest component difference of the scale offsets
        bool        enabledChanged = false;
        bool        missing        = false; // the joint is in only one of the two poses
    };

    // Joints that differ beyond the tolerance, largest rotation first.
    std::vector<JointDiff> diffPoses(const Pose& from, const Pose& to, const Tolerance& tolerance = {});
    bool                   samePose(const Pose& a, const Pose& b, const Tolerance& tolerance = {});

    // Exact fingerprint of a pose (FNV-1a over names and values), for cheap
    // "did anything change" checks every few frames.
    std::uint64_t poseHash(const Pose& pose);

    // What a change did.
    struct ChangeStats
    {
        int                      joints = 0;   // joints that changed
        double                   maxDeg = 0.0; // largest rotation change
        std::string              maxJoint;     // the joint with the largest rotation change
        std::vector<std::string> top;          // up to 5 changed joints, largest first, e.g. "mShoulderLeft 35"
    };
    ChangeStats changeStats(const Pose& before, const Pose& after);
    // "3 joints: mShoulderLeft 35 deg, mElbowLeft 20 deg, mWristLeft 4 deg"
    std::string describeChange(const ChangeStats& stats);

    struct Entry
    {
        std::uint64_t id   = 0;   // only grows; unique within a history
        double        time = 0.0; // Unix seconds: when the change started (saved states: when saved)
        std::string   source;     // "mcp:<tool>", "manual", ...; empty for a saved state
        std::string   summary;    // what the change was about
        std::string   label;      // mark name, or empty
        ChangeStats   change;     // what the change did (empty for a saved state)
        Pose          pose;       // the pose before the change

        bool isChange() const { return !source.empty(); }
    };

    // Errors meant for the model (bad step count, unknown mark, ...).
    struct HistoryError : public std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    // Mark names: 1-40 characters, no control characters, not starting with '#'
    // or a digit (those name ids). Returns an empty string when valid.
    std::string checkMarkName(const std::string& name);

    // Serialization of one entry (one file). Text, versioned, with a checksum
    // so that a truncated or damaged file is detected.
    std::string serializeEntry(const Entry& entry, const std::string& avatar_id);
    bool        parseEntry(const std::string& text, Entry& entry, std::string& avatar_id, std::string& error);

    class PoseHistory
    {
    public:
        explicit PoseHistory(std::size_t capacity = 100);

        std::size_t              capacity() const { return mCapacity; }
        std::size_t              size() const { return mEntries.size(); }
        const std::deque<Entry>& entries() const { return mEntries; }

        // The entry the live pose is at (after undo / redo, or a saved state),
        // or size() when the live pose is newer than every entry.
        std::size_t position() const { return mPosition; }
        std::size_t undoAvailable() const;
        std::size_t redoAvailable() const;

        // A change from before to after. Returns the new entry's id, or 0 when
        // the two poses are the same (nothing recorded).
        std::uint64_t record(const Pose& before, const Pose& after, const std::string& source, const std::string& summary,
                             double now);

        // Where an undo / redo went: apply entry->pose to the avatar.
        struct Move
        {
            const Entry* entry = nullptr;
            int          steps = 0; // > 0: back, < 0: forward
        };

        // live is the pose as it is now. If it differs from the position's
        // entry (changed without a record), that change is recorded first as
        // "manual". Throws HistoryError.
        Move undo(int steps, const Pose& live, double now);
        Move redo(int steps, const Pose& live, double now);
        // To an entry by id ("12", "#12") or mark name; back or forward.
        Move undoTo(const std::string& selector, const Pose& live, double now);

        // Names the live pose. A name used before moves to this pose.
        const Entry& mark(const std::string& name, const Pose& live, double now);

        // What undo(steps) would restore, without moving. Throws HistoryError.
        const Entry& stepsBack(int steps) const;
        // By id ("12", "#12") or mark name. Throws HistoryError.
        const Entry& find(const std::string& selector) const;
        const Entry* findById(std::uint64_t id) const;

        // ---- files ----
        struct LoadReport
        {
            std::size_t              loaded  = 0; // entries read
            std::size_t              skipped = 0; // damaged or foreign files left alone
            std::size_t              removed = 0; // files deleted (over the capacity, temporary files)
            std::vector<std::string> problems;    // one line per skipped file
        };
        // Keeps one file per entry in directory (UTF-8 path; created if
        // needed), loading what is there first. Only files of avatar_id are
        // used. Call before recording anything: if the history already has
        // entries, nothing is loaded and those entries are written instead.
        LoadReport attachDirectory(const std::string& directory_utf8, const std::string& avatar_id);
        bool               attached() const { return !mDirectory.empty(); }
        const std::string& directory() const { return mDirectory; }
        // The last file error (empty if none). The history keeps working in memory.
        const std::string& lastFileError() const { return mFileError; }

    private:
        Entry&      append(Entry entry);
        void        evict(std::uint64_t keep_a, std::uint64_t keep_b);
        void        recordDrift(const Pose& live, double now);
        std::size_t indexOf(const std::string& selector) const;
        Move        moveTo(std::size_t index, const Pose& live, double now);
        std::size_t basePosition(const Pose& live, double now, bool& pushed);
        void        removeTipIfPushed(bool pushed);
        void        writeEntry(const Entry& entry);
        void        removeFile(std::uint64_t id);
        std::string filePath(std::uint64_t id) const;

        std::size_t       mCapacity;
        std::deque<Entry> mEntries;
        std::size_t       mPosition = 0;
        std::uint64_t     mNextId   = 1;
        std::string       mDirectory; // UTF-8
        std::string       mAvatarId;
        std::string       mFileError;
    };

    // Detects a pose that changed and then stayed the same for a while (edits
    // in the poser window: one entry per edit, not one per dragged frame).
    class ChangeDebouncer
    {
    public:
        explicit ChangeDebouncer(double settle_seconds = 1.5) : mSettle(settle_seconds) {}

        void reset();
        bool hasBaseline() const { return mHasBaseline; }
        // The pose as last recorded (or as found when tracking started).
        void          setBaseline(std::uint64_t hash);
        std::uint64_t baseline() const { return mBaseline; }
        // Feed the current pose's hash. True once a pose different from the
        // baseline has stayed unchanged for settle_seconds; the caller records
        // it and calls setBaseline.
        bool observe(std::uint64_t hash, double now);
        bool pending() const { return mPending; }

    private:
        double        mSettle;
        bool          mHasBaseline = false;
        std::uint64_t mBaseline    = 0;
        bool          mPending     = false;
        std::uint64_t mPendingHash = 0;
        double        mPendingSince = 0.0;
    };
}

#endif // PLUS_POSERMCP_POSEHISTORY_H
