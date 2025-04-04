#include "util/sample.h"

#include <cstddef>
#include <cstdlib>

#include "engine/engine.h"
#include "util/math.h"

#ifdef __WINDOWS__
#include <QtGlobal>
typedef qint64 int64_t;
typedef qint32 int32_t;
#endif

// LOOP VECTORIZED below marks the loops that are processed with the 128 bit SSE
// registers as tested with gcc 7.5 and the -ftree-vectorize -fopt-info-vec-optimized flags on
// an Intel i5 CPU. When changing, be careful to not disturb the vectorization.
// https://gcc.gnu.org/projects/tree-ssa/vectorization.html
// This also utilizes AVX registers when compiled for a recent 64-bit CPU
// using scons optimize=native.
// "SINT i" is the preferred loop index type that should allow vectorization in
// general. Unfortunately there are exceptions where "int i" is required for some reasons.

namespace {

#ifdef __AVX__
constexpr size_t kAlignment = 32;
#else
constexpr size_t kAlignment = 16;
#endif

// TODO() Check if uintptr_t is available on all our build targets and use that
// instead of size_t, we can remove the sizeof(size_t) check than
constexpr bool useAlignedAlloc() {
    // This will work on all targets and compilers.
    // It will return true bot 32 bit builds and false for 64 bit builds
    return alignof(max_align_t) < kAlignment &&
            sizeof(CSAMPLE*) == sizeof(size_t);
}

} // anonymous namespace

// static
CSAMPLE* SampleUtil::alloc(SINT size) {
    // To speed up vectorization we align our sample buffers to 16-byte (128
    // bit) boundaries on SSE builds and 32-byte (256 bit) on AVX builds so
    // that vectorized loops doesn't have to do a serial ramp-up before going
    // parallel.
    //
    // Pointers returned by malloc are aligned for the largest scalar type. On
    // most platforms the largest scalar type is long double (16 bytes).
    // However, on MSVC x86 long double is 8 bytes.
    // This can be tested via alignof(std::max_align_t)
    if (useAlignedAlloc()) {
#if defined(_MSC_VER)
        // On MSVC, we use _aligned_malloc to handle aligning pointers to 16-byte
        // boundaries.
        return static_cast<CSAMPLE*>(
                _aligned_malloc(sizeof(CSAMPLE) * size, kAlignment));
#elif defined(_GLIBCXX_HAVE_ALIGNED_ALLOC)
        std::size_t alloc_size = sizeof(CSAMPLE) * size;
        // The size (in bytes) must be an integral multiple of kAlignment
        std::size_t aligned_alloc_size = alloc_size;
        if (alloc_size % kAlignment != 0) {
            aligned_alloc_size += (kAlignment - alloc_size % kAlignment);
        }
        DEBUG_ASSERT(aligned_alloc_size % kAlignment == 0);
        return static_cast<CSAMPLE*>(std::aligned_alloc(kAlignment, aligned_alloc_size));
#else
        // On other platforms that might not support std::aligned_alloc
        // yet but where long double is 8 bytes this code allocates 16 additional
        // slack bytes so we can adjust the pointer we return to the caller to be
        // 16-byte aligned. We record a pointer to the true start of the buffer
        // in the slack space as well so that we can free it correctly.
        const size_t alignment = kAlignment;
        const size_t unaligned_size = sizeof(CSAMPLE) * size + alignment;
        void* pUnaligned = std::malloc(unaligned_size);
        if (pUnaligned == NULL) {
            return NULL;
        }
        // Shift
        void* pAligned = (void*)(((size_t)pUnaligned & ~(alignment - 1)) + alignment);
        // Store pointer to the original buffer in the slack space before the
        // shifted pointer.
        *((void**)(pAligned) - 1) = pUnaligned;
        return static_cast<CSAMPLE*>(pAligned);
#endif
    } else {
        // Our platform already produces aligned pointers (or is an exotic target)
        return static_cast<CSAMPLE*>(std::malloc(sizeof(CSAMPLE) * size));
    }
}

void SampleUtil::free(CSAMPLE* pBuffer) {
    // See SampleUtil::alloc() for details
    if (useAlignedAlloc()) {
#if defined(_MSC_VER)
        _aligned_free(pBuffer);
#elif defined(_GLIBCXX_HAVE_ALIGNED_ALLOC)
        std::free(pBuffer);
#else
        // Pointer to the original memory is stored before pBuffer
        if (!pBuffer) {
            return;
        }
        std::free(*((void**)((void*)pBuffer) - 1));
#endif
    } else {
        std::free(pBuffer);
    }
}

// static
void SampleUtil::applyGain(CSAMPLE* pBuffer, CSAMPLE_GAIN gain,
        SINT numSamples) {
    if (gain == CSAMPLE_GAIN_ONE) {
        return;
    }
    if (gain == CSAMPLE_GAIN_ZERO) {
        clear(pBuffer, numSamples);
        return;
    }

    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numSamples; ++i) {
        pBuffer[i] *= gain;
    }
}

// static
void SampleUtil::applyRampingGain(CSAMPLE* pBuffer, CSAMPLE_GAIN old_gain,
        CSAMPLE_GAIN new_gain, SINT numSamples) {
    if (old_gain == CSAMPLE_GAIN_ONE && new_gain == CSAMPLE_GAIN_ONE) {
        return;
    }
    if (old_gain == CSAMPLE_GAIN_ZERO && new_gain == CSAMPLE_GAIN_ZERO) {
        clear(pBuffer, numSamples);
        return;
    }

    const CSAMPLE_GAIN gain_delta = (new_gain - old_gain)
            / CSAMPLE_GAIN(numSamples / 2);
    if (gain_delta != 0) {
        const CSAMPLE_GAIN start_gain = old_gain + gain_delta;
        // note: LOOP VECTORIZED.
        for (int i = 0; i < numSamples / 2; ++i) {
            const CSAMPLE_GAIN gain = start_gain + gain_delta * i;
            // a loop counter i += 2 prevents vectorizing.
            pBuffer[i * 2] *= gain;
            pBuffer[i * 2 + 1] *= gain;
        }
    } else {
        // note: LOOP VECTORIZED.
        for (int i = 0; i < numSamples; ++i) {
            pBuffer[i] *= old_gain;
        }
    }
}

