// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "test_framework.h"

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    std::printf("running uapp note-slice tests%s%s\n",
                filter != nullptr ? " matching " : "",
                filter != nullptr ? filter : "");
    return uapp::test::run_all(filter);
}
