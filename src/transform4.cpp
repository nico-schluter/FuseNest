#include "transform4.h"

#include <cmath>

namespace fusenest {
namespace {

/// Dot product of two rows of the 3x3 part.
double rowDot(const Mat4& m, int a, int b) {
    return m[a * 4 + 0] * m[b * 4 + 0] + m[a * 4 + 1] * m[b * 4 + 1] +
           m[a * 4 + 2] * m[b * 4 + 2];
}

}  // namespace

Mat4 identity4() {
    return Mat4{1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1};
}

Mat4 multiply4(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    return out;
}

bool isRigid4(const Mat4& m, double eps) {
    if (std::fabs(m[12]) > eps || std::fabs(m[13]) > eps || std::fabs(m[14]) > eps ||
        std::fabs(m[15] - 1.0) > eps)
        return false;

    for (int i = 0; i < 3; ++i)
        if (std::fabs(rowDot(m, i, i) - 1.0) > eps) return false;

    return std::fabs(rowDot(m, 0, 1)) <= eps && std::fabs(rowDot(m, 0, 2)) <= eps &&
           std::fabs(rowDot(m, 1, 2)) <= eps;
}

bool rigidInverse4(const Mat4& m, Mat4& out) {
    if (!isRigid4(m)) return false;

    out = identity4();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = m[c * 4 + r];

    // -R^T t, read straight out of the transposed rows.
    for (int r = 0; r < 3; ++r) {
        double s = 0.0;
        for (int c = 0; c < 3; ++c) s += m[c * 4 + r] * m[c * 4 + 3];
        out[r * 4 + 3] = -s;
    }
    return true;
}

bool conjugate4(const Mat4& motion, const Mat4& frame, Mat4& out) {
    Mat4 inverse{};
    if (!rigidInverse4(frame, inverse)) return false;
    out = multiply4(inverse, multiply4(motion, frame));
    return true;
}

void apply4(const Mat4& m, double& x, double& y, double& z) {
    const double nx = m[0] * x + m[1] * y + m[2] * z + m[3];
    const double ny = m[4] * x + m[5] * y + m[6] * z + m[7];
    const double nz = m[8] * x + m[9] * y + m[10] * z + m[11];
    x = nx;
    y = ny;
    z = nz;
}

}  // namespace fusenest
