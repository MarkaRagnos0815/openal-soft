/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"
#include "config_simd.h"

#include "alu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "alnumeric.h"
#include "alstring.h"
#include "atomic.h"
#include "core/ambidefs.h"
#include "core/async_event.h"
#include "core/bformatdec.h"
#include "core/bs2b.h"
#include "core/bsinc_defs.h"
#include "core/bsinc_tables.h"
#include "core/bufferline.h"
#include "core/buffer_storage.h"
#include "core/context.h"
#include "core/cpu_caps.h"
#include "core/cubic_tables.h"
#include "core/devformat.h"
#include "core/device.h"
#include "core/effects/base.h"
#include "core/effectslot.h"
#include "core/filters/biquad.h"
#include "core/filters/nfc.h"
#include "core/fpu_ctrl.h"
#include "core/hrtf.h"
#include "core/mastering.h"
#include "core/mixer.h"
#include "core/mixer/defs.h"
#include "core/mixer/hrtfdefs.h"
#include "core/resampler_limits.h"
#include "core/storage_formats.h"
#include "core/uhjfilter.h"
#include "core/voice.h"
#include "core/voice_change.h"
#include "intrusive_ptr.h"
#include "opthelpers.h"
#include "ringbuffer.h"
#include "strutils.h"
#include "vecmat.h"

struct CTag;
#if HAVE_SSE
struct SSETag;
#endif
#if HAVE_SSE2
struct SSE2Tag;
#endif
#if HAVE_SSE4_1
struct SSE4Tag;
#endif
#if HAVE_NEON
struct NEONTag;
#endif
struct PointTag;
struct LerpTag;
struct CubicTag;
struct BSincTag;
struct FastBSincTag;


static_assert(!(MaxResamplerPadding&1), "MaxResamplerPadding is not a multiple of two");


namespace {

using uint = unsigned int;
using namespace std::chrono;
using namespace std::string_view_literals;

float InitConeScale()
{
    float ret{1.0f};
    if(auto optval = al::getenv("__ALSOFT_HALF_ANGLE_CONES"))
    {
        if(al::case_compare(*optval, "true"sv) == 0
            || strtol(optval->c_str(), nullptr, 0) == 1)
            ret *= 0.5f;
    }
    return ret;
}
/* Cone scalar */
const float ConeScale{InitConeScale()};

/* Localized scalars for mono sources (initialized in aluInit, after
 * configuration is loaded).
 */
float XScale{1.0f};
float YScale{1.0f};
float ZScale{1.0f};

/* Source distance scale for NFC filters. */
float NfcScale{1.0f};


using HrtfDirectMixerFunc = void(*)(const FloatBufferSpan LeftOut, const FloatBufferSpan RightOut,
    const std::span<const FloatBufferLine> InSamples, const std::span<float2> AccumSamples,
    const std::span<float,BufferLineSize> TempBuf, const std::span<HrtfChannelState> ChanState,
    const size_t IrSize, const size_t SamplesToDo);

auto MixDirectHrtf = HrtfDirectMixerFunc{MixDirectHrtf_<CTag>};

[[nodiscard]] inline
auto SelectHrtfMixer() -> HrtfDirectMixerFunc
{
#if HAVE_NEON
    if((CPUCapFlags&CPU_CAP_NEON))
        return MixDirectHrtf_<NEONTag>;
#endif
#if HAVE_SSE
    if((CPUCapFlags&CPU_CAP_SSE))
        return MixDirectHrtf_<SSETag>;
#endif

    return MixDirectHrtf_<CTag>;
}


inline void BsincPrepare(const uint increment, BsincState *state, const BSincTable *table)
{
    size_t si{BSincScaleCount - 1};
    float sf{0.0f};

    if(increment > MixerFracOne)
    {
        sf = MixerFracOne/static_cast<float>(increment) - table->scaleBase;
        sf = std::max(0.0f, BSincScaleCount*sf*table->scaleRange - 1.0f);
        si = float2uint(sf);
        /* The interpolation factor is fit to this diagonally-symmetric curve
         * to reduce the transition ripple caused by interpolating different
         * scales of the sinc function.
         */
        sf -= static_cast<float>(si);
        sf = 1.0f - std::sqrt(1.0f - sf*sf);
    }

    state->sf = sf;
    state->m = table->m[si];
    state->l = (state->m/2) - 1;
    state->filter = table->Tab.subspan(table->filterOffset[si]);
}

inline ResamplerFunc SelectResampler(Resampler resampler, uint increment)
{
    switch(resampler)
    {
    case Resampler::Point:
        return Resample_<PointTag,CTag>;
    case Resampler::Linear:
#if HAVE_NEON
        if((CPUCapFlags&CPU_CAP_NEON))
            return Resample_<LerpTag,NEONTag>;
#endif
#if HAVE_SSE4_1
        if((CPUCapFlags&CPU_CAP_SSE4_1))
            return Resample_<LerpTag,SSE4Tag>;
#endif
#if HAVE_SSE2
        if((CPUCapFlags&CPU_CAP_SSE2))
            return Resample_<LerpTag,SSE2Tag>;
#endif
        return Resample_<LerpTag,CTag>;
    case Resampler::Spline:
    case Resampler::Gaussian:
#if HAVE_NEON
        if((CPUCapFlags&CPU_CAP_NEON))
            return Resample_<CubicTag,NEONTag>;
#endif
#if HAVE_SSE4_1
        if((CPUCapFlags&CPU_CAP_SSE4_1))
            return Resample_<CubicTag,SSE4Tag>;
#endif
#if HAVE_SSE2
        if((CPUCapFlags&CPU_CAP_SSE2))
            return Resample_<CubicTag,SSE2Tag>;
#endif
#if HAVE_SSE
        if((CPUCapFlags&CPU_CAP_SSE))
            return Resample_<CubicTag,SSETag>;
#endif
        return Resample_<CubicTag,CTag>;
    case Resampler::BSinc12:
    case Resampler::BSinc24:
    case Resampler::BSinc48:
        if(increment > MixerFracOne)
        {
#if HAVE_NEON
            if((CPUCapFlags&CPU_CAP_NEON))
                return Resample_<BSincTag,NEONTag>;
#endif
#if HAVE_SSE
            if((CPUCapFlags&CPU_CAP_SSE))
                return Resample_<BSincTag,SSETag>;
#endif
            return Resample_<BSincTag,CTag>;
        }
        [[fallthrough]];
    case Resampler::FastBSinc12:
    case Resampler::FastBSinc24:
    case Resampler::FastBSinc48:
#if HAVE_NEON
        if((CPUCapFlags&CPU_CAP_NEON))
            return Resample_<FastBSincTag,NEONTag>;
#endif
#if HAVE_SSE
        if((CPUCapFlags&CPU_CAP_SSE))
            return Resample_<FastBSincTag,SSETag>;
#endif
        return Resample_<FastBSincTag,CTag>;
    }

    return Resample_<PointTag,CTag>;
}

} // namespace

void aluInit(CompatFlagBitset flags, const float nfcscale)
{
    MixDirectHrtf = SelectHrtfMixer();
    XScale = flags.test(CompatFlags::ReverseX) ? -1.0f : 1.0f;
    YScale = flags.test(CompatFlags::ReverseY) ? -1.0f : 1.0f;
    ZScale = flags.test(CompatFlags::ReverseZ) ? -1.0f : 1.0f;

    NfcScale = std::clamp(nfcscale, 0.0001f, 10000.0f);
}


ResamplerFunc PrepareResampler(Resampler resampler, uint increment, InterpState *state)
{
    switch(resampler)
    {
    case Resampler::Point:
    case Resampler::Linear:
        break;
    case Resampler::Spline:
        state->emplace<CubicState>(std::span{gSplineFilter.mTable});
        break;
    case Resampler::Gaussian:
        state->emplace<CubicState>(std::span{gGaussianFilter.mTable});
        break;
    case Resampler::FastBSinc12:
    case Resampler::BSinc12:
        BsincPrepare(increment, &state->emplace<BsincState>(), &gBSinc12);
        break;
    case Resampler::FastBSinc24:
    case Resampler::BSinc24:
        BsincPrepare(increment, &state->emplace<BsincState>(), &gBSinc24);
        break;
    case Resampler::FastBSinc48:
    case Resampler::BSinc48:
        BsincPrepare(increment, &state->emplace<BsincState>(), &gBSinc48);
        break;
    }
    return SelectResampler(resampler, increment);
}


void DeviceBase::ProcessHrtf(const size_t SamplesToDo)
{
    /* HRTF is stereo output only. */
    const size_t lidx{RealOut.ChannelIndex[FrontLeft]};
    const size_t ridx{RealOut.ChannelIndex[FrontRight]};

    MixDirectHrtf(RealOut.Buffer[lidx], RealOut.Buffer[ridx], Dry.Buffer, HrtfAccumData,
        mHrtfState->mTemp, mHrtfState->mChannels, mHrtfState->mIrSize, SamplesToDo);
}

/* NOLINTNEXTLINE(readability-make-member-function-const) */
void DeviceBase::ProcessAmbiDec(const size_t SamplesToDo)
{
    AmbiDecoder->process(RealOut.Buffer, Dry.Buffer, SamplesToDo);
}

void DeviceBase::ProcessAmbiDecStablized(const size_t SamplesToDo)
{
    /* Decode with front image stablization. */
    const size_t lidx{RealOut.ChannelIndex[FrontLeft]};
    const size_t ridx{RealOut.ChannelIndex[FrontRight]};
    const size_t cidx{RealOut.ChannelIndex[FrontCenter]};

    AmbiDecoder->processStablize(RealOut.Buffer, Dry.Buffer, lidx, ridx, cidx, SamplesToDo);
}

void DeviceBase::ProcessUhj(const size_t SamplesToDo)
{
    /* UHJ is stereo output only. */
    const size_t lidx{RealOut.ChannelIndex[FrontLeft]};
    const size_t ridx{RealOut.ChannelIndex[FrontRight]};

    /* Encode to stereo-compatible 2-channel UHJ output. */
    mUhjEncoder->encode(RealOut.Buffer[lidx].data(), RealOut.Buffer[ridx].data(),
        {{Dry.Buffer[0].data(), Dry.Buffer[1].data(), Dry.Buffer[2].data()}}, SamplesToDo);
}

void DeviceBase::ProcessBs2b(const size_t SamplesToDo)
{
    /* BS2B is stereo output only. */
    const auto lidx = size_t{RealOut.ChannelIndex[FrontLeft]};
    const auto ridx = size_t{RealOut.ChannelIndex[FrontRight]};

    /* First, copy out the existing direct stereo signal so it doesn't get
     * processed by the BS2B filter.
     */
    const auto leftout = std::span{RealOut.Buffer[lidx]}.first(SamplesToDo);
    const auto rightout = std::span{RealOut.Buffer[ridx]}.first(SamplesToDo);
    const auto ldirect = std::span{Bs2b->mStorage[0]}.first(SamplesToDo);
    const auto rdirect = std::span{Bs2b->mStorage[1]}.first(SamplesToDo);
    std::ranges::copy(leftout, ldirect.begin());
    std::ranges::copy(rightout, rdirect.begin());
    std::ranges::fill(leftout, 0.0f);
    std::ranges::fill(rightout, 0.0f);

    /* Now, decode the ambisonic mix to the "real" output, and apply the BS2B
     * binaural/crossfeed filter.
     */
    AmbiDecoder->process(RealOut.Buffer, Dry.Buffer, SamplesToDo);
    Bs2b->cross_feed(leftout, rightout);

    /* Finally, copy the direct signal back to the filtered output. */
    std::ranges::transform(leftout, ldirect, leftout.begin(), std::plus{});
    std::ranges::transform(rightout, rdirect, rightout.begin(), std::plus{});
}


