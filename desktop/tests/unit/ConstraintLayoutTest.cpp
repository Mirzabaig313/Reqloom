// Tests the 1-D linear constraint solver: preferred sizing, stretch
// distribution, maxima caps, deficit shrinking, and spacing/offsets. Pure
// logic — no Qt.
#include "widgets/ConstraintLayout.h"

#include <gtest/gtest.h>

namespace reqloom::desktop::layout::tests {

TEST(ConstraintLayout, empty_input_yields_no_placements) {
    const auto out = solveLinear({}, 500.0);
    EXPECT_TRUE(out.empty());
}

TEST(ConstraintLayout, single_filling_segment_takes_all_available) {
    const auto out =
        solveLinear({{.minimum = 0, .preferred = 100, .maximum = 1e9, .stretch = 1}}, 500.0);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(out[0].offset, 0.0);
    EXPECT_DOUBLE_EQ(out[0].size, 500.0);
}

TEST(ConstraintLayout, fixed_segment_keeps_preferred_and_filler_takes_remainder) {
    // The request-bar case: a fixed-width method slot, a filling path, a fixed
    // send slot — exactly the intent that replaced magic pixel widths.
    Constraint method{.minimum = 80, .preferred = 80, .maximum = 80, .stretch = 0};
    Constraint path{.minimum = 120, .preferred = 120, .maximum = 1e9, .stretch = 1};
    Constraint send{.minimum = 96, .preferred = 96, .maximum = 96, .stretch = 0};
    const double spacing = 8.0;
    const auto out = solveLinear({method, path, send}, 600.0, spacing);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_DOUBLE_EQ(out[0].size, 80.0);
    EXPECT_DOUBLE_EQ(out[2].size, 96.0);
    // Path absorbs everything left after the two fixed slots and the gaps.
    EXPECT_DOUBLE_EQ(out[1].size, 600.0 - 80.0 - 96.0 - spacing * 2);
    // Offsets accumulate size + spacing.
    EXPECT_DOUBLE_EQ(out[0].offset, 0.0);
    EXPECT_DOUBLE_EQ(out[1].offset, 80.0 + spacing);
    EXPECT_DOUBLE_EQ(out[2].offset, 80.0 + spacing + out[1].size + spacing);
}

TEST(ConstraintLayout, surplus_splits_between_stretchers_by_weight) {
    Constraint a{.minimum = 0, .preferred = 0, .maximum = 1e9, .stretch = 1};
    Constraint b{.minimum = 0, .preferred = 0, .maximum = 1e9, .stretch = 3};
    const auto out = solveLinear({a, b}, 400.0);
    ASSERT_EQ(out.size(), 2u);
    // 1:3 weighting → 100 / 300.
    EXPECT_DOUBLE_EQ(out[0].size, 100.0);
    EXPECT_DOUBLE_EQ(out[1].size, 300.0);
}

TEST(ConstraintLayout, maximum_caps_growth_and_spills_to_other_stretcher) {
    Constraint capped{.minimum = 0, .preferred = 0, .maximum = 50, .stretch = 1};
    Constraint open{.minimum = 0, .preferred = 0, .maximum = 1e9, .stretch = 1};
    const auto out = solveLinear({capped, open}, 400.0);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_DOUBLE_EQ(out[0].size, 50.0);   // hits its cap
    EXPECT_DOUBLE_EQ(out[1].size, 350.0);  // absorbs the rest
}

TEST(ConstraintLayout, deficit_shrinks_stretchers_before_fixed_segments) {
    // Available is smaller than the preferred total. The fixed segment must
    // keep its size; the stretchable one shrinks toward its minimum.
    Constraint fixed{.minimum = 80, .preferred = 80, .maximum = 80, .stretch = 0};
    Constraint flex{.minimum = 40, .preferred = 200, .maximum = 1e9, .stretch = 1};
    const auto out = solveLinear({fixed, flex}, 150.0);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_DOUBLE_EQ(out[0].size, 80.0);
    EXPECT_DOUBLE_EQ(out[1].size, 70.0);  // 150 - 80
}

TEST(ConstraintLayout, deficit_respects_minimum_floor) {
    Constraint flex{.minimum = 40, .preferred = 200, .maximum = 1e9, .stretch = 1};
    const auto out = solveLinear({flex}, 10.0);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(out[0].size, 40.0);  // cannot shrink below its minimum
}

TEST(ConstraintLayout, fixed_only_row_does_not_grow_past_preferred) {
    Constraint a{.minimum = 0, .preferred = 60, .maximum = 60, .stretch = 0};
    Constraint b{.minimum = 0, .preferred = 60, .maximum = 60, .stretch = 0};
    const auto out = solveLinear({a, b}, 400.0);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_DOUBLE_EQ(out[0].size, 60.0);
    EXPECT_DOUBLE_EQ(out[1].size, 60.0);  // no stretch → leftover stays unused
}

}  // namespace reqloom::desktop::layout::tests
