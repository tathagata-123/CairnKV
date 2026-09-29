#pragma once

#include <iostream>

#define MINILSM_CHECK(condition)                                              \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << __FILE__ << ':' << __LINE__                         \
                      << ": check failed: " #condition << '\n';              \
            return false;                                                    \
        }                                                                    \
    } while (false)

#define MINILSM_RUN(test_function)                                           \
    do {                                                                     \
        if (!(test_function)()) {                                            \
            return 1;                                                        \
        }                                                                    \
    } while (false)