CSAMPLE SampleUtil::copyWithRampingNormalization(CSAMPLE* pDest,
        const CSAMPLE* pSrc,
        CSAMPLE_GAIN old_gain,
        CSAMPLE_GAIN targetAmplitude,
        SINT numSamples) {
    SINT numMonoSamples = numSamples / mixxx::kEngineChannelOutputCount.value();
    mixMultichannelToMono(pDest, pSrc, numSamples);

    CSAMPLE maxAmplitude = maxAbsAmplitude(pDest, numMonoSamples);
    CSAMPLE_GAIN gain = maxAmplitude == CSAMPLE_ZERO
            ? 1
            : targetAmplitude / maxAmplitude;
    copyWithRampingGain(pDest, pSrc, old_gain, gain, numSamples);

    return gain;
}

// static
void SampleUtil::applyAlternatingGain(CSAMPLE* pBuffer, CSAMPLE gain1,
        CSAMPLE gain2, SINT numSamples) {
    // This handles gain1 == CSAMPLE_GAIN_ONE && gain2 == CSAMPLE_GAIN_ONE as well.
    if (gain1 == gain2) {
        applyGain(pBuffer, gain1, numSamples);
        return;
    }

    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numSamples / 2; ++i) {
        pBuffer[i * 2] *= gain1;
        pBuffer[i * 2 + 1] *= gain2;
    }
}


void SampleUtil::applyRampingAlternatingGain(CSAMPLE* pBuffer,
        CSAMPLE gain1, CSAMPLE gain2,
        CSAMPLE gain1Old, CSAMPLE gain2Old, SINT numSamples) {
    if (gain1 == gain1Old && gain2 == gain2Old){
        applyAlternatingGain(pBuffer, gain1, gain2, numSamples);
        return;
    }

    const CSAMPLE_GAIN gain1Delta = (gain1 - gain1Old)
            / CSAMPLE_GAIN(numSamples / 2);
    if (gain1Delta != 0) {
        const CSAMPLE_GAIN start_gain = gain1Old + gain1Delta;
        for (int i = 0; i < numSamples / 2; ++i) {
            const CSAMPLE_GAIN gain = start_gain + gain1Delta * i;
            pBuffer[i * 2] *= gain;
        }
    } else {
        // not vectorized: vectorization not profitable.
        for (int i = 0; i < numSamples / 2; ++i) {
            pBuffer[i * 2] *= gain1Old;
        }
    }

    const CSAMPLE_GAIN gain2Delta = (gain2 - gain2Old)
            / CSAMPLE_GAIN(numSamples / 2);
    if (gain2Delta != 0) {
        const CSAMPLE_GAIN start_gain = gain2Old + gain2Delta;
        // note: LOOP VECTORIZED. (gcc + clang >= 14)
        for (int i = 0; i < numSamples / 2; ++i) {
            const CSAMPLE_GAIN gain = start_gain + gain2Delta * i;
            pBuffer[i * 2 + 1] *= gain;
        }
    } else {
        // not vectorized: vectorization not profitable.
        for (int i = 0; i < numSamples / 2; ++i) {
            pBuffer[i * 2 + 1] *= gain2Old;
        }
    }
}

// static
void SampleUtil::add(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        SINT numSamples) {
    // note: LOOP VECTORIZED. (gcc + clang >= 14)
    for (SINT i = 0; i < numSamples; ++i) {
        pDest[i] += pSrc[i];
    }
}

// static
void SampleUtil::addWithGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        CSAMPLE_GAIN gain, SINT numSamples) {
    if (gain == CSAMPLE_GAIN_ZERO) {
        return;
    }

    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numSamples; ++i) {
        pDest[i] += pSrc[i] * gain;
    }
}

void SampleUtil::addWithRampingGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        CSAMPLE_GAIN old_gain, CSAMPLE_GAIN new_gain,
        SINT numSamples) {
    if (old_gain == CSAMPLE_GAIN_ZERO && new_gain == CSAMPLE_GAIN_ZERO) {
        return;
    }

    const CSAMPLE_GAIN gain_delta = (new_gain - old_gain)
            / CSAMPLE_GAIN(numSamples / 2);
    if (gain_delta != 0) {
        const CSAMPLE_GAIN start_gain = old_gain + gain_delta;
        // note: LOOP VECTORIZED.
        for (int i = 0; i < numSamples / 2; ++i) {
            const CSAMPLE_GAIN gain = start_gain + gain_delta * i;
            pDest[i * 2] += pSrc[i * 2] * gain;
            pDest[i * 2 + 1] += pSrc[i * 2 + 1] * gain;
        }
    } else {
        // note: LOOP VECTORIZED.
        for (int i = 0; i < numSamples; ++i) {
            pDest[i] += pSrc[i] * old_gain;
        }
    }
}

// static
void SampleUtil::add2WithGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc1, CSAMPLE_GAIN gain1,
        const CSAMPLE* M_RESTRICT pSrc2, CSAMPLE_GAIN gain2,
        SINT numSamples) {
    if (gain1 == CSAMPLE_GAIN_ZERO) {
        addWithGain(pDest, pSrc2, gain2, numSamples);
        return;
    } else if (gain2 == CSAMPLE_GAIN_ZERO) {
        addWithGain(pDest, pSrc1, gain1, numSamples);
        return;
    }

    // note: LOOP VECTORIZED.
    for (int i = 0; i < numSamples; ++i) {
        pDest[i] += pSrc1[i] * gain1 + pSrc2[i] * gain2;
    }
}