namespace {

/* This RNG method was created based on the math found in opusdec. It's quick,
 * and starting with a seed value of 22222, is suitable for generating
 * whitenoise.
 */
inline uint dither_rng(uint *seed) noexcept
{
    *seed = (*seed * 96314165) + 907633515;
    return *seed;
}


/* Ambisonic upsampler function. It's effectively a matrix multiply. It takes
 * an 'upsampler' and 'rotator' as the input matrices, and creates a matrix
 * that behaves as if the B-Format input was first decoded to a speaker array
 * at its input order, encoded back into the higher order mix, then finally
 * rotated.
 */
void UpsampleBFormatTransform(
    const std::span<std::array<float,MaxAmbiChannels>,MaxAmbiChannels> output,
    const std::span<const std::array<float,MaxAmbiChannels>> upsampler,
    const std::span<const std::array<float,MaxAmbiChannels>,MaxAmbiChannels> rotator,
    size_t ambi_order)
{
    const auto num_chans = AmbiChannelsFromOrder(ambi_order);
    std::ranges::fill(output | std::views::take(upsampler.size()) | std::views::join, 0.0f);
    for(auto i = 0_uz;i < upsampler.size();++i)
    {
        for(auto k = 0_uz;k < num_chans;++k)
        {
            const auto a = upsampler[i][k];
            /* Write the full number of channels. The compiler will have an
             * easier time optimizing if it has a fixed length.
             */
            std::ranges::transform(rotator[k], output[i], output[i].begin(),
                [a](float rot, float dst) noexcept { return rot*a + dst; });
        }
    }
}


constexpr auto GetAmbiScales(AmbiScaling scaletype) noexcept
{
    switch(scaletype)
    {
    case AmbiScaling::FuMa: return std::span{AmbiScale::FromFuMa};
    case AmbiScaling::SN3D: return std::span{AmbiScale::FromSN3D};
    case AmbiScaling::UHJ: return std::span{AmbiScale::FromUHJ};
    case AmbiScaling::N3D: break;
    }
    return std::span{AmbiScale::FromN3D};
}

constexpr auto GetAmbiLayout(AmbiLayout layouttype) noexcept
{
    if(layouttype == AmbiLayout::FuMa) return std::span{AmbiIndex::FromFuMa};
    return std::span{AmbiIndex::FromACN};
}

constexpr auto GetAmbi2DLayout(AmbiLayout layouttype) noexcept
{
    if(layouttype == AmbiLayout::FuMa) return std::span{AmbiIndex::FromFuMa2D};
    return std::span{AmbiIndex::FromACN2D};
}


auto CalcContextParams(ContextBase *ctx) -> bool
{
    auto *props = ctx->mParams.ContextUpdate.exchange(nullptr, std::memory_order_acq_rel);
    if(!props) return false;

    const auto pos = alu::Vector{props->Position[0], props->Position[1], props->Position[2], 1.0f};
    ctx->mParams.Position = pos;

    /* AT then UP */
    auto N = alu::Vector{props->OrientAt[0], props->OrientAt[1], props->OrientAt[2], 0.0f};
    N.normalize();
    auto V = alu::Vector{props->OrientUp[0], props->OrientUp[1], props->OrientUp[2], 0.0f};
    V.normalize();
    /* Build and normalize right-vector */
    auto U = alu::Vector{N.cross_product(V)};
    U.normalize();

    const auto rot = alu::Matrix{
        U[0],  V[0], -N[0],  0.0f,
        U[1],  V[1], -N[1],  0.0f,
        U[2],  V[2], -N[2],  0.0f,
        0.0f,  0.0f,  0.0f,  1.0f};
    const auto vel = alu::Vector{props->Velocity[0], props->Velocity[1], props->Velocity[2], 0.0};

    ctx->mParams.Matrix = rot;
    ctx->mParams.Velocity = rot * vel;

    ctx->mParams.Gain = props->Gain * ctx->mGainBoost;
    ctx->mParams.MetersPerUnit = props->MetersPerUnit
#if ALSOFT_EAX
        * props->DistanceFactor
#endif
        ;
    ctx->mParams.AirAbsorptionGainHF = props->AirAbsorptionGainHF;

    ctx->mParams.DopplerFactor = props->DopplerFactor;
    ctx->mParams.SpeedOfSound = props->SpeedOfSound * props->DopplerVelocity
#if ALSOFT_EAX
        / props->DistanceFactor
#endif
        ;

    ctx->mParams.SourceDistanceModel = props->SourceDistanceModel;
    ctx->mParams.mDistanceModel = props->mDistanceModel;

    AtomicReplaceHead(ctx->mFreeContextProps, props);
    return true;
}

auto CalcEffectSlotParams(EffectSlot *slot, EffectSlot **sorted_slots, ContextBase *context) ->bool
{
    auto *props = slot->Update.exchange(nullptr, std::memory_order_acq_rel);
    if(!props) return false;

    /* If the effect slot target changed, clear the first sorted entry to force
     * a re-sort.
     */
    if(slot->Target != props->Target)
        *sorted_slots = nullptr;
    slot->Gain = props->Gain;
    slot->AuxSendAuto = props->AuxSendAuto;
    slot->Target = props->Target;
    slot->EffectType = props->Type;
    slot->mEffectProps = props->Props;

    slot->RoomRolloff = 0.0f;
    slot->DecayTime = 0.0f;
    slot->DecayLFRatio = 0.0f;
    slot->DecayHFRatio = 0.0f;
    slot->DecayHFLimit = false;
    slot->AirAbsorptionGainHF = 1.0f;
    if(auto *reverbprops = std::get_if<ReverbProps>(&props->Props))
    {
        slot->RoomRolloff = reverbprops->RoomRolloffFactor;
        slot->AirAbsorptionGainHF = reverbprops->AirAbsorptionGainHF;
        /* If this effect slot's Auxiliary Send Auto is off, don't apply the
         * automatic send adjustments based on source distance.
         */
        if(slot->AuxSendAuto)
        {
            slot->DecayTime = reverbprops->DecayTime;
            slot->DecayLFRatio = reverbprops->DecayLFRatio;
            slot->DecayHFRatio = reverbprops->DecayHFRatio;
            slot->DecayHFLimit = reverbprops->DecayHFLimit;
        }
    }

    auto *state = props->State.release();
    auto *oldstate = slot->mEffectState.release();
    slot->mEffectState.reset(state);

    /* Only release the old state if it won't get deleted, since we can't be
     * deleting/freeing anything in the mixer.
     */
    if(!oldstate->releaseIfNoDelete())
    {
        /* Otherwise, if it would be deleted send it off with a release event. */
        auto *ring = context->mAsyncEvents.get();
        auto evt_vec = ring->getWriteVector();
        if(!evt_vec[0].empty()) [[likely]]
        {
            auto &evt = InitAsyncEvent<AsyncEffectReleaseEvent>(evt_vec[0].front());
            evt.mEffectState = oldstate;
            ring->writeAdvance(1);
        }
        else
        {
            /* If writing the event failed, the queue was probably full. Store
             * the old state in the property object where it can eventually be
             * cleaned up sometime later (not ideal, but better than blocking
             * or leaking).
             */
            props->State.reset(oldstate);
        }
    }

    AtomicReplaceHead(context->mFreeEffectSlotProps, props);

    const auto output = std::invoke([slot,context]() -> EffectTarget
    {
        if(auto *target = slot->Target)
            return EffectTarget{&target->Wet, nullptr};
        auto *device = context->mDevice;
        return EffectTarget{&device->Dry, &device->RealOut};
    });
    state->update(context, slot, &slot->mEffectProps, output);
    return true;
}


/* Scales the azimuth of the given vector by 3 if it's in front. Effectively
 * scales +/-30 degrees to +/-90 degrees, leaving > +90 and < -90 alone.
 */
inline auto ScaleAzimuthFront3(std::array<float,3> pos) -> std::array<float,3>
{
    if(pos[2] < 0.0f)
    {
        /* Normalize the length of the x,z components for a 2D vector of the
         * azimuth angle. Negate Z since {0,0,-1} is angle 0.
         */
        const auto len2d = std::sqrt(pos[0]*pos[0] + pos[2]*pos[2]);
        auto x = pos[0] / len2d;
        auto z = -pos[2] / len2d;

        /* Z > cos(pi/6) = -30 < azimuth < 30 degrees. */
        if(z > 0.866025403785f)
        {
            /* Triple the angle represented by x,z. */
            x = x*3.0f - x*x*x*4.0f;
            z = z*z*z*4.0f - z*3.0f;

            /* Scale the vector back to fit in 3D. */
            pos[0] = x * len2d;
            pos[2] = -z * len2d;
        }
        else
        {
            /* If azimuth >= 30 degrees, clamp to 90 degrees. */
            pos[0] = std::copysign(len2d, pos[0]);
            pos[2] = 0.0f;
        }
    }
    return pos;
}

/* Scales the azimuth of the given vector by 1.5 (3/2) if it's in front. */
inline auto ScaleAzimuthFront3_2(std::array<float,3> pos) -> std::array<float,3>
{
    if(pos[2] < 0.0f)
    {
        const auto len2d = std::sqrt(pos[0]*pos[0] + pos[2]*pos[2]);
        float x = pos[0] / len2d;
        float z = -pos[2] / len2d;

        /* Z > cos(pi/3) = -60 < azimuth < 60 degrees. */
        if(z > 0.5f)
        {
            /* Halve the angle represented by x,z. */
            x = std::copysign(std::sqrt((1.0f - z) * 0.5f), x);
            z = std::sqrt((1.0f + z) * 0.5f);

            /* Triple the angle represented by x,z. */
            x = x*3.0f - x*x*x*4.0f;
            z = z*z*z*4.0f - z*3.0f;

            /* Scale the vector back to fit in 3D. */
            pos[0] = x * len2d;
            pos[2] = -z * len2d;
        }
        else
        {
            /* If azimuth >= 60 degrees, clamp to 90 degrees. */
            pos[0] = std::copysign(len2d, pos[0]);
            pos[2] = 0.0f;
        }
    }
    return pos;
}


/* Begin ambisonic rotation helpers.
 *
 * Rotating first-order B-Format just needs a straight-forward X/Y/Z rotation
 * matrix. Higher orders, however, are more complicated. The method implemented
 * here is a recursive algorithm (the rotation for first-order is used to help
 * generate the second-order rotation, which helps generate the third-order
 * rotation, etc).
 *
 * Adapted from
 * <https://github.com/polarch/Spherical-Harmonic-Transform/blob/master/getSHrotMtx.m>,
 * provided under the BSD 3-Clause license.
 *
 * Copyright (c) 2015, Archontis Politis
 * Copyright (c) 2019, Christopher Robinson
 *
 * The u, v, and w coefficients used for generating higher-order rotations are
 * precomputed since they're constant. The second-order coefficients are
 * followed by the third-order coefficients, etc.
 */
constexpr auto CalcRotatorSize(size_t l) noexcept -> size_t
{
    if(l >= 2)
        return (l*2 + 1)*(l*2 + 1) + CalcRotatorSize(l-1);
    return 0;
}

struct RotatorCoeffs {
    struct CoeffValues {
        float u, v, w;
    };
    std::array<CoeffValues,CalcRotatorSize(MaxAmbiOrder)> mCoeffs{};

