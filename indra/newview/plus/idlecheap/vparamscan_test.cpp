// Stand-alone test of vparamscan.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX vparamscan_test.cpp && vparamscan_test.exe
// research/193 B: the witness-first read gives upstream's answer
// (visualParamWeightsAreDefault's walk over every tweakable parameter) after
// any sequence of weight changes and skirt on/off, and the witness answers
// most calls of an avatar off its defaults.
#include "vparamscan.h"

#include <cstdio>
#include <random>

using namespace plus::vparammemo;

namespace
{
    struct FakeParam
    {
        float weight = 0.f;
        float default_weight = 0.f;
        float getWeight() const { return weight; }
        float getDefaultWeight() const { return default_weight; }
    };

    int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

    // Upstream's walk, as llvoavatar.cpp has it.
    bool upstream(const std::vector<BasicEntry<FakeParam>>& entries, bool wearing_skirt)
    {
        for (const BasicEntry<FakeParam>& e : entries)
        {
            if (e.param->getWeight() != e.param->getDefaultWeight() && (wearing_skirt || !e.skirt))
            {
                return false;
            }
        }
        return true;
    }
}

int main()
{
    // The trivial cases.
    {
        std::vector<BasicEntry<FakeParam>> none;
        int witness = 5;    // stale, out of range
        bool by_witness = true;
        CHECK(allDefault(none, witness, false, by_witness));
        CHECK(!by_witness && witness == -1);

        FakeParam p[3];
        p[1].weight = 0.5f;
        std::vector<BasicEntry<FakeParam>> list = { { &p[0], false }, { &p[1], true }, { &p[2], false } };
        witness = -1;
        CHECK(allDefault(list, witness, false, by_witness));    // the only one off is a skirt one, no skirt
        CHECK(!allDefault(list, witness, true, by_witness));    // with a skirt it counts
        CHECK(witness == 1 && !by_witness);
        CHECK(!allDefault(list, witness, true, by_witness) && by_witness);
        CHECK(allDefault(list, witness, false, by_witness) && !by_witness && witness == -1);
    }

    // Random avatars, random changes: always upstream's answer.
    std::mt19937 rng(193);
    unsigned long long calls = 0, witness_hits = 0, default_answers = 0;
    for (int avatar = 0; avatar < 2000; ++avatar)
    {
        const int n = 1 + (int)(rng() % 160);
        std::vector<FakeParam> params(n);
        std::vector<BasicEntry<FakeParam>> list;
        for (int i = 0; i < n; ++i)
        {
            params[i].default_weight = (rng() % 3) ? 0.f : (float)(rng() % 100) / 100.f;
            params[i].weight = params[i].default_weight;
            list.push_back({ &params[i], (rng() % 10) == 0 });
        }
        // a shaped avatar: some off their default; a cloud: none
        const bool cloud = (avatar % 4) == 0;
        if (!cloud)
        {
            for (int k = 0, m = 1 + (int)(rng() % 8); k < m; ++k)
            {
                params[rng() % n].weight += 0.25f;
            }
        }
        int witness = -1;
        bool skirt = (rng() % 2) == 0;
        for (int step = 0; step < 300; ++step)
        {
            switch (rng() % 8)
            {
            case 0: skirt = !skirt; break;
            case 1: params[rng() % n].weight = params[rng() % n].default_weight; break;     // may be another's default
            case 2: { FakeParam& p = params[rng() % n]; p.weight = p.default_weight; } break;
            case 3: params[rng() % n].weight += 0.125f; break;
            case 4: if (witness >= 0) { FakeParam& p = params[witness]; p.weight = p.default_weight; } break;    // the witness goes back
            default: break;     // nothing changed (most frames)
            }
            bool by_witness = false;
            const bool memo = allDefault(list, witness, skirt, by_witness);
            const bool expect = upstream(list, skirt);
            CHECK(memo == expect);
            if (memo != expect)
            {
                std::printf("  avatar %d step %d: memo %d upstream %d\n", avatar, step, (int)memo, (int)expect);
                return 1;
            }
            CHECK(memo ? witness == -1 : (witness >= 0 && witness < n && offDefault(list[witness], skirt)));
            ++calls;
            witness_hits += by_witness ? 1 : 0;
            default_answers += memo ? 1 : 0;
        }
    }
    CHECK(witness_hits * 2 > calls - default_answers);  // the witness answers most "not default" calls

    std::printf("vparamscan: %llu calls, witness %llu, all default %llu, %s\n", calls, witness_hits, default_answers,
                failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
