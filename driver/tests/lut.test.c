#include "../accel/lut.h"
#include "test_utils.h"

static fpt tolerance(void) { return fpt_rconst(0.000001); }

#define ASSERT_FPT_NEAR(expected, actual)                                      \
  do {                                                                         \
    fpt __e = (expected);                                                      \
    fpt __a = (actual);                                                        \
    if (fpt_abs(__e - __a) > tolerance()) {                                    \
      FAILm("expected " #actual " to be near " #expected);                     \
    }                                                                          \
  } while (0)

/* A coarse "flat 0.09 -> S curve -> flat 0.25" profile. */
static const lut_elem_t XS[] = {fpt_rconst(0), fpt_rconst(50), fpt_rconst(100),
                                fpt_rconst(120), fpt_rconst(400)};
static const lut_elem_t YS[] = {fpt_rconst(0.09), fpt_rconst(0.09),
                                fpt_rconst(0.17), fpt_rconst(0.25),
                                fpt_rconst(0.25)};
static const unsigned int N = sizeof(XS) / sizeof(XS[0]);

static fpt eval(const lut_elem_t *xs, const lut_elem_t *ys, unsigned int n,
                double speed) {
  return lut_eval(lut_view_of(xs, ys, n), fpt_rconst(speed));
}

TEST empty_or_null_table_is_identity(void) {
  ASSERT_EQ(FIXEDPT_ONE, eval(XS, YS, 0, 10));
  ASSERT_EQ(FIXEDPT_ONE, eval(NULL, YS, N, 10));
  ASSERT_EQ(FIXEDPT_ONE, eval(XS, NULL, N, 10));
  PASS();
}

TEST single_point_is_constant(void) {
  ASSERT_EQ(YS[0], eval(XS, YS, 1, 0));
  ASSERT_EQ(YS[0], eval(XS, YS, 1, 1000));
  ASSERT_EQ(YS[0], eval(XS, YS, 1, -5));
  PASS();
}

TEST exact_points_are_returned(void) {
  for (unsigned int i = 0; i < N; i++) {
    ASSERT_EQ((fpt)YS[i], lut_eval(lut_view_of(XS, YS, N), (fpt)XS[i]));
  }
  PASS();
}

TEST outside_the_range_holds_flat(void) {
  ASSERT_EQ(YS[0], eval(XS, YS, N, -1));
  ASSERT_EQ(YS[N - 1], eval(XS, YS, N, 401));
  ASSERT_EQ(YS[N - 1], eval(XS, YS, N, 100000));
  PASS();
}

TEST midpoints_interpolate_linearly(void) {
  /* halfway between (50, 0.09) and (100, 0.17) */
  ASSERT_FPT_NEAR(fpt_rconst(0.13), eval(XS, YS, N, 75));
  /* a quarter into (100, 0.17) -> (120, 0.25) */
  ASSERT_FPT_NEAR(fpt_rconst(0.19), eval(XS, YS, N, 105));
  /* a flat segment stays flat */
  ASSERT_FPT_NEAR(fpt_rconst(0.09), eval(XS, YS, N, 25));
  PASS();
}

TEST segment_index_brackets_the_speed(void) {
  struct lut_view v = lut_view_of(XS, YS, N);
  for (int s = 1; s < 400; s++) {
    unsigned int i = lut_segment_index(v, fpt_rconst(s));
    ASSERT(i < N - 1);
    ASSERT(lut_x(v, i) <= fpt_rconst(s));
    ASSERT(fpt_rconst(s) < lut_x(v, i + 1));
  }
  PASS();
}

TEST duplicate_x_steps_without_dividing_by_zero(void) {
  const lut_elem_t xs[] = {fpt_rconst(0), fpt_rconst(10), fpt_rconst(10),
                           fpt_rconst(20)};
  const lut_elem_t ys[] = {fpt_rconst(1), fpt_rconst(1), fpt_rconst(2),
                           fpt_rconst(2)};
  ASSERT_FPT_NEAR(fpt_rconst(1), eval(xs, ys, 4, 5));
  ASSERT_FPT_NEAR(fpt_rconst(2), eval(xs, ys, 4, 15));
  fpt at_dup = eval(xs, ys, 4, 10);
  ASSERT(at_dup == fpt_rconst(1) || at_dup == fpt_rconst(2));
  PASS();
}

TEST non_monotonic_table_never_faults(void) {
  const lut_elem_t xs[] = {fpt_rconst(50), fpt_rconst(10), fpt_rconst(30)};
  const lut_elem_t ys[] = {fpt_rconst(1), fpt_rconst(2), fpt_rconst(3)};
  for (int s = -10; s < 100; s++) {
    fpt out = eval(xs, ys, 3, s);
    ASSERT(out >= fpt_rconst(1) && out <= fpt_rconst(3));
  }
  PASS();
}

TEST view_clamps_count_to_storage(void) {
  static lut_elem_t xs[LUT_MAX_POINTS];
  static lut_elem_t ys[LUT_MAX_POINTS];
  for (unsigned int i = 0; i < LUT_MAX_POINTS; i++) {
    xs[i] = fpt_fromint(i);
    ys[i] = fpt_rconst(0.5);
  }
  struct lut_view v = lut_view_of(xs, ys, LUT_MAX_POINTS * 4);
  ASSERT_EQ((unsigned int)LUT_MAX_POINTS, v.count);
  ASSERT_EQ(fpt_rconst(0.5), lut_eval(v, fpt_rconst(1000)));
  ASSERT_EQ(fpt_rconst(0.5), lut_eval(v, fpt_rconst(63.5)));
  PASS();
}

TEST module_view_uses_min_of_both_counts(void) {
  for (unsigned int i = 0; i < 4; i++) {
    LUT_TABLE_X[i] = fpt_fromint(i * 10);
    LUT_TABLE_Y[i] = fpt_fromint(i + 1);
  }
  struct lut_curve_args none = {};

  LUT_COUNT_X = 4;
  LUT_COUNT_Y = 2; /* only two y values written so far */
  ASSERT_EQ(2u, lut_module_view().count);
  /* beyond the second point -> flat at y[1] = 2, not y[3] = 4 */
  ASSERT_EQ(fpt_fromint(2), __lut_sens_fun(fpt_fromint(35), none));

  LUT_COUNT_Y = 4;
  ASSERT_EQ(fpt_fromint(4), __lut_sens_fun(fpt_fromint(35), none));

  LUT_COUNT_X = LUT_MAX_POINTS * 2;
  LUT_COUNT_Y = LUT_MAX_POINTS * 2;
  ASSERT_EQ((unsigned int)LUT_MAX_POINTS, lut_module_view().count);

  LUT_COUNT_X = 0;
  LUT_COUNT_Y = 0;
  ASSERT_EQ(FIXEDPT_ONE, __lut_sens_fun(fpt_fromint(35), none));
  PASS();
}

SUITE(lut) {
  RUN_TEST(empty_or_null_table_is_identity);
  RUN_TEST(single_point_is_constant);
  RUN_TEST(exact_points_are_returned);
  RUN_TEST(outside_the_range_holds_flat);
  RUN_TEST(midpoints_interpolate_linearly);
  RUN_TEST(segment_index_brackets_the_speed);
  RUN_TEST(duplicate_x_steps_without_dividing_by_zero);
  RUN_TEST(non_monotonic_table_never_faults);
  RUN_TEST(view_clamps_count_to_storage);
  RUN_TEST(module_view_uses_min_of_both_counts);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  TEST_MAIN_BEGIN();
  RUN_SUITE(lut);
  GREATEST_MAIN_END();
}
