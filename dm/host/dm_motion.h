// Live motion (iPhone CoreMotion, DmGamepad.m) -> the six-axis sensor of player 1's controller (hle_hid.c). The front end maps the phone's device frame to the controller's and
// publishes one sample at 100+ Hz; the host answers every GetSixAxisSensorStates with the newest one while it is fresh; upstream routing may select controller motion instead, or the flat sensor when no source is fresh.
//
// Controller frame, as the console held in landscape: x to the screen's right, y to the screen's top, z out of the screen. Lying flat, screen up: acceleration (0, 0, -1) G (the gravity
// vector, as CoreMotion's `gravity` is), no rotation, the identity direction. dir = the controller's x, y and z axes in the world, 3 floats each (the rows of the rotation), the world
// being z up with y the "forward" that Recenter chose; angle = the integral of the angular velocity in revolutions since the last recenter
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct { float acc[3], gyro[3], angle[3], dir[9]; int home_right; } dm_motion;   // acc in G, gyro in revolutions / s, angle in revolutions, controller frame; home_right: the interface orientation it was mapped for (only logged)
void dm_phone_motion_set(const dm_motion *m);   // publishes the newest sample; NULL withdraws it (the sensor lies flat again)

// ---- the frame mapping (pure; shared by the iOS front end and tests/t_motion.c) ----
// A CoreMotion device-frame vector (x along the portrait's right edge, y to the portrait's top, z out of the screen) in the controller frame. home_right = 1: the interface is
// landscape right (home indicator on the right: the phone's top edge points to the player's left, so the screen's top is the device's +x and the screen's right the device's -y); 0: landscape
// left (top edge to the right: the screen's top is the device's -x, its right the device's +y). Both are rotations about z, so the same map takes the angular velocity too
//
// flip = the sign correction found on a device (BotW's tilt maze, landscape right: tilt inverted both ways, yaw right): bit 0 negates x, bit 1 negates y, S = diag(+-1, +-1, 1). "xy" (3),
// the default, is a half turn about z. It applies to vectors here (acceleration, angular velocity, and so the angle integrated from it) and to the direction in dm_motion_dir (S D S)
enum { DM_FLIP_NONE = 0, DM_FLIP_X = 1, DM_FLIP_Y = 2, DM_FLIP_XY = 3 };
static inline int dm_motion_flip_parse(const char *s) {   // DM_MOTION_FLIP: none | xy | x | y; unset or unknown = xy
    if (s && !strcmp(s, "none")) return DM_FLIP_NONE;
    if (s && !strcmp(s, "x")) return DM_FLIP_X;
    if (s && !strcmp(s, "y")) return DM_FLIP_Y;
    return DM_FLIP_XY;
}
static inline const char *dm_motion_flip_name(int flip) { return flip == DM_FLIP_NONE ? "none" : flip == DM_FLIP_X ? "x" : flip == DM_FLIP_Y ? "y" : "xy"; }
static inline void dm_motion_vec(int home_right, int flip, const double v[3], double o[3]) {
    double x = home_right ? -v[1] : v[1], y = home_right ? v[0] : -v[0];
    o[0] = flip & DM_FLIP_X ? -x : x; o[1] = flip & DM_FLIP_Y ? -y : y; o[2] = v[2];
}
// The inverse map: a controller-frame vector in the device frame
static inline void dm_motion_unvec(int home_right, const double c[3], double d[3]) {
    d[0] = home_right ? c[1] : -c[1]; d[1] = home_right ? -c[0] : c[0]; d[2] = c[2];
}
// A device-frame vector in the reference frame, by CoreMotion's rotation matrix m (row-major, device -> reference, z up)
static inline void dm_motion_world(const double m[9], const double d[3], double w[3]) {
    for (int r = 0; r < 3; r++) w[r] = m[3 * r] * d[0] + m[3 * r + 1] * d[1] + m[3 * r + 2] * d[2];
}
// The controller's three axes in the world: dir[3 * a + k] = component k of axis a, from m turned about z by -yaw0 (the heading Recenter stored, dm_motion_yaw), then flipped (S D S)
static inline void dm_motion_dir(int home_right, int flip, const double m[9], double yaw0, float dir[9]) {
    double c = cos(yaw0), s = sin(yaw0);
    for (int a = 0; a < 3; a++) {
        double e[3] = {a == 0 ? 1.0 : 0.0, a == 1 ? 1.0 : 0.0, a == 2 ? 1.0 : 0.0}, d[3], w[3];
        dm_motion_unvec(home_right, e, d); dm_motion_world(m, d, w);
        dir[3 * a + 0] = (float)(c * w[0] + s * w[1]);
        dir[3 * a + 1] = (float)(-s * w[0] + c * w[1]);
        dir[3 * a + 2] = (float)w[2];
    }
    double sg[3] = {flip & DM_FLIP_X ? -1 : 1, flip & DM_FLIP_Y ? -1 : 1, 1};   // S D S: entry (a, k) takes the sign of both axes
    for (int a = 0; a < 3; a++) for (int k = 0; k < 3; k++) dir[3 * a + k] *= (float)(sg[a] * sg[k]);
}
// The heading of the controller in the reference frame, counter-clockwise from above, 0 = along the reference y: of the screen's top edge (the controller's y axis) when it lies
// flatter than 60 degrees to the horizon, else of the direction into the screen (a phone held upright, screen to the player)
static inline double dm_motion_yaw(int home_right, const double m[9]) {
    double e[3] = {0, 1, 0}, d[3], w[3];
    dm_motion_unvec(home_right, e, d); dm_motion_world(m, d, w);
    if (hypot(w[0], w[1]) < 0.5) { double in[3] = {0, 0, -1}; dm_motion_world(m, in, w); }   // into the screen
    return atan2(-w[0], w[1]);
}
