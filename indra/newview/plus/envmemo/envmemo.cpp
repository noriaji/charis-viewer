/**
 * @file envmemo.cpp
 * @brief The sky and water uniforms sent again only when they change
 *        (research/187 N4).
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

#include "llviewerprecompiledheaders.h"

#include "envmemo.h"

#include "llappviewer.h"    // gFrameTimeSeconds

#include <algorithm>

namespace plus::envmemo
{
    namespace
    {
        enum Kind : U32
        {
            KIND_INT,
            KIND_FLOAT,
            KIND_VEC4,
            KIND_VEC3
        };

        // A value as a shader ends up with it, as the value cache of
        // LLGLSLShader keeps it (uniform1i: the int as a float in x;
        // uniform3fv: w 0).
        struct Entry
        {
            S32 index;
            U32 kind;
            LLVector4 value;
        };

        constexpr U32 GROUPS = LLGLSLShader::SG_COUNT;
        constexpr U32 STEPS = 8;    // versions a shader may be behind and still get only the changes

        // What changed from one version to the next, per group: the positions
        // in the table (same layout), or everything (full).
        struct Step
        {
            U32 version = 0;    // 0: not a step (being written, or never)
            bool full[GROUPS] = {};
            std::vector<U16> changed[GROUPS];
        };

        struct Stats
        {
            U64 updates = 0;
            U64 changed_updates = 0;
            U64 applies_full = 0;
            U64 applies_diff = 0;
            U64 values_diff = 0;
            U64 applies_skip = 0;   // the verify: binds the memo would have left alone
            U64 mismatches = 0;
            std::string last_mismatch;
        };

        // Plain values only (no GL objects): safe as statics at exit.
        struct State
        {
            std::vector<Entry> table[GROUPS];   // the values of the last build
            std::vector<Entry> build[GROUPS];   // being built
            bool have = false;                  // table holds a build made with the memo or its verify on
            U32 layout[GROUPS] = { 1, 1, 1, 1 };
            Step steps[STEPS];
            std::vector<S32> slot_of;           // uniform index -> position in the table being built
            std::vector<U32> slot_stamp;
            U32 stamp = 0;
            std::vector<U8> marked;             // the verify: positions a shader gets in a diff
            Stats stats;
        };
        State sState;

        void add(std::vector<Entry>& table, S32 index, U32 kind, const LLVector4& value)
        {
            if (index < 0)
            {
                return;
            }
            State& s = sState;
            if ((size_t)index >= s.slot_of.size())
            {   // grows to the number of reserved uniforms once
                s.slot_of.resize(index + 1, -1);
                s.slot_stamp.resize(index + 1, 0);
            }
            if (s.slot_stamp[index] == s.stamp)
            {   // set again later in the order: the last one is what the shader keeps
                Entry& entry = table[s.slot_of[index]];
                entry.kind = kind;
                entry.value = value;
                return;
            }
            s.slot_stamp[index] = s.stamp;
            s.slot_of[index] = (S32)table.size();
            table.push_back({ index, kind, value });
        }

        // In the order of LLShaderUniforms::apply.
        void fold(std::vector<Entry>& table, const LLShaderUniforms& uniforms)
        {
            for (const auto& u : uniforms.mIntegers)
            {
                add(table, u.mUniform, KIND_INT, LLVector4((F32)u.mValue, 0.f, 0.f, 0.f));
            }
            for (const auto& u : uniforms.mFloats)
            {
                add(table, u.mUniform, KIND_FLOAT, LLVector4(u.mValue, 0.f, 0.f, 0.f));
            }
            for (const auto& u : uniforms.mVectors)
            {
                add(table, u.mUniform, KIND_VEC4, u.mValue);
            }
            for (const auto& u : uniforms.mVector3s)
            {
                add(table, u.mUniform, KIND_VEC3, LLVector4(u.mValue.mV[0], u.mValue.mV[1], u.mValue.mV[2], 0.f));
            }
        }

        // In the order of LLEnvironment::updateShaderUniforms.
        void build(std::vector<Entry>& table, U32 group, const LLShaderUniforms* sky, const LLShaderUniforms* water)
        {
            State& s = sState;
            if (++s.stamp == 0)
            {
                std::fill(s.slot_stamp.begin(), s.slot_stamp.end(), 0);
                s.stamp = 1;
            }
            table.clear();
            fold(table, sky[LLGLSLShader::SG_ANY]);
            fold(table, water[LLGLSLShader::SG_ANY]);
            if (group != LLGLSLShader::SG_ANY)
            {
                fold(table, sky[group]);
                fold(table, water[group]);
            }
        }

        bool same_layout(const std::vector<Entry>& a, const std::vector<Entry>& b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].index != b[i].index || a[i].kind != b[i].kind)
                {
                    return false;
                }
            }
            return true;
        }

        void send(LLGLSLShader* shader, const Entry& entry)
        {
            switch (entry.kind)
            {
            case KIND_INT:
                shader->uniform1i(entry.index, (S32)entry.value.mV[0]);
                break;
            case KIND_FLOAT:
                shader->uniform1f(entry.index, entry.value.mV[0]);
                break;
            case KIND_VEC4:
                shader->uniform4fv(entry.index, 1, entry.value.mV);
                break;
            default:
                shader->uniform3fv(entry.index, 1, entry.value.mV);
                break;
            }
        }

        // The versions after `have` up to `now` are all kept steps, none of
        // them full for the group.
        bool diffable(U32 have, U32 now, U32 group)
        {
            if (!have || now - have >= STEPS)
            {
                return false;
            }
            for (U32 v = have + 1; v != now + 1; ++v)
            {
                const Step& step = sState.steps[v % STEPS];
                if (step.version != v || step.full[group])
                {
                    return false;
                }
            }
            return true;
        }

        void mark_locations(LLGLSLShader* shader, U32 group)
        {
            State& s = sState;
            if (shader->mEnvLayout == s.layout[group] && !shader->mEnvLocations.empty())
            {
                return;
            }
            shader->mEnvLocations.clear();
            for (const Entry& entry : s.table[group])
            {
                if ((size_t)entry.index < shader->mUniform.size() && shader->mUniform[entry.index] >= 0)
                {
                    shader->mEnvLocations.push_back(shader->mUniform[entry.index]);
                }
            }
            std::sort(shader->mEnvLocations.begin(), shader->mEnvLocations.end());
            shader->mEnvLocations.erase(std::unique(shader->mEnvLocations.begin(), shader->mEnvLocations.end()),
                                        shader->mEnvLocations.end());
            shader->mEnvLayout = s.layout[group];
        }

        // The verify: what the shader holds against the values, at the
        // positions the memo would not have sent.
        void verify(LLGLSLShader* shader, U32 group, bool skip, U32 have, U32 now)
        {
            State& s = sState;
            const std::vector<Entry>& table = s.table[group];
            s.marked.assign(table.size(), 0);
            if (!skip)
            {
                for (U32 v = have + 1; v != now + 1; ++v)
                {
                    for (U16 pos : s.steps[v % STEPS].changed[group])
                    {
                        if (pos < s.marked.size())
                        {
                            s.marked[pos] = 1;
                        }
                    }
                }
            }
            for (size_t i = 0; i < table.size(); ++i)
            {
                const Entry& entry = table[i];
                if (s.marked[i] || (size_t)entry.index >= shader->mUniform.size() || shader->mUniform[entry.index] < 0)
                {
                    continue;
                }
                const LLVector4* held = shader->cachedUniform(shader->mUniform[entry.index]);
                if (!held || *held != entry.value)
                {
                    ++s.stats.mismatches;
                    s.stats.last_mismatch = llformat("%s, uniform %d (%s)", shader->mName.c_str(), entry.index,
                                                     skip ? "kept" : "not in the changes");
                    return;
                }
            }
        }

        void endFrame()
        {
            static F32 logged_at = 0.f;
            State& s = sState;
            if (!LLRHI::sEnvUniformMemoVerify)
            {
                s.stats = Stats();
                logged_at = gFrameTimeSeconds;
                return;
            }
            if (gFrameTimeSeconds - logged_at < 5.f)
            {
                return;
            }
            logged_at = gFrameTimeSeconds;
            const Stats& st = s.stats;
            LL_INFOS("EnvMemo") << llformat("environment uniform memo [%s] over %llu updates: changed %llu; dirty binds that "
                                            "sent all %llu, only the changes %llu (%llu values), nothing %llu; verify "
                                            "differences %llu (must stay 0)",
                                            LLRHI::sEnvUniformMemo ? "on" : "off", (unsigned long long)st.updates,
                                            (unsigned long long)st.changed_updates, (unsigned long long)st.applies_full,
                                            (unsigned long long)st.applies_diff, (unsigned long long)st.values_diff,
                                            (unsigned long long)st.applies_skip, (unsigned long long)st.mismatches)
                                << (st.mismatches ? ", last: " + st.last_mismatch : std::string()) << LL_ENDL;
            s.stats = Stats();
        }
    }

    void afterUpdate(const LLShaderUniforms* sky, const LLShaderUniforms* water)
    {
        State& s = sState;
        if (!LLRHI::sEnvUniformMemo && !LLRHI::sEnvUniformMemoVerify)
        {
            // Off: the version still moves, so that every shader is behind
            // once the memo is turned on.
            ++LLGLSLShader::sEnvUniformsVersion;
            s.have = false;
            return;
        }
        endFrame();
        ++s.stats.updates;

        const U32 next = LLGLSLShader::sEnvUniformsVersion + 1;
        Step& step = s.steps[next % STEPS];
        step.version = 0;   // written now; a step only once the version moves to it
        bool changed = false;
        for (U32 g = 0; g < GROUPS; ++g)
        {
            build(s.build[g], g, sky, water);
            step.changed[g].clear();
            step.full[g] = !s.have || !same_layout(s.build[g], s.table[g]);
            if (step.full[g])
            {
                ++s.layout[g];
                changed = true;
            }
            else
            {
                const std::vector<Entry>& was = s.table[g];
                const std::vector<Entry>& made = s.build[g];
                for (size_t i = 0; i < made.size(); ++i)
                {
                    if (made[i].value != was[i].value)
                    {
                        step.changed[g].push_back((U16)i);
                    }
                }
                changed |= !step.changed[g].empty();
            }
            s.table[g].swap(s.build[g]);
        }
        s.have = true;
        if (changed)
        {
            ++s.stats.changed_updates;
            LLGLSLShader::sEnvUniformsVersion = next;
            step.version = next;
        }
    }

    void invalidate()
    {
        ++LLGLSLShader::sEnvUniformsVersion;
        sState.have = false;
    }

    bool apply(LLGLSLShader* shader, LLShaderUniforms* sky, LLShaderUniforms* water)
    {
        if (!LLRHI::sEnvUniformMemo && !LLRHI::sEnvUniformMemoVerify)
        {
            return false;
        }
        State& s = sState;
        const U32 group = (U32)llclamp(shader->mShaderGroup, 0, (S32)GROUPS - 1);
        const U32 now = LLGLSLShader::sEnvUniformsVersion;
        const U32 have = shader->mEnvUniformsVersion;
        const bool skip = s.have && have == now;
        const bool diff = s.have && !skip && diffable(have, now, group);
        // With the verify on, everything is sent as before after comparing
        // what the memo would have left alone.
        const bool full = LLRHI::sEnvUniformMemoVerify || !(skip || diff);
        // Counted as the memo goes (with the verify: as it would go).
        if (skip)
        {
            ++s.stats.applies_skip;
        }
        else if (diff)
        {
            ++s.stats.applies_diff;
            for (U32 v = have + 1; v != now + 1; ++v)
            {
                s.stats.values_diff += s.steps[v % STEPS].changed[group].size();
            }
        }
        else
        {
            ++s.stats.applies_full;
        }
        if (LLRHI::sEnvUniformMemoVerify && (skip || diff))
        {
            verify(shader, group, skip, have, now);
        }

        // Our own values, not someone else's (LLGLSLShader::noteEnvLocation), while in scope.
        struct ApplyingScope
        {
            ApplyingScope() { LLGLSLShader::sApplyingEnvUniforms = true; }
            ~ApplyingScope() { LLGLSLShader::sApplyingEnvUniforms = false; }
        } applying;
        if (full)
        {
            // As LLEnvironment::updateShaderUniforms.
            sky[LLGLSLShader::SG_ANY].apply(shader);
            water[LLGLSLShader::SG_ANY].apply(shader);
            sky[shader->mShaderGroup].apply(shader);
            water[shader->mShaderGroup].apply(shader);
            if (s.have)
            {
                mark_locations(shader, group);
            }
        }
        else if (diff)
        {
            const std::vector<Entry>& table = s.table[group];
            for (U32 v = have + 1; v != now + 1; ++v)
            {
                for (U16 pos : s.steps[v % STEPS].changed[group])
                {
                    send(shader, table[pos]);
                }
            }
        }
        // Not tracked without a build (the memo turned on since the last update).
        shader->mEnvUniformsVersion = s.have ? now : 0;
        return true;
    }
}