// static
void SampleUtil::add3WithGain(CSAMPLE* pDest,
        const CSAMPLE* M_RESTRICT pSrc1, CSAMPLE_GAIN gain1,
        const CSAMPLE* M_RESTRICT pSrc2, CSAMPLE_GAIN gain2,
        const CSAMPLE* M_RESTRICT pSrc3, CSAMPLE_GAIN gain3,
        SINT numSamples) {
    if (gain1 == CSAMPLE_GAIN_ZERO) {
        add2WithGain(pDest, pSrc2, gain2, pSrc3, gain3, numSamples);
        return;
    } else if (gain2 == CSAMPLE_GAIN_ZERO) {
        add2WithGain(pDest, pSrc1, gain1, pSrc3, gain3, numSamples);
        return;
    } else if (gain3 == CSAMPLE_GAIN_ZERO) {
        add2WithGain(pDest, pSrc1, gain1, pSrc2, gain2, numSamples);
        return;
    }

    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numSamples; ++i) {
        pDest[i] += pSrc1[i] * gain1 + pSrc2[i] * gain2 + pSrc3[i] * gain3;
    }
}

// static
void SampleUtil::copyWithGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        CSAMPLE_GAIN gain, SINT numSamples) {
    if (gain == CSAMPLE_GAIN_ONE) {
        copy(pDest, pSrc, numSamples);
        return;
    }
    if (gain == CSAMPLE_GAIN_ZERO) {
        clear(pDest, numSamples);
        return;
    }

    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numSamples; ++i) {
        pDest[i] = pSrc[i] * gain;
    }

    // OR! need to test which fares better
    // copy(pDest, pSrc, iNumSamples);
    // applyGain(pDest, gain);
}

// static
void SampleUtil::copyWithRampingGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        CSAMPLE_GAIN old_gain,
        CSAMPLE_GAIN new_gain,
        SINT numSamples) {
    if (old_gain == CSAMPLE_GAIN_ONE && new_gain == CSAMPLE_GAIN_ONE) {
        copy(pDest, pSrc, numSamples);
        return;
    }
    if (old_gain == CSAMPLE_GAIN_ZERO && new_gain == CSAMPLE_GAIN_ZERO) {
        clear(pDest, numSamples);
        return;
    }

    const CSAMPLE_GAIN gain_delta = (new_gain - old_gain)
            / CSAMPLE_GAIN(numSamples / 2);
    if (gain_delta != 0) {
        const CSAMPLE_GAIN start_gain = old_gain + gain_delta;
        // note: LOOP VECTORIZED only with "int i" (not SINT i).
        for (int i = 0; i < numSamples / 2; ++i) {
            const CSAMPLE_GAIN gain = start_gain + gain_delta * i;
            pDest[i * 2] = pSrc[i * 2] * gain;
            pDest[i * 2 + 1] = pSrc[i * 2 + 1] * gain;
        }
    } else {
        // note: LOOP VECTORIZED.
        for (SINT i = 0; i < numSamples; ++i) {
            pDest[i] = pSrc[i] * old_gain;
        }
    }

    // OR! need to test which fares better
    // copy(pDest, pSrc, iNumSamples);
    // applyRampingGain(pDest, gain);
}

// static
void SampleUtil::convertS16ToFloat32(CSAMPLE* M_RESTRICT pDest,
        const SAMPLE* M_RESTRICT pSrc, SINT numSamples) {
    // SAMPLE_MIN = -32768 is a valid low sample, whereas SAMPLE_MAX = 32767
    // is the highest valid sample. Note that this means that although some
    // sample values convert to -1.0, none will convert to +1.0.
    DEBUG_ASSERT(-SAMPLE_MINIMUM >= SAMPLE_MAXIMUM);
    const CSAMPLE kConversionFactor = SAMPLE_MINIMUM * -1.0f;
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numSamples; ++i) {
        pDest[i] = CSAMPLE(pSrc[i]) / kConversionFactor;
    }
}

//static
void SampleUtil::convertFloat32ToS16(SAMPLE* pDest, const CSAMPLE* pSrc,
        SINT numSamples) {
    // We use here -SAMPLE_MINIMUM for a perfect round trip with convertS16ToFloat32
    // +1.0 is clamped to 32767 (0.99996942)
    DEBUG_ASSERT(-SAMPLE_MINIMUM >= SAMPLE_MAXIMUM);
    const CSAMPLE kConversionFactor = SAMPLE_MINIMUM * -1.0f;
    // note: LOOP VECTORIZED only with "int i" (not SINT i).
    for (int i = 0; i < numSamples; ++i) {
        pDest[i] = static_cast<SAMPLE>(math_clamp(pSrc[i] * kConversionFactor,
                static_cast<CSAMPLE>(SAMPLE_MINIMUM),
                static_cast<CSAMPLE>(SAMPLE_MAXIMUM)));
    }
}

// static
SampleUtil::CLIP_STATUS SampleUtil::sumAbsPerChannel(CSAMPLE* pfAbsL,
        CSAMPLE* pfAbsR, const CSAMPLE* pBuffer, SINT numSamples) {
    CSAMPLE fAbsL = CSAMPLE_ZERO;
    CSAMPLE fAbsR = CSAMPLE_ZERO;
    CSAMPLE clippedL = 0;
    CSAMPLE clippedR = 0;

    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numSamples / 2; ++i) {
        CSAMPLE absl = fabs(pBuffer[i * 2]);
        fAbsL += absl;
        clippedL += absl > CSAMPLE_PEAK ? 1 : 0;
        CSAMPLE absr = fabs(pBuffer[i * 2 + 1]);
        fAbsR += absr;
        // Replacing the code with a bool clipped will prevent vetorizing
        clippedR += absr > CSAMPLE_PEAK ? 1 : 0;
    }

    *pfAbsL = fAbsL;
    *pfAbsR = fAbsR;
    SampleUtil::CLIP_STATUS clipping = SampleUtil::NO_CLIPPING;
    if (clippedL > 0) {
        clipping |= SampleUtil::CLIPPING_LEFT;
    }
    if (clippedR > 0) {
        clipping |= SampleUtil::CLIPPING_RIGHT;
    }
    return clipping;
}

