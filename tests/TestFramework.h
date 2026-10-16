#pragma once

// Minimal self-contained test harness: no external dependencies.
//
//   TEST_CASE ("stutter repeats the slice") { CHECK (x == y); CHECK_NEAR (a, b, 1e-6); }
//
// Run all tests:          ./splinter_tests
// Run matching tests:     ./splinter_tests stutter

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace splinter::test
{

struct TestCase
{
    const char* name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount()
{
    static int failures = 0;
    return failures;
}

struct Registrar
{
    Registrar (const char* name, std::function<void()> body) { registry().push_back ({ name, std::move (body) }); }
};

inline void reportFailure (const char* file, int line, const std::string& message)
{
    ++failureCount();
    std::printf ("    FAILED %s:%d: %s\n", file, line, message.c_str());
}

} // namespace splinter::test

#define SPLINTER_CONCAT_INNER(a, b) a##b
#define SPLINTER_CONCAT(a, b) SPLINTER_CONCAT_INNER (a, b)

#define TEST_CASE(name)                                                                                          \
    static void SPLINTER_CONCAT (splinterTest_, __LINE__)();                                                     \
    static const splinter::test::Registrar SPLINTER_CONCAT (splinterRegistrar_, __LINE__) (                      \
        name, &SPLINTER_CONCAT (splinterTest_, __LINE__));                                                       \
    static void SPLINTER_CONCAT (splinterTest_, __LINE__)()

#define CHECK(condition)                                                                                         \
    do                                                                                                           \
    {                                                                                                            \
        if (! (condition))                                                                                       \
            splinter::test::reportFailure (__FILE__, __LINE__, "CHECK (" #condition ")");                        \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                                  \
    do                                                                                                           \
    {                                                                                                            \
        const double splinterActual_ = static_cast<double> (actual);                                             \
        const double splinterExpected_ = static_cast<double> (expected);                                         \
        if (! (std::fabs (splinterActual_ - splinterExpected_) <= static_cast<double> (tolerance)))              \
            splinter::test::reportFailure (__FILE__, __LINE__,                                                   \
                                           "CHECK_NEAR (" #actual ", " #expected ") got "                        \
                                               + std::to_string (splinterActual_) + " expected "                 \
                                               + std::to_string (splinterExpected_));                            \
    } while (false)
