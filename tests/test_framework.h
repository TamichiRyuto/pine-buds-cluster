// Minimal host-side test framework (host only; never compiled for target).
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <cstdio>

namespace testfw {

inline int failures = 0;
inline int checks = 0;

inline void check(bool cond, const char* expr, const char* file, int line) {
    ++checks;
    if (!cond) {
        ++failures;
        std::printf("FAIL %s:%d  %s\n", file, line, expr);
    }
}

inline void check_eq_f(float actual, float expected, const char* expr,
                       const char* file, int line) {
    ++checks;
    if (actual != expected) {  // exact compare: tests use integer-exact cases only
        ++failures;
        std::printf("FAIL %s:%d  %s  actual=%f expected=%f\n", file, line, expr,
                    static_cast<double>(actual), static_cast<double>(expected));
    }
}

inline int summary() {
    if (failures == 0) {
        std::printf("OK  %d checks passed\n", checks);
        return 0;
    }
    std::printf("NG  %d/%d checks failed\n", failures, checks);
    return 1;
}

}  // namespace testfw

#define CHECK(cond) testfw::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ_F(actual, expected) \
    testfw::check_eq_f((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define RUN_TEST(fn)                     \
    do {                                 \
        std::printf("test: %s\n", #fn);  \
        fn();                            \
    } while (0)

#endif  // TEST_FRAMEWORK_H