    RotatorCoeffs()
    {
        auto coeffs = mCoeffs.begin();

        for(int l = 2;l <= MaxAmbiOrder;++l)
        {
            for(auto n = -l;n <= l;++n)
            {
                for(auto m = -l;m <= l;++m)
                {
                    /* compute u,v,w terms of Eq.8.1 (Table I)
                     *
                     * const bool d{m == 0}; // the delta function d_m0
                     * const double denom{(std::abs(n) == l) ?
                     *     (2*l) * (2*l - 1) : (l*l - n*n)};
                     *
                     * const int abs_m{std::abs(m)};
                     * coeffs->u = std::sqrt((l*l - m*m) / denom);
                     * coeffs->v = std::sqrt((l+abs_m-1) * (l+abs_m) / denom) *
                     *     (1.0+d) * (1.0 - 2.0*d) * 0.5;
                     * coeffs->w = std::sqrt((l-abs_m-1) * (l-abs_m) / denom) *
                     *     (1.0-d) * -0.5;
                     */

                    const auto denom = static_cast<double>((std::abs(n) == l) ?
                          (2*l) * (2*l - 1) : (l*l - n*n));

                    if(m == 0)
                    {
                        coeffs->u = static_cast<float>(std::sqrt(l * l / denom));
                        coeffs->v = static_cast<float>(std::sqrt((l-1) * l / denom) * -1.0);
                        coeffs->w = 0.0f;
                    }
                    else
                    {
                        const auto abs_m = std::abs(m);
                        coeffs->u = static_cast<float>(std::sqrt((l*l - m*m) / denom));
                        coeffs->v = static_cast<float>(std::sqrt((l+abs_m-1) * (l+abs_m) / denom) *
                            0.5);
                        coeffs->w = static_cast<float>(std::sqrt((l-abs_m-1) * (l-abs_m) / denom) *
                            -0.5);
                    }
                    ++coeffs;
                }
            }
        }
    }
};
const RotatorCoeffs RotatorCoeffArray{};

/**
 * Given the matrix, pre-filled with the (zeroth- and) first-order rotation
 * coefficients, this fills in the coefficients for the higher orders up to and
 * including the given order. The matrix is in ACN layout.
 */
void AmbiRotator(AmbiRotateMatrix &matrix, const int order)
{
    /* Don't do anything for < 2nd order. */
    if(order < 2) return;

    static constexpr auto P = [](const int i, const int l, const int a, const int n,
        const size_t last_band, const AmbiRotateMatrix &R)
    {
        const auto ri1 =  R[ 1+2][static_cast<size_t>(i+2_z)];
        const auto rim1 = R[-1+2][static_cast<size_t>(i+2_z)];
        const auto ri0 =  R[ 0+2][static_cast<size_t>(i+2_z)];

        const auto y = last_band + static_cast<size_t>(a+l-1);
        if(n == -l)
            return ri1*R[last_band][y] + rim1*R[last_band + static_cast<size_t>(l-1_z)*2][y];
        if(n == l)
            return ri1*R[last_band + static_cast<size_t>(l-1_z)*2][y] - rim1*R[last_band][y];
        return ri0*R[last_band + static_cast<size_t>(l-1_z+n)][y];
    };

    static constexpr auto U = [](const int l, const int m, const int n, const size_t last_band,
        const AmbiRotateMatrix &R)
    {
        return P(0, l, m, n, last_band, R);
    };
    static constexpr auto V = [](const int l, const int m, const int n, const size_t last_band,
        const AmbiRotateMatrix &R)
    {
        using namespace std::numbers;
        if(m > 0)
        {
            const auto d = (m == 1);
            const auto p0 = P( 1, l,  m-1, n, last_band, R);
            const auto p1 = P(-1, l, -m+1, n, last_band, R);
            return d ? p0*sqrt2_v<float> : (p0 - p1);
        }
        const auto d = (m == -1);
        const auto p0 = P( 1, l,  m+1, n, last_band, R);
        const auto p1 = P(-1, l, -m-1, n, last_band, R);
        return d ? p1*sqrt2_v<float> : (p0 + p1);
    };
    static constexpr auto W = [](const int l, const int m, const int n, const size_t last_band,
        const AmbiRotateMatrix &R)
    {
        assert(m != 0);
        if(m > 0)
        {
            const auto p0 = P( 1, l,  m+1, n, last_band, R);
            const auto p1 = P(-1, l, -m-1, n, last_band, R);
            return p0 + p1;
        }
        const auto p0 = P( 1, l,  m-1, n, last_band, R);
        const auto p1 = P(-1, l, -m+1, n, last_band, R);
        return p0 - p1;
    };

    // compute rotation matrix of each subsequent band recursively
    auto coeffs = RotatorCoeffArray.mCoeffs.cbegin();
    auto band_idx = 4_uz;
    auto last_band = 1_uz;
    for(auto l = 2;l <= order;++l)
    {
        auto y = band_idx;
        for(auto n = -l;n <= l;++n,++y)
        {
            auto x = band_idx;
            for(auto m = -l;m <= l;++m,++x)
            {
                auto r = 0.0f;

                // computes Eq.8.1
                if(const float u{coeffs->u}; u != 0.0f)
                    r += u * U(l, m, n, last_band, matrix);
                if(const float v{coeffs->v}; v != 0.0f)
                    r += v * V(l, m, n, last_band, matrix);
                if(const float w{coeffs->w}; w != 0.0f)
                    r += w * W(l, m, n, last_band, matrix);

                matrix[y][x] = r;
                ++coeffs;
            }
        }
        last_band = band_idx;
        band_idx += static_cast<uint>(l)*2_uz + 1;
    }
}
/* End ambisonic rotation helpers. */


constexpr auto sin30 = 0.5f;
constexpr auto cos30 = 0.866025403785f;
constexpr auto sin45 = std::numbers::sqrt2_v<float>*0.5f;
constexpr auto cos45 = std::numbers::sqrt2_v<float>*0.5f;
constexpr auto sin110 =  0.939692620786f;
constexpr auto cos110 = -0.342020143326f;

struct ChanPosMap {
    Channel channel;
    std::array<float,3> pos;
};


struct GainTriplet { float Base, HF, LF; };

void CalcPanningAndFilters(Voice *voice, const float xpos, const float ypos, const float zpos,
    const float Distance, const float Spread, const GainTriplet &DryGain,
    const std::span<const GainTriplet,MaxSendCount> WetGain,
    const std::span<EffectSlot*,MaxSendCount> SendSlots, const ContextParams &Context,
    DeviceBase *Device)
{
    static constexpr std::array MonoMap{
        ChanPosMap{FrontCenter, std::array{0.0f, 0.0f, -1.0f}}
    };
    static constexpr std::array RearMap{
        ChanPosMap{BackLeft,  std::array{-sin30, 0.0f, cos30}},
        ChanPosMap{BackRight, std::array{ sin30, 0.0f, cos30}},
    };
    static constexpr std::array QuadMap{
        ChanPosMap{FrontLeft,  std::array{-sin45, 0.0f, -cos45}},
        ChanPosMap{FrontRight, std::array{ sin45, 0.0f, -cos45}},
        ChanPosMap{BackLeft,   std::array{-sin45, 0.0f,  cos45}},
        ChanPosMap{BackRight,  std::array{ sin45, 0.0f,  cos45}},
    };
    static constexpr std::array X51Map{
        ChanPosMap{FrontLeft,   std::array{-sin30, 0.0f, -cos30}},
        ChanPosMap{FrontRight,  std::array{ sin30, 0.0f, -cos30}},
        ChanPosMap{FrontCenter, std::array{  0.0f, 0.0f, -1.0f}},
        ChanPosMap{LFE, {}},
        ChanPosMap{SideLeft,    std::array{-sin110, 0.0f, -cos110}},
        ChanPosMap{SideRight,   std::array{ sin110, 0.0f, -cos110}},
    };
    static constexpr std::array X61Map{
        ChanPosMap{FrontLeft,   std::array{-sin30, 0.0f, -cos30}},
        ChanPosMap{FrontRight,  std::array{ sin30, 0.0f, -cos30}},
        ChanPosMap{FrontCenter, std::array{  0.0f, 0.0f, -1.0f}},
        ChanPosMap{LFE, {}},
        ChanPosMap{BackCenter,  std::array{ 0.0f, 0.0f, 1.0f}},
        ChanPosMap{SideLeft,    std::array{-1.0f, 0.0f, 0.0f}},
        ChanPosMap{SideRight,   std::array{ 1.0f, 0.0f, 0.0f}},
    };
    static constexpr std::array X71Map{
        ChanPosMap{FrontLeft,   std::array{-sin30, 0.0f, -cos30}},
        ChanPosMap{FrontRight,  std::array{ sin30, 0.0f, -cos30}},
        ChanPosMap{FrontCenter, std::array{  0.0f, 0.0f, -1.0f}},
        ChanPosMap{LFE, {}},
        ChanPosMap{BackLeft,    std::array{-sin30, 0.0f, cos30}},
        ChanPosMap{BackRight,   std::array{ sin30, 0.0f, cos30}},
        ChanPosMap{SideLeft,    std::array{ -1.0f, 0.0f, 0.0f}},
        ChanPosMap{SideRight,   std::array{  1.0f, 0.0f, 0.0f}},
    };

    std::array StereoMap{
        ChanPosMap{FrontLeft,   std::array{-sin30, 0.0f, -cos30}},
        ChanPosMap{FrontRight,  std::array{ sin30, 0.0f, -cos30}},
    };

    const auto Frequency = static_cast<float>(Device->mSampleRate);
    const uint NumSends{Device->NumAuxSends};

    const auto &props = voice->mProps;
    const auto ismono3d = voice->mFmtChannels == FmtMono && !props.mPanningEnabled;

    std::ranges::for_each(voice->mChans, [NumSends](Voice::ChannelData &chandata)
    {
        chandata.mDryParams.Hrtf.Target = HrtfFilter{};
        chandata.mDryParams.Gains.Target.fill(0.0f);
        std::ranges::for_each(chandata.mWetParams | std::views::take(NumSends),
            [](SendParams &params) -> void { params.Gains.Target.fill(0.0f); });
    });

    const auto [DirectChannels, chans] = std::invoke([&props,&StereoMap](FmtChannels chanfmt)
        -> std::pair<DirectMode,std::span<const ChanPosMap>>
    {
        switch(chanfmt)
        {
        case FmtMono:
            if(!props.mPanningEnabled)
            {
                /* 3D mono buffers are never played direct. */
                return {DirectMode::Off, std::span{MonoMap}};
            }
            /* Mono buffers with panning enabled are basically treated as
             * stereo, each channel being a copy of the buffer samples, using
             * the stereo channel positions and the left/right panning
             * affecting each channel appropriately.
             */
            [[fallthrough]];
        case FmtStereo:
            if(props.DirectChannels == DirectMode::Off)
            {
                auto chanpos = StereoMap | std::views::transform(&ChanPosMap::pos);
                std::ranges::transform(props.StereoPan, chanpos, chanpos.begin(),
                    [](const float a, std::span<float,3> pos)
                {
                    /* StereoPan is counter-clockwise in radians. */
                    return std::array{-std::sin(a), pos[1], -std::cos(a)};
                });
            }
            return {props.DirectChannels, std::span{StereoMap}};

        case FmtRear: return {props.DirectChannels, std::span{RearMap}};
        case FmtQuad: return {props.DirectChannels, std::span{QuadMap}};
        case FmtX51: return {props.DirectChannels, std::span{X51Map}};
        case FmtX61: return {props.DirectChannels, std::span{X61Map}};
        case FmtX71: return {props.DirectChannels, std::span{X71Map}};

        case FmtBFormat2D:
        case FmtBFormat3D:
        case FmtUHJ2:
        case FmtUHJ3:
        case FmtUHJ4:
        case FmtSuperStereo:
            return {DirectMode::Off, {}};
        }
        return {props.DirectChannels, {}};
    }, voice->mFmtChannels);

    voice->mFlags.reset(VoiceHasHrtf).reset(VoiceHasNfc);
    if(auto *decoder = voice->mDecoder.get())
        decoder->mWidthControl = std::min(props.EnhWidth, 0.7f);

    const auto lgain = std::min(1.0f - props.Panning, 1.0f);
    const auto rgain = std::min(1.0f + props.Panning, 1.0f);
    const auto mingain = std::min(lgain, rgain);
    auto SelectChannelGain = [lgain,rgain,mingain](const Channel chan) noexcept -> float
    {
        switch(chan)
        {
        case FrontLeft: return lgain;
        case FrontRight: return rgain;
        case FrontCenter: break;
        case LFE: break;
        case BackLeft: return lgain;
        case BackRight: return rgain;
        case BackCenter: break;
        case SideLeft: return lgain;
        case SideRight: return rgain;
        case TopCenter: break;
        case TopFrontLeft: return lgain;
        case TopFrontCenter: break;
        case TopFrontRight: return rgain;
        case TopBackLeft: return lgain;
        case TopBackCenter: break;
        case TopBackRight: return rgain;
        case BottomFrontLeft: return lgain;
        case BottomFrontRight: return rgain;
        case BottomBackLeft: return lgain;
        case BottomBackRight: return rgain;
        case Aux0: case Aux1: case Aux2: case Aux3: case Aux4: case Aux5: case Aux6: case Aux7:
        case Aux8: case Aux9: case Aux10: case Aux11: case Aux12: case Aux13: case Aux14:
        case Aux15: case MaxChannels: break;
        }
        return mingain;
    };

    if(IsAmbisonic(voice->mFmtChannels))
    {
        /* Special handling for B-Format and UHJ sources. */

        if(Device->AvgSpeakerDist > 0.0f && voice->mFmtChannels != FmtUHJ2
            && voice->mFmtChannels != FmtSuperStereo)
        {
            if(!(Distance > std::numeric_limits<float>::epsilon()))
            {
                /* NOTE: The NFCtrlFilters were created with a w0 of 0, which
                 * is what we want for FOA input. The first channel may have
                 * been previously re-adjusted if panned, so reset it.
                 */
                voice->mChans[0].mDryParams.NFCtrlFilter.adjust(0.0f);
            }
            else
            {
                /* Clamp the distance for really close sources, to prevent
                 * excessive bass.
                 */
                const float mdist{std::max(Distance*NfcScale, Device->AvgSpeakerDist/4.0f)};
                const float w0{SpeedOfSoundMetersPerSec / (mdist * Frequency)};

                /* Only need to adjust the first channel of a B-Format source. */
                voice->mChans[0].mDryParams.NFCtrlFilter.adjust(w0);
            }

            voice->mFlags.set(VoiceHasNfc);
        }

        /* Panning a B-Format sound toward some direction is easy. Just pan the
         * first (W) channel as a normal mono sound. The angular spread is used
         * as a directional scalar to blend between full coverage and full
         * panning.
         */
        const float coverage{!(Distance > std::numeric_limits<float>::epsilon()) ? 1.0f :
            (std::numbers::inv_pi_v<float>*0.5f * Spread)};

        const auto scales = GetAmbiScales(voice->mAmbiScaling);
        auto coeffs = std::invoke([xpos,ypos,zpos](RenderMode mode)
        {
            if(mode != RenderMode::Pairwise)
                return CalcDirectionCoeffs(std::array{xpos, ypos, zpos}, 0.0f);
            const auto pos = ScaleAzimuthFront3_2(std::array{xpos, ypos, zpos});
            return CalcDirectionCoeffs(pos, 0.0f);
        }, Device->mRenderMode);

        if(!(coverage > 0.0f))
        {
            ComputePanGains(&Device->Dry, coeffs, DryGain.Base*scales[0],
                std::span{voice->mChans[0].mDryParams.Gains.Target}.first<MaxAmbiChannels>());
            for(uint i{0};i < NumSends;i++)
            {
                if(const EffectSlot *Slot{SendSlots[i]})
                    ComputePanGains(&Slot->Wet, coeffs, WetGain[i].Base*scales[0],
                        voice->mChans[0].mWetParams[i].Gains.Target);
            }
        }
        else
        {
            /* Local B-Format sources have their XYZ channels rotated according
             * to the orientation.
             */
            /* AT then UP */
            auto N = alu::Vector{props.OrientAt[0], props.OrientAt[1], props.OrientAt[2], 0.0f};
            N.normalize();
            auto V = alu::Vector{props.OrientUp[0], props.OrientUp[1], props.OrientUp[2], 0.0f};
            V.normalize();
            if(!props.HeadRelative)
            {
                N = Context.Matrix * N;
                V = Context.Matrix * V;
            }
            /* Build and normalize right-vector */
            auto U = alu::Vector{N.cross_product(V)};
            U.normalize();

            /* Build a rotation matrix. Manually fill the zeroth- and first-
             * order elements, then construct the rotation for the higher
             * orders.
             */
            auto &shrot = Device->mAmbiRotateMatrix;
            shrot.fill(AmbiRotateMatrix::value_type{});

            shrot[0][0] = 1.0f;
            shrot[1][1] =  U[0]; shrot[1][2] = -U[1]; shrot[1][3] =  U[2];
            shrot[2][1] = -V[0]; shrot[2][2] =  V[1]; shrot[2][3] = -V[2];
            shrot[3][1] = -N[0]; shrot[3][2] =  N[1]; shrot[3][3] = -N[2];
            AmbiRotator(shrot, static_cast<int>(Device->mAmbiOrder));

            /* If the device is higher order than the voice, "upsample" the
             * matrix.
             *
             * NOTE: Starting with second-order, a 2D upsample needs to be
             * applied with a 2D source and 3D output, even when they're the
             * same order. This is because higher orders have a height offset
             * on various channels (i.e. when elevation=0, those height-related
             * channels should be non-0).
             */
            auto &mixmatrix = Device->mAmbiRotateMatrix2;
            if(Device->mAmbiOrder > voice->mAmbiOrder
                || (Device->mAmbiOrder >= 2 && !Device->m2DMixing
                    && Is2DAmbisonic(voice->mFmtChannels)))
            {
                if(voice->mAmbiOrder == 1)
                {
                    const auto upsampler = Is2DAmbisonic(voice->mFmtChannels) ?
                        std::span{AmbiScale::FirstOrder2DUp} : std::span{AmbiScale::FirstOrderUp};
                    UpsampleBFormatTransform(mixmatrix, upsampler, shrot, Device->mAmbiOrder);
                }
                else if(voice->mAmbiOrder == 2)
                {
                    const auto upsampler = Is2DAmbisonic(voice->mFmtChannels)
                        ? std::span{AmbiScale::SecondOrder2DUp}
                        : std::span{AmbiScale::SecondOrderUp};
                    UpsampleBFormatTransform(mixmatrix, upsampler, shrot, Device->mAmbiOrder);
                }
                else if(voice->mAmbiOrder == 3)
                {
                    const auto upsampler = Is2DAmbisonic(voice->mFmtChannels) ?
                        std::span{AmbiScale::ThirdOrder2DUp} : std::span{AmbiScale::ThirdOrderUp};
                    UpsampleBFormatTransform(mixmatrix, upsampler, shrot, Device->mAmbiOrder);
                }
                else if(voice->mAmbiOrder == 4)
                {
                    const auto upsampler = std::span{AmbiScale::FourthOrder2DUp};
                    UpsampleBFormatTransform(mixmatrix, upsampler, shrot, Device->mAmbiOrder);
                }
            }
            else
                mixmatrix = shrot;

            /* Convert the rotation matrix for input ordering and scaling, and
             * whether input is 2D or 3D.
             */
            const auto index_map = Is2DAmbisonic(voice->mFmtChannels) ?
                GetAmbi2DLayout(voice->mAmbiLayout).first(voice->mChans.size()) :
                GetAmbiLayout(voice->mAmbiLayout).first(voice->mChans.size());

            /* Scale the panned W signal inversely to coverage (full coverage
             * means no panned signal), and according to the channel scaling.
             */
            std::ranges::for_each(coeffs, [scale=(1.0f-coverage)*scales[0]](float &coeff) noexcept
            { coeff *= scale; });

            for(size_t c{0};c < index_map.size();++c)
            {
                const auto acn = size_t{index_map[c]};
                const auto scale = scales[acn] * coverage;

                /* For channel 0, combine the B-Format signal (scaled according
                 * to the coverage amount) with the directional pan. For all
                 * other channels, use just the (scaled) B-Format signal.
                 */
                std::ranges::transform(mixmatrix[acn], coeffs, coeffs.begin(),
                    [scale](const float in, const float coeff) noexcept
                { return in*scale + coeff; });

                ComputePanGains(&Device->Dry, coeffs, DryGain.Base,
                    std::span{voice->mChans[c].mDryParams.Gains.Target}.first<MaxAmbiChannels>());

                for(uint i{0};i < NumSends;i++)
                {
                    if(const EffectSlot *Slot{SendSlots[i]})
                        ComputePanGains(&Slot->Wet, coeffs, WetGain[i].Base,
                            voice->mChans[c].mWetParams[i].Gains.Target);
                }

                coeffs.fill(0.0f);
            }
        }
    }
    else if(DirectChannels != DirectMode::Off && !Device->RealOut.RemixMap.empty())
    {
        /* Direct source channels always play local. Skip the virtual channels
         * and write inputs to the matching real outputs.
         */
        voice->mDirect.Buffer = Device->RealOut.Buffer;

        for(size_t c{0};c < chans.size();++c)
        {
            const auto pangain = SelectChannelGain(chans[c].channel);
            if(auto idx = Device->channelIdxByName(chans[c].channel); idx != InvalidChannelIndex)
                voice->mChans[c].mDryParams.Gains.Target[idx] = DryGain.Base * pangain;
            else if(DirectChannels == DirectMode::RemixMismatch)
            {
                const auto remap = std::ranges::find(Device->RealOut.RemixMap, chans[c].channel,
                    &InputRemixMap::channel);
                if(remap != Device->RealOut.RemixMap.end())
                {
                    for(const auto &target : remap->targets)
                    {
                        idx = Device->channelIdxByName(target.channel);
                        if(idx != InvalidChannelIndex)
                            voice->mChans[c].mDryParams.Gains.Target[idx] = DryGain.Base * pangain
                                * target.mix;
                    }
                }
            }
        }

        /* Auxiliary sends still use normal channel panning since they mix to
         * B-Format, which can't channel-match.
         */
        for(size_t c{0};c < chans.size();++c)
        {
            /* Skip LFE */
            if(chans[c].channel == LFE)
                continue;

            const auto pangain = SelectChannelGain(chans[c].channel);
            const auto coeffs = CalcDirectionCoeffs(chans[c].pos, 0.0f);

            for(uint i{0};i < NumSends;i++)
            {
                if(const auto *Slot = SendSlots[i])
                    ComputePanGains(&Slot->Wet, coeffs, WetGain[i].Base * pangain,
                        voice->mChans[c].mWetParams[i].Gains.Target);
            }
        }

        /* With non-HRTF mixing, we can cheat for mono-as-stereo by combining
         * the left and right output gains and mix only one channel to output.
         */
        if(voice->mFmtChannels == FmtMono && props.mPanningEnabled)
        {
            const auto drytarget0 = std::span{voice->mChans[0].mDryParams.Gains.Target};
            const auto drytarget1 = std::span{voice->mChans[1].mDryParams.Gains.Target};
            std::ranges::transform(drytarget0, drytarget1, drytarget0.begin(), std::plus{});

            for(uint i{0};i < NumSends;i++)
            {
                if(!SendSlots[i])
                    continue;

                const auto wettarget0 = std::span{voice->mChans[0].mWetParams[i].Gains.Target};
                const auto wettarget1 = std::span{voice->mChans[1].mWetParams[i].Gains.Target};
                std::ranges::transform(wettarget0, wettarget1, wettarget0.begin(), std::plus{});
            }
        }
    }
    else if(Device->mRenderMode == RenderMode::Hrtf)
    {
        /* Full HRTF rendering. Skip the virtual channels and render to the
         * real outputs.
         */
        voice->mDirect.Buffer = Device->RealOut.Buffer;

        if(Distance > std::numeric_limits<float>::epsilon())
        {
            if(ismono3d)
            {
                const float src_ev{std::asin(std::clamp(ypos, -1.0f, 1.0f))};
                const float src_az{std::atan2(xpos, -zpos)};

                Device->mHrtf->getCoeffs(src_ev, src_az, Distance*NfcScale, Spread,
                    voice->mChans[0].mDryParams.Hrtf.Target.Coeffs,
                    voice->mChans[0].mDryParams.Hrtf.Target.Delay);
                voice->mChans[0].mDryParams.Hrtf.Target.Gain = DryGain.Base;

                const auto coeffs = CalcDirectionCoeffs(std::array{xpos, ypos, zpos}, Spread);
                for(uint i{0};i < NumSends;i++)
                {
                    if(const EffectSlot *Slot{SendSlots[i]})
                        ComputePanGains(&Slot->Wet, coeffs, WetGain[i].Base,
                            voice->mChans[0].mWetParams[i].Gains.Target);
                }
            }
            else for(size_t c{0};c < chans.size();++c)
            {
                /* Skip LFE */
                if(chans[c].channel == LFE) continue;
                const float pangain{SelectChannelGain(chans[c].channel)};

                /* Warp the channel position toward the source position as the
                 * source spread decreases. With no spread, all channels are at
                 * the source position, at full spread (pi*2), each channel is
                 * left unchanged.
                 */
                const auto a = 1.0f - (std::numbers::inv_pi_v<float>*0.5f)*Spread;
                std::array pos{
                    lerpf(chans[c].pos[0], xpos, a),
                    lerpf(chans[c].pos[1], ypos, a),
                    lerpf(chans[c].pos[2], zpos, a)};
                const float len{std::sqrt(pos[0]*pos[0] + pos[1]*pos[1] + pos[2]*pos[2])};
                if(len < 1.0f)
                {
                    pos[0] /= len;
                    pos[1] /= len;
                    pos[2] /= len;
                }

                const float ev{std::asin(std::clamp(pos[1], -1.0f, 1.0f))};
                const float az{std::atan2(pos[0], -pos[2])};

                Device->mHrtf->getCoeffs(ev, az, Distance*NfcScale, 0.0f,
                    voice->mChans[c].mDryParams.Hrtf.Target.Coeffs,
                    voice->mChans[c].mDryParams.Hrtf.Target.Delay);
                voice->mChans[c].mDryParams.Hrtf.Target.Gain = DryGain.Base * pangain;

                const auto coeffs = CalcDirectionCoeffs(pos, 0.0f);
                for(uint i{0};i < NumSends;i++)
                {
                    if(const EffectSlot *Slot{SendSlots[i]})
                        ComputePanGains(&Slot->Wet, coeffs, WetGain[i].Base * pangain,
                            voice->mChans[c].mWetParams[i].Gains.Target);
                }
            }
        }
        else
        {
            /* With no distance, spread is only meaningful for mono sources
             * where it can be 0 or full (non-mono sources are always full
             * spread here).
             */
            const auto spread = Spread * float(ismono3d);

            /* Local sources on HRTF play with each channel panned to its
             * relative location around the listener, providing "virtual
             * speaker" responses.
             */
            for(size_t c{0};c < chans.size();++c)
            {
                /* Skip LFE */
                if(chans[c].channel == LFE)
                    continue;
                const float pangain{SelectChannelGain(chans[c].channel)};

                /* Get the HRIR coefficients and delays for this channel
                 * position.
                 */
                const float ev{std::asin(chans[c].pos[1])};
                const float az{std::atan2(chans[c].pos[0], -chans[c].pos[2])};

                Device->mHrtf->getCoeffs(ev, az, std::numeric_limits<float>::infinity(), spread,
                    voice->mChans[c].mDryParams.Hrtf.Target.Coeffs,
                    voice->mChans[c].mDryParams.Hrtf.Target.Delay);
                voice->mChans[c].mDryParams.Hrtf.Target.Gain = DryGain.Base * pangain;

                /* Normal panning for auxiliary sends. */
                const auto coeffs = CalcDirectionCoeffs(chans[c].pos, spread);

                for(uint i{0};i < NumSends;i++)
                {
                    if(const EffectSlot *Slot{SendSlots[i]})
                        ComputePanGains(&Slot->Wet, coeffs, WetGain[i].Base * pangain,
                            voice->mChans[c].mWetParams[i].Gains.Target);
                }
            }
        }

        voice->mDuplicateMono = voice->mFmtChannels == FmtMono && props.mPanningEnabled;
        voice->mFlags.set(VoiceHasHrtf);
    }
    else
    {
        /* Non-HRTF rendering. Use normal panning to the output. */

        if(Distance > std::numeric_limits<float>::epsilon())
        {
            /* Calculate NFC filter coefficient if needed. */
            if(Device->AvgSpeakerDist > 0.0f)
            {
                /* Clamp the distance for really close sources, to prevent
                 * excessive bass.
                 */
                const float mdist{std::max(Distance*NfcScale, Device->AvgSpeakerDist/4.0f)};
                const float w0{SpeedOfSoundMetersPerSec / (mdist * Frequency)};

                /* Adjust NFC filters. */
                for(size_t c{0};c < chans.size();++c)
                    voice->mChans[c].mDryParams.NFCtrlFilter.adjust(w0);

                voice->mFlags.set(VoiceHasNfc);
            }

            if(ismono3d)
            {
                auto calc_coeffs = [xpos,ypos,zpos,Spread](RenderMode mode)
                {
                    if(mode != RenderMode::Pairwise)
                        return CalcDirectionCoeffs(std::array{xpos, ypos, zpos}, Spread);
                    const auto pos = ScaleAzimuthFront3_2(std::array{xpos, ypos, zpos});
                    return CalcDirectionCoeffs(pos, Spread);
                };
                const auto coeffs = calc_coeffs(Device->mRenderMode);

                ComputePanGains(&Device->Dry, coeffs, DryGain.Base,
                    std::span{voice->mChans[0].mDryParams.Gains.Target}.first<MaxAmbiChannels>());
                for(uint i{0};i < NumSends;i++)
                {
                    if(const EffectSlot *Slot{SendSlots[i]})
                        ComputePanGains(&Slot->Wet, coeffs, WetGain[i].Base,
                            voice->mChans[0].mWetParams[i].Gains.Target);
                }
            }
            else for(size_t c{0};c < chans.size();++c)
            {
                const auto pangain = SelectChannelGain(chans[c].channel);

                /* Special-case LFE */
                if(chans[c].channel == LFE)
                {
                    if(Device->Dry.Buffer.data() == Device->RealOut.Buffer.data())
                    {
                        const auto idx = uint{Device->channelIdxByName(chans[c].channel)};
                        if(idx != InvalidChannelIndex)
                            voice->mChans[c].mDryParams.Gains.Target[idx] = DryGain.Base * pangain;
                    }
                    continue;
                }

                /* Warp the channel position toward the source position as the
                 * spread decreases. With no spread, all channels are at the
                 * source position, at full spread (pi*2), each channel
                 * position is left unchanged.
                 */
                const auto a = 1.0f - (std::numbers::inv_pi_v<float>*0.5f)*Spread;
                auto pos = std::array{
                    lerpf(chans[c].pos[0], xpos, a),
                    lerpf(chans[c].pos[1], ypos, a),
                    lerpf(chans[c].pos[2], zpos, a)};
                const auto len = std::sqrt(pos[0]*pos[0] + pos[1]*pos[1] + pos[2]*pos[2]);
                if(len < 1.0f)
                {
                    pos[0] /= len;
                    pos[1] /= len;
                    pos[2] /= len;
                }

                if(Device->mRenderMode == RenderMode::Pairwise)
                    pos = ScaleAzimuthFront3(pos);
                const auto coeffs = CalcDirectionCoeffs(pos, 0.0f);

                ComputePanGains(&Device->Dry, coeffs, DryGain.Base * pangain,
                    std::span{voice->mChans[c].mDryParams.Gains.Target}.first<MaxAmbiChannels>());
                for(uint i{0};i < NumSends;i++)
                {
                    if(const EffectSlot *Slot{SendSlots[i]})
                        ComputePanGains(&Slot->Wet, coeffs, WetGain[i].Base * pangain,
                            voice->mChans[c].mWetParams[i].Gains.Target);
                }
            }
        }
        else
        {
            if(Device->AvgSpeakerDist > 0.0f)
            {
                /* If the source distance is 0, simulate a plane-wave by using
                 * infinite distance, which results in a w0 of 0.
                 */
                static constexpr float w0{0.0f};
                for(size_t c{0};c < chans.size();++c)
                    voice->mChans[c].mDryParams.NFCtrlFilter.adjust(w0);

                voice->mFlags.set(VoiceHasNfc);
            }

            /* With no distance, spread is only meaningful for 3D mono sources
             * where it can be 0 or full (non-mono sources are always full
             * spread here).
             */
            const auto spread = Spread * float(ismono3d);
            for(size_t c{0};c < chans.size();++c)
            {
                const float pangain{SelectChannelGain(chans[c].channel)};

                /* Special-case LFE */
                if(chans[c].channel == LFE)
                {
                    if(Device->Dry.Buffer.data() == Device->RealOut.Buffer.data())
                    {
                        const uint idx{Device->channelIdxByName(chans[c].channel)};
                        if(idx != InvalidChannelIndex)
                            voice->mChans[c].mDryParams.Gains.Target[idx] = DryGain.Base * pangain;
                    }
                    continue;
                }

                const auto coeffs = CalcDirectionCoeffs((Device->mRenderMode==RenderMode::Pairwise)
                    ? ScaleAzimuthFront3(chans[c].pos) : chans[c].pos, spread);

                ComputePanGains(&Device->Dry, coeffs, DryGain.Base * pangain,
                    std::span{voice->mChans[c].mDryParams.Gains.Target}.first<MaxAmbiChannels>());
                for(uint i{0};i < NumSends;i++)
                {
                    if(const EffectSlot *Slot{SendSlots[i]})
                        ComputePanGains(&Slot->Wet, coeffs, WetGain[i].Base * pangain,
                            voice->mChans[c].mWetParams[i].Gains.Target);
                }
            }
        }

        if(voice->mFmtChannels == FmtMono && props.mPanningEnabled)
        {
            const auto drytarget0 = std::span{voice->mChans[0].mDryParams.Gains.Target};
            const auto drytarget1 = std::span{voice->mChans[1].mDryParams.Gains.Target};
            std::ranges::transform(drytarget0, drytarget1, drytarget0.begin(), std::plus{});

            for(uint i{0};i < NumSends;i++)
            {
                if(!SendSlots[i])
                    continue;

                const auto wettarget0 = std::span{voice->mChans[0].mWetParams[i].Gains.Target};
                const auto wettarget1 = std::span{voice->mChans[1].mWetParams[i].Gains.Target};
                std::ranges::transform(wettarget0, wettarget1, wettarget0.begin(), std::plus{});
            }
        }
    }

    {
        const auto hfNorm = props.Direct.HFReference / Frequency;
        const auto lfNorm = props.Direct.LFReference / Frequency;

        voice->mDirect.FilterType = AF_None;
        if(DryGain.HF != 1.0f) voice->mDirect.FilterType |= AF_LowPass;
        if(DryGain.LF != 1.0f) voice->mDirect.FilterType |= AF_HighPass;

        auto &lowpass = voice->mChans[0].mDryParams.LowPass;
        auto &highpass = voice->mChans[0].mDryParams.HighPass;
        lowpass.setParamsFromSlope(BiquadType::HighShelf, hfNorm, DryGain.HF, 1.0f);
        highpass.setParamsFromSlope(BiquadType::LowShelf, lfNorm, DryGain.LF, 1.0f);
        for(size_t c{1};c < chans.size();++c)
        {
            voice->mChans[c].mDryParams.LowPass.copyParamsFrom(lowpass);
            voice->mChans[c].mDryParams.HighPass.copyParamsFrom(highpass);
        }
    }
    for(uint i{0};i < NumSends;i++)
    {
        const auto hfNorm = props.Send[i].HFReference / Frequency;
        const auto lfNorm = props.Send[i].LFReference / Frequency;

        voice->mSend[i].FilterType = AF_None;
        if(WetGain[i].HF != 1.0f) voice->mSend[i].FilterType |= AF_LowPass;
        if(WetGain[i].LF != 1.0f) voice->mSend[i].FilterType |= AF_HighPass;

        auto &lowpass = voice->mChans[0].mWetParams[i].LowPass;
        auto &highpass = voice->mChans[0].mWetParams[i].HighPass;
        lowpass.setParamsFromSlope(BiquadType::HighShelf, hfNorm, WetGain[i].HF, 1.0f);
        highpass.setParamsFromSlope(BiquadType::LowShelf, lfNorm, WetGain[i].LF, 1.0f);
        for(size_t c{1};c < chans.size();++c)
        {
            voice->mChans[c].mWetParams[i].LowPass.copyParamsFrom(lowpass);
            voice->mChans[c].mWetParams[i].HighPass.copyParamsFrom(highpass);
        }
    }
}

void CalcNonAttnSourceParams(Voice *voice, const ContextBase *context)
{
    const auto &props = voice->mProps;
    auto *Device = context->mDevice;
    auto SendSlots = std::array<EffectSlot*,MaxSendCount>{};

    voice->mDirect.Buffer = Device->Dry.Buffer;
    for(uint i{0};i < Device->NumAuxSends;i++)
    {
        SendSlots[i] = props.Send[i].Slot;
        if(!SendSlots[i] || SendSlots[i]->EffectType == EffectSlotType::None)
        {
            SendSlots[i] = nullptr;
            voice->mSend[i].Buffer = {};
        }
        else
            voice->mSend[i].Buffer = SendSlots[i]->Wet.Buffer;
    }

    /* Calculate the stepping value */
    const auto Pitch = static_cast<float>(voice->mFrequency) /
        static_cast<float>(Device->mSampleRate) * props.Pitch;
    if(Pitch > float{MaxPitch})
        voice->mStep = MaxPitch<<MixerFracBits;
    else
        voice->mStep = std::max(fastf2u(Pitch * MixerFracOne), 1u);
    voice->mResampler = PrepareResampler(props.mResampler, voice->mStep, &voice->mResampleState);

    /* Calculate gains */
    const auto MinGain = std::min(props.MinGain, props.MaxGain);
    const auto srcgain = std::clamp(props.Gain, MinGain, props.MaxGain);
    const auto DryGain = GainTriplet{
        .Base = std::min(GainMixMax, srcgain * props.Direct.Gain * context->mParams.Gain),
        .HF = props.Direct.GainHF,
        .LF = props.Direct.GainLF
    };

    auto WetGain = std::array<GainTriplet,MaxSendCount>{};
    std::ranges::transform(props.Send | std::views::take(Device->NumAuxSends), WetGain.begin(),
        [context,srcgain](const VoiceProps::SendData &send) noexcept
    {
        return GainTriplet {
            .Base = std::min(GainMixMax, srcgain * send.Gain * context->mParams.Gain),
            .HF = send.GainHF,
            .LF = send.GainLF
        };
    });

    CalcPanningAndFilters(voice, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, DryGain, WetGain, SendSlots,
        context->mParams, Device);
}

void CalcAttnSourceParams(Voice *voice, const ContextBase *context)
{
    const auto &props = voice->mProps;
    auto *Device = context->mDevice;
    const auto NumSends = Device->NumAuxSends;

    /* Set mixing buffers and get send parameters. */
    voice->mDirect.Buffer = Device->Dry.Buffer;

    auto SendSlots = std::array<EffectSlot*,MaxSendCount>{};
    auto RoomRolloff = std::array<float,MaxSendCount>{};
    for(uint i{0};i < NumSends;i++)
    {
        SendSlots[i] = props.Send[i].Slot;
        if(!SendSlots[i] || SendSlots[i]->EffectType == EffectSlotType::None)
        {
            SendSlots[i] = nullptr;
            voice->mSend[i].Buffer = {};
        }
        else
        {
            /* NOTE: Contrary to the EFX docs, the effect's room rolloff factor
             * applies to the selected distance model along with the source's
             * room rolloff factor, not necessarily the inverse distance model.
             */
            RoomRolloff[i] = props.RoomRolloffFactor + SendSlots[i]->RoomRolloff;

            voice->mSend[i].Buffer = SendSlots[i]->Wet.Buffer;
        }
    }

    /* Transform source to listener space (convert to head relative) */
    auto Position = alu::Vector{props.Position[0], props.Position[1], props.Position[2], 1.0f};
    auto Velocity = alu::Vector{props.Velocity[0], props.Velocity[1], props.Velocity[2], 0.0f};
    auto Direction = alu::Vector{props.Direction[0], props.Direction[1], props.Direction[2], 0.0f};
    if(!props.HeadRelative)
    {
        /* Transform source vectors */
        Position = context->mParams.Matrix * (Position - context->mParams.Position);
        Velocity = context->mParams.Matrix * Velocity;
        Direction = context->mParams.Matrix * Direction;
    }
    else
    {
        /* Offset the source velocity to be relative of the listener velocity */
        Velocity += context->mParams.Velocity;
    }

    auto ToSource = alu::Vector{Position[0], Position[1], Position[2], 0.0f};
    const auto Distance = ToSource.normalize();
    const auto directional = bool{Direction.normalize() > 0.0f};

    /* Calculate distance attenuation */
    const auto DistanceModel = context->mParams.SourceDistanceModel ? props.mDistanceModel
        : context->mParams.mDistanceModel;

    const auto AttenDistance = std::invoke([Distance,DistanceModel,&props]
    {
        switch(DistanceModel)
        {
        case DistanceModel::InverseClamped:
        case DistanceModel::LinearClamped:
        case DistanceModel::ExponentClamped:
            if(!(props.RefDistance <= props.MaxDistance))
                return props.RefDistance;
            return std::clamp(Distance, props.RefDistance, props.MaxDistance);

        case DistanceModel::Inverse:
        case DistanceModel::Linear:
        case DistanceModel::Exponent:
        case DistanceModel::Disable:
            break;
        }
        return Distance;
    });


    auto DryGain = GainTriplet{ .Base = props.Gain, .HF = 1.0f, .LF = 1.0f };
    auto WetGain = std::array<GainTriplet,MaxSendCount>{};
    WetGain.fill(DryGain);

    auto DryAttnBase = 1.0f;
    switch(DistanceModel)
    {
    case DistanceModel::Inverse:
    case DistanceModel::InverseClamped:
        if(props.RefDistance > 0.0f)
        {
            if(auto dist = lerpf(props.RefDistance, AttenDistance, props.RolloffFactor); dist>0.0f)
            {
                DryAttnBase = props.RefDistance / dist;
                DryGain.Base *= DryAttnBase;
            }

            auto wetgains = WetGain | std::views::transform(&GainTriplet::Base);
            std::ranges::transform(wetgains | std::views::take(NumSends), RoomRolloff,
                wetgains.begin(), [&props,AttenDistance](const float gain, const float rolloff)
            {
                auto dist = lerpf(props.RefDistance, AttenDistance, rolloff);
                if(dist > 0.0f) return gain * (props.RefDistance / dist);
                return gain;
            });
        }
        break;

    case DistanceModel::Linear:
    case DistanceModel::LinearClamped:
        if(props.MaxDistance != props.RefDistance)
        {
            auto scale = (AttenDistance-props.RefDistance) / (props.MaxDistance-props.RefDistance);
            DryAttnBase = std::max(1.0f - scale*props.RolloffFactor, 0.0f);
            DryGain.Base *= DryAttnBase;

            auto wetgains = WetGain | std::views::transform(&GainTriplet::Base);
            std::ranges::transform(wetgains | std::views::take(NumSends), RoomRolloff,
                wetgains.begin(), [scale](const float gain, const float rolloff)
            { return gain * std::max(1.0f - scale*rolloff, 0.0f); });
        }
        break;

    case DistanceModel::Exponent:
    case DistanceModel::ExponentClamped:
        if(AttenDistance > 0.0f && props.RefDistance > 0.0f)
        {
            const auto dist_ratio = AttenDistance / props.RefDistance;
            DryAttnBase = std::pow(dist_ratio, -props.RolloffFactor);
            DryGain.Base *= DryAttnBase;
            auto wetgains = WetGain | std::views::transform(&GainTriplet::Base);
            std::ranges::transform(wetgains | std::views::take(NumSends), RoomRolloff,
                wetgains.begin(), [dist_ratio](const float gain, const float rolloff)
            { return gain * std::pow(dist_ratio, -rolloff); });
        }
        break;

    case DistanceModel::Disable:
        break;
    }

    /* Calculate directional soundcones */
    auto WetCone = 1.0f;
    auto WetConeHF = 1.0f;
    if(directional && props.InnerAngle < 360.0f)
    {
        static constexpr auto Rad2Deg = static_cast<float>(180.0 / std::numbers::pi);
        const auto Angle = Rad2Deg*2.0f * std::acos(-Direction.dot_product(ToSource)) * ConeScale;

        auto ConeGain = 1.0f;
        auto ConeHF = 1.0f;
        if(Angle >= props.OuterAngle)
        {
            ConeGain = props.OuterGain;
            ConeHF = props.OuterGainHF;
        }
        else if(Angle >= props.InnerAngle)
        {
            const auto scale = (Angle-props.InnerAngle) / (props.OuterAngle-props.InnerAngle);
            ConeGain = lerpf(1.0f, props.OuterGain, scale);
            ConeHF = lerpf(1.0f, props.OuterGainHF, scale);
        }

        DryGain.Base *= ConeGain;
        if(props.DryGainHFAuto)
            DryGain.HF *= ConeHF;
        if(props.WetGainAuto)
            WetCone = ConeGain;
        if(props.WetGainHFAuto)
            WetConeHF = ConeHF;
    }

    /* Apply gain and frequency filters */
    const auto MinGain = std::min(props.MinGain, props.MaxGain);
    const auto MaxGain = props.MaxGain;

    DryGain.Base = std::clamp(DryGain.Base, MinGain, MaxGain) * props.Direct.Gain;
    DryGain.Base = std::min(GainMixMax, DryGain.Base * context->mParams.Gain);
    DryGain.HF = DryGain.HF * props.Direct.GainHF;
    DryGain.LF = props.Direct.GainLF;

    std::ranges::transform(props.Send | std::views::take(NumSends), WetGain, WetGain.begin(),
        [context,WetCone,WetConeHF,MinGain,MaxGain](const VoiceProps::SendData &send,
            const GainTriplet &wetgain)
    {
        const auto gain = std::clamp(wetgain.Base*WetCone, MinGain, MaxGain) * send.Gain;
        return GainTriplet{
            .Base = std::min(GainMixMax, gain * context->mParams.Gain),
            .HF = send.GainHF * WetConeHF,
            .LF = send.GainLF
        };
    });

    /* Distance-based air absorption and initial send decay. */
    if(Distance > props.RefDistance) [[likely]]
    {
        /* FIXME: In keeping with EAX, the base air absorption gain should be
         * taken from the reverb property in the "primary fx slot" when it has
         * a reverb effect and the environment flag set, and be applied to the
         * direct path and all environment sends, rather than each path using
         * the air absorption gain associated with the given slot's effect. At
         * this point in the mixer, and even in EFX itself, there's no concept
         * of a "primary fx slot" so it's unclear which effect slot should be
         * checked.
         *
         * The HF reference is also intended to be handled the same way, but
         * again, there's no concept of a "primary fx slot" here and no way to
         * know which effect slot to look at for the reference frequency.
         */
        const auto distance_units = (Distance-props.RefDistance) * props.RolloffFactor;
        const auto distance_meters = distance_units * context->mParams.MetersPerUnit;
        const auto absorb = distance_meters * props.AirAbsorptionFactor;
        if(absorb > std::numeric_limits<float>::epsilon())
            DryGain.HF *= std::pow(context->mParams.AirAbsorptionGainHF, absorb);

        /* If the source's Auxiliary Send Filter Gain Auto is off, no extra
         * adjustment is applied to the send gains.
         */
        for(uint i{props.WetGainAuto ? 0u : NumSends};i < NumSends;++i)
        {
            if(!SendSlots[i] || !(SendSlots[i]->DecayTime > 0.0f))
                continue;

            if(SendSlots[i]->AirAbsorptionGainHF < 1.0f
                && absorb > std::numeric_limits<float>::epsilon())
                WetGain[i].HF *= std::pow(SendSlots[i]->AirAbsorptionGainHF, absorb);

            const float DecayDistance{SendSlots[i]->DecayTime * SpeedOfSoundMetersPerSec};

            /* Apply a decay-time transformation to the wet path, based on the
             * source distance. The initial decay of the reverb effect is
             * calculated and applied to the wet path.
             *
             * FIXME: This is very likely not correct. It more likely should
             * work by calculating a rolloff dynamically based on the reverb
             * parameters (and source distance?) and add it to the room rolloff
             * with the reverb and source rolloff parameters.
             */
            const float baseAttn{DryAttnBase};
            const float fact{distance_meters / DecayDistance};
            const float gain{std::pow(ReverbDecayGain, fact)*(1.0f-baseAttn) + baseAttn};
            WetGain[i].Base *= gain;
        }
    }


    /* Initial source pitch */
    auto Pitch = props.Pitch;

    /* Calculate velocity-based doppler effect */
    if(const auto DopplerFactor = props.DopplerFactor * context->mParams.DopplerFactor;
        DopplerFactor > 0.0f)
    {
        const auto &lvelocity = context->mParams.Velocity;
        const auto vss = Velocity.dot_product(ToSource) * -DopplerFactor;
        const auto vls = lvelocity.dot_product(ToSource) * -DopplerFactor;

        const auto SpeedOfSound = context->mParams.SpeedOfSound;
        if(!(vls < SpeedOfSound))
        {
            /* Listener moving away from the source at the speed of sound.
             * Sound waves can't catch it.
             */
            Pitch = 0.0f;
        }
        else if(!(vss < SpeedOfSound))
        {
            /* Source moving toward the listener at the speed of sound. Sound
             * waves bunch up to extreme frequencies.
             */
            Pitch = std::numeric_limits<float>::infinity();
        }
        else
        {
            /* Source and listener movement is nominal. Calculate the proper
             * doppler shift.
             */
            Pitch *= (SpeedOfSound-vls) / (SpeedOfSound-vss);
        }
    }

    /* Adjust pitch based on the buffer and output frequencies, and calculate
     * fixed-point stepping value.
     */
    Pitch *= static_cast<float>(voice->mFrequency) / static_cast<float>(Device->mSampleRate);
    if(Pitch > float{MaxPitch})
        voice->mStep = MaxPitch<<MixerFracBits;
    else
        voice->mStep = std::max(fastf2u(Pitch * MixerFracOne), 1u);
    voice->mResampler = PrepareResampler(props.mResampler, voice->mStep, &voice->mResampleState);

    auto spread = 0.0f;
    if(props.Radius > Distance)
        spread = std::numbers::pi_v<float>*2.0f - Distance/props.Radius*std::numbers::pi_v<float>;
    else if(Distance > 0.0f)
        spread = std::asin(props.Radius/Distance) * 2.0f;

    CalcPanningAndFilters(voice, ToSource[0]*XScale, ToSource[1]*YScale, ToSource[2]*ZScale,
        Distance, spread, DryGain, WetGain, SendSlots, context->mParams, Device);
}

void CalcSourceParams(Voice *voice, ContextBase *context, bool force)
{
    if(auto *props = voice->mUpdate.exchange(nullptr, std::memory_order_acq_rel))
    {
        voice->mProps = static_cast<VoiceProps&>(*props);
        AtomicReplaceHead(context->mFreeVoiceProps, props);
    }
    else if(!force)
        return;

    const auto &props = voice->mProps;
    const auto ismono3d = voice->mFmtChannels == FmtMono && !voice->mProps.mPanningEnabled;
    if((props.DirectChannels != DirectMode::Off && !ismono3d && !IsAmbisonic(voice->mFmtChannels))
        || props.mSpatializeMode == SpatializeMode::Off
        || (props.mSpatializeMode == SpatializeMode::Auto && !ismono3d))
        CalcNonAttnSourceParams(voice, context);
    else
        CalcAttnSourceParams(voice, context);
}


void SendSourceStateEvent(ContextBase *context, uint id, VChangeState state)
{
    auto *ring = context->mAsyncEvents.get();
    auto evt_vec = ring->getWriteVector();
    if(evt_vec[0].empty()) return;

    auto &evt = InitAsyncEvent<AsyncSourceStateEvent>(evt_vec[0].front());
    evt.mId = id;
    switch(state)
    {
    case VChangeState::Reset: evt.mState = AsyncSrcState::Reset; break;
    case VChangeState::Stop: evt.mState = AsyncSrcState::Stop; break;
    case VChangeState::Play: evt.mState = AsyncSrcState::Play; break;
    case VChangeState::Pause: evt.mState = AsyncSrcState::Pause; break;
    /* Shouldn't happen. */
    case VChangeState::Restart:
        break;
    }

    ring->writeAdvance(1);
}

void ProcessVoiceChanges(ContextBase *ctx)
{
    auto *cur = ctx->mCurrentVoiceChange.load(std::memory_order_acquire);
    auto *next = cur->mNext.load(std::memory_order_acquire);
    if(!next) return;

    const auto enabledevt = ctx->mEnabledEvts.load(std::memory_order_acquire);
    while(next)
    {
        cur = next;

        auto sendevt = false;
        if(cur->mState == VChangeState::Reset || cur->mState == VChangeState::Stop)
        {
            if(auto *voice = cur->mVoice)
            {
                voice->mCurrentBuffer.store(nullptr, std::memory_order_relaxed);
                voice->mLoopBuffer.store(nullptr, std::memory_order_relaxed);
                /* A source ID indicates the voice was playing or paused, which
                 * gets a reset/stop event.
                 */
                sendevt = voice->mSourceID.exchange(0u, std::memory_order_relaxed) != 0u;
                auto oldvstate = Voice::Playing;
                voice->mPlayState.compare_exchange_strong(oldvstate, Voice::Stopping,
                    std::memory_order_relaxed, std::memory_order_acquire);
                voice->mPendingChange.store(false, std::memory_order_release);
            }
            /* Reset state change events are always sent, even if the voice is
             * already stopped or even if there is no voice.
             */
            sendevt |= (cur->mState == VChangeState::Reset);
        }
        else if(cur->mState == VChangeState::Pause)
        {
            auto *voice = cur->mVoice;
            auto oldvstate = Voice::Playing;
            sendevt = voice->mPlayState.compare_exchange_strong(oldvstate, Voice::Stopping,
                std::memory_order_release, std::memory_order_acquire);
        }
        else if(cur->mState == VChangeState::Play)
        {
            /* NOTE: When playing a voice, sending a source state change event
             * depends if there's an old voice to stop and if that stop is
             * successful. If there is no old voice, a playing event is always
             * sent. If there is an old voice, an event is sent only if the
             * voice is already stopped.
             */
            if(auto *oldvoice = cur->mOldVoice)
            {
                oldvoice->mCurrentBuffer.store(nullptr, std::memory_order_relaxed);
                oldvoice->mLoopBuffer.store(nullptr, std::memory_order_relaxed);
                oldvoice->mSourceID.store(0u, std::memory_order_relaxed);
                auto oldvstate = Voice::Playing;
                sendevt = !oldvoice->mPlayState.compare_exchange_strong(oldvstate, Voice::Stopping,
                    std::memory_order_relaxed, std::memory_order_acquire);
                oldvoice->mPendingChange.store(false, std::memory_order_release);
            }
            else
                sendevt = true;

            auto *voice = cur->mVoice;
            voice->mPlayState.store(Voice::Playing, std::memory_order_release);
        }
        else if(cur->mState == VChangeState::Restart)
        {
            /* Restarting a voice never sends a source change event. */
            auto *oldvoice = cur->mOldVoice;
            oldvoice->mCurrentBuffer.store(nullptr, std::memory_order_relaxed);
            oldvoice->mLoopBuffer.store(nullptr, std::memory_order_relaxed);
            /* If there's no sourceID, the old voice finished so don't start
             * the new one at its new offset.
             */
            if(oldvoice->mSourceID.exchange(0u, std::memory_order_relaxed) != 0u)
            {
                /* Otherwise, set the voice to stopping if it's not already (it
                 * might already be, if paused), and play the new voice as
                 * appropriate.
                 */
                auto oldvstate = Voice::Playing;
                oldvoice->mPlayState.compare_exchange_strong(oldvstate, Voice::Stopping,
                    std::memory_order_relaxed, std::memory_order_acquire);

                auto *voice = cur->mVoice;
                voice->mPlayState.store((oldvstate == Voice::Playing) ? Voice::Playing
                    : Voice::Stopped, std::memory_order_release);
            }
            oldvoice->mPendingChange.store(false, std::memory_order_release);
        }
        if(sendevt && enabledevt.test(al::to_underlying(AsyncEnableBits::SourceState)))
            SendSourceStateEvent(ctx, cur->mSourceID, cur->mState);

        next = cur->mNext.load(std::memory_order_acquire);
    }
    ctx->mCurrentVoiceChange.store(cur, std::memory_order_release);
}

void ProcessParamUpdates(ContextBase *ctx, const std::span<EffectSlot*> slots,
    const std::span<EffectSlot*> sorted_slots, const std::span<Voice*> voices)
{
    ProcessVoiceChanges(ctx);

    IncrementRef(ctx->mUpdateCount);
    if(!ctx->mHoldUpdates.load(std::memory_order_acquire)) [[likely]]
    {
        bool force{CalcContextParams(ctx)};
        auto sorted_slot_base = std::to_address(sorted_slots.begin());
        for(auto *slot : slots)
            force |= CalcEffectSlotParams(slot, sorted_slot_base, ctx);

        for(auto *voice : voices)
        {
            /* Only update voices that have a source. */
            if(voice->mSourceID.load(std::memory_order_relaxed) != 0)
                CalcSourceParams(voice, ctx, force);
        }
    }
    IncrementRef(ctx->mUpdateCount);
}

void ProcessContexts(DeviceBase *device, const uint SamplesToDo)
{
    ASSUME(SamplesToDo > 0);

    const auto curtime = device->getClockTime();

    const auto contexts = std::span{*device->mContexts.load(std::memory_order_acquire)};
    std::ranges::for_each(contexts, [SamplesToDo,curtime](ContextBase *ctx)
    {
        const auto auxslotspan = std::span{*ctx->mActiveAuxSlots.load(std::memory_order_acquire)};
        const auto auxslots = auxslotspan.first(auxslotspan.size()>>1);
        const auto sorted_slots = auxslotspan.last(auxslotspan.size()>>1);
        const auto voices = ctx->getVoicesSpanAcquired();

        /* Process pending property updates for objects on the context. */
        ProcessParamUpdates(ctx, auxslots, sorted_slots, voices);

        /* Clear auxiliary effect slot mixing buffers. */
        std::ranges::fill(auxslots | std::views::transform(&EffectSlot::Wet)
            | std::views::transform(&MixParams::Buffer) | std::views::join | std::views::join,
            0.0f);

        /* Process voices that have a playing source. */
        std::ranges::for_each(voices, [ctx,curtime,SamplesToDo](Voice *voice)
        {
            const auto vstate = voice->mPlayState.load(std::memory_order_acquire);
            if(vstate != Voice::Stopped && vstate != Voice::Pending)
                voice->mix(vstate, ctx, curtime, SamplesToDo);
        });

        /* Process effects. */
        if(!auxslots.empty())
        {
            /* Sort the slots into extra storage, so that effect slots come
             * before their effect slot target (or their targets' target). Skip
             * sorting if it has already been done.
             */
            if(!sorted_slots[0])
            {
                /* First, copy the slots to the sorted list and partition them,
                 * so that all slots without a target slot go to the end.
                 */
                static constexpr auto has_target = [](const EffectSlot *slot) noexcept -> bool
                { return slot->Target != nullptr; };
                auto split_point = std::partition_copy(auxslots.rbegin(), auxslots.rend(),
                    sorted_slots.begin(), sorted_slots.rbegin(), has_target).first;
                /* There must be at least one slot without a slot target. */
                assert(split_point != sorted_slots.end());

                /* Starting from the back of the sorted list, continue
                 * partitioning the front of the list given each target until
                 * all targets are accounted for. This ensures all slots
                 * without a target go last, all slots directly targeting those
                 * last slots go second-to-last, all slots directly targeting
                 * those second-last slots go third-to-last, etc.
                 */
                auto next_target = sorted_slots.end();
                while(std::distance(sorted_slots.begin(), split_point) > 1)
                {
                    /* This shouldn't happen, but if there's unsorted slots
                     * left that don't target any sorted slots, they can't
                     * contribute to the output, so leave them.
                     */
                    if(next_target == split_point) [[unlikely]]
                        break;

                    --next_target;
                    auto not_next = [next_target](const EffectSlot *slot) noexcept -> bool
                    { return slot->Target != *next_target; };
                    split_point = std::partition(sorted_slots.begin(), split_point, not_next);
                }
            }

            std::ranges::for_each(sorted_slots, [SamplesToDo](const EffectSlot *slot)
            {
                auto *state = slot->mEffectState.get();
                state->process(SamplesToDo, slot->Wet.Buffer, state->mOutTarget);
            });
        }

        /* Signal the event handler if there are any events to read. */
        if(auto *ring = ctx->mAsyncEvents.get(); ring->readSpace() > 0)
        {
            ctx->mEventsPending.store(true, std::memory_order_release);
            ctx->mEventsPending.notify_all();
        }
    });
}


void ApplyDistanceComp(const std::span<FloatBufferLine> Samples, const size_t SamplesToDo,
    const std::span<const DistanceComp::ChanData,MaxOutputChannels> chandata)
{
    ASSUME(SamplesToDo > 0);

    std::ignore = std::ranges::mismatch(chandata, Samples,
        [SamplesToDo](const DistanceComp::ChanData &distcomp, FloatBufferSpan chanbuffer)
    {
        const auto gain = distcomp.Gain;
        const auto distbuf = distcomp.Buffer;

        const auto base = distbuf.size();
        if(base < 1) return true;

        const auto inout = chanbuffer.first(SamplesToDo);
        if(SamplesToDo >= base) [[likely]]
        {
            const auto inout_start = std::prev(inout.end(), ptrdiff_t(base));
            const auto delay_end = std::ranges::rotate(inout, inout_start).begin();
            std::ranges::swap_ranges(std::span{inout.begin(), delay_end}, distbuf);
        }
        else
        {
            const auto delay_start = std::ranges::swap_ranges(inout, distbuf).in2;
            std::ranges::rotate(distbuf, delay_start);
        }
        std::ranges::transform(inout, inout.begin(), [gain](const float s) noexcept -> float
        { return s*gain; });

        return true;
    });
}

void ApplyDither(const std::span<FloatBufferLine> Samples, uint *dither_seed,
    const float quant_scale, const size_t SamplesToDo)
{
    static constexpr auto invRNGRange = 1.0 / std::numeric_limits<uint>::max();
    ASSUME(SamplesToDo > 0);

    /* Dithering. Generate whitenoise (uniform distribution of random values
     * between -1 and +1) and add it to the sample values, after scaling up to
     * the desired quantization depth and before rounding.
     */
    const auto invscale = 1.0f / quant_scale;
    auto seed = *dither_seed;
    auto dither_sample = [&seed,invscale,quant_scale](const float sample) noexcept -> float
    {
        auto val = sample * quant_scale;
        const auto rng0 = dither_rng(&seed);
        const auto rng1 = dither_rng(&seed);
        val += static_cast<float>(rng0*invRNGRange - rng1*invRNGRange);
        return fast_roundf(val) * invscale;
    };
    for(const FloatBufferSpan inout : Samples)
        std::ranges::transform(inout.first(SamplesToDo), inout.begin(), dither_sample);
    *dither_seed = seed;
}


/* Base template left undefined. Should be marked =delete, but Clang 3.8.1
 * chokes on that given the inline specializations.
 */
template<typename T>
inline T SampleConv(float) noexcept;

template<> inline float SampleConv(float val) noexcept
{ return val; }
template<> inline int32_t SampleConv(float val) noexcept
{
    /* Floats have a 23-bit mantissa, plus an implied 1 bit and a sign bit.
     * This means a normalized float has at most 25 bits of signed precision.
     * When scaling and clamping for a signed 32-bit integer, these following
     * values are the best a float can give.
     */
    return fastf2i(std::clamp(val*2147483648.0f, -2147483648.0f, 2147483520.0f));
}
template<> inline int16_t SampleConv(float val) noexcept
{ return static_cast<int16_t>(fastf2i(std::clamp(val*32768.0f, -32768.0f, 32767.0f))); }
template<> inline int8_t SampleConv(float val) noexcept
{ return static_cast<int8_t>(fastf2i(std::clamp(val*128.0f, -128.0f, 127.0f))); }

/* Define unsigned output variations. */
template<> inline uint32_t SampleConv(float val) noexcept
{ return static_cast<uint32_t>(SampleConv<int32_t>(val)) + 2147483648u; }
template<> inline uint16_t SampleConv(float val) noexcept
{ return static_cast<uint16_t>(SampleConv<int16_t>(val) + 32768); }
template<> inline uint8_t SampleConv(float val) noexcept
{ return static_cast<uint8_t>(SampleConv<int8_t>(val) + 128); }

template<typename T>
void Write(const std::span<const FloatBufferLine> InBuffer, void *OutBuffer, const size_t Offset,
    const size_t SamplesToDo, const size_t FrameStep)
{
    ASSUME(FrameStep > 0);
    ASSUME(SamplesToDo > 0);

    const auto output = std::span{static_cast<T*>(OutBuffer), (Offset+SamplesToDo)*FrameStep}
        .subspan(Offset*FrameStep);

    /* If there's extra channels in the interleaved output buffer to skip,
     * clear the whole output buffer. This is simpler to ensure the extra
     * channels are silent than trying to clear just the extra channels.
     */
    if(FrameStep > InBuffer.size())
        std::ranges::fill(output, SampleConv<T>(0.0f));

    auto outbase = output.begin();
    for(const auto &srcbuf : InBuffer)
    {
        auto out = outbase++;
        *out = SampleConv<T>(srcbuf.front());
        std::ranges::for_each(srcbuf | std::views::take(SamplesToDo) | std::views::drop(1),
            [FrameStep,&out](const float s) noexcept
        {
            std::advance(out, FrameStep);
            *out = SampleConv<T>(s);
        });
    }
}

template<typename T>
void Write(const std::span<const FloatBufferLine> InBuffer, std::span<void*> OutBuffers,
    const size_t Offset, const size_t SamplesToDo)
{
    ASSUME(SamplesToDo > 0);

    std::ignore = std::ranges::mismatch(OutBuffers, InBuffer,
        [Offset,SamplesToDo](void *dstbuf, const FloatConstBufferSpan srcbuf)
    {
        const auto dst = std::span{static_cast<T*>(dstbuf), Offset+SamplesToDo}.subspan(Offset);
        std::ranges::transform(srcbuf | std::views::take(SamplesToDo), dst.begin(), SampleConv<T>);
        return true;
    });
}

} // namespace