// static
CSAMPLE SampleUtil::sumSquared(const CSAMPLE* pBuffer, SINT numSamples) {
    CSAMPLE sumSq = CSAMPLE_ZERO;

    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numSamples; ++i) {
        sumSq += pBuffer[i] * pBuffer[i];
    }

    return sumSq;
}

// static
CSAMPLE SampleUtil::rms(const CSAMPLE* pBuffer, SINT numSamples) {
    return sqrtf(sumSquared(pBuffer, numSamples) / numSamples);
}

CSAMPLE SampleUtil::maxAbsAmplitude(const CSAMPLE* pBuffer, SINT numSamples) {
    CSAMPLE max = pBuffer[0];
    // note: LOOP VECTORIZED.
    for (SINT i = 1; i < numSamples; ++i) {
        CSAMPLE absValue = abs(pBuffer[i]);
        if (absValue > max) {
            max = absValue;
        }
    }
    return max;
}

// static
void SampleUtil::copyClampBuffer(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc, SINT iNumSamples) {
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < iNumSamples; ++i) {
        pDest[i] = clampSample(pSrc[i]);
    }
}

// static
void SampleUtil::interleaveBuffer(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc1,
        const CSAMPLE* M_RESTRICT pSrc2,
        SINT numFrames) {
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numFrames; ++i) {
        pDest[2 * i] = pSrc1[i];
        pDest[2 * i + 1] = pSrc2[i];
    }
}

// static
void SampleUtil::interleaveBuffer(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc1,
        const CSAMPLE* M_RESTRICT pSrc2,
        const CSAMPLE* M_RESTRICT pSrc3,
        const CSAMPLE* M_RESTRICT pSrc4,
        const CSAMPLE* M_RESTRICT pSrc5,
        const CSAMPLE* M_RESTRICT pSrc6,
        const CSAMPLE* M_RESTRICT pSrc7,
        const CSAMPLE* M_RESTRICT pSrc8,
        SINT numFrames) {
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numFrames; ++i) {
        pDest[8 * i] = pSrc1[i];
        pDest[8 * i + 1] = pSrc2[i];
        pDest[8 * i + 2] = pSrc3[i];
        pDest[8 * i + 3] = pSrc4[i];
        pDest[8 * i + 4] = pSrc5[i];
        pDest[8 * i + 5] = pSrc6[i];
        pDest[8 * i + 6] = pSrc7[i];
        pDest[8 * i + 7] = pSrc8[i];
    }
}

// static
void SampleUtil::deinterleaveBuffer(CSAMPLE* M_RESTRICT pDest1,
        CSAMPLE* M_RESTRICT pDest2,
        const CSAMPLE* M_RESTRICT pSrc,
        SINT numFrames) {
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numFrames; ++i) {
        pDest1[i] = pSrc[i * 2];
        pDest2[i] = pSrc[i * 2 + 1];
    }
}

// static
void SampleUtil::deinterleaveBuffer(CSAMPLE* M_RESTRICT pDest1,
        CSAMPLE* M_RESTRICT pDest2,
        CSAMPLE* M_RESTRICT pDest3,
        CSAMPLE* M_RESTRICT pDest4,
        CSAMPLE* M_RESTRICT pDest5,
        CSAMPLE* M_RESTRICT pDest6,
        CSAMPLE* M_RESTRICT pDest7,
        CSAMPLE* M_RESTRICT pDest8,
        const CSAMPLE* M_RESTRICT pSrc,
        SINT numFrames) {
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numFrames; ++i) {
        pDest1[i] = pSrc[i * 8];
        pDest2[i] = pSrc[i * 8 + 1];
        pDest3[i] = pSrc[i * 8 + 2];
        pDest4[i] = pSrc[i * 8 + 3];
        pDest5[i] = pSrc[i * 8 + 4];
        pDest6[i] = pSrc[i * 8 + 5];
        pDest7[i] = pSrc[i * 8 + 6];
        pDest8[i] = pSrc[i * 8 + 7];
    }
}

// static
void SampleUtil::linearCrossfadeStereoBuffersOut(
        CSAMPLE* M_RESTRICT pDestSrcFadeOut,
        const CSAMPLE* M_RESTRICT pSrcFadeIn,
        SINT numSamples) {
    // M_RESTRICT unoptimizes the function for some reason.
    const CSAMPLE_GAIN cross_inc = CSAMPLE_GAIN_ONE
            / CSAMPLE_GAIN(numSamples / 2);
    // note: LOOP VECTORIZED only with "int i" (not SINT i)
    for (int i = 0; i < numSamples / 2; ++i) {
        const CSAMPLE_GAIN cross_mix = cross_inc * i;
        pDestSrcFadeOut[i * 2] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 2] += pSrcFadeIn[i * 2] * cross_mix;
    }
    // note: LOOP VECTORIZED only with "int i" (not SINT i)
    for (int i = 0; i < numSamples / 2; ++i) {
        const CSAMPLE_GAIN cross_mix = cross_inc * i;
        pDestSrcFadeOut[i * 2 + 1] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 2 + 1] += pSrcFadeIn[i * 2 + 1] * cross_mix;
    }
}

