// Phone motion -> controller frame (dm_motion.h): flat, tilted forward, tilted left, a turn about the vertical, the upright fallback, in both landscape orientations and every axis flip (DM_MOTION_FLIP: none, x, y, xy = the default)
#include "../dm_motion.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
static int near3(const double *a, double x, double y, double z) { return fabs(a[0] - x) < 1e-6 && fabs(a[1] - y) < 1e-6 && fabs(a[2] - z) < 1e-6; }
static int fnear3(const float *a, double x, double y, double z) { return fabs(a[0] - x) < 1e-5 && fabs(a[1] - y) < 1e-5 && fabs(a[2] - z) < 1e-5; }
static const double I3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static void rz(double a, double m[9]) { double c = cos(a), s = sin(a); double r[9] = {c, -s, 0, s, c, 0, 0, 0, 1}; memcpy(m, r, sizeof r); }

static double sgn(int flip, int axis) { return axis == 0 && (flip & DM_FLIP_X) ? -1 : axis == 1 && (flip & DM_FLIP_Y) ? -1 : 1; }
// dir against the unflipped rows D (9 numbers): S D S, entry (a, k) times the signs of both axes
static int dir_is(const float *dir, const double D[9], int flip) {
    for (int a = 0; a < 3; a++) for (int k = 0; k < 3; k++) if (fabs(dir[3 * a + k] - D[3 * a + k] * sgn(flip, a) * sgn(flip, k)) > 1e-5) return 0;
    return 1;
}
static void t_flat(int hr, int flip) {
    double g[3] = {0, 0, -1}, o[3];
    dm_motion_vec(hr, flip, g, o);
    CHECK(near3(o, 0, 0, -1), "flat: gravity stays (0, 0, -1)");
    float dir[9]; dm_motion_dir(hr, flip, I3, dm_motion_yaw(hr, I3), dir);
    CHECK(dir_is(dir, I3, flip) && fnear3(dir, 1, 0, 0) && fnear3(dir + 3, 0, 1, 0) && fnear3(dir + 6, 0, 0, 1), "flat, recentred: the identity direction, whatever the flip");
}
static void t_tilt(int hr, int flip) {
    double th = 0.5, s = sin(th), c = cos(th), o[3], sx = sgn(flip, 0), sy = sgn(flip, 1);
    // the screen's top edge lowered: forward. home right: the top is the device's +x; home left: its -x
    double fwd[3] = {hr ? s : -s, 0, -c};
    dm_motion_vec(hr, flip, fwd, o);
    CHECK(near3(o, 0, sy * s, -c), "tilted forward: gravity gains y (negated by a y flip)");
    // the screen's left edge lowered: left. home right: the left is the device's +y; home left: its -y
    double left[3] = {0, hr ? s : -s, -c};
    dm_motion_vec(hr, flip, left, o);
    CHECK(near3(o, -sx * s, 0, -c), "tilted left: gravity gains -x (negated by an x flip)");
    // the angular velocity maps like any vector: a turn about the screen's top axis, and one about the vertical (z, which no flip touches: yaw stays right)
    double w1[3] = {hr ? 1 : -1, 0, 0}, w2[3] = {0, 0, 1};
    dm_motion_vec(hr, flip, w1, o); CHECK(near3(o, 0, sy, 0), "a turn about the screen's top axis is about y");
    dm_motion_vec(hr, flip, w2, o); CHECK(near3(o, 0, 0, 1), "a turn about the screen's normal is about z");
    // unvec is the inverse of the unflipped map
    double v[3] = {0.3, -0.7, 0.2}, d[3], b[3];
    dm_motion_vec(hr, DM_FLIP_NONE, v, d); dm_motion_unvec(hr, d, b); CHECK(near3(b, 0.3, -0.7, 0.2), "unvec inverts vec");
}
static void t_yaw(int hr, int flip) {
    // recentred while still, then turned 90 degrees counter-clockwise (seen from above): the controller's y axis points to -x, its x axis to +y
    double y0 = dm_motion_yaw(hr, I3), m[9]; rz(M_PI / 2, m);
    float dir[9]; dm_motion_dir(hr, flip, m, y0, dir);
    double want[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    CHECK(dir_is(dir, want, flip) && (flip == DM_FLIP_X || flip == DM_FLIP_Y || (fnear3(dir, 0, 1, 0) && fnear3(dir + 3, -1, 0, 0))), "a quarter turn left after Recenter (a turn about z is unchanged by every flip)");
    // the heading itself is relative: recentring at any heading gives the identity again
    rz(0, m); dm_motion_dir(hr, flip, m, dm_motion_yaw(hr, m), dir);
    CHECK(fnear3(dir, 1, 0, 0) && fnear3(dir + 3, 0, 1, 0), "recentring at any heading gives the identity");
    // the sense of dm_motion_yaw: turned left by 0.5 rad reads +0.5
    rz(0.5, m); CHECK(fabs(dm_motion_yaw(hr, m) - dm_motion_yaw(hr, I3) - 0.5) < 1e-9, "yaw grows counter-clockwise");
}
static void t_upright(int flip) {
    // landscape right, held upright with the screen to the player (forward +y): device x up, y to -x, z to -y
    double m[9] = {0, -1, 0, 0, 0, -1, 1, 0, 0};
    CHECK(fabs(dm_motion_yaw(1, m)) < 1e-9, "upright: the heading follows the direction into the screen");
    float dir[9]; dm_motion_dir(1, flip, m, 0, dir);
    double D[9]; { float u[9]; dm_motion_dir(1, DM_FLIP_NONE, m, 0, u); for (int i = 0; i < 9; i++) D[i] = u[i]; }
    CHECK(near3(D + 3, 0, 0, 1), "upright: the screen's top axis points up");
    CHECK(dir_is(dir, D, flip), "upright: a flip conjugates the rows (xz, yz, zx, zy negate for xy)");
}
static void t_modes(void) {
    CHECK(dm_motion_flip_parse(NULL) == DM_FLIP_XY && dm_motion_flip_parse("xy") == DM_FLIP_XY && dm_motion_flip_parse("bogus") == DM_FLIP_XY, "the default and unknown names are xy");
    CHECK(dm_motion_flip_parse("none") == DM_FLIP_NONE && dm_motion_flip_parse("x") == DM_FLIP_X && dm_motion_flip_parse("y") == DM_FLIP_Y, "none, x and y");
    CHECK(!strcmp(dm_motion_flip_name(DM_FLIP_XY), "xy") && !strcmp(dm_motion_flip_name(DM_FLIP_NONE), "none"), "names round-trip");
    // the default, landscape right, resting phone as logged on the device: acc (0.03, -0.49, -0.88) in the unflipped frame becomes (-0.03, 0.49, -0.88)
    double r[3] = {-0.49, -0.03, -0.88}, o[3];   // the device-frame reading that maps to (0.03, -0.49, -0.88) unflipped
    dm_motion_vec(1, DM_FLIP_NONE, r, o); CHECK(near3(o, 0.03, -0.49, -0.88), "device reading, unflipped");
    dm_motion_vec(1, dm_motion_flip_parse(NULL), r, o); CHECK(near3(o, -0.03, 0.49, -0.88), "device reading, default xy: x and y negated, z kept");
}
int main(void) {
    for (int hr = 0; hr < 2; hr++) for (int f = 0; f < 4; f++) { t_flat(hr, f); t_tilt(hr, f); t_yaw(hr, f); }
    for (int f = 0; f < 4; f++) t_upright(f);
    t_modes();
    printf("motion frame: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
