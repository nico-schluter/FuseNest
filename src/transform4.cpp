#include "transform4.h"

namespace fusenest {

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

void apply4(const Mat4& m, double& x, double& y, double& z) {
    const double nx = m[0] * x + m[1] * y + m[2] * z + m[3];
    const double ny = m[4] * x + m[5] * y + m[6] * z + m[7];
    const double nz = m[8] * x + m[9] * y + m[10] * z + m[11];
    x = nx;
    y = ny;
    z = nz;
}

}  // namespace fusenest
