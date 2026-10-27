// Phone motion -> controller frame (dm_motion.h): flat, tilted forward, tilted left, a turn about the vertical, the upright fallback, in both landscape orientations
#include "../dm_motion.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
static int near3(const double *a, double x, double y, double z) { return fabs(a[0] - x) < 1e-6 && fabs(a[1] - y) < 1e-6 && fabs(a[2] - z) < 1e-6; }
static int fnear3(const float *a, double x, double y, double z) { return fabs(a[0] - x) < 1e-5 && fabs(a[1] - y) < 1e-5 && fabs(a[2] - z) < 1e-5; }
static const double I3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static void rz(double a, double m[9]) { double c = cos(a), s = sin(a); double r[9] = {c, -s, 0, s, c, 0, 0, 0, 1}; memcpy(m, r, sizeof r); }

static void t_flat(int hr) {
    double g[3] = {0, 0, -1}, o[3];
    dm_motion_vec(hr, g, o);
    CHECK(near3(o, 0, 0, -1), "flat: gravity stays (0, 0, -1)");
    float dir[9]; dm_motion_dir(hr, I3, dm_motion_yaw(hr, I3), dir);
    CHECK(fnear3(dir, 1, 0, 0) && fnear3(dir + 3, 0, 1, 0) && fnear3(dir + 6, 0, 0, 1), "flat, recentred: the identity direction");
}
static void t_tilt(int hr) {
    double th = 0.5, s = sin(th), c = cos(th), o[3];
    // the screen's top edge lowered: forward. home right: the top is the device's +x; home left: its -x
    double fwd[3] = {hr ? s : -s, 0, -c};
    dm_motion_vec(hr, fwd, o);
    CHECK(near3(o, 0, s, -c), "tilted forward: gravity gains +y");
    // the screen's left edge lowered: left. home right: the left is the device's +y; home left: its -y
    double left[3] = {0, hr ? s : -s, -c};
    dm_motion_vec(hr, left, o);
    CHECK(near3(o, -s, 0, -c), "tilted left: gravity gains -x");
    // the angular velocity maps like any vector: a turn about the screen's top axis, and one about the vertical (z)
    double w1[3] = {hr ? 1 : -1, 0, 0}, w2[3] = {0, 0, 1};
    dm_motion_vec(hr, w1, o); CHECK(near3(o, 0, 1, 0), "a turn about the screen's top axis is about y");
    dm_motion_vec(hr, w2, o); CHECK(near3(o, 0, 0, 1), "a turn about the screen's normal is about z");
    // unvec is the inverse
    double v[3] = {0.3, -0.7, 0.2}, d[3], b[3];
    dm_motion_vec(hr, v, d); dm_motion_unvec(hr, d, b); CHECK(near3(b, 0.3, -0.7, 0.2), "unvec inverts vec");
}
static void t_yaw(int hr) {
    // recentred while still, then turned 90 degrees counter-clockwise (seen from above): the controller's y axis points to -x, its x axis to +y
    double y0 = dm_motion_yaw(hr, I3), m[9]; rz(M_PI / 2, m);
    float dir[9]; dm_motion_dir(hr, m, y0, dir);
    CHECK(fnear3(dir, 0, 1, 0) && fnear3(dir + 3, -1, 0, 0) && fnear3(dir + 6, 0, 0, 1), "a quarter turn left after Recenter");
    // the heading itself is relative: turning back the other way gives the identity again
    rz(0, m); dm_motion_dir(hr, m, dm_motion_yaw(hr, m), dir);
    CHECK(fnear3(dir, 1, 0, 0) && fnear3(dir + 3, 0, 1, 0), "recentring at any heading gives the identity");
    // 180 degrees and the sense of dm_motion_yaw: turned left by 30 degrees reads +30
    rz(0.5, m); CHECK(fabs(dm_motion_yaw(hr, m) - dm_motion_yaw(hr, I3) - 0.5) < 1e-9, "yaw grows counter-clockwise");
}
static void t_upright(void) {
    // landscape right, held upright with the screen to the player (forward +y): device x up, y to -x, z to -y
    double m[9] = {0, -1, 0, 0, 0, -1, 1, 0, 0};
    CHECK(fabs(dm_motion_yaw(1, m)) < 1e-9, "upright: the heading follows the direction into the screen");
    float dir[9]; dm_motion_dir(1, m, 0, dir);
    CHECK(fnear3(dir + 3, 0, 0, 1), "upright: the screen's top axis points up");
}
int main(void) {
    for (int hr = 0; hr < 2; hr++) { t_flat(hr); t_tilt(hr); t_yaw(hr); }
    t_upright();
    printf("motion frame: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
