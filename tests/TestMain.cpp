#include "TestFramework.h"

#include <cstring>

int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failedTests = 0;

    for (const auto& test : splinter::test::registry())
    {
        if (filter != nullptr && std::strstr (test.name, filter) == nullptr)
            continue;

        const int failuresBefore = splinter::test::failureCount();
        std::printf ("[ RUN  ] %s\n", test.name);
        test.body();
        ++run;

        if (splinter::test::failureCount() != failuresBefore)
        {
            ++failedTests;
            std::printf ("[ FAIL ] %s\n", test.name);
        }
    }

    std::printf ("\n%d test(s) run, %d failed\n", run, failedTests);
    return failedTests == 0 ? 0 : 1;
}
