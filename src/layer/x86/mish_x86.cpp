// Copyright 2020 Tencent
// SPDX-License-Identifier: BSD-3-Clause

#include "mish_x86.h"

#include "x86_activation.h"

namespace ncnn {

Mish_x86::Mish_x86()
{
#if __SSE2__
    support_packing = true;
#endif // __SSE2__
}

int Mish_x86::forward_inplace(Mat& bottom_top_blob, const Option& opt) const
{
    int w = bottom_top_blob.w;
    int h = bottom_top_blob.h;
    int d = bottom_top_blob.d;
    int channels = bottom_top_blob.c;
    int elempack = bottom_top_blob.elempack;
    int size = w * h * d * elempack;

    #pragma omp parallel for num_threads(opt.num_threads)
    for (int q = 0; q < channels; q++)
    {
        float* ptr = bottom_top_blob.channel(q);

        int i = 0;
#if __SSE2__
#if __AVX__
#if __AVX512F__
        for (; i + 15 < size; i += 16)
        {
            __m512 _p = _mm512_loadu_ps(ptr);
            _p = mish_avx512(_p);
            _mm512_storeu_ps(ptr, _p);
            ptr += 16;
        }
#endif
        for (; i + 7 < size; i += 8)
        {
            __m256 _p = _mm256_loadu_ps(ptr);
            _p = mish_avx(_p);
            _mm256_storeu_ps(ptr, _p);
            ptr += 8;
        }
#endif // __AVX__
        for (; i + 3 < size; i += 4)
        {
            __m128 _p = _mm_loadu_ps(ptr);
            _p = mish_sse(_p);
            _mm_storeu_ps(ptr, _p);
            ptr += 4;
        }
#endif // __SSE2__
        for (; i < size; i++)
        {
            *ptr = *ptr * tanhf(logf(expf(*ptr) + 1.f));
            ptr++;
        }
    }

    return 0;
}

} // namespace ncnn
