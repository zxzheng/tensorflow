/* Copyright 2019 Google LLC. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_LITE_EXPERIMENTAL_RUY_TEST_H_
#define TENSORFLOW_LITE_EXPERIMENTAL_RUY_TEST_H_

#include <algorithm>
#include <ctime>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include "tensorflow/lite/experimental/ruy/pmu.h"
#include "tensorflow/lite/experimental/ruy/ruy.h"
#include "tensorflow/lite/experimental/ruy/time.h"

#ifdef RUY_TEST_EXTERNAL_PATHS
#define EIGEN_USE_THREADS
#define EIGEN_USE_CUSTOM_THREAD_POOL
#include "third_party/eigen3/Eigen/Core"
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "third_party/lapack/blas.h"
#include "public/gemmlowp.h"
#endif

#ifdef GEMMLOWP_PROFILING
#include "profiling/profiler.h"
#endif

namespace ruy {

const float kClampRatio = 0.1f;

enum class ExternalPath { kNone, kGemmlowp, kEigen, kEigenTensor, kOpenBlas };

inline std::vector<std::string>* CoveredPaths() {
  static std::vector<std::string> covered_paths;
  return &covered_paths;
}

const char* PathName(Path path) {
#define RUY_PATHNAME_CASE(NAME) \
  case Path::NAME:              \
    return #NAME;
  switch (path) {
    RUY_PATHNAME_CASE(kReference)
    RUY_PATHNAME_CASE(kStandardCpp)
    RUY_PATHNAME_CASE(kNeon)
    RUY_PATHNAME_CASE(kNeonDotprod)
    default:
      RUY_CHECK(false);
      return nullptr;
  }
#undef RUY_PATHNAME_CASE
}

const char* TuningName(Tuning tuning) {
#define RUY_SUBPATHNAME_CASE(NAME) \
  case Tuning::NAME:               \
    return #NAME;
  switch (tuning) {
    RUY_SUBPATHNAME_CASE(kInOrder)
    RUY_SUBPATHNAME_CASE(kOutOfOrder)
    default:
      RUY_CHECK(false);
      return nullptr;
  }
#undef RUY_SUBPATHNAME_CASE
}

const char* PathName(ExternalPath path) {
#define RUY_PATHNAME_CASE(NAME) \
  case ExternalPath::NAME:      \
    return #NAME;
  switch (path) {
    RUY_PATHNAME_CASE(kGemmlowp)
    RUY_PATHNAME_CASE(kEigen)
    RUY_PATHNAME_CASE(kEigenTensor)
    RUY_PATHNAME_CASE(kOpenBlas)
    default:
      RUY_CHECK(false);
      return nullptr;
  }
#undef RUY_PATHNAME_CASE
}

std::ostream& operator<<(std::ostream& stream, Path path) {
  return stream << PathName(path);
}

std::ostream& operator<<(std::ostream& stream, ExternalPath external_path) {
  return stream << PathName(external_path);
}

template <typename ContainerType>
std::string Join(const ContainerType& container) {
  if (container.empty()) {
    return "<empty>";
  }
  std::ostringstream stream;
  auto it = container.begin();
  stream << *it++;
  for (; it != container.end(); ++it) {
    stream << ", ";
    stream << *it;
  }
  return stream.str();
}

struct LogCoveredPathsOnDestruction final {
  ~LogCoveredPathsOnDestruction() {
    std::cerr << "Covered paths: " << Join(*CoveredPaths()) << std::endl;
  }
  static void Singleton() { static LogCoveredPathsOnDestruction singleton; }
};

enum class RandomRange {
  kGeneral,
  kAvoidMinValue,
  kReasonableSrcZeroPoint,
  kReasonableDstZeroPoint,
  kBias
};

template <typename Scalar,
          bool IsFloatingPoint = std::is_floating_point<Scalar>::value>
struct RandomRangeBounds {};

template <typename Scalar>
struct RandomRangeBounds<Scalar, true> {
  static Scalar GetMinBound(RandomRange range) {
    switch (range) {
      case RandomRange::kGeneral:
        return -1;
      case RandomRange::kAvoidMinValue:
        return -1;
      case RandomRange::kReasonableSrcZeroPoint:
        return 0;
      case RandomRange::kReasonableDstZeroPoint:
        return 0;
      case RandomRange::kBias:
        return -1;
      default:
        RUY_CHECK(false);
        return 0;
    }
  }
  static Scalar GetMaxBound(RandomRange range) {
    switch (range) {
      case RandomRange::kGeneral:
        return 1;
      case RandomRange::kAvoidMinValue:
        return 1;
      case RandomRange::kReasonableSrcZeroPoint:
        return 0;
      case RandomRange::kReasonableDstZeroPoint:
        return 0;
      case RandomRange::kBias:
        return 1;
      default:
        RUY_CHECK(false);
        return 0;
    }
  }
};

template <typename Scalar>
Scalar WeightedSum(Scalar s1, float weight1, Scalar s2, float weight2) {
  float sum = s1 * weight1 + s2 * weight2;
  float clamped = std::min<float>(
      std::numeric_limits<Scalar>::max(),
      std::max<float>(std::numeric_limits<Scalar>::lowest(), sum));
  return static_cast<Scalar>(clamped);
}

template <typename Scalar>
Scalar Parametrized(float param) {
  return WeightedSum(std::numeric_limits<Scalar>::max(), param,
                     std::numeric_limits<Scalar>::lowest(), 1 - param);
}

template <typename Scalar>
struct RandomRangeBounds<Scalar, false> {
  static Scalar GetMinBound(RandomRange range) {
    switch (range) {
      case RandomRange::kGeneral:
        return std::numeric_limits<Scalar>::lowest();
      case RandomRange::kAvoidMinValue:
        return 1 + std::numeric_limits<Scalar>::lowest();
      case RandomRange::kReasonableSrcZeroPoint:
        return std::numeric_limits<Scalar>::lowest();
      case RandomRange::kReasonableDstZeroPoint:
        return Parametrized<Scalar>(0.4);
      case RandomRange::kBias:
        return std::is_same<Scalar, std::int32_t>::value
                   ? static_cast<Scalar>(-10000)
                   : 0;
      default:
        RUY_CHECK(false);
        return 0;
    }
  }
  static Scalar GetMaxBound(RandomRange range) {
    switch (range) {
      case RandomRange::kGeneral:
        return std::numeric_limits<Scalar>::max();
      case RandomRange::kAvoidMinValue:
        return std::numeric_limits<Scalar>::max();
      case RandomRange::kReasonableSrcZeroPoint:
        return std::numeric_limits<Scalar>::max();
      case RandomRange::kReasonableDstZeroPoint:
        return Parametrized<Scalar>(0.6);
      case RandomRange::kBias:
        return std::is_same<Scalar, std::int32_t>::value
                   ? static_cast<Scalar>(10000)
                   : 0;
      default:
        RUY_CHECK(false);
        return 0;
    }
  }
};

inline std::default_random_engine& global_random_engine() {
  static std::default_random_engine engine;
  return engine;
};

template <typename Scalar>
struct UniformRandomDistribution {
  UniformRandomDistribution(RandomRange range)
      : dist(RandomRangeBounds<Scalar>::GetMinBound(range),
             RandomRangeBounds<Scalar>::GetMaxBound(range)) {}
  Scalar Get() { return dist(global_random_engine()); }
  // std::uniform_int_distribution is specified not to support char types,
  // only short and wider types. MSVC actually generates an error on
  // std::uniform_int_distribution<std::int8_t>.
  using StdDistType = typename std::conditional<
      std::is_floating_point<Scalar>::value,
      std::uniform_real_distribution<Scalar>,
      std::uniform_int_distribution<std::int32_t>>::type;
  StdDistType dist;
};

template <typename Scalar>
void MakeRandomScalar(UniformRandomDistribution<Scalar>* uniform_dist,
                      Scalar* dst) {
  *dst = uniform_dist->Get();
}

template <typename Scalar>
void MakeRandomVector(UniformRandomDistribution<Scalar>* uniform_dist, int size,
                      std::vector<Scalar>* dst) {
  dst->resize(size);
  for (auto& x : *dst) {
    MakeRandomScalar(uniform_dist, &x);
  }
}

template <typename Scalar>
void MakeRandomScalar(RandomRange range, Scalar* dst) {
  UniformRandomDistribution<Scalar> dist(range);
  *dst = dist.Get();
  if (range == RandomRange::kReasonableDstZeroPoint ||
      range == RandomRange::kReasonableSrcZeroPoint) {
    if (global_random_engine()() & 1) {
      *dst = SymmetricZeroPoint<Scalar>();
    }
  }
}

template <typename Scalar>
void MakeRandomVector(RandomRange range, int size, std::vector<Scalar>* dst) {
  UniformRandomDistribution<Scalar> dist(range);
  dst->resize(size);
  for (auto& x : *dst) {
    MakeRandomScalar(&dist, &x);
  }
}

enum class LayoutStyle { kPackedLinear, kLinear };

void MakeLayout(int rows, int cols, Order order, LayoutStyle layout_style,
                Layout* layout) {
  layout->rows = rows;
  layout->cols = cols;
  layout->order = order;

  const int packed_stride = order == Order::kColMajor ? rows : cols;

  RUY_CHECK(layout_style == LayoutStyle::kPackedLinear ||
            layout_style == LayoutStyle::kLinear);
  if (layout_style == LayoutStyle::kPackedLinear) {
    layout->stride = packed_stride;
  } else {
    layout->stride = packed_stride + 1;
  }
}

template <typename Scalar>
struct StorageMatrix {
  std::vector<Scalar> data;
  Matrix<Scalar> matrix;
};

template <typename Scalar>
void VerifyConsistentFields(const StorageMatrix<Scalar>& storage_matrix) {
  if (storage_matrix.data.empty()) {
    RUY_CHECK_EQ(storage_matrix.matrix.data.get(), nullptr);
    RUY_CHECK_EQ(storage_matrix.matrix.layout.rows, 0);
    RUY_CHECK_EQ(storage_matrix.matrix.layout.cols, 0);
  } else {
    RUY_CHECK_EQ(storage_matrix.matrix.data.get(), storage_matrix.data.data());
    RUY_CHECK_EQ(FlatSize(storage_matrix.matrix.layout),
                 storage_matrix.data.size());
  }
}

template <typename Scalar>
void MakeRandom(int rows, int cols, Order order, Scalar zero_point,
                LayoutStyle layout_style, RandomRange range,
                StorageMatrix<Scalar>* storage_matrix) {
  MakeLayout(rows, cols, order, layout_style, &storage_matrix->matrix.layout);
  storage_matrix->matrix.zero_point = zero_point;
  UniformRandomDistribution<Scalar> data_dist(range);
  MakeRandomVector(&data_dist, FlatSize(storage_matrix->matrix.layout),
                   &storage_matrix->data);
  storage_matrix->matrix.data = storage_matrix->data.data();
  VerifyConsistentFields(*storage_matrix);
}

template <typename Scalar>
struct TestResult {
  StorageMatrix<Scalar> storage_matrix;
  Path path = Path::kNone;
  Tuning tuning = Tuning::kAuto;
  ExternalPath external_path = ExternalPath::kNone;
  float latency;
  float l1_refill_rate;
  float l2_refill_rate;
  float l3_refill_rate;
  float mispred_rate;
  float frontend_stall_rate;
  float backend_stall_rate;
};

template <typename Scalar>
std::string PathName(const TestResult<Scalar>& result) {
  std::string pathname;
  if (result.path != Path::kNone) {
    pathname.assign(PathName(result.path));
  } else if (result.external_path != ExternalPath::kNone) {
    pathname.assign(PathName(result.external_path));
  } else {
    RUY_CHECK(false);
  }
  if (result.tuning != Tuning::kAuto) {
    pathname.append("/");
    pathname.append(TuningName(result.tuning));
  }
  return pathname;
}

enum class ExpectedOutcome { kSuccess, kDeath };

template <typename tLhsScalar, typename tRhsScalar, typename SpecType>
struct TestSet final {
  using LhsScalar = tLhsScalar;
  using RhsScalar = tRhsScalar;
  using AccumScalar = typename SpecType::AccumScalar;
  using DstScalar = typename SpecType::DstScalar;
  using Spec = SpecType;

  void Run() {
    MakeZeroPoints();
    MakeLhsRhs();
    MakeSpec();
    MakeOtherParams();
    MakeResultPaths();
    Eval();
    Verify();
  }

 private:
  void MakeZeroPoints();
  void MakeLhsRhs();
  void MakeSpec();
  void MakeResultPaths();
  void MakeOtherParams();
  void EvalAndVerify();
  void Eval();
  void Verify();

  void EvalResult(TestResult<DstScalar>* result);
  void Benchmark(TestResult<DstScalar>* result);
  void VerifyTestResults() const;
  void VerifyNonTrivial() const;

 public:
  enum class LifeStage {
    kInitial,
    kHasZeroPoints,
    kHasLhsRhs,
    kHasSpec,
    kHasOtherParams,
    kHasResultPaths,
    kEvaluated,
    kFinal
  };

  ~TestSet() {
    RUY_CHECK(life_stage == LifeStage::kFinal);
    LogCoveredPathsOnDestruction::Singleton();
  }

  LifeStage life_stage = LifeStage::kInitial;

  int rows = 0;
  int cols = 0;
  int depth = 0;
  Order lhs_order = Order::kRowMajor;
  Order rhs_order = Order::kColMajor;
  Order dst_order = Order::kColMajor;
  LayoutStyle layout_style = LayoutStyle::kPackedLinear;
  ExpectedOutcome expected_outcome = ExpectedOutcome::kSuccess;

  bool use_specified_zero_points = false;
  LhsScalar lhs_zero_point = 0;
  RhsScalar rhs_zero_point = 0;
  DstScalar dst_zero_point = 0;

  std::vector<AccumScalar> per_channel_multiplier_fixedpoint;
  std::vector<int> per_channel_multiplier_exponent;

  StorageMatrix<LhsScalar> lhs;
  StorageMatrix<RhsScalar> rhs;
  Spec spec;
  std::vector<AccumScalar> bias_data;
  std::vector<TestResult<DstScalar>> results;

  std::vector<Path> paths;
  std::vector<ExternalPath> external_paths;

  bool benchmark = false;
  bool perchannel = false;
  int max_num_threads = 0;
};

Context& GlobalContext() {
  static Context context;
  return context;
}

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RUY_TSAN
#endif
#if __has_feature(address_sanitizer)
#define RUY_ASAN
#endif
#endif  // defined(__has_feature)

template <typename LhsScalar, typename RhsScalar, typename DstScalar,
          typename Spec>
void EvalRuy(Path path, Tuning tuning, const Matrix<LhsScalar>& lhs,
             const Matrix<RhsScalar>& rhs, const Spec& spec,
             Matrix<DstScalar>* dst, ExpectedOutcome expected_outcome,
             bool benchmark, int max_num_threads) {
  GlobalContext().explicit_tuning = tuning;
  if (max_num_threads) {
    GlobalContext().max_num_threads = max_num_threads;
  } else if (benchmark) {
    GlobalContext().max_num_threads = 1;
  } else {
    GlobalContext().max_num_threads = 1 + global_random_engine()() % 8;
  }
  GlobalContext().SetRuntimeEnabledPaths(path);
  if (expected_outcome == ExpectedOutcome::kSuccess) {
    Mul<kAllPaths>(lhs, rhs, spec, &GlobalContext(), dst);
    RUY_CHECK(GlobalContext().last_taken_path == path);
  } else if (expected_outcome == ExpectedOutcome::kDeath) {
    // TODO(benoitjacob) TSan and ASan seem to be breaking ASSERT_DEATH.
    // Report a bug?
#if (!defined NDEBUG) && (!defined RUY_ASAN) && (!defined RUY_TSAN)
    ASSERT_DEATH(Mul<kAllPaths>(lhs, rhs, spec, &GlobalContext(), dst), "");
#endif
  } else {
    RUY_CHECK(false);
  }
  GlobalContext().explicit_tuning = Tuning::kAuto;
  GlobalContext().max_num_threads = 1;
}

#ifdef RUY_TEST_EXTERNAL_PATHS

template <typename Scalar, gemmlowp::MapOrder tOrder>
void WrapGemmlowp(const Matrix<Scalar>& src,
                  gemmlowp::MatrixMap<const Scalar, tOrder>* dst) {
  RUY_CHECK(src.layout.order == (tOrder == gemmlowp::MapOrder::ColMajor
                                     ? Order::kColMajor
                                     : Order::kRowMajor));
  *dst = gemmlowp::MatrixMap<const Scalar, tOrder>(
      src.data.get(), src.layout.rows, src.layout.cols, src.layout.stride);
}

template <typename Scalar, gemmlowp::MapOrder tOrder>
void WrapGemmlowpMutable(Matrix<Scalar>* src,
                         gemmlowp::MatrixMap<Scalar, tOrder>* dst) {
  RUY_CHECK(src->layout.order == (tOrder == gemmlowp::MapOrder::ColMajor
                                      ? Order::kColMajor
                                      : Order::kRowMajor));
  *dst = gemmlowp::MatrixMap<Scalar, tOrder>(
      src->data.get(), src->layout.rows, src->layout.cols, src->layout.stride);
}

template <Order tOrder>
struct GemmlowpOrder {};

template <>
struct GemmlowpOrder<Order::kColMajor> {
  static constexpr gemmlowp::MapOrder kValue = gemmlowp::MapOrder::ColMajor;
};

template <>
struct GemmlowpOrder<Order::kRowMajor> {
  static constexpr gemmlowp::MapOrder kValue = gemmlowp::MapOrder::RowMajor;
};

gemmlowp::GemmContext& GlobalGemmlowpContext() {
  static gemmlowp::GemmContext context;
  return context;
}

template <Order LhsOrder, Order RhsOrder, Order DstOrder, typename LhsScalar,
          typename RhsScalar, typename DstScalar, typename Spec>
void EvalGemmlowp(const Matrix<LhsScalar>& lhs, const Matrix<RhsScalar>& rhs,
                  const Spec& spec, int max_num_threads,
                  Matrix<DstScalar>* dst) {
  static constexpr gemmlowp::MapOrder kGemmlowpLhsOrder =
      GemmlowpOrder<LhsOrder>::kValue;
  static constexpr gemmlowp::MapOrder kGemmlowpRhsOrder =
      GemmlowpOrder<RhsOrder>::kValue;
  static constexpr gemmlowp::MapOrder kGemmlowpDstOrder =
      GemmlowpOrder<DstOrder>::kValue;
  gemmlowp::MatrixMap<const LhsScalar, kGemmlowpLhsOrder> gemmlowp_lhs;
  gemmlowp::MatrixMap<const RhsScalar, kGemmlowpRhsOrder> gemmlowp_rhs;
  gemmlowp::MatrixMap<DstScalar, kGemmlowpDstOrder> gemmlowp_dst;
  WrapGemmlowp(lhs, &gemmlowp_lhs);
  WrapGemmlowp(rhs, &gemmlowp_rhs);
  WrapGemmlowpMutable(dst, &gemmlowp_dst);

  gemmlowp::OutputStageScaleInt32ByFixedPointAndExponent quantize_down_stage;
  quantize_down_stage.result_offset_after_shift = dst->zero_point;
  quantize_down_stage.result_fixedpoint_multiplier = spec.multiplier_fixedpoint;
  quantize_down_stage.result_exponent = spec.multiplier_exponent;
  gemmlowp::OutputStageScaleInt32ByFixedPointAndExponentPC<
      gemmlowp::VectorShape::Col>
      quantize_down_stage_pc;
  quantize_down_stage_pc.result_offset_after_shift = dst->zero_point;
  using ColVectorMap =
      gemmlowp::VectorMap<const std::int32_t, gemmlowp::VectorShape::Col>;
  quantize_down_stage_pc.result_fixedpoint_multiplier =
      ColVectorMap(spec.multiplier_fixedpoint_perchannel, lhs.layout.rows);
  quantize_down_stage_pc.result_exponent =
      ColVectorMap(spec.multiplier_exponent_perchannel, lhs.layout.rows);

  gemmlowp::OutputStageClamp clamp_stage;
  clamp_stage.min = spec.clamp_min;
  clamp_stage.max = spec.clamp_max;
  using OutputStageSaturatingCast = typename std::conditional<
      std::is_same<DstScalar, std::uint8_t>::value,
      gemmlowp::OutputStageSaturatingCastToUint8,
      gemmlowp::OutputStageSaturatingCastToInt16>::type;
  OutputStageSaturatingCast saturating_cast_stage;

  GlobalGemmlowpContext().set_max_num_threads(max_num_threads ? max_num_threads
                                                              : 1);
  if (spec.bias) {
    using ColVectorMap =
        gemmlowp::VectorMap<const std::int32_t, gemmlowp::VectorShape::Col>;
    gemmlowp::OutputStageBiasAddition<ColVectorMap> bias_add_stage;
    bias_add_stage.bias_vector = ColVectorMap(spec.bias, dst->layout.rows);
#ifndef GEMMLOWP_SSE4  // gemmlowp perchannel stuff does not build on SSE
    if (spec.multiplier_exponent_perchannel) {
      const auto& output_pipeline =
          std::make_tuple(bias_add_stage, quantize_down_stage_pc, clamp_stage,
                          saturating_cast_stage);
      gemmlowp::GemmWithOutputPipeline<
          LhsScalar, DstScalar, gemmlowp::L8R8WithLhsNonzeroBitDepthParams>(
          &GlobalGemmlowpContext(), gemmlowp_lhs, gemmlowp_rhs, &gemmlowp_dst,
          -lhs.zero_point, -rhs.zero_point, output_pipeline);
    } else
#endif
    {
      const auto& output_pipeline =
          std::make_tuple(bias_add_stage, quantize_down_stage, clamp_stage,
                          saturating_cast_stage);
      gemmlowp::GemmWithOutputPipeline<
          LhsScalar, DstScalar, gemmlowp::L8R8WithLhsNonzeroBitDepthParams>(
          &GlobalGemmlowpContext(), gemmlowp_lhs, gemmlowp_rhs, &gemmlowp_dst,
          -lhs.zero_point, -rhs.zero_point, output_pipeline);
    }
  } else {
#ifndef GEMMLOWP_SSE4  // gemmlowp perchannel stuff does not build on SSE
    if (spec.multiplier_exponent_perchannel) {
      const auto& output_pipeline = std::make_tuple(
          quantize_down_stage_pc, clamp_stage, saturating_cast_stage);
      gemmlowp::GemmWithOutputPipeline<
          LhsScalar, DstScalar, gemmlowp::L8R8WithLhsNonzeroBitDepthParams>(
          &GlobalGemmlowpContext(), gemmlowp_lhs, gemmlowp_rhs, &gemmlowp_dst,
          -lhs.zero_point, -rhs.zero_point, output_pipeline);
    } else
#endif
    {
      const auto& output_pipeline = std::make_tuple(
          quantize_down_stage, clamp_stage, saturating_cast_stage);
      gemmlowp::GemmWithOutputPipeline<
          LhsScalar, DstScalar, gemmlowp::L8R8WithLhsNonzeroBitDepthParams>(
          &GlobalGemmlowpContext(), gemmlowp_lhs, gemmlowp_rhs, &gemmlowp_dst,
          -lhs.zero_point, -rhs.zero_point, output_pipeline);
    }
  }
}

inline constexpr int Mash(Order LhsOrder, Order RhsOrder, Order DstOrder) {
  return (LhsOrder == Order::kRowMajor ? 4 : 0) +
         (RhsOrder == Order::kRowMajor ? 2 : 0) +
         (DstOrder == Order::kRowMajor ? 1 : 0);
}

template <typename LhsScalar, typename RhsScalar, typename DstScalar,
          typename Spec>
void EvalGemmlowp(const Matrix<LhsScalar>& lhs, const Matrix<RhsScalar>& rhs,
                  const Spec& spec, int max_num_threads,
                  Matrix<DstScalar>* dst) {
  int index = Mash(lhs.layout.order, rhs.layout.order, dst->layout.order);
  switch (index) {
#define EVALGEMMLOWP_CASE3(LHS, RHS, DST) \
  case Mash(LHS, RHS, DST):               \
    return EvalGemmlowp<LHS, RHS, DST>(lhs, rhs, spec, max_num_threads, dst);
#define EVALGEMMLOWP_CASE2(LHS, RHS)             \
  EVALGEMMLOWP_CASE3(LHS, RHS, Order::kColMajor) \
  EVALGEMMLOWP_CASE3(LHS, RHS, Order::kRowMajor)
#define EVALGEMMLOWP_CASE1(LHS)             \
  EVALGEMMLOWP_CASE2(LHS, Order::kColMajor) \
  EVALGEMMLOWP_CASE2(LHS, Order::kRowMajor)

    EVALGEMMLOWP_CASE1(Order::kColMajor)
    EVALGEMMLOWP_CASE1(Order::kRowMajor)

#undef EVALGEMMLOWP_CASE1
#undef EVALGEMMLOWP_CASE2
#undef EVALGEMMLOWP_CASE3

    default:
      RUY_CHECK(false);
  }
}

template <Order tOrder>
struct EigenOrder {};

template <>
struct EigenOrder<Order::kColMajor> {
  static constexpr int kValue = Eigen::ColMajor;
};

template <>
struct EigenOrder<Order::kRowMajor> {
  static constexpr int kValue = Eigen::RowMajor;
};

template <Order LhsOrder, Order RhsOrder, Order DstOrder, typename LhsScalar,
          typename RhsScalar, typename DstScalar, typename Spec>
void EvalEigen(const Matrix<LhsScalar>& lhs, const Matrix<RhsScalar>& rhs,
               const Spec& spec, int max_num_threads, Matrix<DstScalar>* dst) {
  RUY_CHECK_EQ(lhs.zero_point, 0);
  RUY_CHECK_EQ(rhs.zero_point, 0);
  RUY_CHECK_EQ(dst->zero_point, 0);
  RUY_CHECK_EQ(spec.multiplier_fixedpoint, 0);
  RUY_CHECK_EQ(spec.multiplier_exponent, 0);

  static constexpr int kEigenLhsOrder = EigenOrder<LhsOrder>::kValue;
  static constexpr int kEigenRhsOrder = EigenOrder<RhsOrder>::kValue;
  static constexpr int kEigenDstOrder = EigenOrder<DstOrder>::kValue;

  using EigenLhsType = typename Eigen::Matrix<LhsScalar, Eigen::Dynamic,
                                              Eigen::Dynamic, kEigenLhsOrder>::
      template StridedConstMapType<Eigen::OuterStride<Eigen::Dynamic>>::type;
  using EigenRhsType = typename Eigen::Matrix<RhsScalar, Eigen::Dynamic,
                                              Eigen::Dynamic, kEigenRhsOrder>::
      template StridedConstMapType<Eigen::OuterStride<Eigen::Dynamic>>::type;
  using EigenDstType = typename Eigen::Matrix<DstScalar, Eigen::Dynamic,
                                              Eigen::Dynamic, kEigenDstOrder>::
      template StridedMapType<Eigen::OuterStride<Eigen::Dynamic>>::type;
  using EigenBiasType =
      typename Eigen::Matrix<DstScalar, Eigen::Dynamic, 1>::ConstMapType;

  EigenLhsType eigen_lhs(lhs.data.get(), lhs.layout.rows, lhs.layout.cols,
                         Eigen::OuterStride<Eigen::Dynamic>(lhs.layout.stride));
  EigenRhsType eigen_rhs(rhs.data.get(), rhs.layout.rows, rhs.layout.cols,
                         Eigen::OuterStride<Eigen::Dynamic>(rhs.layout.stride));
  EigenDstType eigen_dst(
      dst->data.get(), dst->layout.rows, dst->layout.cols,
      Eigen::OuterStride<Eigen::Dynamic>(dst->layout.stride));
  Eigen::setNbThreads(max_num_threads ? max_num_threads : 1);

  if (spec.bias) {
    EigenBiasType eigen_bias(spec.bias, dst->layout.rows);
    if (spec.clamp_max == std::numeric_limits<DstScalar>::infinity() &&
        spec.clamp_min == -std::numeric_limits<DstScalar>::infinity()) {
      eigen_dst.noalias() = (eigen_lhs * eigen_rhs).colwise() + eigen_bias;
    } else {
      eigen_dst.noalias() = ((eigen_lhs * eigen_rhs).colwise() + eigen_bias)
                                .cwiseMin(spec.clamp_max)
                                .cwiseMax(spec.clamp_min);
    }
  } else {
    if (spec.clamp_max == std::numeric_limits<DstScalar>::infinity() &&
        spec.clamp_min == -std::numeric_limits<DstScalar>::infinity()) {
      eigen_dst.noalias() = eigen_lhs * eigen_rhs;
    } else {
      eigen_dst.noalias() = (eigen_lhs * eigen_rhs)
                                .cwiseMin(spec.clamp_max)
                                .cwiseMax(spec.clamp_min);
    }
  }
}

template <typename LhsScalar, typename RhsScalar, typename DstScalar,
          typename Spec>
void EvalEigen(const Matrix<LhsScalar>& lhs, const Matrix<RhsScalar>& rhs,
               const Spec& spec, int max_num_threads, Matrix<DstScalar>* dst) {
  int index = Mash(lhs.layout.order, rhs.layout.order, dst->layout.order);
  switch (index) {
#define EVALEIGEN_CASE3(LHS, RHS, DST) \
  case Mash(LHS, RHS, DST):            \
    return EvalEigen<LHS, RHS, DST>(lhs, rhs, spec, max_num_threads, dst);
#define EVALEIGEN_CASE2(LHS, RHS)             \
  EVALEIGEN_CASE3(LHS, RHS, Order::kColMajor) \
  EVALEIGEN_CASE3(LHS, RHS, Order::kRowMajor)
#define EVALEIGEN_CASE1(LHS)             \
  EVALEIGEN_CASE2(LHS, Order::kColMajor) \
  EVALEIGEN_CASE2(LHS, Order::kRowMajor)

    EVALEIGEN_CASE1(Order::kColMajor)
    EVALEIGEN_CASE1(Order::kRowMajor)

#undef EVALEIGEN_CASE1
#undef EVALEIGEN_CASE2
#undef EVALEIGEN_CASE3

    default:
      RUY_CHECK(false);
  }
}

template <Order LhsOrder, Order RhsOrder, Order DstOrder, typename Scalar,
          typename Spec>
void EvalEigenTensor(const Matrix<Scalar>& lhs, const Matrix<Scalar>& rhs,
                     const Spec& spec, int max_num_threads,
                     Matrix<Scalar>* dst) {
  RUY_CHECK_EQ(lhs.zero_point, 0);
  RUY_CHECK_EQ(rhs.zero_point, 0);
  RUY_CHECK_EQ(dst->zero_point, 0);
  RUY_CHECK_EQ(spec.multiplier_fixedpoint, 0);
  RUY_CHECK_EQ(spec.multiplier_exponent, 0);

  // Eigen::TensorMap only supports packed layouts
  RUY_CHECK(IsPacked(lhs.layout));
  RUY_CHECK(IsPacked(rhs.layout));
  RUY_CHECK(IsPacked(dst->layout));

  using TensorLhsType =
      Eigen::TensorMap<Eigen::Tensor<const Scalar, 2, Eigen::ColMajor>>;
  using TensorRhsType =
      Eigen::TensorMap<Eigen::Tensor<const Scalar, 2, Eigen::ColMajor>>;
  using TensorDstType =
      Eigen::TensorMap<Eigen::Tensor<Scalar, 2, Eigen::ColMajor>>;
  using TensorBiasType =
      Eigen::TensorMap<Eigen::Tensor<const Scalar, 1, Eigen::ColMajor>>;

  const bool tr = DstOrder == Order::kRowMajor;
  const auto& contract_lhs = tr ? rhs : lhs;
  const auto& contract_rhs = tr ? lhs : rhs;

  TensorLhsType tensor_lhs(
      contract_lhs.data.get(),
      LhsOrder == Order::kColMajor ? contract_lhs.layout.rows
                                   : contract_lhs.layout.cols,
      LhsOrder == Order::kColMajor ? contract_lhs.layout.cols
                                   : contract_lhs.layout.rows);
  TensorRhsType tensor_rhs(
      contract_rhs.data.get(),
      RhsOrder == Order::kColMajor ? contract_rhs.layout.rows
                                   : contract_rhs.layout.cols,
      RhsOrder == Order::kColMajor ? contract_rhs.layout.cols
                                   : contract_rhs.layout.rows);
  TensorDstType tensor_dst(
      dst->data.get(),
      DstOrder == Order::kColMajor ? dst->layout.rows : dst->layout.cols,
      DstOrder == Order::kColMajor ? dst->layout.cols : dst->layout.rows);
  using DimPair =
      typename Eigen::Tensor<Scalar, 1, 0, Eigen::Index>::DimensionPair;
  Eigen::array<DimPair, 1> contract_dims(
      {DimPair((LhsOrder == Order::kColMajor) ? 1 : 0,
               (RhsOrder == Order::kColMajor) ? 0 : 1)});
  Eigen::array<int, 2> shuffle(DstOrder == Order::kColMajor ? 0 : 1,
                               DstOrder == Order::kColMajor ? 1 : 0);
  static Eigen::ThreadPool pool(max_num_threads ? max_num_threads : 1);
  static Eigen::ThreadPoolDevice device(&pool, pool.NumThreads());
  if (spec.bias) {
    TensorBiasType tensor_bias(spec.bias, dst->layout.rows);
    Eigen::array<int, 2> bias_2d_shape(tr ? 1 : dst->layout.rows,
                                       tr ? dst->layout.rows : 1);
    Eigen::array<int, 2> bcast(tr ? dst->layout.cols : 1,
                               tr ? 1 : dst->layout.cols);
    if (spec.clamp_max == std::numeric_limits<Scalar>::infinity() &&
        spec.clamp_min == -std::numeric_limits<Scalar>::infinity()) {
      tensor_dst.device(device) =
          tensor_lhs.contract(tensor_rhs, contract_dims);
    } else {
      tensor_dst.device(device) =
          (tensor_lhs.contract(tensor_rhs, contract_dims) +
           tensor_bias.reshape(bias_2d_shape).broadcast(bcast))
              .cwiseMin(spec.clamp_max)
              .cwiseMax(spec.clamp_min);
    }
  } else {
    if (spec.clamp_max == std::numeric_limits<Scalar>::infinity() &&
        spec.clamp_min == -std::numeric_limits<Scalar>::infinity()) {
      tensor_dst.device(device) =
          tensor_lhs.contract(tensor_rhs, contract_dims);
    } else {
      tensor_dst.device(device) = tensor_lhs.contract(tensor_rhs, contract_dims)
                                      .cwiseMin(spec.clamp_max)
                                      .cwiseMax(spec.clamp_min);
    }
  }
}

template <typename Scalar, typename Spec>
void EvalEigenTensor(const Matrix<Scalar>& lhs, const Matrix<Scalar>& rhs,
                     const Spec& spec, int max_num_threads,
                     Matrix<Scalar>* dst) {
  int index = Mash(lhs.layout.order, rhs.layout.order, dst->layout.order);
  switch (index) {
#define EVALEIGENTENSOR_CASE3(LHS, RHS, DST) \
  case Mash(LHS, RHS, DST):                  \
    return EvalEigenTensor<LHS, RHS, DST>(lhs, rhs, spec, max_num_threads, dst);
#define EVALEIGENTENSOR_CASE2(LHS, RHS)             \
  EVALEIGENTENSOR_CASE3(LHS, RHS, Order::kColMajor) \
  EVALEIGENTENSOR_CASE3(LHS, RHS, Order::kRowMajor)
#define EVALEIGENTENSOR_CASE1(LHS)             \
  EVALEIGENTENSOR_CASE2(LHS, Order::kColMajor) \
  EVALEIGENTENSOR_CASE2(LHS, Order::kRowMajor)

    EVALEIGENTENSOR_CASE1(Order::kColMajor)
    EVALEIGENTENSOR_CASE1(Order::kRowMajor)

#undef EVALEIGENTENSOR_CASE1
#undef EVALEIGENTENSOR_CASE2
#undef EVALEIGENTENSOR_CASE3

    default:
      RUY_CHECK(false);
  }
}

template <typename Scalar>
struct GenericBlasGemm {};

template <>
struct GenericBlasGemm<lapack::doublereal> {
  static void Run(char* transa, char* transb, lapack::integer* m,
                  lapack::integer* n, lapack::integer* k,
                  lapack::doublereal* alpha, lapack::doublereal* a,
                  lapack::integer* lda, lapack::doublereal* b,
                  lapack::integer* ldb, lapack::doublereal* beta,
                  lapack::doublereal* c, lapack::integer* ldc) {
    dgemm_(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
  }
};

template <>
struct GenericBlasGemm<lapack::real> {
  static void Run(char* transa, char* transb, lapack::integer* m,
                  lapack::integer* n, lapack::integer* k, lapack::real* alpha,
                  lapack::real* a, lapack::integer* lda, lapack::real* b,
                  lapack::integer* ldb, lapack::real* beta, lapack::real* c,
                  lapack::integer* ldc) {
    sgemm_(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
  }
};

template <typename Scalar, typename Spec>
void EvalOpenBlas(const Matrix<Scalar>& lhs, const Matrix<Scalar>& rhs,
                  const Spec& spec, int max_num_threads, Matrix<Scalar>* dst) {
  RUY_CHECK_EQ(lhs.zero_point, 0);
  RUY_CHECK_EQ(rhs.zero_point, 0);
  RUY_CHECK_EQ(dst->zero_point, 0);
  RUY_CHECK_EQ(spec.multiplier_fixedpoint, 0);
  RUY_CHECK_EQ(spec.multiplier_exponent, 0);

  Matrix<Scalar> gemm_lhs;
  Matrix<Scalar> gemm_rhs;
  Matrix<Scalar> gemm_dst;
  gemm_dst = *dst;

  // Use Transpose to reduce to the all-column-major case.
  // Notice that ruy::Matrix merely holds a pointer, does not own data,
  // so Transpose is cheap -- no actual matrix data is being transposed here.
  if (dst->layout.order == Order::kColMajor) {
    gemm_lhs = lhs;
    gemm_rhs = rhs;
  } else {
    gemm_lhs = rhs;
    gemm_rhs = lhs;
    Transpose(&gemm_lhs);
    Transpose(&gemm_rhs);
    Transpose(&gemm_dst);
  }
  bool transposed_lhs = false;
  bool transposed_rhs = false;

  if (gemm_lhs.layout.order == Order::kRowMajor) {
    Transpose(&gemm_lhs);
    transposed_lhs = true;
  }
  if (gemm_rhs.layout.order == Order::kRowMajor) {
    Transpose(&gemm_rhs);
    transposed_rhs = true;
  }

  RUY_CHECK(gemm_lhs.layout.order == Order::kColMajor);
  RUY_CHECK(gemm_rhs.layout.order == Order::kColMajor);
  RUY_CHECK(gemm_dst.layout.order == Order::kColMajor);

  char transa = transposed_lhs ? 'T' : 'N';
  char transb = transposed_rhs ? 'T' : 'N';
  int m = gemm_lhs.layout.rows;
  int n = gemm_rhs.layout.cols;
  int k = gemm_lhs.layout.cols;
  float alpha = 1;
  Scalar* a = gemm_lhs.data.get();
  int lda = gemm_lhs.layout.stride;
  Scalar* b = gemm_rhs.data.get();
  int ldb = gemm_rhs.layout.stride;
  float beta = 0;
  Scalar* c = gemm_dst.data.get();
  int ldc = gemm_dst.layout.stride;
  GenericBlasGemm<Scalar>::Run(&transa, &transb, &m, &n, &k, &alpha, a, &lda, b,
                               &ldb, &beta, c, &ldc);

  // BLAS does not allow us to express the bias-addition and clamping, so
  // we use Eigen for that.

  using EigenDstType =
      typename Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>::
          template StridedMapType<Eigen::OuterStride<Eigen::Dynamic>>::type;
  using EigenBiasType =
      typename Eigen::Matrix<Scalar, Eigen::Dynamic, 1>::ConstMapType;

  EigenDstType eigen_dst(
      gemm_dst.data.get(), gemm_dst.layout.rows, gemm_dst.layout.cols,
      Eigen::OuterStride<Eigen::Dynamic>(gemm_dst.layout.stride));
  Eigen::setNbThreads(max_num_threads ? max_num_threads : 1);

  if (spec.bias) {
    EigenBiasType eigen_bias(spec.bias, dst->layout.rows);
    if (spec.clamp_max == std::numeric_limits<Scalar>::infinity() &&
        spec.clamp_min == -std::numeric_limits<Scalar>::infinity()) {
      eigen_dst.noalias() = eigen_dst.colwise() + eigen_bias;
    } else {
      eigen_dst.noalias() = (eigen_dst.colwise() + eigen_bias)
                                .cwiseMin(spec.clamp_max)
                                .cwiseMax(spec.clamp_min);
    }
  } else {
    if (spec.clamp_max == std::numeric_limits<Scalar>::infinity() &&
        spec.clamp_min == -std::numeric_limits<Scalar>::infinity()) {
    } else {
      eigen_dst.noalias() =
          eigen_dst.cwiseMin(spec.clamp_max).cwiseMax(spec.clamp_min);
    }
  }
}

template <typename TestSetType>
struct SupportsGemmlowp {
  static constexpr bool kValue =
      std::is_same<typename TestSetType::LhsScalar, std::uint8_t>::value &&
      std::is_same<typename TestSetType::RhsScalar, std::uint8_t>::value;
};

template <typename TestSetType>
struct UsesSingleScalarType {
  static constexpr bool kValue =
      std::is_same<typename TestSetType::DstScalar,
                   typename TestSetType::LhsScalar>::value &&
      std::is_same<typename TestSetType::DstScalar,
                   typename TestSetType::RhsScalar>::value &&
      std::is_same<typename TestSetType::DstScalar,
                   typename TestSetType::AccumScalar>::value;
};

template <typename TestSetType,
          bool IsFloatingPoint =
              std::is_floating_point<typename TestSetType::AccumScalar>::value,
          bool EnableGemmlowp = SupportsGemmlowp<TestSetType>::kValue,
          bool SingleScalarType = UsesSingleScalarType<TestSetType>::kValue>
struct EvalExternalPathImpl {
  using DstScalar = typename TestSetType::DstScalar;
  static void Run(TestSetType*, TestResult<DstScalar>*) { RUY_CHECK(false); }
};

template <typename TestSetType>
struct EvalExternalPathImpl<TestSetType, true, false, true> {
  using DstScalar = typename TestSetType::DstScalar;
  static void Run(TestSetType* test_set, TestResult<DstScalar>* test_result) {
    if (test_result->external_path == ExternalPath::kEigen) {
      EvalEigen(test_set->lhs.matrix, test_set->rhs.matrix, test_set->spec,
                test_set->max_num_threads, &test_result->storage_matrix.matrix);
    } else if (test_result->external_path == ExternalPath::kEigenTensor) {
      EvalEigenTensor(test_set->lhs.matrix, test_set->rhs.matrix,
                      test_set->spec, test_set->max_num_threads,
                      &test_result->storage_matrix.matrix);
    } else if (test_result->external_path == ExternalPath::kOpenBlas) {
      EvalOpenBlas(test_set->lhs.matrix, test_set->rhs.matrix, test_set->spec,
                   test_set->max_num_threads,
                   &test_result->storage_matrix.matrix);
    } else {
      RUY_CHECK(false);
    }
  }
};

template <typename TestSetType, bool SingleScalarType>
struct EvalExternalPathImpl<TestSetType, false, true, SingleScalarType> {
  using DstScalar = typename TestSetType::DstScalar;
  static void Run(TestSetType* test_set, TestResult<DstScalar>* test_result) {
    if (test_result->external_path == ExternalPath::kGemmlowp) {
      EvalGemmlowp(test_set->lhs.matrix, test_set->rhs.matrix, test_set->spec,
                   test_set->max_num_threads,
                   &test_result->storage_matrix.matrix);
    } else {
      RUY_CHECK(false);
    }
  }
};

template <typename TestSetType>
void EvalExternalPath(
    TestSetType* test_set,
    TestResult<typename TestSetType::DstScalar>* test_result) {
  EvalExternalPathImpl<TestSetType>::Run(test_set, test_result);
}

#endif  // RUY_TEST_EXTERNAL_PATHS

template <typename Scalar>
bool Agree(const Matrix<Scalar>& matrix1, const Matrix<Scalar>& matrix2,
           int depth) {
  RUY_CHECK_EQ(matrix1.layout.rows, matrix2.layout.rows);
  RUY_CHECK_EQ(matrix1.layout.cols, matrix2.layout.cols);
  RUY_CHECK_EQ(matrix1.zero_point, matrix2.zero_point);
  const int size = matrix1.layout.rows * matrix1.layout.cols;
  double tolerated_max_diff = 0;
  double tolerated_mean_diff = 0;
  if (std::is_floating_point<Scalar>::value) {
    // TODO: replace hardcoded 100 by something more sensible, probably
    // roughly sqrt(depth) based on central limit theorem.
    double max_abs_val = 0;
    for (int row = 0; row < matrix1.layout.rows; row++) {
      for (int col = 0; col < matrix1.layout.cols; col++) {
        max_abs_val =
            std::max(max_abs_val,
                     std::abs(static_cast<double>(Element(matrix1, row, col))));
        max_abs_val =
            std::max(max_abs_val,
                     std::abs(static_cast<double>(Element(matrix2, row, col))));
      }
    }
    tolerated_max_diff = max_abs_val * std::numeric_limits<Scalar>::epsilon() *
                         4 * std::sqrt(static_cast<float>(depth));
    tolerated_mean_diff = tolerated_max_diff / std::sqrt(size);
  } else if (RUY_OPT_SET & RUY_OPT_NATIVE_ROUNDING) {
    tolerated_max_diff = 1;
    // totally empirical
    tolerated_mean_diff = std::min(1.0, 2.0 * std::pow(size, -0.2));
  }
  double sum_diff = 0;
  for (int row = 0; row < matrix1.layout.rows; row++) {
    for (int col = 0; col < matrix1.layout.cols; col++) {
      double elem1 = Element(matrix1, row, col);
      double elem2 = Element(matrix2, row, col);
      double diff = elem1 - elem2;

      sum_diff += diff;
      if (std::abs(diff) > tolerated_max_diff) {
        return false;
      }
    }
  }
  double mean_diff = sum_diff / size;
  if (std::abs(mean_diff) > tolerated_mean_diff) {
    return false;
  }
  return true;
}

template <typename Scalar>
bool Agree(const StorageMatrix<Scalar>& storage_matrix1,
           const StorageMatrix<Scalar>& storage_matrix2, int depth) {
  VerifyConsistentFields(storage_matrix1);
  VerifyConsistentFields(storage_matrix2);
  return Agree(storage_matrix1.matrix, storage_matrix2.matrix, depth);
}

template <typename Scalar>
bool Agree(const TestResult<Scalar>& result1, const TestResult<Scalar>& result2,
           int depth) {
  return Agree(result1.storage_matrix, result2.storage_matrix, depth);
}

struct Stats {
  double median;
  double mean;
  double min;
  double max;
};

std::string StatsAsString(const Stats& stats) {
  char buf[256];
  snprintf(buf, sizeof(buf), "(median = %g, mean = %g, min = %g, max = %g)",
           stats.median, stats.mean, stats.min, stats.max);
  return std::string(buf);
}

template <typename Scalar>
void GetMatrixStats(const Matrix<Scalar>& matrix, Stats* stats) {
  double min = std::numeric_limits<double>::infinity();
  double max = -std::numeric_limits<double>::infinity();
  double sum = 0;
  std::vector<double> allvals;
  for (int row = 0; row < matrix.layout.rows; row++) {
    for (int col = 0; col < matrix.layout.cols; col++) {
      double val = Element(matrix, row, col);
      min = std::min(min, val);
      max = std::max(max, val);
      sum += val;
      allvals.push_back(val);
    }
  }
  std::sort(allvals.begin(), allvals.end());
  stats->min = min;
  stats->max = max;
  stats->mean = sum / allvals.size();
  stats->median = allvals[allvals.size() / 2];
}

struct ErrorAnalysis {
  Stats stats_good;
  Stats stats_bad;
  // The below is to help document departure from bit exactness. It's probably
  // not going to be relevant to floating-point.
  std::set<int> error_rows;
  std::set<int> error_cols;
  int row_of_first_error = 0;
  int col_of_first_error = 0;
  double first_error_good_value = 0;
  double first_error_bad_value = 0;
};

template <typename TestSetType>
void AnalyzeTestError(const TestSetType& test_set, int first_bad_result_index,
                      ErrorAnalysis* error_analysis) {
  const auto& good_matrix = test_set.results[0].storage_matrix.matrix;
  const auto& bad_matrix =
      test_set.results[first_bad_result_index].storage_matrix.matrix;
  GetMatrixStats(good_matrix, &error_analysis->stats_good);
  GetMatrixStats(bad_matrix, &error_analysis->stats_bad);
  bool found_first_error = false;
  for (int row = 0; row < good_matrix.layout.rows; row++) {
    for (int col = 0; col < good_matrix.layout.cols; col++) {
      if (Element(good_matrix, row, col) != Element(bad_matrix, row, col)) {
        if (!found_first_error) {
          found_first_error = true;
          error_analysis->row_of_first_error = row;
          error_analysis->col_of_first_error = col;
          error_analysis->first_error_good_value =
              Element(good_matrix, row, col);
          error_analysis->first_error_bad_value = Element(bad_matrix, row, col);
        }
        error_analysis->error_rows.insert(row);
        error_analysis->error_cols.insert(col);
      }
    }
  }
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void ComputeAccumRangeBeforeMultiplier(
    const Matrix<LhsScalar>& lhs, const Matrix<RhsScalar>& rhs,
    const SpecType& spec, typename SpecType::AccumScalar* accum_min,
    typename SpecType::AccumScalar* accum_max) {
  Context context;
  context.SetRuntimeEnabledPaths(Path::kReference);
  using AccumScalar = typename SpecType::AccumScalar;
  Matrix<AccumScalar> dst_before_multiplier;
  MakeSimpleLayout(lhs.layout.rows, rhs.layout.cols, Order::kColMajor,
                   &dst_before_multiplier.layout);
  const int size = FlatSize(dst_before_multiplier.layout);
  std::vector<AccumScalar> dst_before_multiplier_data(size);
  dst_before_multiplier.data = dst_before_multiplier_data.data();
  ruy::BasicSpec<AccumScalar, AccumScalar> spec_before_multiplier;
  spec_before_multiplier.bias = spec.bias;
  Mul<Path::kReference>(lhs, rhs, spec_before_multiplier, &context,
                        &dst_before_multiplier);
  *accum_min = *std::min_element(dst_before_multiplier_data.begin(),
                                 dst_before_multiplier_data.end());
  *accum_max = *std::max_element(dst_before_multiplier_data.begin(),
                                 dst_before_multiplier_data.end());
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void ComputeReasonableMultiplier(const Matrix<LhsScalar>& lhs,
                                 const Matrix<RhsScalar>& rhs,
                                 typename SpecType::DstScalar dst_zero_point,
                                 const SpecType& spec, double* multiplier) {
  using AccumScalar = typename SpecType::AccumScalar;
  using DstScalar = typename SpecType::DstScalar;
  if (std::is_floating_point<DstScalar>::value ||
      std::is_same<DstScalar, std::int32_t>::value) {
    *multiplier = 0;
    return;
  }
  if (getenv("QUICK_BENCHMARK")) {
    *multiplier = static_cast<double>(std::numeric_limits<DstScalar>::max()) /
                  (static_cast<double>(lhs.layout.cols) *
                   std::numeric_limits<LhsScalar>::max() *
                   std::numeric_limits<RhsScalar>::max());
    return;
  }
  AccumScalar accum_min;
  AccumScalar accum_max;
  ComputeAccumRangeBeforeMultiplier(lhs, rhs, spec, &accum_min, &accum_max);
  accum_min = std::min(accum_min, 0);
  accum_max = std::max(accum_max, 0);
  const double dst_pos_range_width =
      static_cast<double>(std::numeric_limits<DstScalar>::max()) -
      dst_zero_point;
  const double dst_neg_range_width =
      dst_zero_point -
      static_cast<double>(std::numeric_limits<DstScalar>::lowest());
  if (accum_max == 0 && accum_min == 0) {
    *multiplier = 1;
  } else if (std::abs(accum_max) * dst_pos_range_width >
             std::abs(accum_min) * dst_neg_range_width) {
    *multiplier = dst_pos_range_width / accum_max;
  } else {
    *multiplier = dst_neg_range_width / -accum_min;
  }
  RUY_CHECK_GT(*multiplier, 0.0);
}

void QuantizeMultiplier(double multiplier_double,
                        std::int32_t* multiplier_fixedpoint,
                        int* multiplier_exponent) {
  RUY_CHECK_GT(multiplier_double, 0);
  if (multiplier_double == 0.) {
    *multiplier_fixedpoint = 0;
    *multiplier_exponent = 0;
    return;
  }
  const double q = std::frexp(multiplier_double, multiplier_exponent);
  auto q_fixed = static_cast<std::int64_t>(std::round(q * (1ll << 31)));
  RUY_CHECK_LE(q_fixed, (1ll << 31));
  if (q_fixed == (1ll << 31)) {
    q_fixed /= 2;
    ++*multiplier_exponent;
  }
  RUY_CHECK_LE(q_fixed, std::numeric_limits<std::int32_t>::max());
  *multiplier_fixedpoint = static_cast<std::int32_t>(q_fixed);
}

template <typename TestSetType>
void SwitchMultiplierToPerChannel(TestSetType* test_set) {
  test_set->per_channel_multiplier_fixedpoint.resize(test_set->rows);
  test_set->per_channel_multiplier_exponent.resize(test_set->rows);
  for (int i = 0; i < test_set->rows; i++) {
    // multipliers typically range in [2^30 ; 2^31 - 1].
    // Values in [0, 2^30 - 1] are normally unused, but harmless.
    // Thus a good way to randomize multipliers is to subtract from them
    // a random value smaller than 2^30 but still significant compared to it.
    std::int32_t nudged_multiplier = test_set->spec.multiplier_fixedpoint -
                                     (global_random_engine()() % (1 << 26));
    int nudged_exponent =
        test_set->spec.multiplier_exponent - 1 + (global_random_engine()() % 4);
    test_set->per_channel_multiplier_fixedpoint[i] = nudged_multiplier;
    test_set->per_channel_multiplier_exponent[i] = nudged_exponent;
  }
  test_set->spec.multiplier_fixedpoint_perchannel =
      test_set->per_channel_multiplier_fixedpoint.data();
  test_set->spec.multiplier_exponent_perchannel =
      test_set->per_channel_multiplier_exponent.data();
  test_set->spec.multiplier_fixedpoint = 0;
  test_set->spec.multiplier_exponent = 0;
}

template <
    typename TestSetType,
    bool IsApplicable =
        std::is_same<typename TestSetType::AccumScalar, std::int32_t>::value &&
        !std::is_same<typename TestSetType::DstScalar, std::int32_t>::value>
struct MakeSpecMultiplierFieldsImpl {};

template <typename TestSetType>
struct MakeSpecMultiplierFieldsImpl<TestSetType, true> {
  static void Run(TestSetType* test_set) {
    double multiplier;
    ComputeReasonableMultiplier(test_set->lhs.matrix, test_set->rhs.matrix,
                                test_set->dst_zero_point, test_set->spec,
                                &multiplier);
    QuantizeMultiplier(multiplier, &test_set->spec.multiplier_fixedpoint,
                       &test_set->spec.multiplier_exponent);
    if (!test_set->benchmark) {
      test_set->perchannel = global_random_engine()() & 1;
    }
    if (test_set->perchannel) {
      SwitchMultiplierToPerChannel(test_set);
    }
  }
};

template <typename TestSetType>
struct MakeSpecMultiplierFieldsImpl<TestSetType, false> {
  static void Run(TestSetType* test_set) {
    test_set->spec.multiplier_fixedpoint = 0;
    test_set->spec.multiplier_exponent = 0;
  }
};

template <typename LhsScalar, typename RhsScalar, typename Spec>
void MakeSpecClampFields(const Matrix<LhsScalar>& lhs,
                         const Matrix<RhsScalar>& rhs,
                         typename Spec::DstScalar dst_zero_point, Spec* spec) {
  using AccumScalar = typename Spec::AccumScalar;
  using DstScalar = typename Spec::DstScalar;

  if (getenv("BENCHMARK_ONLY_MATMUL")) {
    spec->clamp_min = -std::numeric_limits<DstScalar>::infinity();
    spec->clamp_max = std::numeric_limits<DstScalar>::infinity();
    return;
  }

  if (getenv("QUICK_BENCHMARK")) {
    spec->clamp_min = std::numeric_limits<DstScalar>::lowest() + 1;
    spec->clamp_max = std::numeric_limits<DstScalar>::max() - 1;
    return;
  }
  Context context;
  context.SetRuntimeEnabledPaths(Path::kReference);
  Matrix<DstScalar> unclamped_dst;
  MakeSimpleLayout(lhs.layout.rows, rhs.layout.cols, Order::kColMajor,
                   &unclamped_dst.layout);
  unclamped_dst.zero_point = dst_zero_point;
  const int size = FlatSize(unclamped_dst.layout);
  std::vector<DstScalar> unclamped_dst_data(size);
  unclamped_dst.data = unclamped_dst_data.data();
  ruy::BasicSpec<AccumScalar, DstScalar> spec_unclamped;
  spec_unclamped.bias = spec->bias;
  spec_unclamped.multiplier_fixedpoint = spec->multiplier_fixedpoint;
  spec_unclamped.multiplier_exponent = spec->multiplier_exponent;
  spec_unclamped.multiplier_fixedpoint_perchannel =
      spec->multiplier_fixedpoint_perchannel;
  spec_unclamped.multiplier_exponent_perchannel =
      spec->multiplier_exponent_perchannel;
  Mul<Path::kReference>(lhs, rhs, spec_unclamped, &context, &unclamped_dst);
  // If dst is std::int32_t, no need to set the clamp min/max.
  if (!std::is_same<typename Spec::DstScalar, std::int32_t>::value) {
    std::sort(unclamped_dst_data.begin(), unclamped_dst_data.end());
    const int clamp_count = static_cast<int>(std::floor(kClampRatio * size));
    RUY_CHECK_LT(clamp_count, size);
    spec->clamp_min = unclamped_dst_data[clamp_count];
    spec->clamp_max = unclamped_dst_data[size - 1 - clamp_count];
  }
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::MakeZeroPoints() {
  RUY_CHECK(life_stage == LifeStage::kInitial);
  if (!use_specified_zero_points) {
    MakeRandomScalar(RandomRange::kReasonableSrcZeroPoint, &lhs_zero_point);
    MakeRandomScalar(RandomRange::kReasonableSrcZeroPoint, &rhs_zero_point);
    // If destination is std::int32_t, no dst_zero_point is necessary.
    if (std::is_same<DstScalar, std::int32_t>::value) {
      dst_zero_point = 0;
    } else {
      MakeRandomScalar(RandomRange::kReasonableDstZeroPoint, &dst_zero_point);
    }
  }
  life_stage = LifeStage::kHasZeroPoints;
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::MakeLhsRhs() {
  RUY_CHECK(life_stage == LifeStage::kHasZeroPoints);
  MakeRandom(rows, depth, lhs_order, lhs_zero_point, layout_style,
             RandomRange::kAvoidMinValue, &lhs);
  MakeRandom(depth, cols, rhs_order, rhs_zero_point, layout_style,
             RandomRange::kGeneral, &rhs);
  life_stage = LifeStage::kHasLhsRhs;
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::MakeSpec() {
  RUY_CHECK(life_stage == LifeStage::kHasLhsRhs);

  if (!getenv("BENCHMARK_ONLY_MATMUL") && (global_random_engine()() & 1)) {
    MakeRandomVector(RandomRange::kBias, rows, &bias_data);
    spec.bias = bias_data.data();
  }
  MakeSpecMultiplierFieldsImpl<TestSet>::Run(this);
  MakeSpecClampFields(lhs.matrix, rhs.matrix, dst_zero_point, &spec);
  life_stage = LifeStage::kHasSpec;
}

inline int GetIntEnvVarOrZero(const char* name) {
  const char* val = getenv(name);
  if (!val) {
    return 0;
  }
  return std::stoi(val);
}

inline int GetHexIntEnvVarOrZero(const char* name) {
  const char* val = getenv(name);
  if (!val) {
    return 0;
  }
  return std::stoi(val, 0, 16);
}

inline bool GetBoolEnvVarOrFalse(const char* name) {
  return static_cast<bool>(GetIntEnvVarOrZero(name));
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::MakeOtherParams() {
  RUY_CHECK(life_stage == LifeStage::kHasSpec);
  if (max_num_threads == 0) {
    max_num_threads = GetIntEnvVarOrZero("THREADS");
  }
  life_stage = LifeStage::kHasOtherParams;
}

std::vector<Path> PathsBitfieldAsVector(Path paths_bitfield) {
  std::vector<Path> result;
  std::uint32_t remaining_paths = static_cast<std::uint32_t>(paths_bitfield);
  std::uint32_t test_bit = 1;
  while (remaining_paths) {
    if (remaining_paths & test_bit) {
      result.push_back(static_cast<Path>(test_bit));
    }
    remaining_paths &= ~test_bit;
    test_bit <<= 1;
  }
  return result;
}

std::vector<Tuning> EnumerateTuningsForPath(Path path, bool benchmark) {
  if (benchmark) {
    return {Tuning::kAuto};
  }
  if (path == Path::kNeon || path == Path::kNeonDotprod) {
    return {Tuning::kInOrder, Tuning::kOutOfOrder, Tuning::kAuto};
  }
  return {Tuning::kAuto};
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::MakeResultPaths() {
  RUY_CHECK(life_stage == LifeStage::kHasOtherParams);

  Path paths_bitfield = static_cast<Path>(GetHexIntEnvVarOrZero("PATHS"));

  if (paths_bitfield == Path::kNone) {
    // Use a dummy Context just to perform the resolution of specific runtime
    // enabled paths.
    Context context;
    paths_bitfield = context.GetRuntimeEnabledPaths();
  }

  // Trim bits that don't correspond to a compiled path,
  // to allow specifying e.g. ffff to mean 'all paths' regardless of whether all
  // those bits exist as actual paths.
  paths_bitfield = paths_bitfield & kAllPaths;
  paths = PathsBitfieldAsVector(paths_bitfield);

#ifdef RUY_TEST_EXTERNAL_PATHS

  using TestSetType = TestSet<LhsScalar, RhsScalar, SpecType>;

  if (!getenv("NOEXT")) {
    if (SupportsGemmlowp<TestSetType>::kValue) {
#ifdef GEMMLOWP_SSE4
      const bool gemmlowp_supported = !spec.multiplier_fixedpoint_perchannel;
#else
      const bool gemmlowp_supported = true;
#endif
      if (gemmlowp_supported) {
        external_paths.push_back(ExternalPath::kGemmlowp);
      }
    }
    if (UsesSingleScalarType<TestSetType>::kValue &&
        std::is_floating_point<AccumScalar>::value) {
      external_paths.push_back(ExternalPath::kEigen);
      if (layout_style == LayoutStyle::kPackedLinear) {
        external_paths.push_back(ExternalPath::kEigenTensor);
      }
// We link against a generic BLAS target that only maps to OpenBLAS on specific
// architectures.
#if defined __aarch64__ || defined __arm__
      // OpenBLAS multi-threading is disabled, so avoid mixing single-threaded
      // and multi-threaded benchmark results.
      if (max_num_threads == 1) {
        external_paths.push_back(ExternalPath::kOpenBlas);
      }
#endif
    }
  }

#endif  // RUY_TEST_EXTERNAL_PATHS

  for (Path path : paths) {
    for (Tuning tuning : EnumerateTuningsForPath(path, benchmark)) {
      results.emplace_back();
      TestResult<DstScalar>& result = results.back();
      result.path = path;
      result.tuning = tuning;
      MakeRandom(rows, cols, dst_order, dst_zero_point, layout_style,
                 RandomRange::kGeneral, &result.storage_matrix);
    }
  }

  for (ExternalPath external_path : external_paths) {
    results.emplace_back();
    TestResult<DstScalar>& result = results.back();
    result.external_path = external_path;
    MakeRandom(rows, cols, dst_order, dst_zero_point, layout_style,
               RandomRange::kGeneral, &result.storage_matrix);
  }

  life_stage = LifeStage::kHasResultPaths;
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::EvalResult(
    TestResult<typename SpecType::DstScalar>* result) {
  RUY_CHECK(result->path != Path::kNone ||
            result->external_path != ExternalPath::kNone);
  if (result->path != Path::kNone) {
    EvalRuy(result->path, result->tuning, lhs.matrix, rhs.matrix, spec,
            &result->storage_matrix.matrix, expected_outcome, benchmark,
            max_num_threads);
  } else {
#ifdef RUY_TEST_EXTERNAL_PATHS
    using TestSetType = TestSet<LhsScalar, RhsScalar, SpecType>;
    EvalExternalPath(this, result);
#endif
  }
  const std::string& pathname = PathName(*result);
  if (std::find(CoveredPaths()->begin(), CoveredPaths()->end(), pathname) ==
      CoveredPaths()->end()) {
    CoveredPaths()->push_back(pathname);
  }
}

using f32 = float;
using f64 = double;
using u8 = std::uint8_t;
using i8 = std::int8_t;
using u16 = std::uint16_t;
using i16 = std::int16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

template <typename Scalar>
const char* TypeName() {
  return nullptr;
}

#define RUY_TYPENAME(TYPE)       \
  template <>                    \
  const char* TypeName<TYPE>() { \
    return #TYPE;                \
  }

RUY_TYPENAME(f32)
RUY_TYPENAME(f64)
RUY_TYPENAME(u8)
RUY_TYPENAME(i8)
RUY_TYPENAME(u16)
RUY_TYPENAME(i16)
RUY_TYPENAME(u32)
RUY_TYPENAME(i32)
RUY_TYPENAME(u64)
RUY_TYPENAME(i64)

#undef RUY_TYPENAME

template <typename Scalar>
const char* SymmetryName(const Matrix<Scalar>& matrix) {
  if (matrix.zero_point == SymmetricZeroPoint<Scalar>()) {
    return "symm";
  } else {
    return "asymm";
  }
}

template <typename Scalar>
int StorageSize(const Matrix<Scalar>& matrix) {
  return sizeof(Scalar) * FlatSize(matrix.layout);
}

template <typename Scalar>
void MakeColdData(int num_copies, const Matrix<Scalar>& matrix,
                  std::vector<Scalar>* cold_data) {
  const int flatsize = FlatSize(matrix.layout);
  cold_data->resize(num_copies * flatsize);
  for (int i = 0; i < num_copies; i++) {
    memcpy(cold_data->data() + i * flatsize, matrix.data.get(),
           sizeof(Scalar) * flatsize);
  }
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::Benchmark(
    TestResult<typename SpecType::DstScalar>* result) {
  using DstScalar = typename SpecType::DstScalar;

  const bool cold = getenv("RUY_BENCHMARK_COLD");
  const LhsScalar* orig_lhs_data = nullptr;
  const RhsScalar* orig_rhs_data = nullptr;
  DstScalar* orig_dst_data = nullptr;
  std::vector<LhsScalar> cold_lhs_data;
  std::vector<RhsScalar> cold_rhs_data;
  std::vector<DstScalar> cold_dst_data;
  int num_matmul_sets = 0;

  if (cold) {
    const int kWorkingSetSize = 100 << 20;
    const int each_matmul_set_size = StorageSize(lhs.matrix) +
                                     StorageSize(rhs.matrix) +
                                     StorageSize(result->storage_matrix.matrix);
    num_matmul_sets =
        (kWorkingSetSize + each_matmul_set_size - 1) / each_matmul_set_size;

    MakeColdData(num_matmul_sets, lhs.matrix, &cold_lhs_data);
    MakeColdData(num_matmul_sets, rhs.matrix, &cold_rhs_data);
    MakeColdData(num_matmul_sets, result->storage_matrix.matrix,
                 &cold_dst_data);

    orig_lhs_data = lhs.matrix.data.get();
    orig_rhs_data = rhs.matrix.data.get();
    orig_dst_data = result->storage_matrix.matrix.data.get();
  }
  int kRepeats = 4;
  const double kBenchmarkMinSecs = 0.5;

#ifdef GEMMLOWP_PROFILING
  const char* lhstype = TypeName<LhsScalar>();
  const char* lhssymm = SymmetryName(lhs.matrix);
  const char* rhstype = TypeName<RhsScalar>();
  const char* rhssymm = SymmetryName(rhs.matrix);

  printf("Profiling path=%s shape=(%dx%dx%d) lhs=(%s,%s) rhs=(%s,%s)\n",
         PathName(*result).c_str(), rows, depth, cols, lhstype, lhssymm,
         rhstype, rhssymm);
  gemmlowp::RegisterCurrentThreadForProfiling();
  gemmlowp::StartProfiling();
#endif

  double latency = std::numeric_limits<double>::infinity();
  int data_index = 0;
  const bool record_pmu = getenv("RUY_BENCHMARK_PMU");
  for (int repeat = 0; repeat < kRepeats; repeat++) {
    PmuEvents pmu_events;
    if (record_pmu && repeat == kRepeats - 1) {
      pmu_events.StartRecording();
    }
    TimePoint time_start = Clock::now();
    TimePoint t = time_start;
    int iters = 0;
    int iters_at_a_time = 1;
    while (ToSeconds(t - time_start) < kBenchmarkMinSecs) {
      for (int i = 0; i < iters_at_a_time; i++) {
        if (cold) {
          lhs.matrix.data =
              cold_lhs_data.data() + data_index * FlatSize(lhs.matrix.layout);
          rhs.matrix.data =
              cold_rhs_data.data() + data_index * FlatSize(rhs.matrix.layout);
          result->storage_matrix.matrix.data =
              cold_dst_data.data() +
              data_index * FlatSize(result->storage_matrix.matrix.layout);
          data_index++;
          if (data_index == num_matmul_sets) {
            data_index = 0;
          }
        }
        EvalResult(result);
        iters++;
      }
      iters_at_a_time *= 2;
      t = Clock::now();
    }
    latency = std::min(latency, ToSeconds(t - time_start) / iters);
    if (record_pmu && repeat == kRepeats - 1) {
      pmu_events.StopRecording();
      const float normalization_factor =
          static_cast<float>(iters) * rows * cols * depth;
      result->l1_refill_rate =
          pmu_events.L1RefillCount() / normalization_factor;
      result->l2_refill_rate =
          pmu_events.L2RefillCount() / normalization_factor;
      result->l3_refill_rate =
          pmu_events.L3RefillCount() / normalization_factor;
      result->mispred_rate = pmu_events.BranchMispredictionRate();
      result->frontend_stall_rate = pmu_events.FrontendStallRate();
      result->backend_stall_rate = pmu_events.BackendStallRate();
    }
  }
  result->latency = latency;

#ifdef GEMMLOWP_PROFILING
  gemmlowp::FinishProfiling();
  fflush(stdout);
#endif

  if (cold) {
    lhs.matrix.data = orig_lhs_data;
    rhs.matrix.data = orig_rhs_data;
    memcpy(orig_dst_data, result->storage_matrix.matrix.data.get(),
           sizeof(DstScalar) * FlatSize(result->storage_matrix.matrix.layout));
    result->storage_matrix.matrix.data = orig_dst_data;
  }
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::Eval() {
  RUY_CHECK(life_stage == LifeStage::kHasResultPaths);
  for (auto& result : results) {
    if (benchmark) {
      Benchmark(&result);
    } else {
      EvalResult(&result);
    }
  }
  life_stage = LifeStage::kEvaluated;
}

template <typename Scalar>
std::string DumpRegion(const Matrix<Scalar>& matrix, int center_row,
                       int center_col) {
  static constexpr int kRadius = 20;
  int first_row = std::max(0, center_row - kRadius);
  int last_row = std::min(matrix.layout.rows - 1, center_row + kRadius);
  int first_col = std::max(0, center_col - kRadius);
  int last_col = std::min(matrix.layout.cols - 1, center_col + kRadius);
  std::ostringstream stream;
  for (int row = first_row; row <= last_row; row++) {
    for (int col = first_col; col <= last_col; col++) {
      stream << static_cast<double>(Element(matrix, row, col)) << " ";
    }
    stream << "\n";
  }
  return stream.str();
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::VerifyTestResults() const {
  const int depth = lhs.matrix.layout.cols;
  for (int i = 0; i < results.size() - 1; i++) {
    if (!Agree(results[i], results[i + 1], depth)) {
      std::string paths_in_agreement;
      paths_in_agreement.append(PathName(results[0]));
      for (int j = 1; j <= i; j++) {
        paths_in_agreement.append(", ");
        paths_in_agreement.append(PathName(results[j]));
      }
      ErrorAnalysis error_analysis;
      AnalyzeTestError(*this, i + 1, &error_analysis);
      std::cerr << "Error: path (" << PathName(results[i + 1])
                << ") disagrees with the other paths (" << paths_in_agreement
                << "), which agree with each other." << std::endl;
      std::cerr << "Shape: rows = " << rows << ", cols = " << cols
                << ", depth = " << depth << std::endl;
      std::cerr << "Stats of the good result matrix: "
                << StatsAsString(error_analysis.stats_good) << std::endl;
      std::cerr << "Stats of the bad result matrix:  "
                << StatsAsString(error_analysis.stats_bad) << std::endl;
      if (error_analysis.error_rows.size() < rows) {
        std::cerr << "Rows containing errors: "
                  << Join(error_analysis.error_rows) << std::endl;
      } else {
        std::cerr << "Errors found in ALL rows." << std::endl;
      }
      if (error_analysis.error_cols.size() < cols) {
        std::cerr << "Cols containing errors: "
                  << Join(error_analysis.error_cols) << std::endl;
      } else {
        std::cerr << "Errors found in ALL cols." << std::endl;
      }
      std::cerr << "The first error occurs at row "
                << error_analysis.row_of_first_error << ", col "
                << error_analysis.col_of_first_error << std::endl;
      std::cerr << "Good value: " << error_analysis.first_error_good_value
                << std::endl;
      std::cerr << "Bad value : " << error_analysis.first_error_bad_value
                << std::endl;
      std::cerr << "Region of Good result matrix around first error:\n\n"
                << DumpRegion(results[0].storage_matrix.matrix,
                              error_analysis.row_of_first_error,
                              error_analysis.col_of_first_error)
                << std::endl;
      std::cerr << "Region of Bad result matrix around first error:\n\n"
                << DumpRegion(results[i + 1].storage_matrix.matrix,
                              error_analysis.row_of_first_error,
                              error_analysis.col_of_first_error)
                << std::endl;
      RUY_CHECK(false);
    }
  }
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::VerifyNonTrivial() const {
  if (getenv("QUICK_BENCHMARK")) {
    return;
  }
  if (results.front().path != Path::kReference) {
    return;
  }
  Context context;
  context.SetRuntimeEnabledPaths(Path::kReference);
  const auto& dst_storage = results.front().storage_matrix;
  const Matrix<DstScalar>& dst = dst_storage.matrix;
  Matrix<DstScalar> unclamped_dst;
  unclamped_dst.layout = dst.layout;
  unclamped_dst.zero_point = dst.zero_point;
  const int size = FlatSize(unclamped_dst.layout);
  std::vector<DstScalar> unclamped_dst_data(size);
  unclamped_dst.data = unclamped_dst_data.data();
  ruy::BasicSpec<AccumScalar, DstScalar> spec_unclamped;
  spec_unclamped.bias = spec.bias;
  spec_unclamped.multiplier_fixedpoint = spec.multiplier_fixedpoint;
  spec_unclamped.multiplier_exponent = spec.multiplier_exponent;
  Mul<Path::kReference>(lhs.matrix, rhs.matrix, spec_unclamped, &context,
                        &unclamped_dst);
  int count_clamped = 0;
  bool found_distinct_values = false;
  for (int row = 0; row < dst.layout.rows; row++) {
    for (int col = 0; col < dst.layout.cols; col++) {
      count_clamped +=
          (Element(dst, row, col) != Element(unclamped_dst, row, col));
      found_distinct_values |= (Element(dst, row, col) != Element(dst, 0, 0));
    }
  }
  if (!spec.multiplier_exponent_perchannel) {
    RUY_CHECK_LE(count_clamped, std::floor(2 * kClampRatio * size));
    if (size > 10) {
      RUY_CHECK(found_distinct_values);
    }
  }
}

template <typename LhsScalar, typename RhsScalar, typename SpecType>
void TestSet<LhsScalar, RhsScalar, SpecType>::Verify() {
  RUY_CHECK(life_stage == LifeStage::kEvaluated);
  if (expected_outcome == ExpectedOutcome::kSuccess) {
    VerifyTestResults();
    VerifyNonTrivial();
  }
  life_stage = LifeStage::kFinal;
}

template <typename TestSetType>
void TestRCC(int rows, int depth, int cols, ExpectedOutcome expected_outcome) {
  TestSetType test_set;
  test_set.rows = rows;
  test_set.depth = depth;
  test_set.cols = cols;
  test_set.lhs_order = Order::kRowMajor;
  test_set.rhs_order = Order::kColMajor;
  test_set.dst_order = Order::kColMajor;
  test_set.layout_style = LayoutStyle::kPackedLinear;
  test_set.expected_outcome = expected_outcome;
  test_set.Run();
}

template <typename TestSetType>
void TestRCC(int rows, int depth, int cols) {
  TestRCC<TestSetType>(rows, depth, cols, ExpectedOutcome::kSuccess);
}

template <typename TestSetType>
void TestNonRCC(int rows, int depth, int cols,
                ExpectedOutcome expected_outcome) {
  TestSetType test_set;
  test_set.rows = rows;
  test_set.depth = depth;
  test_set.cols = cols;
  test_set.lhs_order = Order::kColMajor;
  test_set.rhs_order = Order::kColMajor;
  test_set.dst_order = Order::kColMajor;
  test_set.layout_style = LayoutStyle::kPackedLinear;
  test_set.expected_outcome = expected_outcome;
  test_set.Run();
}

template <typename TestSetType>
void TestLinearAllOrders(int rows, int depth, int cols,
                         ExpectedOutcome expected_outcome) {
  const std::vector<Order> orders{Order::kColMajor, Order::kRowMajor};

  for (Order lhs_order : orders) {
    for (Order rhs_order : orders) {
      for (Order dst_order : orders) {
        TestSetType test_set;
        test_set.rows = rows;
        test_set.depth = depth;
        test_set.cols = cols;
        test_set.lhs_order = lhs_order;
        test_set.rhs_order = rhs_order;
        test_set.dst_order = dst_order;
        test_set.layout_style = LayoutStyle::kLinear;
        test_set.expected_outcome = expected_outcome;
        test_set.Run();
      }
    }
  }
}

template <typename TestSetType>
void TestLinearAllOrders(int rows, int depth, int cols) {
  TestLinearAllOrders<TestSetType>(rows, depth, cols,
                                   ExpectedOutcome::kSuccess);
}

}  // namespace ruy

#endif  // TENSORFLOW_LITE_EXPERIMENTAL_RUY_TEST_H_
