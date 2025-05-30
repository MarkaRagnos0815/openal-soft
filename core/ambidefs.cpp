
#include "config.h"

#include "ambidefs.h"

#include <algorithm>
#include <functional>
#include <numbers>
#include <span>


namespace {

static_assert(AmbiScale::FromN3D.size() == MaxAmbiChannels);
static_assert(AmbiScale::FromSN3D.size() == MaxAmbiChannels);
static_assert(AmbiScale::FromFuMa.size() == MaxAmbiChannels);
static_assert(AmbiScale::FromUHJ.size() == MaxAmbiChannels);

static_assert(AmbiIndex::FromFuMa.size() == MaxAmbiChannels);
static_assert(AmbiIndex::FromFuMa2D.size() == MaxAmbi2DChannels);


using AmbiChannelFloatArray = std::array<float,MaxAmbiChannels>;

constexpr auto inv_sqrt3f = static_cast<float>(1.0/std::numbers::sqrt3);


/* These HF gains are derived from the same 32-point speaker array. The scale
 * factor between orders represents the same scale factors for any (regular)
 * speaker array decoder. e.g. Given a first-order source and second-order
 * output, applying an HF scale of HFScales[1][0] / HFScales[2][0] to channel 0
 * will result in that channel being subsequently decoded for second-order as
 * if it was a first-order decoder for that same speaker array.
 */
constexpr std::array HFScales{
    std::array{4.000000000e+00f, 2.309401077e+00f, 1.192569588e+00f, 7.189495850e-01f, 4.784482742e-01f},
    std::array{4.000000000e+00f, 2.309401077e+00f, 1.192569588e+00f, 7.189495850e-01f, 4.784482742e-01f},
    std::array{2.981423970e+00f, 2.309401077e+00f, 1.192569588e+00f, 7.189495850e-01f, 4.784482742e-01f},
    std::array{2.359168820e+00f, 2.031565936e+00f, 1.444598386e+00f, 7.189495850e-01f, 4.784482742e-01f},
    std::array{1.947005434e+00f, 1.764337084e+00f, 1.424707344e+00f, 9.755104127e-01f, 4.784482742e-01f},
};

/* Same as above, but using a 10-point horizontal-only speaker array. Should
 * only be used when the device is mixing in 2D B-Format for horizontal-only
 * output.
 */
constexpr std::array HFScales2D{
    std::array{2.236067977e+00f, 1.581138830e+00f, 9.128709292e-01f, 6.050756345e-01f, 4.370160244e-01f},
    std::array{2.236067977e+00f, 1.581138830e+00f, 9.128709292e-01f, 6.050756345e-01f, 4.370160244e-01f},
    std::array{1.825741858e+00f, 1.581138830e+00f, 9.128709292e-01f, 6.050756345e-01f, 4.370160244e-01f},
    std::array{1.581138830e+00f, 1.460781803e+00f, 1.118033989e+00f, 6.050756345e-01f, 4.370160244e-01f},
    std::array{1.414213562e+00f, 1.344997024e+00f, 1.144122806e+00f, 8.312538756e-01f, 4.370160244e-01f},
};


/* This calculates a first-order "upsampler" matrix. It combines a first-order
 * decoder matrix with a max-order encoder matrix, creating a matrix that
 * behaves as if the B-Format input signal is first decoded to a speaker array
 * at first-order, then those speaker feeds are encoded to a higher-order
 * signal. While not perfect, this should accurately encode a lower-order
 * signal into a higher-order signal.
 */
constexpr std::array FirstOrderDecoder{
    std::array{1.250000000e-01f,  1.250000000e-01f,  1.250000000e-01f,  1.250000000e-01f},
    std::array{1.250000000e-01f,  1.250000000e-01f,  1.250000000e-01f, -1.250000000e-01f},
    std::array{1.250000000e-01f, -1.250000000e-01f,  1.250000000e-01f,  1.250000000e-01f},
    std::array{1.250000000e-01f, -1.250000000e-01f,  1.250000000e-01f, -1.250000000e-01f},
    std::array{1.250000000e-01f,  1.250000000e-01f, -1.250000000e-01f,  1.250000000e-01f},
    std::array{1.250000000e-01f,  1.250000000e-01f, -1.250000000e-01f, -1.250000000e-01f},
    std::array{1.250000000e-01f, -1.250000000e-01f, -1.250000000e-01f,  1.250000000e-01f},
    std::array{1.250000000e-01f, -1.250000000e-01f, -1.250000000e-01f, -1.250000000e-01f},
};
constexpr std::array FirstOrderEncoder{
    CalcAmbiCoeffs( inv_sqrt3f,  inv_sqrt3f,  inv_sqrt3f),
    CalcAmbiCoeffs( inv_sqrt3f,  inv_sqrt3f, -inv_sqrt3f),
    CalcAmbiCoeffs(-inv_sqrt3f,  inv_sqrt3f,  inv_sqrt3f),
    CalcAmbiCoeffs(-inv_sqrt3f,  inv_sqrt3f, -inv_sqrt3f),
    CalcAmbiCoeffs( inv_sqrt3f, -inv_sqrt3f,  inv_sqrt3f),
    CalcAmbiCoeffs( inv_sqrt3f, -inv_sqrt3f, -inv_sqrt3f),
    CalcAmbiCoeffs(-inv_sqrt3f, -inv_sqrt3f,  inv_sqrt3f),
    CalcAmbiCoeffs(-inv_sqrt3f, -inv_sqrt3f, -inv_sqrt3f),
};
static_assert(FirstOrderDecoder.size() == FirstOrderEncoder.size(), "First-order mismatch");

/* This calculates a 2D first-order "upsampler" matrix. Same as the first-order
 * matrix, just using a more optimized speaker array for horizontal-only
 * content.
 */
constexpr std::array FirstOrder2DDecoder{
    std::array{1.666666667e-01f, -9.622504486e-02f, 0.0f,  1.666666667e-01f},
    std::array{1.666666667e-01f, -1.924500897e-01f, 0.0f,  0.000000000e+00f},
    std::array{1.666666667e-01f, -9.622504486e-02f, 0.0f, -1.666666667e-01f},
    std::array{1.666666667e-01f,  9.622504486e-02f, 0.0f, -1.666666667e-01f},
    std::array{1.666666667e-01f,  1.924500897e-01f, 0.0f,  0.000000000e+00f},
    std::array{1.666666667e-01f,  9.622504486e-02f, 0.0f,  1.666666667e-01f},
};
constexpr std::array FirstOrder2DEncoder{
    CalcAmbiCoeffs(-0.50000000000f, 0.0f,  0.86602540379f),
    CalcAmbiCoeffs(-1.00000000000f, 0.0f,  0.00000000000f),
    CalcAmbiCoeffs(-0.50000000000f, 0.0f, -0.86602540379f),
    CalcAmbiCoeffs( 0.50000000000f, 0.0f, -0.86602540379f),
    CalcAmbiCoeffs( 1.00000000000f, 0.0f,  0.00000000000f),
    CalcAmbiCoeffs( 0.50000000000f, 0.0f,  0.86602540379f),
};
static_assert(FirstOrder2DDecoder.size() == FirstOrder2DEncoder.size(), "First-order 2D mismatch");


/* This calculates a second-order "upsampler" matrix. Same as the first-order
 * matrix, just using a slightly more dense speaker array suitable for second-
 * order content.
 */
constexpr std::array SecondOrderDecoder{
    std::array{8.333333333e-02f,  0.000000000e+00f, -7.588274978e-02f,  1.227808683e-01f,  0.000000000e+00f,  0.000000000e+00f, -1.591525047e-02f, -1.443375673e-01f,  1.167715449e-01f},
    std::array{8.333333333e-02f, -1.227808683e-01f,  0.000000000e+00f,  7.588274978e-02f, -1.443375673e-01f,  0.000000000e+00f, -9.316949906e-02f,  0.000000000e+00f, -7.216878365e-02f},
    std::array{8.333333333e-02f, -7.588274978e-02f,  1.227808683e-01f,  0.000000000e+00f,  0.000000000e+00f, -1.443375673e-01f,  1.090847495e-01f,  0.000000000e+00f, -4.460276122e-02f},
    std::array{8.333333333e-02f,  0.000000000e+00f,  7.588274978e-02f,  1.227808683e-01f,  0.000000000e+00f,  0.000000000e+00f, -1.591525047e-02f,  1.443375673e-01f,  1.167715449e-01f},
    std::array{8.333333333e-02f, -1.227808683e-01f,  0.000000000e+00f, -7.588274978e-02f,  1.443375673e-01f,  0.000000000e+00f, -9.316949906e-02f,  0.000000000e+00f, -7.216878365e-02f},
    std::array{8.333333333e-02f,  7.588274978e-02f, -1.227808683e-01f,  0.000000000e+00f,  0.000000000e+00f, -1.443375673e-01f,  1.090847495e-01f,  0.000000000e+00f, -4.460276122e-02f},
    std::array{8.333333333e-02f,  0.000000000e+00f, -7.588274978e-02f, -1.227808683e-01f,  0.000000000e+00f,  0.000000000e+00f, -1.591525047e-02f,  1.443375673e-01f,  1.167715449e-01f},
    std::array{8.333333333e-02f,  1.227808683e-01f,  0.000000000e+00f, -7.588274978e-02f, -1.443375673e-01f,  0.000000000e+00f, -9.316949906e-02f,  0.000000000e+00f, -7.216878365e-02f},
    std::array{8.333333333e-02f,  7.588274978e-02f,  1.227808683e-01f,  0.000000000e+00f,  0.000000000e+00f,  1.443375673e-01f,  1.090847495e-01f,  0.000000000e+00f, -4.460276122e-02f},
    std::array{8.333333333e-02f,  0.000000000e+00f,  7.588274978e-02f, -1.227808683e-01f,  0.000000000e+00f,  0.000000000e+00f, -1.591525047e-02f, -1.443375673e-01f,  1.167715449e-01f},
    std::array{8.333333333e-02f,  1.227808683e-01f,  0.000000000e+00f,  7.588274978e-02f,  1.443375673e-01f,  0.000000000e+00f, -9.316949906e-02f,  0.000000000e+00f, -7.216878365e-02f},
    std::array{8.333333333e-02f, -7.588274978e-02f, -1.227808683e-01f,  0.000000000e+00f,  0.000000000e+00f,  1.443375673e-01f,  1.090847495e-01f,  0.000000000e+00f, -4.460276122e-02f},
};
constexpr std::array SecondOrderEncoder{
    CalcAmbiCoeffs( 0.000000000e+00f, -5.257311121e-01f,  8.506508084e-01f),
    CalcAmbiCoeffs(-8.506508084e-01f,  0.000000000e+00f,  5.257311121e-01f),
    CalcAmbiCoeffs(-5.257311121e-01f,  8.506508084e-01f,  0.000000000e+00f),
    CalcAmbiCoeffs( 0.000000000e+00f,  5.257311121e-01f,  8.506508084e-01f),
    CalcAmbiCoeffs(-8.506508084e-01f,  0.000000000e+00f, -5.257311121e-01f),
    CalcAmbiCoeffs( 5.257311121e-01f, -8.506508084e-01f,  0.000000000e+00f),
    CalcAmbiCoeffs( 0.000000000e+00f, -5.257311121e-01f, -8.506508084e-01f),
    CalcAmbiCoeffs( 8.506508084e-01f,  0.000000000e+00f, -5.257311121e-01f),
    CalcAmbiCoeffs( 5.257311121e-01f,  8.506508084e-01f,  0.000000000e+00f),
    CalcAmbiCoeffs( 0.000000000e+00f,  5.257311121e-01f, -8.506508084e-01f),
    CalcAmbiCoeffs( 8.506508084e-01f,  0.000000000e+00f,  5.257311121e-01f),
    CalcAmbiCoeffs(-5.257311121e-01f, -8.506508084e-01f,  0.000000000e+00f),
};
static_assert(SecondOrderDecoder.size() == SecondOrderEncoder.size(), "Second-order mismatch");

/* This calculates a 2D second-order "upsampler" matrix. Same as the second-
 * order matrix, just using a more optimized speaker array for horizontal-only
 * content.
 */
constexpr std::array SecondOrder2DDecoder{
    std::array{1.666666667e-01f, -9.622504486e-02f, 0.0f,  1.666666667e-01f, -1.490711985e-01f, 0.0f, 0.0f, 0.0f,  8.606629658e-02f},
    std::array{1.666666667e-01f, -1.924500897e-01f, 0.0f,  0.000000000e+00f,  0.000000000e+00f, 0.0f, 0.0f, 0.0f, -1.721325932e-01f},
    std::array{1.666666667e-01f, -9.622504486e-02f, 0.0f, -1.666666667e-01f,  1.490711985e-01f, 0.0f, 0.0f, 0.0f,  8.606629658e-02f},
    std::array{1.666666667e-01f,  9.622504486e-02f, 0.0f, -1.666666667e-01f, -1.490711985e-01f, 0.0f, 0.0f, 0.0f,  8.606629658e-02f},
    std::array{1.666666667e-01f,  1.924500897e-01f, 0.0f,  0.000000000e+00f,  0.000000000e+00f, 0.0f, 0.0f, 0.0f, -1.721325932e-01f},
    std::array{1.666666667e-01f,  9.622504486e-02f, 0.0f,  1.666666667e-01f,  1.490711985e-01f, 0.0f, 0.0f, 0.0f,  8.606629658e-02f},
};
constexpr std::array SecondOrder2DEncoder{
    CalcAmbiCoeffs(-0.50000000000f, 0.0f,  0.86602540379f),
    CalcAmbiCoeffs(-1.00000000000f, 0.0f,  0.00000000000f),
    CalcAmbiCoeffs(-0.50000000000f, 0.0f, -0.86602540379f),
    CalcAmbiCoeffs( 0.50000000000f, 0.0f, -0.86602540379f),
    CalcAmbiCoeffs( 1.00000000000f, 0.0f,  0.00000000000f),
    CalcAmbiCoeffs( 0.50000000000f, 0.0f,  0.86602540379f),
};
static_assert(SecondOrder2DDecoder.size() == SecondOrder2DEncoder.size(),
    "Second-order 2D mismatch");


/* This calculates a third-order "upsampler" matrix. Same as the first-order
 * matrix, just using a more dense speaker array suitable for third-order
 * content.
 */
constexpr std::array ThirdOrderDecoder{
    std::array{5.000000000e-02f,  3.090169944e-02f,  8.090169944e-02f,  0.000000000e+00f,  0.000000000e+00f,  6.454972244e-02f,  9.045084972e-02f,  0.000000000e+00f, -1.232790000e-02f, -1.256118221e-01f,  0.000000000e+00f,  1.126112056e-01f,  7.944389175e-02f,  0.000000000e+00f,  2.421151497e-02f,  0.000000000e+00f},
    std::array{5.000000000e-02f, -3.090169944e-02f,  8.090169944e-02f,  0.000000000e+00f,  0.000000000e+00f, -6.454972244e-02f,  9.045084972e-02f,  0.000000000e+00f, -1.232790000e-02f,  1.256118221e-01f,  0.000000000e+00f, -1.126112056e-01f,  7.944389175e-02f,  0.000000000e+00f,  2.421151497e-02f,  0.000000000e+00f},
    std::array{5.000000000e-02f,  3.090169944e-02f, -8.090169944e-02f,  0.000000000e+00f,  0.000000000e+00f, -6.454972244e-02f,  9.045084972e-02f,  0.000000000e+00f, -1.232790000e-02f, -1.256118221e-01f,  0.000000000e+00f,  1.126112056e-01f, -7.944389175e-02f,  0.000000000e+00f, -2.421151497e-02f,  0.000000000e+00f},
    std::array{5.000000000e-02f, -3.090169944e-02f, -8.090169944e-02f,  0.000000000e+00f,  0.000000000e+00f,  6.454972244e-02f,  9.045084972e-02f,  0.000000000e+00f, -1.232790000e-02f,  1.256118221e-01f,  0.000000000e+00f, -1.126112056e-01f, -7.944389175e-02f,  0.000000000e+00f, -2.421151497e-02f,  0.000000000e+00f},
    std::array{5.000000000e-02f,  8.090169944e-02f,  0.000000000e+00f,  3.090169944e-02f,  6.454972244e-02f,  0.000000000e+00f, -5.590169944e-02f,  0.000000000e+00f, -7.216878365e-02f, -7.763237543e-02f,  0.000000000e+00f, -2.950836627e-02f,  0.000000000e+00f, -1.497759251e-01f,  0.000000000e+00f, -7.763237543e-02f},
    std::array{5.000000000e-02f,  8.090169944e-02f,  0.000000000e+00f, -3.090169944e-02f, -6.454972244e-02f,  0.000000000e+00f, -5.590169944e-02f,  0.000000000e+00f, -7.216878365e-02f, -7.763237543e-02f,  0.000000000e+00f, -2.950836627e-02f,  0.000000000e+00f,  1.497759251e-01f,  0.000000000e+00f,  7.763237543e-02f},
    std::array{5.000000000e-02f, -8.090169944e-02f,  0.000000000e+00f,  3.090169944e-02f, -6.454972244e-02f,  0.000000000e+00f, -5.590169944e-02f,  0.000000000e+00f, -7.216878365e-02f,  7.763237543e-02f,  0.000000000e+00f,  2.950836627e-02f,  0.000000000e+00f, -1.497759251e-01f,  0.000000000e+00f, -7.763237543e-02f},
    std::array{5.000000000e-02f, -8.090169944e-02f,  0.000000000e+00f, -3.090169944e-02f,  6.454972244e-02f,  0.000000000e+00f, -5.590169944e-02f,  0.000000000e+00f, -7.216878365e-02f,  7.763237543e-02f,  0.000000000e+00f,  2.950836627e-02f,  0.000000000e+00f,  1.497759251e-01f,  0.000000000e+00f,  7.763237543e-02f},
    std::array{5.000000000e-02f,  0.000000000e+00f,  3.090169944e-02f,  8.090169944e-02f,  0.000000000e+00f,  0.000000000e+00f, -3.454915028e-02f,  6.454972244e-02f,  8.449668365e-02f,  0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  3.034486645e-02f, -6.779013272e-02f,  1.659481923e-01f,  4.797944664e-02f},
    std::array{5.000000000e-02f,  0.000000000e+00f,  3.090169944e-02f, -8.090169944e-02f,  0.000000000e+00f,  0.000000000e+00f, -3.454915028e-02f, -6.454972244e-02f,  8.449668365e-02f,  0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f,  3.034486645e-02f,  6.779013272e-02f,  1.659481923e-01f, -4.797944664e-02f},
    std::array{5.000000000e-02f,  0.000000000e+00f, -3.090169944e-02f,  8.090169944e-02f,  0.000000000e+00f,  0.000000000e+00f, -3.454915028e-02f, -6.454972244e-02f,  8.449668365e-02f,  0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f, -3.034486645e-02f, -6.779013272e-02f, -1.659481923e-01f,  4.797944664e-02f},
    std::array{5.000000000e-02f,  0.000000000e+00f, -3.090169944e-02f, -8.090169944e-02f,  0.000000000e+00f,  0.000000000e+00f, -3.454915028e-02f,  6.454972244e-02f,  8.449668365e-02f,  0.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f, -3.034486645e-02f,  6.779013272e-02f, -1.659481923e-01f, -4.797944664e-02f},
    std::array{5.000000000e-02f,  5.000000000e-02f,  5.000000000e-02f,  5.000000000e-02f,  6.454972244e-02f,  6.454972244e-02f,  0.000000000e+00f,  6.454972244e-02f,  0.000000000e+00f,  1.016220987e-01f,  6.338656910e-02f, -1.092600649e-02f, -7.364853795e-02f,  1.011266756e-01f, -7.086833869e-02f, -1.482646439e-02f},
    std::array{5.000000000e-02f,  5.000000000e-02f,  5.000000000e-02f, -5.000000000e-02f, -6.454972244e-02f,  6.454972244e-02f,  0.000000000e+00f, -6.454972244e-02f,  0.000000000e+00f,  1.016220987e-01f, -6.338656910e-02f, -1.092600649e-02f, -7.364853795e-02f, -1.011266756e-01f, -7.086833869e-02f,  1.482646439e-02f},
    std::array{5.000000000e-02f, -5.000000000e-02f,  5.000000000e-02f,  5.000000000e-02f, -6.454972244e-02f, -6.454972244e-02f,  0.000000000e+00f,  6.454972244e-02f,  0.000000000e+00f, -1.016220987e-01f, -6.338656910e-02f,  1.092600649e-02f, -7.364853795e-02f,  1.011266756e-01f, -7.086833869e-02f, -1.482646439e-02f},
    std::array{5.000000000e-02f, -5.000000000e-02f,  5.000000000e-02f, -5.000000000e-02f,  6.454972244e-02f, -6.454972244e-02f,  0.000000000e+00f, -6.454972244e-02f,  0.000000000e+00f, -1.016220987e-01f,  6.338656910e-02f,  1.092600649e-02f, -7.364853795e-02f, -1.011266756e-01f, -7.086833869e-02f,  1.482646439e-02f},
    std::array{5.000000000e-02f,  5.000000000e-02f, -5.000000000e-02f,  5.000000000e-02f,  6.454972244e-02f, -6.454972244e-02f,  0.000000000e+00f, -6.454972244e-02f,  0.000000000e+00f,  1.016220987e-01f, -6.338656910e-02f, -1.092600649e-02f,  7.364853795e-02f,  1.011266756e-01f,  7.086833869e-02f, -1.482646439e-02f},
    std::array{5.000000000e-02f,  5.000000000e-02f, -5.000000000e-02f, -5.000000000e-02f, -6.454972244e-02f, -6.454972244e-02f,  0.000000000e+00f,  6.454972244e-02f,  0.000000000e+00f,  1.016220987e-01f,  6.338656910e-02f, -1.092600649e-02f,  7.364853795e-02f, -1.011266756e-01f,  7.086833869e-02f,  1.482646439e-02f},
    std::array{5.000000000e-02f, -5.000000000e-02f, -5.000000000e-02f,  5.000000000e-02f, -6.454972244e-02f,  6.454972244e-02f,  0.000000000e+00f, -6.454972244e-02f,  0.000000000e+00f, -1.016220987e-01f,  6.338656910e-02f,  1.092600649e-02f,  7.364853795e-02f,  1.011266756e-01f,  7.086833869e-02f, -1.482646439e-02f},
    std::array{5.000000000e-02f, -5.000000000e-02f, -5.000000000e-02f, -5.000000000e-02f,  6.454972244e-02f,  6.454972244e-02f,  0.000000000e+00f,  6.454972244e-02f,  0.000000000e+00f, -1.016220987e-01f, -6.338656910e-02f,  1.092600649e-02f,  7.364853795e-02f, -1.011266756e-01f,  7.086833869e-02f,  1.482646439e-02f},
};
constexpr std::array ThirdOrderEncoder{
    CalcAmbiCoeffs( 0.35682208976f,  0.93417235897f,  0.00000000000f),
    CalcAmbiCoeffs(-0.35682208976f,  0.93417235897f,  0.00000000000f),
    CalcAmbiCoeffs( 0.35682208976f, -0.93417235897f,  0.00000000000f),
    CalcAmbiCoeffs(-0.35682208976f, -0.93417235897f,  0.00000000000f),
    CalcAmbiCoeffs( 0.93417235897f,  0.00000000000f,  0.35682208976f),
    CalcAmbiCoeffs( 0.93417235897f,  0.00000000000f, -0.35682208976f),
    CalcAmbiCoeffs(-0.93417235897f,  0.00000000000f,  0.35682208976f),
    CalcAmbiCoeffs(-0.93417235897f,  0.00000000000f, -0.35682208976f),
    CalcAmbiCoeffs( 0.00000000000f,  0.35682208976f,  0.93417235897f),
    CalcAmbiCoeffs( 0.00000000000f,  0.35682208976f, -0.93417235897f),
    CalcAmbiCoeffs( 0.00000000000f, -0.35682208976f,  0.93417235897f),
    CalcAmbiCoeffs( 0.00000000000f, -0.35682208976f, -0.93417235897f),
    CalcAmbiCoeffs(     inv_sqrt3f,      inv_sqrt3f,      inv_sqrt3f),
    CalcAmbiCoeffs(     inv_sqrt3f,      inv_sqrt3f,     -inv_sqrt3f),
    CalcAmbiCoeffs(    -inv_sqrt3f,      inv_sqrt3f,      inv_sqrt3f),
    CalcAmbiCoeffs(    -inv_sqrt3f,      inv_sqrt3f,     -inv_sqrt3f),
    CalcAmbiCoeffs(     inv_sqrt3f,     -inv_sqrt3f,      inv_sqrt3f),
    CalcAmbiCoeffs(     inv_sqrt3f,     -inv_sqrt3f,     -inv_sqrt3f),
    CalcAmbiCoeffs(    -inv_sqrt3f,     -inv_sqrt3f,      inv_sqrt3f),
    CalcAmbiCoeffs(    -inv_sqrt3f,     -inv_sqrt3f,     -inv_sqrt3f),
};
static_assert(ThirdOrderDecoder.size() == ThirdOrderEncoder.size(), "Third-order mismatch");

/* This calculates a 2D third-order "upsampler" matrix. Same as the third-order
 * matrix, just using a more optimized speaker array for horizontal-only
 * content.
 */
constexpr std::array ThirdOrder2DDecoder{
    std::array{1.250000000e-01f, -5.523559567e-02f, 0.0f,  1.333505242e-01f, -9.128709292e-02f, 0.0f, 0.0f, 0.0f,  9.128709292e-02f, -1.104247249e-01f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  4.573941867e-02f},
    std::array{1.250000000e-01f, -1.333505242e-01f, 0.0f,  5.523559567e-02f, -9.128709292e-02f, 0.0f, 0.0f, 0.0f, -9.128709292e-02f,  4.573941867e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.104247249e-01f},
    std::array{1.250000000e-01f, -1.333505242e-01f, 0.0f, -5.523559567e-02f,  9.128709292e-02f, 0.0f, 0.0f, 0.0f, -9.128709292e-02f,  4.573941867e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  1.104247249e-01f},
    std::array{1.250000000e-01f, -5.523559567e-02f, 0.0f, -1.333505242e-01f,  9.128709292e-02f, 0.0f, 0.0f, 0.0f,  9.128709292e-02f, -1.104247249e-01f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -4.573941867e-02f},
    std::array{1.250000000e-01f,  5.523559567e-02f, 0.0f, -1.333505242e-01f, -9.128709292e-02f, 0.0f, 0.0f, 0.0f,  9.128709292e-02f,  1.104247249e-01f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -4.573941867e-02f},
    std::array{1.250000000e-01f,  1.333505242e-01f, 0.0f, -5.523559567e-02f, -9.128709292e-02f, 0.0f, 0.0f, 0.0f, -9.128709292e-02f, -4.573941867e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  1.104247249e-01f},
    std::array{1.250000000e-01f,  1.333505242e-01f, 0.0f,  5.523559567e-02f,  9.128709292e-02f, 0.0f, 0.0f, 0.0f, -9.128709292e-02f, -4.573941867e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.104247249e-01f},
    std::array{1.250000000e-01f,  5.523559567e-02f, 0.0f,  1.333505242e-01f,  9.128709292e-02f, 0.0f, 0.0f, 0.0f,  9.128709292e-02f,  1.104247249e-01f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  4.573941867e-02f},
};
constexpr std::array ThirdOrder2DEncoder{
    CalcAmbiCoeffs(-0.38268343237f, 0.0f,  0.92387953251f),
    CalcAmbiCoeffs(-0.92387953251f, 0.0f,  0.38268343237f),
    CalcAmbiCoeffs(-0.92387953251f, 0.0f, -0.38268343237f),
    CalcAmbiCoeffs(-0.38268343237f, 0.0f, -0.92387953251f),
    CalcAmbiCoeffs( 0.38268343237f, 0.0f, -0.92387953251f),
    CalcAmbiCoeffs( 0.92387953251f, 0.0f, -0.38268343237f),
    CalcAmbiCoeffs( 0.92387953251f, 0.0f,  0.38268343237f),
    CalcAmbiCoeffs( 0.38268343237f, 0.0f,  0.92387953251f),
};
static_assert(ThirdOrder2DDecoder.size() == ThirdOrder2DEncoder.size(), "Third-order 2D mismatch");


/* This calculates a 2D fourth-order "upsampler" matrix. There is no 3D fourth-
 * order upsampler since fourth-order is the max order we'll be supporting for
 * the foreseeable future. This is only necessary for mixing horizontal-only
 * fourth-order content to 3D.
 */
constexpr std::array FourthOrder2DDecoder{
    std::array{1.000000000e-01f,  3.568220898e-02f, 0.0f,  1.098185471e-01f,  6.070619982e-02f, 0.0f, 0.0f, 0.0f,  8.355491589e-02f,  7.735682057e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  5.620301997e-02f,  8.573754253e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  2.785781628e-02f},
    std::array{1.000000000e-01f,  9.341723590e-02f, 0.0f,  6.787159473e-02f,  9.822469464e-02f, 0.0f, 0.0f, 0.0f, -3.191513794e-02f,  2.954767620e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -9.093839659e-02f, -5.298871540e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -7.293270986e-02f},
    std::array{1.000000000e-01f,  1.154700538e-01f, 0.0f,  0.000000000e+00f,  0.000000000e+00f, 0.0f, 0.0f, 0.0f, -1.032795559e-01f, -9.561828875e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  0.000000000e+00f,  0.000000000e+00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  9.014978717e-02f},
    std::array{1.000000000e-01f,  9.341723590e-02f, 0.0f, -6.787159473e-02f, -9.822469464e-02f, 0.0f, 0.0f, 0.0f, -3.191513794e-02f,  2.954767620e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  9.093839659e-02f,  5.298871540e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -7.293270986e-02f},
    std::array{1.000000000e-01f,  3.568220898e-02f, 0.0f, -1.098185471e-01f, -6.070619982e-02f, 0.0f, 0.0f, 0.0f,  8.355491589e-02f,  7.735682057e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -5.620301997e-02f, -8.573754253e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  2.785781628e-02f},
    std::array{1.000000000e-01f, -3.568220898e-02f, 0.0f, -1.098185471e-01f,  6.070619982e-02f, 0.0f, 0.0f, 0.0f,  8.355491589e-02f, -7.735682057e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -5.620301997e-02f,  8.573754253e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  2.785781628e-02f},
    std::array{1.000000000e-01f, -9.341723590e-02f, 0.0f, -6.787159473e-02f,  9.822469464e-02f, 0.0f, 0.0f, 0.0f, -3.191513794e-02f, -2.954767620e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  9.093839659e-02f, -5.298871540e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -7.293270986e-02f},
    std::array{1.000000000e-01f, -1.154700538e-01f, 0.0f,  0.000000000e+00f,  0.000000000e+00f, 0.0f, 0.0f, 0.0f, -1.032795559e-01f,  9.561828875e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  0.000000000e+00f,  0.000000000e+00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  9.014978717e-02f},
    std::array{1.000000000e-01f, -9.341723590e-02f, 0.0f,  6.787159473e-02f, -9.822469464e-02f, 0.0f, 0.0f, 0.0f, -3.191513794e-02f, -2.954767620e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -9.093839659e-02f,  5.298871540e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -7.293270986e-02f},
    std::array{1.000000000e-01f, -3.568220898e-02f, 0.0f,  1.098185471e-01f, -6.070619982e-02f, 0.0f, 0.0f, 0.0f,  8.355491589e-02f, -7.735682057e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  5.620301997e-02f, -8.573754253e-02f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  2.785781628e-02f},
};
constexpr std::array FourthOrder2DEncoder{
    CalcAmbiCoeffs( 3.090169944e-01f,  0.000000000e+00f,  9.510565163e-01f),
    CalcAmbiCoeffs( 8.090169944e-01f,  0.000000000e+00f,  5.877852523e-01f),
    CalcAmbiCoeffs( 1.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f),
    CalcAmbiCoeffs( 8.090169944e-01f,  0.000000000e+00f, -5.877852523e-01f),
    CalcAmbiCoeffs( 3.090169944e-01f,  0.000000000e+00f, -9.510565163e-01f),
    CalcAmbiCoeffs(-3.090169944e-01f,  0.000000000e+00f, -9.510565163e-01f),
    CalcAmbiCoeffs(-8.090169944e-01f,  0.000000000e+00f, -5.877852523e-01f),
    CalcAmbiCoeffs(-1.000000000e+00f,  0.000000000e+00f,  0.000000000e+00f),
    CalcAmbiCoeffs(-8.090169944e-01f,  0.000000000e+00f,  5.877852523e-01f),
    CalcAmbiCoeffs(-3.090169944e-01f,  0.000000000e+00f,  9.510565163e-01f),
};
static_assert(FourthOrder2DDecoder.size() == FourthOrder2DEncoder.size(), "Fourth-order 2D mismatch");


template<size_t N, size_t M>
constexpr auto CalcAmbiUpsampler(const std::array<std::array<float,N>,M> &decoder,
    const std::array<AmbiChannelFloatArray,M> &encoder)
{
    auto res = std::array<AmbiChannelFloatArray,N>{};

    for(size_t i{0};i < decoder[0].size();++i)
    {
        for(size_t j{0};j < encoder[0].size();++j)
        {
            double sum{0.0};
            for(size_t k{0};k < decoder.size();++k)
                sum += double{decoder[k][i]} * encoder[k][j];
            res[i][j] = static_cast<float>(sum);
        }
    }

    return res;
}

} // namespace