auto DeviceBase::renderSamples(const uint numSamples) -> uint
{
    const auto samplesToDo = std::min(numSamples, uint{BufferLineSize});

    /* Clear main mixing buffers. */
    std::ranges::fill(MixBuffer | std::views::join, 0.0f);

    {
        const auto mixLock = getWriteMixLock();

        /* Process and mix each context's sources and effects. */
        ProcessContexts(this, samplesToDo);

        /* Every second's worth of samples is converted and added to clock base
         * so that large sample counts don't overflow during conversion. This
         * also guarantees a stable conversion.
         */
        auto samplesDone = mSamplesDone.load(std::memory_order_relaxed) + samplesToDo;
        auto clockBaseSec = mClockBaseSec.load(std::memory_order_relaxed) +
            seconds32{samplesDone/mSampleRate};
        mSamplesDone.store(samplesDone%mSampleRate, std::memory_order_relaxed);
        mClockBaseSec.store(clockBaseSec, std::memory_order_relaxed);
    }

    /* Apply any needed post-process for finalizing the Dry mix to the RealOut
     * (Ambisonic decode, UHJ encode, etc).
     */
    postProcess(samplesToDo);

    /* Apply compression, limiting sample amplitude if needed or desired. */
    if(Limiter) Limiter->process(samplesToDo, RealOut.Buffer);

    /* Apply delays and attenuation for mismatched speaker distances. */
    if(ChannelDelays)
        ApplyDistanceComp(RealOut.Buffer, samplesToDo, ChannelDelays->mChannels);

    /* Apply dithering. The compressor should have left enough headroom for the
     * dither noise to not saturate.
     */
    if(DitherDepth > 0.0f)
        ApplyDither(RealOut.Buffer, &DitherSeed, DitherDepth, samplesToDo);

    return samplesToDo;
}

