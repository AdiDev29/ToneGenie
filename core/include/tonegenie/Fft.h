#pragma once

#include <complex>
#include <vector>

namespace tg
{

/** Small allocation-free (after construction) radix-2 complex FFT. */
class Fft
{
public:
    explicit Fft (int size);

    int size() const noexcept { return n; }

    /** In-place forward transform (no scaling). */
    void forward (std::complex<float>* data) const noexcept { transform (data, false); }

    /** In-place inverse transform, scaled by 1/N. */
    void inverse (std::complex<float>* data) const noexcept { transform (data, true); }

private:
    void transform (std::complex<float>* data, bool inverse) const noexcept;

    int n = 0;
    std::vector<int> rev;
    std::vector<float> twRe, twIm;
};

/** Returns true if v is a power of two. */
inline bool isPowerOfTwo (int v) noexcept { return v > 0 && (v & (v - 1)) == 0; }

} // namespace tg
