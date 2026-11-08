/**
 * @file llrhipassstate.cpp
 * @brief The world passes' declared states: the switch, the comparison, verify's counts (research/204).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

#include "linden_common.h"

#include "llrhipassstate.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace LLRHI
{
    PassStateBackend* sPassStateBackend = nullptr;
    PassStateCopy* sPassStateCopy = nullptr;

    // Read once: the mode asked for does not change while the viewer runs
    // (auto moves between its own states below).
    PassStateMode passStateMode()
    {
        static const PassStateMode mode = []()
        {
            const char* env = std::getenv("LLRHI_PASS_STATE");
            if (!env || !*env)
            {
                return PassStateMode::Auto;     // research/218 vk-s2f: the default (bugloop round 11: no difference)
            }
            if (!strcmp(env, "verify"))
            {
                return PassStateMode::Verify;
            }
            if (!strcmp(env, "auto"))
            {
                return PassStateMode::Auto;
            }
            if (!strcmp(env, "on") || !strcmp(env, "1"))
            {
                return PassStateMode::On;
            }
            return PassStateMode::Off;
        }();
        return mode;
    }

    const char* passStateModeName(PassStateMode mode)
    {
        switch (mode)
        {
            case PassStateMode::Verify:
                return "verify";
            case PassStateMode::Auto:
                return "auto";
            case PassStateMode::On:
                return "on";
            default:
                return "off";
        }
    }

    // ==== pass state compare: begin ====
    void comparePassState(const PassState& d, const PassStateActual& a, PassStateCheck& out)
    {
        const PassState& s = a.state;
        U32 differ = 0;
        if ((d.declared & PassState::BLEND) &&
            (d.blend != s.blend ||
             (d.blend && (d.blend_src_rgb != s.blend_src_rgb || d.blend_dst_rgb != s.blend_dst_rgb ||
                          d.blend_src_a != s.blend_src_a || d.blend_dst_a != s.blend_dst_a ||
                          d.blend_eq_rgb != s.blend_eq_rgb || d.blend_eq_a != s.blend_eq_a))))
        {
            differ |= PassState::BLEND;
        }
        if ((d.declared & PassState::COLOR_MASK) &&
            (d.color_mask[0] != s.color_mask[0] || d.color_mask[1] != s.color_mask[1] ||
             d.color_mask[2] != s.color_mask[2] || d.color_mask[3] != s.color_mask[3]))
        {
            differ |= PassState::COLOR_MASK;
        }
        // Without the depth test nothing is written or compared (a pipeline
        // with the test off ignores both).
        if ((d.declared & PassState::DEPTH) &&
            (d.depth_test != s.depth_test ||
             (d.depth_test && (d.depth_write != s.depth_write || d.depth_func != s.depth_func))))
        {
            differ |= PassState::DEPTH;
        }
        if ((d.declared & PassState::CULL) && (d.cull != s.cull || (d.cull && d.cull_face != s.cull_face)))
        {
            differ |= PassState::CULL;
        }
        if ((d.declared & PassState::STENCIL) && d.stencil != s.stencil)
        {
            differ |= PassState::STENCIL;
        }
        if ((d.declared & PassState::POLYGON_OFFSET) && d.polygon_offset != s.polygon_offset)
        {
            differ |= PassState::POLYGON_OFFSET;
        }
        if ((d.declared & PassState::DEPTH_CLAMP) && d.depth_clamp != s.depth_clamp)
        {
            differ |= PassState::DEPTH_CLAMP;
        }
        if ((d.declared & PassState::PROGRAM) && d.program != s.program)
        {
            differ |= PassState::PROGRAM;
        }
        out.differ = differ;
        out.sampled_undeclared = 0;
        out.declared_unsampled = 0;
        out.declared_unbound = 0;
        // The units: only for the program declared (another program reads other units).
        const bool all = (d.declared & PassState::SAMPLERS) != 0;
        const bool part = (d.declared & PassState::SAMPLERS_PART) != 0;
        if ((all || part) && a.program_known && !(differ & PassState::PROGRAM))
        {
            if (all)
            {
                out.sampled_undeclared = a.sampled_units & ~d.sampler_units;
            }
            out.declared_unsampled = d.sampler_units & ~a.sampled_units;
            out.declared_unbound = d.sampler_units & a.sampled_units & ~a.bound_units;
        }
    }

    PassStateMode PassStateAuto::step(U32 frame, bool differ)
    {
        if (differ)
        {
            return PassStateMode::Off;
        }
        if (!counted_any || frame != last_frame)
        {
            counted_any = true;
            last_frame = frame;
            ++clean_frames;
        }
        return clean_frames >= CLEAN_FRAMES ? PassStateMode::On : PassStateMode::Verify;
    }
    // ==== pass state compare: end ====

    // ==== pass state write: begin ====
    U32 passStateWrites(const PassState& want, const PassState& now)
    {
        U32 fields = 0;
        if (want.declared & PassState::BLEND)
        {
            if (want.blend != now.blend)
            {
                fields |= FIELD_BLEND;
            }
            if (want.blend && (want.blend_src_rgb != now.blend_src_rgb || want.blend_dst_rgb != now.blend_dst_rgb ||
                               want.blend_src_a != now.blend_src_a || want.blend_dst_a != now.blend_dst_a))
            {
                fields |= FIELD_BLEND_FUNC;
            }
        }
        if ((want.declared & PassState::COLOR_MASK) &&
            (want.color_mask[0] != now.color_mask[0] || want.color_mask[1] != now.color_mask[1] ||
             want.color_mask[2] != now.color_mask[2] || want.color_mask[3] != now.color_mask[3]))
        {
            fields |= FIELD_COLOR_MASK;
        }
        if (want.declared & PassState::DEPTH)
        {
            if (want.depth_test != now.depth_test)
            {
                fields |= FIELD_DEPTH_TEST;
            }
            if (want.depth_test && want.depth_write != now.depth_write)
            {
                fields |= FIELD_DEPTH_WRITE;
            }
            if (want.depth_test && want.depth_func != now.depth_func)
            {
                fields |= FIELD_DEPTH_FUNC;
            }
        }
        if (want.declared & PassState::CULL)
        {
            if (want.cull != now.cull)
            {
                fields |= FIELD_CULL;
            }
            if (want.cull && want.cull_face != now.cull_face)
            {
                fields |= FIELD_CULL_FACE;
            }
        }
        if ((want.declared & PassState::STENCIL) && want.stencil != now.stencil)
        {
            fields |= FIELD_STENCIL;
        }
        if ((want.declared & PassState::POLYGON_OFFSET) && want.polygon_offset != now.polygon_offset)
        {
            fields |= FIELD_POLYGON_OFFSET;
        }
        if ((want.declared & PassState::DEPTH_CLAMP) && want.depth_clamp != now.depth_clamp)
        {
            fields |= FIELD_DEPTH_CLAMP;
        }
        return fields;
    }

    U32 passStateDiffers(const PassState& a, const PassState& b, U32 fields)
    {
        U32 differ = 0;
        if (a.blend != b.blend)
        {
            differ |= FIELD_BLEND;
        }
        if (a.blend_src_rgb != b.blend_src_rgb || a.blend_dst_rgb != b.blend_dst_rgb || a.blend_src_a != b.blend_src_a ||
            a.blend_dst_a != b.blend_dst_a || a.blend_eq_rgb != b.blend_eq_rgb || a.blend_eq_a != b.blend_eq_a)
        {
            differ |= FIELD_BLEND_FUNC;
        }
        if (a.color_mask[0] != b.color_mask[0] || a.color_mask[1] != b.color_mask[1] || a.color_mask[2] != b.color_mask[2] ||
            a.color_mask[3] != b.color_mask[3])
        {
            differ |= FIELD_COLOR_MASK;
        }
        if (a.depth_test != b.depth_test)
        {
            differ |= FIELD_DEPTH_TEST;
        }
        if (a.depth_write != b.depth_write)
        {
            differ |= FIELD_DEPTH_WRITE;
        }
        if (a.depth_func != b.depth_func)
        {
            differ |= FIELD_DEPTH_FUNC;
        }
        if (a.cull != b.cull)
        {
            differ |= FIELD_CULL;
        }
        if (a.cull_face != b.cull_face)
        {
            differ |= FIELD_CULL_FACE;
        }
        if (a.stencil != b.stencil)
        {
            differ |= FIELD_STENCIL;
        }
        if (a.polygon_offset != b.polygon_offset)
        {
            differ |= FIELD_POLYGON_OFFSET;
        }
        if (a.depth_clamp != b.depth_clamp)
        {
            differ |= FIELD_DEPTH_CLAMP;
        }
        if (a.program != b.program)
        {
            differ |= FIELD_PROGRAM;
        }
        return differ & fields;
    }

    U32 passStateRestores(const PassState& written, U32 fields, const PassState& now)
    {
        return fields & ~passStateDiffers(written, now, fields);
    }
    // ==== pass state write: end ====
}

namespace
{
    using namespace LLRHI;

    constexpr U32 ENTRIES = 32;         // pass names kept apart (11 declared now)
    constexpr U32 LOGGED_MAX = 8;       // differences logged one by one

    const char* const sGroupNames[PassState::GROUP_COUNT] = { "blend", "color mask", "depth", "cull", "stencil", "polygon offset",
                                                              "depth clamp", "program", "samplers", "samplers (part)" };
    const char* const sViewNames[PASS_VIEW_COUNT] = { "main", "probe", "reflection" };

    struct Counts
    {
        U32 passes = 0;
        U32 draws = 0;
        U32 checks = 0;
        U32 differ_checks = 0;          // checks with any difference
        U32 groups[PassState::GROUP_COUNT] = {};
        U32 sampled_undeclared = 0;     // units, summed over checks
        U32 declared_unsampled = 0;
        U32 declared_unbound = 0;
        U32 not_declared = 0;           // checks of a scope that declared nothing (a pool's mistake)
        U32 census_passes = 0;          // passes whose census was taken
        U64 state_calls = 0;
        U64 unit_bindings = 0;
        U64 program_changes = 0;
        // research/207 T4
        U32 copy_checks = 0;            // copy watches (before and after the draws)
        U32 copy_differ = 0;            // of those, with the copy and the backend apart
        U32 copy_fields = 0;            // PassStateField bits seen apart
        U32 would_write = 0;            // passes the write-through would write to
        U32 would_fields = 0;           // PassStateField bits it would write
    };

    struct Entry
    {
        const char* name = nullptr;
        Counts views[PASS_VIEW_COUNT];
    };

    struct Global
    {
        PassStateMode asked = PassStateMode::Off;
        PassStateMode now = PassStateMode::Off;    // Off, Verify or On (auto starts as Verify)
        PassStateAuto progress;
        Entry entries[ENTRIES];
        U32 entry_count = 0;
        U32 overflow = 0;               // passes of names past ENTRIES (not counted)
        U32 logged = 0;
        bool warned_no_backend = false;
        bool told_on = false;
        PassStateScope* open = nullptr;
        // research/207 T4, on: the write-through
        U64 on_passes = 0;              // passes written through (declared, with the copy)
        U64 on_written = 0;             // of those, with a field written (a declaration verify would have flagged)
        U64 on_fields = 0;              // fields written
        U64 on_kept = 0;                // passes with a written field the draws wrote again (left as they wrote it)
        U32 on_logged = 0;
        bool warned_no_copy = false;
        // research/204 10 (vk-s4c2): the scope whose write-through lives and holds its whole declaration (openPassState)
        const PassStateScope* writing = nullptr;
    };

    // research/204 10 (vk-s4c2): LLRHI_PASS_UNITS.
    struct Units
    {
        PassUnitsMode asked = PassUnitsMode::Off;
        PassUnitsMode now = PassUnitsMode::Off;     // Off, Verify or On (auto starts as Verify)
        PassStateAuto progress;
        U32 reads_seen = 0;             // the backend's reads at the last cleanup
        U64 cleanups = 0;               // since the last report
        U64 units_left = 0;
        U64 reads = 0;
        U64 kept = 0;                   // on: cleanups left out
        U32 not_watched = 0;
    };

    Units& units()
    {
        static Units u = []()
        {
            Units made;
            made.asked = passUnitsMode();
            made.now = made.asked == PassUnitsMode::Auto ? PassUnitsMode::Verify : made.asked;
            return made;
        }();
        return u;
    }

    const char* passUnitsModeName(PassUnitsMode mode)
    {
        switch (mode)
        {
            case PassUnitsMode::Verify:
                return "verify";
            case PassUnitsMode::Auto:
                return "auto";
            case PassUnitsMode::On:
                return "on";
            default:
                return "off";
        }
    }

    Global& global()
    {
        static Global g = []()
        {
            Global made;
            made.asked = passStateMode();
            made.now = made.asked == PassStateMode::Auto ? PassStateMode::Verify : made.asked;
            return made;
        }();
        return g;
    }

    U8 entry_of(Global& g, const char* name)
    {
        for (U32 i = 0; i < g.entry_count; ++i)
        {
            if (g.entries[i].name == name || !strcmp(g.entries[i].name, name))
            {
                return (U8)i;
            }
        }
        if (g.entry_count >= ENTRIES)
        {
            ++g.overflow;
            return 0xff;
        }
        g.entries[g.entry_count].name = name;
        return (U8)g.entry_count++;
    }

    void put_bits(std::ostringstream& out, U32 bits)
    {
        out << "{";
        bool first = true;
        for (U32 u = 0; u < PASS_STATE_UNITS; ++u)
        {
            if (bits & (1u << u))
            {
                out << (first ? "" : ",") << u;
                first = false;
            }
        }
        out << "}";
    }

    // One difference, in words (verify, the first LOGGED_MAX).
    std::string describe(const char* name, U8 view, const PassState& d, const PassStateActual& a, const PassStateCheck& c)
    {
        const PassState& s = a.state;
        std::ostringstream out;
        out << std::hex;
        out << name << " (" << sViewNames[view] << "):";
        if (c.differ & PassState::BLEND)
        {
            out << " blend declared " << d.blend << " 0x" << d.blend_src_rgb << "/0x" << d.blend_dst_rgb << "/0x" << d.blend_src_a << "/0x"
                << d.blend_dst_a << " eq 0x" << d.blend_eq_rgb << "/0x" << d.blend_eq_a << ", actual " << s.blend << " 0x" << s.blend_src_rgb
                << "/0x" << s.blend_dst_rgb << "/0x" << s.blend_src_a << "/0x" << s.blend_dst_a << " eq 0x" << s.blend_eq_rgb << "/0x"
                << s.blend_eq_a << ";";
        }
        if (c.differ & PassState::COLOR_MASK)
        {
            out << " color mask declared " << d.color_mask[0] << d.color_mask[1] << d.color_mask[2] << d.color_mask[3] << ", actual "
                << s.color_mask[0] << s.color_mask[1] << s.color_mask[2] << s.color_mask[3] << ";";
        }
        if (c.differ & PassState::DEPTH)
        {
            out << " depth declared " << d.depth_test << d.depth_write << " 0x" << d.depth_func << ", actual " << s.depth_test
                << s.depth_write << " 0x" << s.depth_func << ";";
        }
        if (c.differ & PassState::CULL)
        {
            out << " cull declared " << d.cull << " 0x" << d.cull_face << ", actual " << s.cull << " 0x" << s.cull_face << ";";
        }
        if (c.differ & PassState::STENCIL)
        {
            out << " stencil declared " << d.stencil << ", actual " << s.stencil << ";";
        }
        if (c.differ & PassState::POLYGON_OFFSET)
        {
            out << " polygon offset declared " << d.polygon_offset << ", actual " << s.polygon_offset << ";";
        }
        if (c.differ & PassState::DEPTH_CLAMP)
        {
            out << " depth clamp declared " << d.depth_clamp << ", actual " << s.depth_clamp << ";";
        }
        out << std::dec;
        if (c.differ & PassState::PROGRAM)
        {
            out << " program declared " << d.program << ", actual " << s.program << ";";
        }
        if (c.sampled_undeclared || c.declared_unsampled || c.declared_unbound)
        {
            out << " units declared ";
            put_bits(out, d.sampler_units);
            out << ", read ";
            put_bits(out, a.sampled_units);
            out << ", bound ";
            put_bits(out, a.bound_units);
            out << ";";
        }
        return out.str();
    }

    // The census: what the backend counted between two snapshots (none
    // when a total started over in between: the reports reset some).
    void add_census(Counts& c, const PassStateCounters& from, const PassStateCounters& to)
    {
        if (to.state_calls < from.state_calls || to.unit_bindings < from.unit_bindings || to.program_changes < from.program_changes)
        {
            return;
        }
        ++c.census_passes;
        c.state_calls += to.state_calls - from.state_calls;
        c.unit_bindings += to.unit_bindings - from.unit_bindings;
        c.program_changes += to.program_changes - from.program_changes;
    }

    // ---- research/207 T4: the copy watch and the write-through's words
    const char* const sFieldNames[FIELD_COUNT] = { "blend", "blend func", "color mask", "depth test", "depth write", "depth func",
                                                   "cull", "cull face", "stencil", "polygon offset", "depth clamp", "program" };

    U32 count_bits(U32 bits)
    {
        U32 n = 0;
        for (; bits; bits &= bits - 1)
        {
            ++n;
        }
        return n;
    }

    void put_fields(std::ostringstream& out, U32 fields)
    {
        bool first = true;
        for (U32 i = 0; i < FIELD_COUNT; ++i)
        {
            if (fields & (1u << i))
            {
                out << (first ? "" : ", ") << sFieldNames[i];
                first = false;
            }
        }
    }

    // The values of `fields` in `s`, for the log.
    void put_values(std::ostringstream& out, const PassState& s, U32 fields)
    {
        out << std::hex;
        if (fields & (FIELD_BLEND | FIELD_BLEND_FUNC))
        {
            out << " blend " << s.blend << " 0x" << s.blend_src_rgb << "/0x" << s.blend_dst_rgb << "/0x" << s.blend_src_a << "/0x"
                << s.blend_dst_a << " eq 0x" << s.blend_eq_rgb << "/0x" << s.blend_eq_a;
        }
        if (fields & FIELD_COLOR_MASK)
        {
            out << " mask " << s.color_mask[0] << s.color_mask[1] << s.color_mask[2] << s.color_mask[3];
        }
        if (fields & (FIELD_DEPTH_TEST | FIELD_DEPTH_WRITE | FIELD_DEPTH_FUNC))
        {
            out << " depth " << s.depth_test << s.depth_write << " 0x" << s.depth_func;
        }
        if (fields & (FIELD_CULL | FIELD_CULL_FACE))
        {
            out << " cull " << s.cull << " 0x" << s.cull_face;
        }
        if (fields & FIELD_STENCIL)
        {
            out << " stencil " << s.stencil;
        }
        if (fields & FIELD_POLYGON_OFFSET)
        {
            out << " polygon offset " << s.polygon_offset;
        }
        if (fields & FIELD_DEPTH_CLAMP)
        {
            out << " depth clamp " << s.depth_clamp;
        }
        out << std::dec;
        if (fields & FIELD_PROGRAM)
        {
            out << " program " << s.program;
        }
    }

    // Verify: the viewer's copy against the backend (every field). `when`:
    // "before the draws" or "after the draws". True when they are apart.
    bool copy_watch(Global& g, Counts& c, const char* name, U8 view, const char* when)
    {
        if (!sPassStateCopy || !sPassStateBackend)
        {
            return false;
        }
        PassState copy;
        sPassStateCopy->read(copy);
        PassStateActual actual;
        if (!sPassStateBackend->actual(actual))
        {
            return false;
        }
        ++c.copy_checks;
        const U32 apart = passStateDiffers(copy, actual.state, FIELDS_ALL);
        if (!apart)
        {
            return false;
        }
        ++c.copy_differ;
        c.copy_fields |= apart;
        if (g.logged < LOGGED_MAX)
        {
            ++g.logged;
            std::ostringstream out;
            out << name << " (" << sViewNames[view] << "), " << when << ": ";
            put_fields(out, apart);
            out << "; copy";
            put_values(out, copy, apart);
            out << "; backend";
            put_values(out, actual.state, apart);
            LL_WARNS("PassState") << "copy watch " << g.logged << "/" << LOGGED_MAX << ": the copy and the backend apart: " << out.str()
                                  << LL_ENDL;
        }
        return true;
    }
}

namespace LLRHI
{
    PassStateScope::PassStateScope(const char* name, PassView view, U32 frame)
    {
        if (!name)
        {
            return;
        }
        Global& g = global();
        if (g.now == PassStateMode::Off)
        {
            return;     // off (or auto turned it off): nothing at all
        }
        mName = name;
        mView = (U8)(view < PASS_VIEW_COUNT ? view : PASS_VIEW_MAIN);
        mFrame = frame;
        mDeclaring = true;
        mOuter = g.open;
        g.open = this;
        if (g.now == PassStateMode::On)
        {
            if (!g.told_on)
            {
                g.told_on = true;
                LL_INFOS("PassState") << "pass states on: the world passes' declarations are published (nothing compared; research/204)"
                                      << LL_ENDL;
            }
            return;
        }
        // Verify (or auto, still verifying).
        mEntry = entry_of(g, name);
        if (mEntry == 0xff)
        {
            return;
        }
        ++g.entries[mEntry].views[mView].passes;
        if (!sPassStateBackend)
        {
            if (!g.warned_no_backend)
            {
                g.warned_no_backend = true;
                LL_WARNS("PassState") << "LLRHI_PASS_STATE=" << passStateModeName(g.asked)
                                      << ": no backend to compare with (not Vulkan); nothing is checked"
                                      << LL_ENDL;
            }
            return;
        }
        mCounting = sPassStateBackend->counters(mStart);
    }

    PassStateScope::~PassStateScope()
    {
        if (!mName)
        {
            return;
        }
        Global& g = global();
        if (g.open == this)
        {
            g.open = mOuter;
        }
        if (mCounting && mEntry != 0xff && sPassStateBackend)
        {
            PassStateCounters end;
            if (sPassStateBackend->counters(end))
            {
                add_census(g.entries[mEntry].views[mView], mStart, end);
            }
        }
    }

    void PassStateScope::declare(const PassState& state)
    {
        if (!mDeclaring)
        {
            return;
        }
        mState = state;
        mDeclared = true;
    }

    void PassStateScope::check(U32 draws)
    {
        if (mEntry == 0xff || !draws)
        {
            return;     // off, on, or not counted
        }
        Global& g = global();
        if (g.now != PassStateMode::Verify)
        {
            return;     // auto decided while this pass was open
        }
        Counts& c = g.entries[mEntry].views[mView];
        c.draws += draws;
        if (!mDeclared)
        {
            ++c.not_declared;
            return;
        }
        PassStateActual actual;
        if (!sPassStateBackend || !sPassStateBackend->actual(actual))
        {
            return;
        }
        PassStateCheck result;
        comparePassState(mState, actual, result);
        ++c.checks;
        const bool differ = result.differ || result.sampled_undeclared || result.declared_unsampled || result.declared_unbound;
        if (differ)
        {
            ++c.differ_checks;
            for (U32 i = 0; i < PassState::GROUP_COUNT; ++i)
            {
                c.groups[i] += (result.differ >> i) & 1u;
            }
            auto units = [](U32 bits)
            {
                U32 n = 0;
                for (; bits; bits &= bits - 1)
                {
                    ++n;
                }
                return n;
            };
            c.sampled_undeclared += units(result.sampled_undeclared);
            c.declared_unsampled += units(result.declared_unsampled);
            c.declared_unbound += units(result.declared_unbound);
            if (g.logged < LOGGED_MAX)
            {
                ++g.logged;
                LL_WARNS("PassState") << "difference " << g.logged << "/" << LOGGED_MAX << ": "
                                      << describe(mName, mView, mState, actual, result) << LL_ENDL;
            }
        }
        // research/207 T4: the copy watch (the copy must say what the backend
        // has: the write-through compares with the copy, not the backend).
        const bool copy_apart = copy_watch(g, c, mName, mView, "before the draws");
        if (g.asked == PassStateMode::Auto)
        {
            const PassStateMode next = g.progress.step(mFrame, differ || copy_apart);
            if (next != PassStateMode::Verify)
            {
                g.now = next;
                LL_INFOS("PassState") << "LLRHI_PASS_STATE=auto: "
                                      << (next == PassStateMode::On ? "on after " : "off (a difference, see above) after ")
                                      << g.progress.clean_frames << " clean world frames" << LL_ENDL;
            }
        }
    }

    // research/207 T4 (4.2 (2)): the write-through around a pass's draws.
    PassStateWrite::PassStateWrite(const PassStateScope& scope)
    {
        if (!scope.mDeclared)
        {
            return;     // off, a pass that is not declared, or a pool that declared nothing
        }
        Global& g = global();
        if (g.now == PassStateMode::On)
        {
            if (!sPassStateCopy)
            {
                if (!g.warned_no_copy)
                {
                    g.warned_no_copy = true;
                    LL_WARNS("PassState") << "LLRHI_PASS_STATE on: no state copy (llrender not initialised); nothing written" << LL_ENDL;
                }
                return;
            }
            mScope = &scope;
            ++g.on_passes;
            sPassStateCopy->read(mBefore);
            mWritten = passStateWrites(scope.mState, mBefore);
            // research/204 10 (vk-s4c2): the declaration is published (openPassState) while this
            // lives, once the copy holds all of it (a value the copy could not write: not published).
            mSavedWriting = g.writing;
            mPublishing = true;
            if (!mWritten)
            {
                g.writing = &scope;
                return;     // the declaration is what the copy says: the draws are drawn as they would be anyway
            }
            ++g.on_written;
            g.on_fields += count_bits(mWritten);
            if (g.on_logged < LOGGED_MAX)
            {
                ++g.on_logged;
                std::ostringstream out;
                out << scope.mName << " (" << sViewNames[scope.mView] << "): ";
                put_fields(out, mWritten);
                out << "; was";
                put_values(out, mBefore, mWritten);
                out << "; declared";
                put_values(out, scope.mState, mWritten);
                LL_WARNS("PassState") << "write-through " << g.on_logged << "/" << LOGGED_MAX
                                      << ": the declaration differs from the copy, written for the draws (verify would count it): "
                                      << out.str() << LL_ENDL;
            }
            sPassStateCopy->write(scope.mState, mWritten);
            PassState after;
            sPassStateCopy->read(after);
            g.writing = passStateWrites(scope.mState, after) ? nullptr : &scope;
            return;
        }
        if (g.now != PassStateMode::Verify || scope.mEntry == 0xff || !sPassStateCopy)
        {
            return;
        }
        // Verify: what the write-through would write (nothing, once the
        // declaration and the copy agree), and the copy watch at the end.
        mScope = &scope;
        mWatch = true;
        PassState copy;
        sPassStateCopy->read(copy);
        const U32 would = passStateWrites(scope.mState, copy);
        if (would)
        {
            Counts& c = g.entries[scope.mEntry].views[scope.mView];
            ++c.would_write;
            c.would_fields |= would;
        }
    }

    PassStateWrite::~PassStateWrite()
    {
        if (!mScope)
        {
            return;
        }
        Global& g = global();
        if (mPublishing)
        {
            g.writing = mSavedWriting;  // research/204 10: published no more (the outer write-through's again)
        }
        if (mWatch)
        {
            if (g.now == PassStateMode::Verify && mScope->mEntry != 0xff)
            {
                Counts& c = g.entries[mScope->mEntry].views[mScope->mView];
                const bool apart = copy_watch(g, c, mScope->mName, mScope->mView, "after the draws");
                if (apart && g.asked == PassStateMode::Auto)
                {
                    const PassStateMode next = g.progress.step(mScope->mFrame, true);
                    if (next != PassStateMode::Verify)
                    {
                        g.now = next;
                        LL_INFOS("PassState") << "LLRHI_PASS_STATE=auto: off (the copy watch, see above) after "
                                              << g.progress.clean_frames << " clean world frames" << LL_ENDL;
                    }
                }
            }
            return;
        }
        if (!mWritten || !sPassStateCopy)
        {
            return;
        }
        // Back to what the copy said before, for the fields the draws left
        // as written (a field the draws wrote again is theirs: kept).
        PassState now;
        sPassStateCopy->read(now);
        const U32 restore = passStateRestores(mScope->mState, mWritten, now);
        if (restore != mWritten)
        {
            ++g.on_kept;
        }
        if (restore)
        {
            sPassStateCopy->write(mBefore, restore);
        }
    }

    // research/204 10 (vk-s4c2): read once; unset: off.
    PassUnitsMode passUnitsMode()
    {
        static const PassUnitsMode mode = []()
        {
            const char* env = std::getenv("LLRHI_PASS_UNITS");
            if (env && !strcmp(env, "verify"))
            {
                return PassUnitsMode::Verify;
            }
            if (env && !strcmp(env, "auto"))
            {
                return PassUnitsMode::Auto;
            }
            if (env && (!strcmp(env, "on") || !strcmp(env, "1")))
            {
                return PassUnitsMode::On;
            }
            return PassUnitsMode::Off;
        }();
        return mode;
    }

    bool passUnitsKeep()
    {
        Units& u = units();
        if (u.now != PassUnitsMode::On)
        {
            return false;
        }
        ++u.kept;
        return true;
    }

    void passUnitsLeft(U32 left, U32 frame)
    {
        Units& u = units();
        if (u.now != PassUnitsMode::Verify || !left)
        {
            return;
        }
        if (!sPassStateBackend)
        {
            return;     // not Vulkan: nothing to watch with
        }
        // The reads since the last cleanup belong to the units it left (a world frame's last
        // cleanup's reads are seen at the next frame's first).
        const U32 reads = sPassStateBackend->unitsRead();
        const bool read = reads != u.reads_seen;
        u.reads += reads - u.reads_seen;
        u.reads_seen = reads;
        if (!sPassStateBackend->unitsLeft(left))
        {
            ++u.not_watched;
            return;
        }
        ++u.cleanups;
        for (U32 bits = left; bits; bits &= bits - 1)
        {
            ++u.units_left;
        }
        if (u.asked == PassUnitsMode::Auto)
        {
            const PassStateMode next = u.progress.step(frame, read);
            if (next != PassStateMode::Verify)
            {
                u.now = next == PassStateMode::On ? PassUnitsMode::On : PassUnitsMode::Off;
                LL_INFOS("PassState") << "LLRHI_PASS_UNITS=auto: "
                                      << (next == PassStateMode::On ? "on (the world passes keep their units) after "
                                                                    : "off (a unit a cleanup left was read before it was bound) after ")
                                      << u.progress.clean_frames << " clean world frames" << LL_ENDL;
            }
        }
    }

    const PassState* openPassState()
    {
        Global& g = global();
        // research/204 10 (vk-s4c2, design review 8): only inside the open pass's own write-through,
        // with the copy holding the whole declaration (so the backend's state is the declaration).
        if (g.now != PassStateMode::On || !g.open || g.writing != g.open)
        {
            return nullptr;
        }
        return g.open->declared();
    }

    std::string passStateReport(U32 frames)
    {
        Global& g = global();
        Units& u = units();
        // research/204 10 (vk-s4c2): the pass units' line part (LLRHI_PASS_UNITS not off).
        std::ostringstream unit_text;
        if (u.asked != PassUnitsMode::Off)
        {
            const F64 per_frame = 1.0 / (frames ? frames : 1);
            unit_text << std::fixed << std::setprecision(2) << "units (LLRHI_PASS_UNITS=" << passUnitsModeName(u.asked);
            if (u.asked == PassUnitsMode::Auto)
            {
                unit_text << ", now " << passUnitsModeName(u.now) << ", clean world frames " << u.progress.clean_frames << "/"
                          << PassStateAuto::CLEAN_FRAMES;
            }
            unit_text << "): per frame cleanups " << u.cleanups * per_frame << " (units left " << u.units_left * per_frame
                      << "), read before bound " << u.reads << ", cleanups left out " << u.kept * per_frame;
            if (u.not_watched)
            {
                unit_text << ", not watched (another thread's GL state) " << u.not_watched;
            }
            u.cleanups = u.units_left = u.reads = u.kept = 0;
            u.not_watched = 0;
        }
        if (g.asked == PassStateMode::Off || (!g.entry_count && !g.on_passes))
        {
            return unit_text.str();
        }
        frames = frames ? frames : 1;
        const F64 per = 1.0 / frames;
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        if (u.asked != PassUnitsMode::Off)
        {
            out << unit_text.str() << "; ";
        }
        out << "LLRHI_PASS_STATE=" << passStateModeName(g.asked);
        if (g.asked == PassStateMode::Auto)
        {
            out << " (now " << passStateModeName(g.now) << ", clean world frames " << g.progress.clean_frames << "/"
                << PassStateAuto::CLEAN_FRAMES << ")";
        }
        bool any = false;
        // research/207 T4, on: the write-through (written > 0 means a
        // declaration the copy did not hold: the draws differ from off's).
        if (g.on_passes)
        {
            any = true;
            out << "; write-through per frame: passes " << g.on_passes * per << ", written " << g.on_written * per << " (fields "
                << g.on_fields * per << "), kept " << g.on_kept * per;
            g.on_passes = g.on_written = g.on_fields = g.on_kept = 0;
        }
        if (g.entry_count)
        {
            out << "; per frame, by pass and view: passes/draws/checks, differ, census (state calls, unit bindings, programs),"
                   " copy watch (checks, apart), write-through would write";
        }
        for (U32 i = 0; i < g.entry_count; ++i)
        {
            Entry& e = g.entries[i];
            for (U32 v = 0; v < PASS_VIEW_COUNT; ++v)
            {
                Counts& c = e.views[v];
                if (!c.passes)
                {
                    continue;
                }
                any = true;
                out << "; " << e.name << " " << sViewNames[v] << " " << c.passes * per << "/" << c.draws * per << "/" << c.checks * per
                    << ", differ " << c.differ_checks * per;
                if (c.differ_checks)
                {
                    out << " (";
                    bool first = true;
                    for (U32 k = 0; k < PassState::GROUP_COUNT; ++k)
                    {
                        if (c.groups[k])
                        {
                            out << (first ? "" : ", ") << sGroupNames[k] << " " << c.groups[k] * per;
                            first = false;
                        }
                    }
                    if (c.sampled_undeclared || c.declared_unsampled || c.declared_unbound)
                    {
                        out << (first ? "" : ", ") << "units read not declared " << c.sampled_undeclared * per << " declared not read "
                            << c.declared_unsampled * per << " declared unbound " << c.declared_unbound * per;
                    }
                    out << ")";
                }
                if (c.not_declared)
                {
                    out << ", checks without a declaration " << c.not_declared * per;
                }
                if (c.census_passes)
                {
                    const F64 per_pass = 1.0 / c.census_passes;
                    out << ", census " << c.state_calls * per << "/" << c.unit_bindings * per << "/" << c.program_changes * per
                        << " (a pass " << c.state_calls * per_pass << "/" << c.unit_bindings * per_pass << "/"
                        << c.program_changes * per_pass << ")";
                }
                // research/207 T4
                if (c.copy_checks)
                {
                    out << ", copy watch " << c.copy_checks * per << " apart " << c.copy_differ * per;
                    if (c.copy_differ)
                    {
                        out << " (";
                        put_fields(out, c.copy_fields);
                        out << ")";
                    }
                }
                if (c.would_write)
                {
                    out << ", would write " << c.would_write * per << " (";
                    put_fields(out, c.would_fields);
                    out << ")";
                }
                c = Counts();
            }
        }
        if (g.overflow)
        {
            out << "; passes of names not kept " << g.overflow * per;
            g.overflow = 0;
        }
        return any ? out.str() : std::string();
    }
}