// static
void SampleUtil::linearCrossfadeStemBuffersOut(
        CSAMPLE* M_RESTRICT pDestSrcFadeOut,
        const CSAMPLE* M_RESTRICT pSrcFadeIn,
        SINT numSamples) {
    // M_RESTRICT unoptimizes the function for some reason.
    const CSAMPLE_GAIN cross_inc = CSAMPLE_GAIN_ONE / CSAMPLE_GAIN(numSamples / 8);
    // note: LOOP VECTORIZED.
    for (int i = 0; i < numSamples / 8; ++i) {
        const CSAMPLE_GAIN cross_mix = cross_inc * i;
        pDestSrcFadeOut[i * 8] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 8] += pSrcFadeIn[i * 8] * cross_mix;
        pDestSrcFadeOut[i * 8 + 1] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 8 + 1] += pSrcFadeIn[i * 8 + 1] * cross_mix;
        pDestSrcFadeOut[i * 8 + 2] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 8 + 2] += pSrcFadeIn[i * 8 + 2] * cross_mix;
        pDestSrcFadeOut[i * 8 + 3] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 8 + 3] += pSrcFadeIn[i * 8 + 3] * cross_mix;
        pDestSrcFadeOut[i * 8 + 4] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 8 + 4] += pSrcFadeIn[i * 8 + 4] * cross_mix;
        pDestSrcFadeOut[i * 8 + 5] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 8 + 5] += pSrcFadeIn[i * 8 + 5] * cross_mix;
        pDestSrcFadeOut[i * 8 + 6] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 8 + 6] += pSrcFadeIn[i * 8 + 6] * cross_mix;
        pDestSrcFadeOut[i * 8 + 7] *= (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeOut[i * 8 + 7] += pSrcFadeIn[i * 8 + 7] * cross_mix;
    }
}

// static
void SampleUtil::linearCrossfadeBuffersOut(
        CSAMPLE* pDestSrcFadeOut,
        const CSAMPLE* pSrcFadeIn,
        SINT numSamples,
        int channelCount) {
    switch (channelCount) {
    case mixxx::audio::ChannelCount::stereo():
        return SampleUtil::linearCrossfadeStereoBuffersOut(
                pDestSrcFadeOut,
                pSrcFadeIn,
                numSamples);
    case mixxx::audio::ChannelCount::stem():
        return SampleUtil::linearCrossfadeStemBuffersOut(
                pDestSrcFadeOut,
                pSrcFadeIn,
                numSamples);
    default:
        // Fallback to unoptimised function
        {
            DEBUG_ASSERT(numSamples % channelCount == 0);
            int numFrame = numSamples / channelCount;
            const CSAMPLE_GAIN cross_inc =
                    CSAMPLE_GAIN_ONE / CSAMPLE_GAIN(numSamples / channelCount);
            for (int i = 0; i < numFrame; ++i) {
                const CSAMPLE_GAIN cross_mix = cross_inc * i;
                // note: LOOP VECTORIZED.
                for (int chIdx = 0; chIdx < channelCount; chIdx++) {
                    pDestSrcFadeOut[i * channelCount + chIdx] *= (CSAMPLE_GAIN_ONE - cross_mix);
                    pDestSrcFadeOut[i * channelCount + chIdx] +=
                            pSrcFadeIn[i * channelCount + chIdx] * cross_mix;
                }
            }
        }
        break;
    }
}

// static
void SampleUtil::linearCrossfadeStereoBuffersIn(
        CSAMPLE* M_RESTRICT pDestSrcFadeIn,
        const CSAMPLE* M_RESTRICT pSrcFadeOut,
        SINT numSamples) {
    // M_RESTRICT unoptimizes the function for some reason.
    const CSAMPLE_GAIN cross_inc = CSAMPLE_GAIN_ONE / CSAMPLE_GAIN(numSamples / 2);
    /// note: LOOP VECTORIZED only with "int i" (not SINT i)
    for (int i = 0; i < numSamples / 2; ++i) {
        const CSAMPLE_GAIN cross_mix = cross_inc * i;
        pDestSrcFadeIn[i * 2] *= cross_mix;
        pDestSrcFadeIn[i * 2] += pSrcFadeOut[i * 2] * (CSAMPLE_GAIN_ONE - cross_mix);
    }
    // note: LOOP VECTORIZED only with "int i" (not SINT i)
    for (int i = 0; i < numSamples / 2; ++i) {
        const CSAMPLE_GAIN cross_mix = cross_inc * i;
        pDestSrcFadeIn[i * 2 + 1] *= cross_mix;
        pDestSrcFadeIn[i * 2 + 1] += pSrcFadeOut[i * 2 + 1] * (CSAMPLE_GAIN_ONE - cross_mix);
    }
}

