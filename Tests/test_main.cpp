// SPDX-License-Identifier: AGPL-3.0-or-later
// LiBeDAW unit tests — main entry point
#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
