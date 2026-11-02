#ifndef __ACCEL_LUT_H_
#define __ACCEL_LUT_H_

#include "../dbg.h"
#include "../fixedptc.h"

/*
 * Look-up table (LUT) acceleration.
 *
 * The curve is given as up to LUT_MAX_POINTS (speed, sensitivity) pairs with
 * increasing speeds (counts/ms, normalized to 1000 DPI). Between two points
 * the sensitivity is linearly interpolated; below the first point and above
 * the last point it is held flat at the nearest point's value. Userspace can
 * sample any curve shape (splines, hand-drawn, ...) and hand the samples to
 * the driver, which never has to evaluate anything expensive.
 *
 * Style: everything below `lut_module_view` is a pure function of its
 * arguments. Tables are passed around as an immutable `struct lut_view`
 * value; the only mutable state is the storage the kernel writes module
 * parameters into, and it is read in exactly one place.
 *
 * Safety (this runs in the input event path):
 *  - storage is static and fixed-size; nothing is allocated;
 *  - every index is bounded by the view's count, which is clamped to
 *    LUT_MAX_POINTS when the view is built; the search loop has a hard bound;
 *  - an empty, partial or non-monotonic table never faults: evaluation
 *    degrades to identity (1.0) or to the nearest point's value.
 */

#define LUT_MAX_POINTS 128

/*
 * Element type of the tables: the exact C type named in the
 * module_param_array declaration (`long` on 64-bit, `int` on 32-bit), which is
 * also the underlying type of `fpt` on those platforms.
 */
#if FIXEDPT_BITS == 64
typedef long lut_elem_t;
#else
typedef int lut_elem_t;
#endif

/* ------------------------------------------------------------------------
 * Mutable storage (written by the kernel's module parameter machinery)
 * --------------------------------------------------------------------- */

static lut_elem_t LUT_TABLE_X[LUT_MAX_POINTS];
static lut_elem_t LUT_TABLE_Y[LUT_MAX_POINTS];
static unsigned int LUT_COUNT_X = 0;
static unsigned int LUT_COUNT_Y = 0;

struct lut_curve_args {};

/* ------------------------------------------------------------------------
 * Values
 * --------------------------------------------------------------------- */

/* An immutable view over `count` valid (x, y) pairs. */
struct lut_view {
  const lut_elem_t *xs;
  const lut_elem_t *ys;
  unsigned int count;
};

/* The segment between two neighbouring points. */
struct lut_segment {
  fpt x0;
  fpt y0;
  fpt x1;
  fpt y1;
};

/* ------------------------------------------------------------------------
 * Pure functions
 * --------------------------------------------------------------------- */

static inline unsigned int lut_min_u(unsigned int a, unsigned int b) {
  return a < b ? a : b;
}

static inline struct lut_view lut_view_of(const lut_elem_t *xs,
                                          const lut_elem_t *ys,
                                          unsigned int count) {
  const unsigned int usable = (xs == NULL || ys == NULL) ? 0 : count;
  return (struct lut_view){
      .xs = xs, .ys = ys, .count = lut_min_u(usable, LUT_MAX_POINTS)};
}

static inline int lut_is_empty(struct lut_view v) { return v.count == 0; }

static inline unsigned int lut_last_index(struct lut_view v) {
  return v.count - 1;
}

static inline fpt lut_x(struct lut_view v, unsigned int i) {
  return (fpt)v.xs[i];
}

static inline fpt lut_y(struct lut_view v, unsigned int i) {
  return (fpt)v.ys[i];
}

static inline int lut_is_below(struct lut_view v, fpt s) {
  return s <= lut_x(v, 0);
}

static inline int lut_is_above(struct lut_view v, fpt s) {
  return s >= lut_x(v, lut_last_index(v));
}

/*
 * Index i of the segment [x_i, x_{i+1}) containing s, for a non-empty view
 * with s strictly inside (x_0, x_last). Binary search; the iteration count is
 * bounded by log2(LUT_MAX_POINTS) + 1 whatever the table holds.
 */
static inline unsigned int lut_segment_index(struct lut_view v, fpt s) {
  unsigned int lo = 0;
  unsigned int hi = lut_last_index(v);
  unsigned int fuel = 16;

  while (hi - lo > 1 && fuel-- > 0) {
    const unsigned int mid = lo + (hi - lo) / 2;
    const int mid_is_left_of_s = lut_x(v, mid) <= s;
    lo = mid_is_left_of_s ? mid : lo;
    hi = mid_is_left_of_s ? hi : mid;
  }
  return lo;
}

static inline struct lut_segment lut_segment_at(struct lut_view v,
                                                unsigned int i) {
  return (struct lut_segment){.x0 = lut_x(v, i),
                              .y0 = lut_y(v, i),
                              .x1 = lut_x(v, i + 1),
                              .y1 = lut_y(v, i + 1)};
}

/* Linear interpolation inside a segment; a degenerate segment steps. */
static inline fpt lut_lerp(struct lut_segment seg, fpt s) {
  if (seg.x1 <= seg.x0) {
    return seg.y1;
  }
  const fpt t = fpt_div(fpt_sub(s, seg.x0), fpt_sub(seg.x1, seg.x0));
  return fpt_add(seg.y0, fpt_mul(fpt_sub(seg.y1, seg.y0), t));
}

static inline fpt lut_interior(struct lut_view v, fpt s) {
  const unsigned int i = lut_segment_index(v, s);
  const struct lut_segment seg = lut_segment_at(v, i);
  dbg("lut: segment %u: (%s, %s) -> (%s, %s)", i, fptoa(seg.x0), fptoa(seg.y0),
      fptoa(seg.x1), fptoa(seg.y1));
  return lut_lerp(seg, s);
}

/* Sensitivity multiplier for speed s under the table v. */
static inline fpt lut_eval(struct lut_view v, fpt s) {
  return lut_is_empty(v)   ? FIXEDPT_ONE
         : lut_is_below(v, s) ? lut_y(v, 0)
         : lut_is_above(v, s) ? lut_y(v, lut_last_index(v))
                              : lut_interior(v, s);
}

/* ------------------------------------------------------------------------
 * The one impure function: snapshot the module's tables into a view
 * --------------------------------------------------------------------- */

static inline struct lut_view lut_module_view(void) {
  /* Read each count once; userspace may be rewriting them through sysfs.
   * Points past min(count_x, count_y) have no y (or x) yet and are ignored. */
  const unsigned int nx = *(volatile unsigned int *)&LUT_COUNT_X;
  const unsigned int ny = *(volatile unsigned int *)&LUT_COUNT_Y;
  return lut_view_of(LUT_TABLE_X, LUT_TABLE_Y, lut_min_u(nx, ny));
}

/**
 * Sensitivity Function for the look-up table curve.
 */
static inline fpt __lut_sens_fun(fpt input_speed, struct lut_curve_args args) {
  (void)args;
  const struct lut_view v = lut_module_view();
  dbg("lut: points                %u", v.count);
  return lut_eval(v, input_speed);
}

#endif // !__ACCEL_LUT_H_
