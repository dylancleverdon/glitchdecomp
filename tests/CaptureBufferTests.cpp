#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/CaptureBuffer.h"

using namespace splinter;
using namespace splinter::test;

TEST_CASE ("capture buffer stores and reads back by absolute index")
{
    CaptureBuffer capture;
    capture.prepare (1000); // rounds up to 1024

    CHECK (capture.capacity() == 1024);
    CHECK (capture.writeIndex() == 0);
    CHECK (capture.newestIndex() == -1);

    auto ramp = makeRamp (3000, 0.0f, 1.0f);
    capture.write (ramp.left.data(), ramp.right.data(), 3000);

    CHECK (capture.writeIndex() == 3000);
    CHECK (capture.oldestIndex() == 3000 - 1024);
    CHECK_NEAR (capture.sample (0, 2999), 2999.0f, 0.0);
    CHECK_NEAR (capture.sample (1, 2999), -2999.0f, 0.0);
    CHECK_NEAR (capture.sample (0, 3000 - 1024), 3000.0f - 1024.0f, 0.0);
}

TEST_CASE ("capture buffer returns silence outside the stored range")
{
    CaptureBuffer capture;
    capture.prepare (64);

    auto ramp = makeRamp (100, 1.0f, 1.0f);
    capture.write (ramp.left.data(), ramp.right.data(), 100);

    CHECK (capture.sample (0, 100) == 0.0f);   // not written yet
    CHECK (capture.sample (0, 35) == 0.0f);    // overwritten
    CHECK (capture.sample (0, -5) == 0.0f);
    CHECK (capture.sample (2, 99) == 0.0f);    // bad channel
    CHECK (capture.sample (0, 36) == 37.0f);   // oldest stored
}

TEST_CASE ("capture buffer interpolation is exact on integer positions and linear data")
{
    CaptureBuffer capture;
    capture.prepare (256);

    auto ramp = makeRamp (200, 0.0f, 1.0f);
    capture.write (ramp.left.data(), ramp.right.data(), 200);

    CHECK_NEAR (capture.sampleInterpolated (0, 100.0), 100.0, 1e-4);
    CHECK_NEAR (capture.sampleInterpolated (0, 100.25), 100.25, 1e-3);
    CHECK_NEAR (capture.sampleInterpolated (1, 50.5), -50.5, 1e-3);
    CHECK (capture.sampleInterpolated (0, std::nan ("")) == 0.0f);
}