void DeviceBase::renderSamples(const std::span<void*> outBuffers, const uint numSamples)
{
    auto mixer_mode = FPUCtl{};
    auto total = 0u;
    while(const auto todo = numSamples - total)
    {
        const auto samplesToDo = renderSamples(todo);

        switch(FmtType)
        {
#define HANDLE_WRITE(T) case T:                                               \
    Write<DevFmtType_t<T>>(RealOut.Buffer, outBuffers, total, samplesToDo); break;
        HANDLE_WRITE(DevFmtByte)
        HANDLE_WRITE(DevFmtUByte)
        HANDLE_WRITE(DevFmtShort)
        HANDLE_WRITE(DevFmtUShort)
        HANDLE_WRITE(DevFmtInt)
        HANDLE_WRITE(DevFmtUInt)
        HANDLE_WRITE(DevFmtFloat)
        }
#undef HANDLE_WRITE

        total += samplesToDo;
    }
}

void DeviceBase::renderSamples(void *outBuffer, const uint numSamples, const size_t frameStep)
{
    auto mixer_mode = FPUCtl{};
    auto total = 0u;
    while(const auto todo = numSamples - total)
    {
        const auto samplesToDo = renderSamples(todo);

        if(outBuffer) [[likely]]
        {
            /* Finally, interleave and convert samples, writing to the device's
             * output buffer.
             */
            switch(FmtType)
            {
#define HANDLE_WRITE(T) case T:                                               \
    Write<DevFmtType_t<T>>(RealOut.Buffer, outBuffer, total, samplesToDo, frameStep); break;
            HANDLE_WRITE(DevFmtByte)
            HANDLE_WRITE(DevFmtUByte)
            HANDLE_WRITE(DevFmtShort)
            HANDLE_WRITE(DevFmtUShort)
            HANDLE_WRITE(DevFmtInt)
            HANDLE_WRITE(DevFmtUInt)
            HANDLE_WRITE(DevFmtFloat)
#undef HANDLE_WRITE
            }
        }

        total += samplesToDo;
    }
}

