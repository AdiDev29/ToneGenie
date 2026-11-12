#include "tonegenie/Fft.h"

#include <cassert>
#include <cmath>

namespace tg
{

Fft::Fft (int size) : n (size)
{
    assert (isPowerOfTwo (size));

    int bits = 0;
    while ((1 << bits) < n)
        ++bits;

    rev.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        rev[(size_t) i] = r;
    }

    twRe.resize ((size_t) (n / 2 > 0 ? n / 2 : 1));
    twIm.resize (twRe.size());
    for (int k = 0; k < n / 2; ++k)
    {
        const double a = -2.0 * 3.14159265358979323846 * (double) k / (double) n;
        twRe[(size_t) k] = (float) std::cos (a);
        twIm[(size_t) k] = (float) std::sin (a);
    }
}

void Fft::transform (std::complex<float>* data, bool inverse) const noexcept
{
    // std::complex<float> is layout-compatible with float[2].
    auto* d = reinterpret_cast<float*> (data);

    for (int i = 0; i < n; ++i)
    {
        const int j = rev[(size_t) i];
        if (i < j)
        {
            const float re = d[2 * i], im = d[2 * i + 1];
            d[2 * i] = d[2 * j];
            d[2 * i + 1] = d[2 * j + 1];
            d[2 * j] = re;
            d[2 * j + 1] = im;
        }
    }

    const float sign = inverse ? -1.0f : 1.0f;

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len >> 1;
        const int step = n / len;

        for (int i = 0; i < n; i += len)
        {
            float* a = d + 2 * i;
            float* b = d + 2 * (i + half);

            for (int j = 0; j < half; ++j)
            {
                const float wr = twRe[(size_t) (j * step)];
                const float wi = sign * twIm[(size_t) (j * step)];

                const float br = b[2 * j], bi = b[2 * j + 1];
                const float vr = br * wr - bi * wi;
                const float vi = br * wi + bi * wr;

                const float ur = a[2 * j], ui = a[2 * j + 1];
                a[2 * j] = ur + vr;
                a[2 * j + 1] = ui + vi;
                b[2 * j] = ur - vr;
                b[2 * j + 1] = ui - vi;
            }
        }
    }

    if (inverse)
    {
        const float s = 1.0f / (float) n;
        for (int i = 0; i < 2 * n; ++i)
            d[i] *= s;
    }
}

} // namespace tg