// static
void SampleUtil::linearCrossfadeStemBuffersIn(
        CSAMPLE* M_RESTRICT pDestSrcFadeIn,
        const CSAMPLE* M_RESTRICT pSrcFadeOut,
        SINT numSamples) {
    // M_RESTRICT unoptimizes the function for some reason.
    const CSAMPLE_GAIN cross_inc = CSAMPLE_GAIN_ONE / CSAMPLE_GAIN(numSamples / 8);
    // note: LOOP VECTORIZED.
    for (int i = 0; i < numSamples / 8; ++i) {
        const CSAMPLE_GAIN cross_mix = cross_inc * i;
        pDestSrcFadeIn[i * 8] *= cross_mix;
        pDestSrcFadeIn[i * 8] += pSrcFadeOut[i * 8] * (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeIn[i * 8 + 1] *= cross_mix;
        pDestSrcFadeIn[i * 8 + 1] += pSrcFadeOut[i * 8 + 1] * (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeIn[i * 8 + 2] *= cross_mix;
        pDestSrcFadeIn[i * 8 + 2] += pSrcFadeOut[i * 8 + 2] * (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeIn[i * 8 + 3] *= cross_mix;
        pDestSrcFadeIn[i * 8 + 3] += pSrcFadeOut[i * 8 + 3] * (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeIn[i * 8 + 4] *= cross_mix;
        pDestSrcFadeIn[i * 8 + 4] += pSrcFadeOut[i * 8 + 4] * (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeIn[i * 8 + 5] *= cross_mix;
        pDestSrcFadeIn[i * 8 + 5] += pSrcFadeOut[i * 8 + 5] * (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeIn[i * 8 + 6] *= cross_mix;
        pDestSrcFadeIn[i * 8 + 6] += pSrcFadeOut[i * 8 + 6] * (CSAMPLE_GAIN_ONE - cross_mix);
        pDestSrcFadeIn[i * 8 + 7] *= cross_mix;
        pDestSrcFadeIn[i * 8 + 7] += pSrcFadeOut[i * 8 + 7] * (CSAMPLE_GAIN_ONE - cross_mix);
    }
}

// static
void SampleUtil::linearCrossfadeBuffersIn(
        CSAMPLE* pDestSrcFadeIn,
        const CSAMPLE* pSrcFadeOut,
        SINT numSamples,
        int channelCount) {
    switch (channelCount) {
    case mixxx::audio::ChannelCount::stereo():
        return SampleUtil::linearCrossfadeStereoBuffersIn(
                pDestSrcFadeIn,
                pSrcFadeOut,
                numSamples);
    case mixxx::audio::ChannelCount::stem():
        return SampleUtil::linearCrossfadeStemBuffersIn(
                pDestSrcFadeIn,
                pSrcFadeOut,
                numSamples);
    default:
        // Fallback to unoptimised function
        {
            int numFrame = numSamples / channelCount;
            const CSAMPLE_GAIN cross_inc =
                    CSAMPLE_GAIN_ONE / CSAMPLE_GAIN(numSamples / channelCount);
            for (int i = 0; i < numFrame; ++i) {
                const CSAMPLE_GAIN cross_mix = cross_inc * i;
                // note: LOOP VECTORIZED.
                for (int chIdx = 0; chIdx < channelCount; chIdx++) {
                    pDestSrcFadeIn[i * channelCount + chIdx] *= cross_mix;
                    pDestSrcFadeIn[i * channelCount + chIdx] +=
                            pSrcFadeOut[i * channelCount + chIdx] *
                            (CSAMPLE_GAIN_ONE - cross_mix);
                }
            }
        }
        break;
    }
}

// static
void SampleUtil::mixStereoToMono(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        SINT numSamples) {
    const CSAMPLE_GAIN mixScale = CSAMPLE_GAIN_ONE
            / (CSAMPLE_GAIN_ONE + CSAMPLE_GAIN_ONE);
    // note: LOOP VECTORIZED
    for (SINT i = 0; i < numSamples / 2; ++i) {
        pDest[i * 2] = (pSrc[i * 2] + pSrc[i * 2 + 1]) * mixScale;
        pDest[i * 2 + 1] = pDest[i * 2];
    }
}

// static
void SampleUtil::mixStereoToMono(CSAMPLE* pBuffer, SINT numSamples) {
    const CSAMPLE_GAIN mixScale = CSAMPLE_GAIN_ONE / (CSAMPLE_GAIN_ONE + CSAMPLE_GAIN_ONE);
    // note: LOOP VECTORIZED
    for (SINT i = 0; i < numSamples / 2; ++i) {
        pBuffer[i * 2] = (pBuffer[i * 2] + pBuffer[i * 2 + 1]) * mixScale;
        pBuffer[i * 2 + 1] = pBuffer[i * 2];
    }
}

// static
void SampleUtil::mixMultichannelToMono(CSAMPLE* pDest, const CSAMPLE* pSrc, SINT numSamples) {
    auto chCount = mixxx::kEngineChannelOutputCount.value();
    const CSAMPLE_GAIN mixScale = CSAMPLE_GAIN_ONE / (CSAMPLE_GAIN_ONE * chCount);
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numSamples / chCount; ++i) {
        pDest[i] = CSAMPLE_ZERO;
        for (auto ch = 0; ch < chCount; ++ch) {
            pDest[i] += pSrc[i * chCount + ch];
        }
        pDest[i] *= mixScale;
    }
}

// static
void SampleUtil::mixMultichannelToStereo(CSAMPLE* pDest,
        const CSAMPLE* pSrc,
        SINT numFrames,
        mixxx::audio::ChannelCount numChannels,
        int excludeChannelMask) {
    DEBUG_ASSERT(numChannels > mixxx::audio::ChannelCount::stereo());
    int stereoChCount = numChannels / mixxx::audio::ChannelCount::stereo();
    // Making sure we aren't using this function with more channel than supported with the mask
    DEBUG_ASSERT(stereoChCount < static_cast<int>(sizeof(excludeChannelMask) * 8));
    SampleUtil::clear(pDest, numFrames * mixxx::audio::ChannelCount::stereo());
    for (int stemIdx = 0; stemIdx < stereoChCount; stemIdx++) {
        if (excludeChannelMask >> stemIdx & 0b1) {
            continue;
        }
        // note: LOOP VECTORIZED.
        for (int i = 0; i < numFrames; i++) {
            const int srcIdx = numChannels * i +
                    stemIdx * mixxx::audio::ChannelCount::stereo();
            const int destIdx = mixxx::audio::ChannelCount::stereo() * i;
            pDest[destIdx] +=
                    pSrc[srcIdx];
            pDest[destIdx + 1] +=
                    pSrc[srcIdx + 1];
        }
    }
}

// static
void SampleUtil::mixMultichannelToStereo(CSAMPLE* pDest,
        const CSAMPLE* pSrc,
        SINT numFrames,
        mixxx::audio::ChannelCount numChannels) {
    DEBUG_ASSERT(numChannels > mixxx::audio::ChannelCount::stereo());
    int stereoChCount = numChannels / mixxx::audio::ChannelCount::stereo();
    SampleUtil::clear(pDest, numFrames * mixxx::audio::ChannelCount::stereo());
    for (int stemIdx = 0; stemIdx < stereoChCount; stemIdx++) {
        // note: LOOP VECTORIZED.
        for (int i = 0; i < numFrames; i++) {
            const int srcIdx = numChannels * i +
                    stemIdx * mixxx::audio::ChannelCount::stereo();
            const int destIdx = mixxx::audio::ChannelCount::stereo() * i;
            pDest[destIdx] +=
                    pSrc[srcIdx];
            pDest[destIdx + 1] +=
                    pSrc[srcIdx + 1];
        }
    }
}

// static
void SampleUtil::doubleMonoToDualMono(CSAMPLE* pBuffer, SINT numFrames) {
    // backward loop
    SINT i = numFrames;
    // not vectorized: vector version will never be profitable.
    while (0 < i--) {
        const CSAMPLE s = pBuffer[i];
        pBuffer[i * 2] = s;
        pBuffer[i * 2 + 1] = s;
    }
}

// static
void SampleUtil::copyMonoToDualMono(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc, SINT numFrames) {
    // forward loop
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numFrames; ++i) {
        const CSAMPLE s = pSrc[i];
        pDest[i * 2] = s;
        pDest[i * 2 + 1] = s;
    }
}

// static
void SampleUtil::addMonoToStereoWithGain(CSAMPLE_GAIN gain,
        CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        SINT numFrames) {
    if (gain == 0.0) {
        // no need to add silence
        return;
    }
    // forward loop
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numFrames; ++i) {
        const CSAMPLE s = pSrc[i] * gain;
        pDest[i * 2] += s;
        pDest[i * 2 + 1] += s;
    }
}

// static
void SampleUtil::addMonoToStereo(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        SINT numFrames) {
    // lets hope the compiler inlines here and optimizes the multiplication away
    return addMonoToStereoWithGain(1, pDest, pSrc, numFrames);
}

// static
void SampleUtil::stripMultiToStereo(
        CSAMPLE* pBuffer,
        SINT numFrames,
        mixxx::audio::ChannelCount numChannels) {
    DEBUG_ASSERT(numChannels > mixxx::audio::ChannelCount::stereo());
    // forward loop
    for (SINT i = 0; i < numFrames; ++i) {
        pBuffer[i * 2] = pBuffer[i * numChannels];
        pBuffer[i * 2 + 1] = pBuffer[i * numChannels + 1];
    }
}

// static
void SampleUtil::copyOneStereoFromMulti(
        CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        SINT numFrames,
        mixxx::audio::ChannelCount numChannels,
        int sourceChannel) {
    DEBUG_ASSERT(numChannels > mixxx::audio::ChannelCount::stereo());
    // forward loop
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numFrames; ++i) {
        pDest[i * 2] = pSrc[i * numChannels + sourceChannel];
        pDest[i * 2 + 1] = pSrc[i * numChannels + sourceChannel + 1];
    }
}

// static
void SampleUtil::insertStereoToMulti(
        CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        SINT numFrames,
        mixxx::audio::ChannelCount numChannels,
        int channelOffset) {
    DEBUG_ASSERT(numChannels > mixxx::audio::ChannelCount::stereo() &&
            channelOffset - 1 < numFrames);
    // forward loop
    // note: LOOP VECTORIZED.
    for (SINT i = 0; i < numFrames; ++i) {
        pDest[i * numChannels + channelOffset] = pSrc[i * 2];
        pDest[i * numChannels + channelOffset + 1] = pSrc[i * 2 + 1];
    }
}

// static
void SampleUtil::reverse(CSAMPLE* pBuffer, SINT numSamples) {
    for (SINT j = 0; j < numSamples / 4; ++j) {
        const SINT endpos = (numSamples - 1) - j * 2 ;
        CSAMPLE temp1 = pBuffer[j * 2];
        CSAMPLE temp2 = pBuffer[j * 2 + 1];
        pBuffer[j * 2] = pBuffer[endpos - 1];
        pBuffer[j * 2 + 1] = pBuffer[endpos];
        pBuffer[endpos - 1] = temp1;
        pBuffer[endpos] = temp2;
    }
}

// static
void SampleUtil::copyReverse(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc,
        SINT numSamples,
        int channelCount) {
    DEBUG_ASSERT(numSamples % channelCount == 0);
    for (SINT frameIdx = 0; frameIdx < numSamples / channelCount; ++frameIdx) {
        const int endpos = (numSamples - 1) - frameIdx * channelCount;
        // note: LOOP VECTORIZED.
        for (int chIdx = 0; chIdx < channelCount; chIdx++) {
            pDest[frameIdx * channelCount + chIdx] = pSrc[endpos - channelCount + chIdx + 1];
        }
    }
}
// static
void SampleUtil::copy1WithGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc0,
        CSAMPLE_GAIN gain0,
        int iNumSamples) {
    if (gain0 == CSAMPLE_GAIN_ZERO) {
        clear(pDest, iNumSamples);
        return;
    }
    // note: LOOP VECTORIZED.
    for (int i = 0; i < iNumSamples; ++i) {
        pDest[i] = pSrc0[i] * gain0;
    }
}
// static
void SampleUtil::copy1WithRampingGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc0,
        CSAMPLE_GAIN gain0in,
        CSAMPLE_GAIN gain0out,
        int iNumSamples) {
    if (gain0in == CSAMPLE_GAIN_ZERO && gain0out == CSAMPLE_GAIN_ZERO) {
        clear(pDest, iNumSamples);
        return;
    }
    const CSAMPLE_GAIN gain_delta0 = (gain0out - gain0in) / (iNumSamples / 2);
    const CSAMPLE_GAIN start_gain0 = gain0in + gain_delta0;
    // note: LOOP VECTORIZED.
    for (int i = 0; i < iNumSamples / 2; ++i) {
        const CSAMPLE_GAIN gain0 = start_gain0 + gain_delta0 * i;
        pDest[i * 2] = pSrc0[i * 2] * gain0;
        pDest[i * 2 + 1] = pSrc0[i * 2 + 1] * gain0;
    }
}
// static
void SampleUtil::copy2WithGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc0,
        CSAMPLE_GAIN gain0,
        const CSAMPLE* M_RESTRICT pSrc1,
        CSAMPLE_GAIN gain1,
        int iNumSamples) {
    if (gain0 == CSAMPLE_GAIN_ZERO) {
        copy1WithGain(pDest, pSrc1, gain1, iNumSamples);
        return;
    }
    if (gain1 == CSAMPLE_GAIN_ZERO) {
        copy1WithGain(pDest, pSrc0, gain0, iNumSamples);
        return;
    }
    // note: LOOP VECTORIZED.
    for (int i = 0; i < iNumSamples; ++i) {
        pDest[i] = pSrc0[i] * gain0 +
                pSrc1[i] * gain1;
    }
}
// static
void SampleUtil::copy2WithRampingGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc0,
        CSAMPLE_GAIN gain0in,
        CSAMPLE_GAIN gain0out,
        const CSAMPLE* M_RESTRICT pSrc1,
        CSAMPLE_GAIN gain1in,
        CSAMPLE_GAIN gain1out,
        int iNumSamples) {
    if (gain0in == CSAMPLE_GAIN_ZERO && gain0out == CSAMPLE_GAIN_ZERO) {
        copy1WithRampingGain(pDest, pSrc1, gain1in, gain1out, iNumSamples);
        return;
    }
    if (gain1in == CSAMPLE_GAIN_ZERO && gain1out == CSAMPLE_GAIN_ZERO) {
        copy1WithRampingGain(pDest, pSrc0, gain0in, gain0out, iNumSamples);
        return;
    }
    const CSAMPLE_GAIN gain_delta0 = (gain0out - gain0in) / (iNumSamples / 2);
    const CSAMPLE_GAIN start_gain0 = gain0in + gain_delta0;
    const CSAMPLE_GAIN gain_delta1 = (gain1out - gain1in) / (iNumSamples / 2);
    const CSAMPLE_GAIN start_gain1 = gain1in + gain_delta1;
    // note: LOOP VECTORIZED.
    for (int i = 0; i < iNumSamples / 2; ++i) {
        const CSAMPLE_GAIN gain0 = start_gain0 + gain_delta0 * i;
        const CSAMPLE_GAIN gain1 = start_gain1 + gain_delta1 * i;
        pDest[i * 2] = pSrc0[i * 2] * gain0 +
                pSrc1[i * 2] * gain1;
        pDest[i * 2 + 1] = pSrc0[i * 2 + 1] * gain0 +
                pSrc1[i * 2 + 1] * gain1;
    }
}
// static
void SampleUtil::copy3WithGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc0,
        CSAMPLE_GAIN gain0,
        const CSAMPLE* M_RESTRICT pSrc1,
        CSAMPLE_GAIN gain1,
        const CSAMPLE* M_RESTRICT pSrc2,
        CSAMPLE_GAIN gain2,
        int iNumSamples) {
    if (gain0 == CSAMPLE_GAIN_ZERO) {
        copy2WithGain(pDest, pSrc1, gain1, pSrc2, gain2, iNumSamples);
        return;
    }
    if (gain1 == CSAMPLE_GAIN_ZERO) {
        copy2WithGain(pDest, pSrc0, gain0, pSrc2, gain2, iNumSamples);
        return;
    }
    if (gain2 == CSAMPLE_GAIN_ZERO) {
        copy2WithGain(pDest, pSrc0, gain0, pSrc1, gain1, iNumSamples);
        return;
    }
    // note: LOOP VECTORIZED.
    for (int i = 0; i < iNumSamples; ++i) {
        pDest[i] = pSrc0[i] * gain0 +
                pSrc1[i] * gain1 +
                pSrc2[i] * gain2;
    }
}
// static
void SampleUtil::copy3WithRampingGain(CSAMPLE* M_RESTRICT pDest,
        const CSAMPLE* M_RESTRICT pSrc0,
        CSAMPLE_GAIN gain0in,
        CSAMPLE_GAIN gain0out,
        const CSAMPLE* M_RESTRICT pSrc1,
        CSAMPLE_GAIN gain1in,
        CSAMPLE_GAIN gain1out,
        const CSAMPLE* M_RESTRICT pSrc2,
        CSAMPLE_GAIN gain2in,
        CSAMPLE_GAIN gain2out,
        int iNumSamples) {
    if (gain0in == CSAMPLE_GAIN_ZERO && gain0out == CSAMPLE_GAIN_ZERO) {
        copy2WithRampingGain(pDest,
                pSrc1,
                gain1in,
                gain1out,
                pSrc2,
                gain2in,
                gain2out,
                iNumSamples);
        return;
    }
    if (gain1in == CSAMPLE_GAIN_ZERO && gain1out == CSAMPLE_GAIN_ZERO) {
        copy2WithRampingGain(pDest,
                pSrc0,
                gain0in,
                gain0out,
                pSrc2,
                gain2in,
                gain2out,
                iNumSamples);
        return;
    }
    if (gain2in == CSAMPLE_GAIN_ZERO && gain2out == CSAMPLE_GAIN_ZERO) {
        copy2WithRampingGain(pDest,
                pSrc0,
                gain0in,
                gain0out,
                pSrc1,
                gain1in,
                gain1out,
                iNumSamples);
        return;
    }
    const CSAMPLE_GAIN gain_delta0 = (gain0out - gain0in) / (iNumSamples / 2);
    const CSAMPLE_GAIN start_gain0 = gain0in + gain_delta0;
    const CSAMPLE_GAIN gain_delta1 = (gain1out - gain1in) / (iNumSamples / 2);
    const CSAMPLE_GAIN start_gain1 = gain1in + gain_delta1;
    const CSAMPLE_GAIN gain_delta2 = (gain2out - gain2in) / (iNumSamples / 2);
    const CSAMPLE_GAIN start_gain2 = gain2in + gain_delta2;
    // note: LOOP VECTORIZED.
    for (int i = 0; i < iNumSamples / 2; ++i) {
        const CSAMPLE_GAIN gain0 = start_gain0 + gain_delta0 * i;
        const CSAMPLE_GAIN gain1 = start_gain1 + gain_delta1 * i;
        const CSAMPLE_GAIN gain2 = start_gain2 + gain_delta2 * i;
        pDest[i * 2] = pSrc0[i * 2] * gain0 +
                pSrc1[i * 2] * gain1 +
                pSrc2[i * 2] * gain2;
        pDest[i * 2 + 1] = pSrc0[i * 2 + 1] * gain0 +
                pSrc1[i * 2 + 1] * gain1 +
                pSrc2[i * 2 + 1] * gain2;
    }
}
