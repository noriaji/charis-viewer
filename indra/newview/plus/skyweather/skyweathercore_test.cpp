// Stand-alone test of skyweathercore.h (not part of the viewer build):
//   cl /nologo /EHsc /std:c++17 /W4 /WX skyweathercore_test.cpp && skyweathercore_test.exe
// The priority of research/371 section 3.4 (the marking of the land's weather is checked by tools/skyweather-test).
#include "skyweathercore.h"

#include <cstdio>

using namespace plus::skyweather::core;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main()
{
    struct Row { Inputs in; bool expect; };
    const Row rows[] = {
        //  on     has    land   rain snow allow
        { { true,  true,  false, 0, 0, false }, true  },    // the user's own weather: always
        { { true,  true,  true,  0, 0, false }, false },    // the land's weather without the opt-in
        { { true,  true,  true,  0, 0, true  }, true  },    // ... with it
        { { true,  true,  false, 2, 0, true  }, false },    // the user's own rain wins
        { { true,  true,  false, 0, 1, true  }, false },    // ... and the own snow
        { { true,  true,  true,  3, 0, true  }, false },    // ... over the land's too
        { { false, true,  false, 0, 0, true  }, false },    // PlusSkyWeather off
        { { true,  false, false, 0, 0, true  }, false },    // a sky with no weather: nothing changes
        { { true,  false, true,  0, 0, true  }, false },
    };
    for (const Row& r : rows)
    {
        CHECK(applies(r.in) == r.expect);
    }

    if (failures == 0)
    {
        std::printf("skyweathercore_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
