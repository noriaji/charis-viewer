// cl /nologo /std:c++20 /EHsc /W4 /WX updatestate_test.cpp updatestate.cpp && updatestate_test
// Stand-alone test of plus/update's state (research/326 section 8); not built into the viewer.

#include "updatestate.h"

#include <cstdio>
#include <cstdlib>

using namespace plus::update;

static int sFailures = 0;

#define EXPECT(cond) do { if (!(cond)) { std::printf("FAILED line %d: %s\n", __LINE__, #cond); ++sFailures; } } while (0)

static void testHappyPath()
{
    Machine m;
    EXPECT(m.phase() == Phase::Idle);
    EXPECT(!m.engaged() && !m.chipWanted());
    EXPECT(!m.progress(1, 2));          // not downloading
    EXPECT(!m.ready());
    EXPECT(!m.downloading(10));

    EXPECT(m.begin("0.1.0 alpha 2"));
    EXPECT(m.phase() == Phase::Checking && m.engaged() && m.chipWanted());
    EXPECT(!m.begin("again"));           // no second start
    EXPECT(m.version() == "0.1.0 alpha 2");

    const uint64_t mb = 1024 * 1024;
    EXPECT(m.downloading(280 * mb));
    EXPECT(m.phase() == Phase::Downloading && m.percent() == 0);

    uint32_t s = m.serial();
    EXPECT(m.progress(1000, 0));         // under a whole MB and a whole percent: nothing to show
    EXPECT(m.serial() == s);
    EXPECT(m.progress(8 * mb, 0));
    EXPECT(m.serial() == s + 1);
    EXPECT(m.percent() == 2);            // 8 / 280 = 2.8 %
    EXPECT(m.progress(500 * mb, 0));     // clamped to the total
    EXPECT(m.doneBytes() == 280 * mb && m.percent() == 100);

    EXPECT(m.ready());
    EXPECT(m.phase() == Phase::Ready && m.engaged() && m.chipWanted());
    EXPECT(!m.ready());
    EXPECT(!m.requestCancel());          // nothing to stop once ready
    EXPECT(m.requestRestart());
    EXPECT(!m.requestRestart());         // once
    EXPECT(m.restartRequested());
    EXPECT(m.cancelRestart());           // the quit was called off: the button is back
    {
        Machine other;
        EXPECT(!other.cancelRestart());  // Idle
        EXPECT(other.begin("v"));
        EXPECT(!other.cancelRestart());  // Checking
        EXPECT(other.failed(Failure::FeedUnreadable));
        EXPECT(!other.cancelRestart());  // Failed
    }
    EXPECT(!m.cancelRestart());
    EXPECT(!m.restartRequested());
    EXPECT(m.requestRestart());
    EXPECT(m.hideReady());
    EXPECT(!m.hideReady());
    EXPECT(m.engaged() && !m.chipWanted()); // hidden, still goes in at the end
    EXPECT(m.clear());                   // thrown away (a setting turned updates off)
    EXPECT(m.phase() == Phase::Idle && !m.engaged() && !m.restartRequested());
}

static void testCancel()
{
    Machine m;
    EXPECT(!m.requestCancel());
    EXPECT(!m.cancelled());
    EXPECT(m.begin("v"));
    EXPECT(m.requestCancel());
    EXPECT(!m.requestCancel());          // once
    EXPECT(m.cancelRequested());
    EXPECT(m.cancelled());
    EXPECT(m.phase() == Phase::Idle && !m.cancelRequested());

    EXPECT(m.begin("v"));                // a new start clears what was asked before
    EXPECT(!m.cancelRequested());
    EXPECT(m.downloading(0));
    EXPECT(m.progress(3 * 1024 * 1024, 0));
    EXPECT(m.percent() == 0);            // no total: no percent
    EXPECT(m.requestCancel());
    EXPECT(m.cancelled());
    EXPECT(m.doneBytes() == 0 && m.totalBytes() == 0);

    // A cancel asked for, then the download ends anyway: not ready (the agreement was taken back)
    EXPECT(m.begin("v"));
    EXPECT(m.downloading(100));
    EXPECT(m.progress(100, 100));
    EXPECT(m.requestCancel());
    EXPECT(!m.ready());
    EXPECT(m.phase() == Phase::Downloading);
    EXPECT(m.cancelled());
    EXPECT(m.phase() == Phase::Idle);
}

static void testFailure()
{
    {
        Machine c;                       // a cancel while the feed is read, then its error: a cancel
        EXPECT(c.begin("v"));
        EXPECT(c.requestCancel());
        EXPECT(!c.failed(Failure::FeedUnreadable));
        EXPECT(c.phase() == Phase::Checking && c.failure() == Failure::None);
        EXPECT(c.cancelled());
    }
    Machine m;
    EXPECT(!m.failed(Failure::DownloadFailed));
    EXPECT(m.begin("v"));
    EXPECT(!m.failed(Failure::None));
    EXPECT(m.failed(Failure::NoUpdateInFeed));
    EXPECT(m.phase() == Phase::Failed && m.engaged() && m.chipWanted());
    EXPECT(m.failure() == Failure::NoUpdateInFeed);
    EXPECT(!m.cancelled());
    EXPECT(m.begin("v"));                // Retry
    EXPECT(m.failure() == Failure::None);
    EXPECT(m.downloading(100));
    EXPECT(m.requestCancel());
    EXPECT(!m.failed(Failure::PackageMismatch));  // stopped by the user: not a failure with Retry
    EXPECT(m.phase() == Phase::Downloading);
    EXPECT(m.cancelled());
    EXPECT(m.begin("v"));
    EXPECT(m.failed(Failure::PackageMismatch));
    EXPECT(!m.cancelRequested());
    EXPECT(m.clear());
    EXPECT(m.phase() == Phase::Idle && m.failure() == Failure::None);
    EXPECT(!m.clear());
}

static void testNames()
{
    EXPECT(phaseName(Phase::Downloading)[0] == 'd');
    EXPECT(failureName(Failure::HandOffFailed)[0] == 't');
    EXPECT(failureName(Failure::VersionMismatch)[0] == 't');
    EXPECT(wholeMB(1024 * 1024 - 1) == 0 && wholeMB(1024 * 1024) == 1);
}

int main()
{
    testHappyPath();
    testCancel();
    testFailure();
    testNames();
    if (sFailures)
    {
        std::printf("%d failed\n", sFailures);
        return EXIT_FAILURE;
    }
    std::printf("all passed\n");
    return EXIT_SUCCESS;
}
