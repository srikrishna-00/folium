// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "test_framework.h"

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    // Shared by every test executable now that there is more than one, so
    // the banner names the suite generically rather than one slice.
    std::printf("running uapp tests%s%s\n",
                filter != nullptr ? " matching " : "",
                filter != nullptr ? filter : "");
    return uapp::test::run_all(filter);
}