void DeviceBase::doDisconnect(std::string msg)
{
    const auto mixLock = getWriteMixLock();

    if(Connected.exchange(false, std::memory_order_acq_rel))
    {
        auto evt = std::array{AsyncEvent{std::in_place_type<AsyncDisconnectEvent>}};
        auto &disconnect = std::get<AsyncDisconnectEvent>(evt.front());
        disconnect.msg = std::move(msg);

        for(auto *ctx : *mContexts.load())
        {
            auto *ring = ctx->mAsyncEvents.get();
            if(ring->write(evt) > 0)
            {
                ctx->mEventsPending.store(true, std::memory_order_release);
                ctx->mEventsPending.notify_all();
            }

            if(!ctx->mStopVoicesOnDisconnect.load())
            {
                ProcessVoiceChanges(ctx);
                continue;
            }

            std::ranges::for_each(ctx->getVoicesSpanAcquired(), [](Voice *voice) -> void
            {
                voice->mCurrentBuffer.store(nullptr, std::memory_order_relaxed);
                voice->mLoopBuffer.store(nullptr, std::memory_order_relaxed);
                voice->mSourceID.store(0u, std::memory_order_relaxed);
                voice->mPlayState.store(Voice::Stopped, std::memory_order_release);
            });
        }
    }
}