constinit const std::array<AmbiChannelFloatArray,4> AmbiScale::FirstOrderUp{CalcAmbiUpsampler(FirstOrderDecoder, FirstOrderEncoder)};
constinit const std::array<AmbiChannelFloatArray,4> AmbiScale::FirstOrder2DUp{CalcAmbiUpsampler(FirstOrder2DDecoder, FirstOrder2DEncoder)};
constinit const std::array<AmbiChannelFloatArray,9> AmbiScale::SecondOrderUp{CalcAmbiUpsampler(SecondOrderDecoder, SecondOrderEncoder)};
constinit const std::array<AmbiChannelFloatArray,9> AmbiScale::SecondOrder2DUp{CalcAmbiUpsampler(SecondOrder2DDecoder, SecondOrder2DEncoder)};
constinit const std::array<AmbiChannelFloatArray,16> AmbiScale::ThirdOrderUp{CalcAmbiUpsampler(ThirdOrderDecoder, ThirdOrderEncoder)};
constinit const std::array<AmbiChannelFloatArray,16> AmbiScale::ThirdOrder2DUp{CalcAmbiUpsampler(ThirdOrder2DDecoder, ThirdOrder2DEncoder)};
constinit const std::array<AmbiChannelFloatArray,25> AmbiScale::FourthOrder2DUp{CalcAmbiUpsampler(FourthOrder2DDecoder, FourthOrder2DEncoder)};


auto AmbiScale::GetHFOrderScales(const uint src_order, const uint dev_order,
    const bool horizontalOnly) noexcept -> std::array<float,MaxAmbiOrder+1>
{
    auto res = std::array<float,MaxAmbiOrder+1>{};

    const auto scales = horizontalOnly ? std::span{HFScales2D} : std::span{HFScales};
    std::transform(scales[src_order].begin(), scales[src_order].end(), scales[dev_order].begin(),
        res.begin(), std::divides{});

    return res;
}
