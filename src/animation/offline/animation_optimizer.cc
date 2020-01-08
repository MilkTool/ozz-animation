//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2019 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "ozz/animation/offline/animation_optimizer.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>

// Internal include file
#define OZZ_INCLUDE_PRIVATE_HEADER  // Allows to include private headers.
#include "animation/offline/decimate.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_animation_utils.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/skeleton_utils.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/math_constant.h"
#include "ozz/base/maths/math_ex.h"

namespace ozz {
namespace animation {
namespace offline {

// Setup default values (favoring quality).
AnimationOptimizer::AnimationOptimizer() : fast(true) {}

namespace {

AnimationOptimizer::Setting GetJointSetting(
    const AnimationOptimizer& _optimizer, int _joint) {
  AnimationOptimizer::Setting setting = _optimizer.setting;
  AnimationOptimizer::JointsSetting::const_iterator it =
      _optimizer.joints_setting_override.find(_joint);
  if (it != _optimizer.joints_setting_override.end()) {
    setting = it->second;
  }
  return setting;
}

struct HierarchyBuilder {
  HierarchyBuilder(const RawAnimation& _animation, const Skeleton& _skeleton,
                   const AnimationOptimizer& _optimizer)
      : specs(_animation.tracks.size()),
        animation(_animation),
        optimizer(_optimizer) {
    assert(_animation.num_tracks() == _skeleton.num_joints());

    // Computes hierarchical scale, iterating skeleton forward (root to
    // leaf).
    IterateJointsDF(
        _skeleton,
        IterateMemFun<HierarchyBuilder, &HierarchyBuilder::ComputeScaleForward>(
            *this));

    // Computes hierarchical length, iterating skeleton backward (leaf to root).
    IterateJointsDFReverse(
        _skeleton,
        IterateMemFun<HierarchyBuilder,
                      &HierarchyBuilder::ComputeLengthBackward>(*this));
  }

  struct Spec {
    float length;  // Length of a joint hierarchy (max of all child).
    float scale;   // Scale of a joint hierarchy (accumulated from all parents).
    float tolerance;  // Tolerance of a joint hierarchy (min of all child).
  };

  // Defines the length of a joint hierarchy (of all child).
  ozz::Vector<Spec>::Std specs;

 private:
  // Extracts maximum translations and scales for each track/joint.
  void ComputeScaleForward(int _joint, int _parent) {
    Spec& joint_spec = specs[_joint];

    // Compute joint maximum animated scale.
    float max_scale = 0.f;
    const RawAnimation::JointTrack& track = animation.tracks[_joint];
    if (track.scales.size() != 0) {
      for (size_t j = 0; j < track.scales.size(); ++j) {
        const math::Float3& scale = track.scales[j].value;
        const float max_element = math::Max(
            math::Max(std::abs(scale.x), std::abs(scale.y)), std::abs(scale.z));
        max_scale = math::Max(max_scale, max_element);
      }
    } else {
      max_scale = 1.f;  // Default scale.
    }

    // Accumulate with parent scale.
    joint_spec.scale = max_scale;
    if (_parent != Skeleton::kNoParent) {
      const Spec& parent_spec = specs[_parent];
      joint_spec.scale *= parent_spec.scale;
    }

    // Computes self setting distance and tolerance.
    // Distance is now scaled with accumulated parent scale.
    const AnimationOptimizer::Setting setting =
        GetJointSetting(optimizer, _joint);
    joint_spec.length = setting.distance * specs[_joint].scale;
    joint_spec.tolerance = setting.tolerance;
  }

  // Propagate child translations back to the root.
  void ComputeLengthBackward(int _joint, int _parent) {
    // Self translation doesn't matter if joint has no parent.
    if (_parent == Skeleton::kNoParent) {
      return;
    }

    // Compute joint maximum animated length.
    float max_length_sq = 0.f;
    const RawAnimation::JointTrack& track = animation.tracks[_joint];
    for (size_t j = 0; j < track.translations.size(); ++j) {
      max_length_sq =
          math::Max(max_length_sq, LengthSqr(track.translations[j].value));
    }
    const float max_length = std::sqrt(max_length_sq);

    const Spec& joint_spec = specs[_joint];
    Spec& parent_spec = specs[_parent];

    // Set parent hierarchical spec to its most impacting child, aka max
    // length and min tolerance.
    parent_spec.length = math::Max(
        parent_spec.length, joint_spec.length + max_length * parent_spec.scale);
    parent_spec.tolerance =
        math::Min(parent_spec.tolerance, joint_spec.tolerance);
  }

  // Disables copy and assignment.
  HierarchyBuilder(const HierarchyBuilder&);
  void operator=(const HierarchyBuilder&);

  // Targeted animation.
  const RawAnimation& animation;

  // Usefull to access settings and compute hierarchy length.
  const AnimationOptimizer& optimizer;
};

class PositionAdapter {
 public:
  PositionAdapter(float _scale) : scale_(_scale) {}
  bool Decimable(const RawAnimation::TranslationKey&) const { return true; }
  RawAnimation::TranslationKey Lerp(
      const RawAnimation::TranslationKey& _left,
      const RawAnimation::TranslationKey& _right,
      const RawAnimation::TranslationKey& _ref) const {
    const float alpha = (_ref.time - _left.time) / (_right.time - _left.time);
    assert(alpha >= 0.f && alpha <= 1.f);
    const RawAnimation::TranslationKey key = {
        _ref.time, LerpTranslation(_left.value, _right.value, alpha)};
    return key;
  }
  float Distance(const RawAnimation::TranslationKey& _a,
                 const RawAnimation::TranslationKey& _b) const {
    return Length(_a.value - _b.value) * scale_;
  }

 private:
  float scale_;
};

class RotationAdapter {
 public:
  RotationAdapter(float _radius) : radius_(_radius) {}
  bool Decimable(const RawAnimation::RotationKey&) const { return true; }
  RawAnimation::RotationKey Lerp(const RawAnimation::RotationKey& _left,
                                 const RawAnimation::RotationKey& _right,
                                 const RawAnimation::RotationKey& _ref) const {
    const float alpha = (_ref.time - _left.time) / (_right.time - _left.time);
    assert(alpha >= 0.f && alpha <= 1.f);
    const RawAnimation::RotationKey key = {
        _ref.time, LerpRotation(_left.value, _right.value, alpha)};
    return key;
  }
  float Distance(const RawAnimation::RotationKey& _left,
                 const RawAnimation::RotationKey& _right) const {
    // Compute the shortest unsigned angle between the 2 quaternions.
    // cos_half_angle is w component of a-1 * b.
    const float cos_half_angle = Dot(_left.value, _right.value);
    const float sine_half_angle =
        std::sqrt(1.f - math::Min(1.f, cos_half_angle * cos_half_angle));
    // Deduces distance between 2 points on a circle with radius and a given
    // angle. Using half angle helps as it allows to have a right-angle
    // triangle.
    const float distance = 2.f * sine_half_angle * radius_;
    return distance;
  }

 private:
  float radius_;
};

class ScaleAdapter {
 public:
  ScaleAdapter(float _length) : length_(_length) {}
  bool Decimable(const RawAnimation::ScaleKey&) const { return true; }
  RawAnimation::ScaleKey Lerp(const RawAnimation::ScaleKey& _left,
                              const RawAnimation::ScaleKey& _right,
                              const RawAnimation::ScaleKey& _ref) const {
    const float alpha = (_ref.time - _left.time) / (_right.time - _left.time);
    assert(alpha >= 0.f && alpha <= 1.f);
    const RawAnimation::ScaleKey key = {
        _ref.time, LerpScale(_left.value, _right.value, alpha)};
    return key;
  }
  float Distance(const RawAnimation::ScaleKey& _left,
                 const RawAnimation::ScaleKey& _right) const {
    return Length(_left.value - _right.value) * length_;
  }

 private:
  float length_;
};

struct LTMIterator {
  LTMIterator(const ozz::Range<const ozz::math::Transform>& _locals,
              const ozz::Range<ozz::math::Transform>& _models)
      : locals_(_locals), models_(_models) {}

  LTMIterator(const LTMIterator& _it)
      : locals_(_it.locals_), models_(_it.models_) {}

  void operator()(int _joint, int _parent) {
    if (_parent == ozz::animation::Skeleton::kNoParent) {
      models_[_joint] = locals_[_joint];
    } else {
      const ozz::math::Transform& local = locals_[_joint];
      const ozz::math::Transform& parent = models_[_parent];
      ozz::math::Transform& model = models_[_joint];

      model.translation =
          parent.translation +
          TransformVector(parent.rotation, local.translation * parent.scale);
      model.rotation = parent.rotation * local.rotation;
      model.scale = parent.scale * local.scale;
    }
  }
  const ozz::Range<const ozz::math::Transform>& locals_;
  const ozz::Range<ozz::math::Transform>& models_;

 private:
  void operator=(const LTMIterator&);
};

// Reimplement local to model-space because ozz runtime version isn't based on
// ozz::math::Transform
bool LocalToModel(const ozz::animation::Skeleton& _skeleton,
                  const ozz::Range<const ozz::math::Transform>& _locals,
                  const ozz::Range<ozz::math::Transform>& _models,
                  int from = -1) {
  assert(static_cast<size_t>(_skeleton.num_joints()) <= _locals.count() &&
         static_cast<size_t>(_skeleton.num_joints()) <= _models.count());

  LTMIterator it(_locals, _models);
  ozz::animation::IterateJointsDF(_skeleton, it, from);

  return true;
}

template <typename _Track, typename _Times>
void CopyKeyTimes(const _Track& _track, _Times* _key_times) {
  for (size_t i = 0; i < _track.size(); ++i) {
    _key_times->push_back(_track[i].time);
  }
}

template <typename _Times>
void SetupKeyTimes(const RawAnimation& _animation, _Times* _key_times) {
  // Gets union of all possible keyframe times.
  for (int i = 0; i < _animation.num_tracks(); ++i) {
    const RawAnimation::JointTrack& track = _animation.tracks[i];
    CopyKeyTimes(track.translations, _key_times);
    CopyKeyTimes(track.rotations, _key_times);
    CopyKeyTimes(track.scales, _key_times);
  }
  std::sort(_key_times->begin(), _key_times->end());
  _key_times->erase(std::unique(_key_times->begin(), _key_times->end()),
                    _key_times->end());
}

float Compare(const ozz::math::Transform& _reference,
              const ozz::math::Transform& _test, float _distance) {
  // return Length(_reference.translation - _test.translation);

  // Translation error in model space takes intrinsically into account
  // translation rotation and scale of its parents. Rotation error is only
  // impacting local space, hence the use of a distance paramete simulating
  // skinning (or any user defined requirement). Its impact on children will be
  // measured as a translation error indeed.

  const math::Quaternion diff = _reference.rotation * Conjugate(_test.rotation);
  const math::Float3 axis = (diff.x == 0.f && diff.y == 0.f && diff.z == 0.f)
                                ? math::Float3::x_axis()
                                : math::Float3(diff.x, diff.y, diff.z);
  const math::Float3 abs(std::abs(axis.x), std::abs(axis.y), std::abs(axis.z));
  const size_t smallest =
      abs.x < abs.y ? (abs.x < abs.z ? 0 : 2) : (abs.y < abs.z ? 1 : 2);
  const math::Float3 binormal(smallest == 0, smallest == 1, smallest == 2);
  const math::Float3 normal =
      Normalize(Cross(binormal, axis)) * _reference.scale;

  const float rotation_error =
      Length(TransformVector(diff, normal) - normal) * _distance;

  const float translation_error =
      Length(_reference.translation - _test.translation);

  // const float scale_error = _distance * Length(_reference.scale -
  // _test.scale);

  const float error = translation_error + rotation_error /*+ scale_error*/;
  return error;
}

class CompareIterator {
 public:
  CompareIterator(
      const ozz::Range<const ozz::math::Transform>& _reference_models,
      const ozz::Range<ozz::math::Transform>& _models,
      const ozz::Range<const AnimationOptimizer::Setting>& _settings,
      const ozz::Range<float>& _errors)
      : reference_models_(_reference_models),
        models_(_models),
        settings_(_settings),
        errors_(_errors) {
    assert(reference_models_.count() == models_.count() &&
           reference_models_.count() == settings_.count() &&
           reference_models_.count() == errors_.count());
  }

  void operator()(int _joint, int /*_parent*/) {
    const float distance = settings_[_joint].distance;
    const float err =
        Compare(reference_models_[_joint], models_[_joint], distance);
    errors_[_joint] = err;
  }

 private:
  void operator=(const CompareIterator&);

  const ozz::Range<const ozz::math::Transform> reference_models_;
  const ozz::Range<const ozz::math::Transform> models_;
  const ozz::Range<const AnimationOptimizer::Setting> settings_;

  const ozz::Range<float> errors_;
};

class RatioIterator {
 public:
  RatioIterator(const ozz::Range<const AnimationOptimizer::Setting>& _settings,
                const ozz::Range<const float>& _errors)
      : settings_(_settings),
        errors_(_errors),
        ratio_(-std::numeric_limits<float>::max()) {
    assert(settings_.count() == errors_.count());
  }

  void operator()(int _joint, int /*_parent*/) {
    // < 0 if better than target
    // = 0 if on target
    // > 0 if worst than target
    const float ratio = (errors_[_joint] - settings_[_joint].tolerance) /
                        settings_[_joint].tolerance;

    // Stores worst ratio of the hierarchy.
    ratio_ = ozz::math::Max(ratio_, ratio);
  }

  float ratio() const { return ratio_; }

 private:
  void operator=(const RatioIterator&);

  const ozz::Range<const AnimationOptimizer::Setting> settings_;
  const ozz::Range<const float> errors_;

  float ratio_;
};

template <typename _Track>
struct TransformComponent;
template <>
struct TransformComponent<RawAnimation::JointTrack::Translations> {
  static math::Float3* Get(math::Transform* _transform) {
    return &_transform->translation;
  }
};
template <>
struct TransformComponent<RawAnimation::JointTrack::Rotations> {
  static math::Quaternion* Get(math::Transform* _transform) {
    return &_transform->rotation;
  }
};
template <>
struct TransformComponent<RawAnimation::JointTrack::Scales> {
  static math::Float3* Get(math::Transform* _transform) {
    return &_transform->scale;
  }
};

template <typename _Track>
struct TrackComponent;
template <>
struct TrackComponent<RawAnimation::JointTrack::Translations> {
  static const RawAnimation::JointTrack::Translations& Get(
      const RawAnimation::JointTrack& _track) {
    return _track.translations;
  }
};
template <>
struct TrackComponent<RawAnimation::JointTrack::Rotations> {
  static const RawAnimation::JointTrack::Rotations& Get(
      const RawAnimation::JointTrack& _track) {
    return _track.rotations;
  }
};
template <>
struct TrackComponent<RawAnimation::JointTrack::Scales> {
  static const RawAnimation::JointTrack::Scales& Get(
      const RawAnimation::JointTrack& _track) {
    return _track.scales;
  }
};

// TODO comment
template <typename _Track>
class Spanner {
 public:
  Spanner(const _Track& _track, const ozz::Vector<bool>::Std& _included)
      : track_(_track), included_(_included), span_end_(0), inside_(false) {
    assert(included_.empty() || track_.size() == included_.size());
    ++*this;
  }

  // Returns true if time is inside a span of updated (aka not-included)
  // keyframes.
  bool Update(float _time) {
    const bool inside = _time > begin_ && _time < end_;
    if (inside_ && _time >= end_) {
      ++*this;
    }
    inside_ = inside;
    return inside_;
  }

 private:
  Spanner(const Spanner&);
  void operator=(const Spanner&);

  Spanner& operator++() {
    const size_t size = included_.size();

    // Finds first removed keyframe.
    size_t span_begin = span_end_;
    for (; span_begin < size && included_[span_begin]; ++span_begin) {
    }

    // Span starts at previous keyframe
    begin_ = span_begin == 0 ? 0.f : track_[span_begin - 1].time;

    // Find next maintained keyframe.
    for (span_end_ = span_begin + 1; span_end_ < size && !included_[span_end_];
         ++span_end_) {
    }

    // Span ends at next keyframe.
    end_ = span_end_ >= size ? std::numeric_limits<float>::max()
                             : track_[span_end_].time;
    return *this;
  }

  const _Track& track_;
  const ozz::Vector<bool>::Std& included_;

  size_t span_end_;
  bool inside_;
  float begin_;
  float end_;
};

class Comparer {
 public:
  Comparer(const RawAnimation& _original, const RawAnimation& _solution,
           const Skeleton& _skeleton)
      : solution_(_solution), skeleton_(_skeleton) {
    // Comparison only needs to happen at each of the keyframe times.
    // So we get the union of all possible keyframe times
    SetupKeyTimes(_original, &key_times_);

    const size_t key_times_count = key_times_.size();
    const int joints_count = _skeleton.num_joints();
    solution_locals_.resize(key_times_count, SkeletonTransforms(joints_count));
    solution_models_ = solution_locals_;

    // Populates test local & model space transforms.
    for (size_t i = 0; i < key_times_.size(); ++i) {
      ozz::animation::offline::SampleAnimation(
          _original, key_times_[i], ozz::make_range(solution_locals_[i]),
          false);
      LocalToModel(skeleton_, ozz::make_range(solution_locals_[i]),
                   ozz::make_range(solution_models_[i]));
    }

    // Solution animation equals reference at initialization time.
    reference_models_ = solution_models_;
    cached_errors_.resize(key_times_count, SkeletonErrors(joints_count, 0.f));
  }

  // Updates cached error matrix following an update of _joint
  // Solution animation track shall have already been update.
  void Update(size_t _joint,
              const ozz::Range<const AnimationOptimizer::Setting>& _settings) {
    for (size_t i = 0; i < key_times_.size(); ++i) {
      // Update joint local and model space
      SampleTrack(solution_.tracks[_joint], key_times_[i],
                  &solution_locals_[i][_joint], false);

      LocalToModel(skeleton_, ozz::make_range(solution_locals_[i]),
                   ozz::make_range(solution_models_[i]),
                   static_cast<int>(_joint));

      // Compare
      IterateJointsDF(skeleton_,
                      CompareIterator(make_range(reference_models_[i]),
                                      make_range(solution_models_[i]),
                                      _settings, make_range(cached_errors_[i])),
                      static_cast<int>(_joint));
    }
  }

  template <typename _Track>
  float EstimateError(
      _Track _track, size_t _joint,
      const ozz::Range<const AnimationOptimizer::Setting>& _settings,
      const ozz::Vector<bool>::Std& _included) const {
    // TODO use better allocation & copy strategy for models (partial ??)
    SkeletonTransforms locals;
    SkeletonTransforms models;
    SkeletonErrors errors;

    Spanner<_Track> spanner(
        TrackComponent<_Track>::Get(solution_.tracks[_joint]), _included);

    static int inl = 0;
    static int outl = 0;

    float worst_ratio = -std::numeric_limits<float>::max();
    // TODO Loops though all time range, but exits as soon as worst_ratio is
    // past the limit.
    for (size_t i = 0; i < key_times_.size() /* && worst_ratio < 0.f*/; ++i) {
      const float key_time = key_times_[i];
      if (!spanner.Update(key_time)) {
        ++outl;
        // Reuses precomputed errors
        const float ratio =
            IterateJointsDF(
                skeleton_,
                RatioIterator(_settings, make_range(cached_errors_[i])),
                static_cast<int>(_joint))
                .ratio();

        worst_ratio = ozz::math::Max(worst_ratio, ratio);
      } else {
        ++inl;
        // Error value needs to be recomputed.
        // Update joint local and model space.
        locals = solution_locals_[i];
        SampleTrackComponent(_track, key_time,
                             TransformComponent<_Track>::Get(&locals[_joint]),
                             false);

        models = solution_models_[i];
        LocalToModel(skeleton_, ozz::make_range(locals),
                     ozz::make_range(models), static_cast<int>(_joint));

        // Compare
        errors = cached_errors_[i];
        IterateJointsDF(
            skeleton_,
            CompareIterator(make_range(reference_models_[i]),
                            make_range(models), _settings, make_range(errors)),
            static_cast<int>(_joint));

        // Gets joint error ratio, which the worst of its hierarchy.
        const float ratio =
            IterateJointsDF(skeleton_,
                            RatioIterator(_settings, make_range(errors)),
                            static_cast<int>(_joint))
                .ratio();

        // Stores worst ratio of whole time range.
        worst_ratio = ozz::math::Max(worst_ratio, ratio);
      }
    }
    return worst_ratio;
  }

 private:
  Comparer(const Comparer&);
  void operator=(const Comparer&);

  const RawAnimation& solution_;
  const Skeleton& skeleton_;

  typedef ozz::Vector<ozz::math::Transform>::Std SkeletonTransforms;
  typedef ozz::Vector<SkeletonTransforms>::Std SkeletonTransformKeys;

  // All model-space transforms for the reference animation.
  SkeletonTransformKeys reference_models_;

  // Locals and models-space transforms for the solution animation.
  SkeletonTransformKeys solution_locals_;
  SkeletonTransformKeys solution_models_;

  typedef ozz::Vector<float>::Std SkeletonErrors;
  typedef ozz::Vector<SkeletonErrors>::Std SkeletonErrorKeys;
  SkeletonErrorKeys cached_errors_;

  typedef ozz::Vector<float>::Std KeyTimes;
  ozz::Vector<float>::Std key_times_;
};

// Abstract class used for hill climbing algorithm
class VTrack {
 public:
  VTrack(float _initial_tolerance, int _joint)
      : tolerance_(_initial_tolerance), candidate_error_(0.f), joint_(_joint) {
    // TODO update initial error.
  }
  virtual ~VTrack() {}
  void Initialize(
      const Comparer& _comparer,
      const ozz::Range<const AnimationOptimizer::Setting>& _settings) {
    // Decimates  size in order to find next candidate track.
    Decimate(tolerance_);
    UpdateCandidateError(_comparer, _settings);
  }
  // Computes transition to next solution.
  void Transition(
      const ozz::Range<const AnimationOptimizer::Setting>& /*_settings*/) {
    const size_t validated_size = ValidatedSize();
    for (size_t candidate_size = CandidateSize();
         candidate_size > 1 && candidate_size == validated_size;
         candidate_size = CandidateSize()) {
      // TODO should first try with initial tolerance.
      // Computes next tolerance to use for decimation.
      tolerance_ *= 1.2f;

      // Decimates validated track in order to find next candidate track.
      Decimate(tolerance_);
    }
  }

  // Updates error ratio of the candidate track compared to original.
  void UpdateCandidateError(
      const Comparer& _comparer,
      const ozz::Range<const AnimationOptimizer::Setting>& _settings) {
    candidate_error_ = EstimateCandidateError(_comparer, _settings);
  }

  // Proposed candidate was validated, must be retained as a solution.
  virtual void Validate(
      Comparer& _comparer,
      const ozz::Range<const AnimationOptimizer::Setting>& _settings) = 0;

  // -1 < x < 0 if transition pass is within tolerance range. The minimum
  // the better. x >= 0 if transition pass is exceeding tolerance range.
  float delta() const {
    // TODO precompute this delta value.
    const size_t original_size = OriginalSize();
    if (original_size == 0) {
      return 0.f;
    }
    const size_t validated_size = ValidatedSize();
    const size_t candidate_size = CandidateSize();
    const float size_ratio =
        static_cast<float>(validated_size - candidate_size) /
        static_cast<float>(original_size);
    return candidate_error_ * size_ratio;
  }
  int joint() const { return joint_; }

 private:
  virtual size_t OriginalSize() const = 0;
  virtual size_t ValidatedSize() const = 0;
  virtual size_t CandidateSize() const = 0;

  virtual float EstimateCandidateError(
      const Comparer& _comparer,
      const ozz::Range<const AnimationOptimizer::Setting>& _settings) const = 0;

  // Decimates track at given _tolerance;
  virtual void Decimate(float _tolerance) = 0;

  // Error tolerance of candidate track.
  float tolerance_;

  // TODO removes
  // Error value for candidate track.
  float candidate_error_;

  // Joint that this track applies to.
  int joint_;
};

template <typename _Track, typename _Adapter>
class TTrack : public VTrack {
 public:
  TTrack(const _Track& _original, _Track* _solution, const _Adapter& _adapter,
         float _initial_tolerance, int _joint)
      : VTrack(_initial_tolerance, _joint),
        adapter_(_adapter),
        original_(_original),
        validated_(_solution) {
    // Initialize validated track with a copy of original.
    assert(validated_ && "Destination track must be provided");
    *validated_ = original_;
  }

 private:
  void operator=(const TTrack&);

  virtual float EstimateCandidateError(
      const Comparer& _comparer,
      const ozz::Range<const AnimationOptimizer::Setting>& _settings) const {
    return _comparer.EstimateError(candidate_, joint(), _settings, included_);
  }

  virtual void Validate(
      Comparer& _comparer,
      const ozz::Range<const AnimationOptimizer::Setting>& _settings) {
    *validated_ = candidate_;
    _comparer.Update(joint(), _settings);

    // TODO explain outdated bits
    included_.clear();
  }

  virtual void Decimate(float _tolerance) {
    // TODO Need to prove decimate doesn't need original track
    ozz::animation::offline::Decimate(*validated_, adapter_, _tolerance,
                                      &candidate_, &included_);
  }

  virtual size_t OriginalSize() const { return original_.size(); }
  virtual size_t ValidatedSize() const { return validated_->size(); }
  virtual size_t CandidateSize() const { return candidate_.size(); }

  const _Adapter adapter_;
  const _Track& original_;
  _Track* validated_;
  _Track candidate_;

  // Vector used to store keyframes from candidate_ that are included from
  // validated_.
  ozz::Vector<bool>::Std included_;
};

typedef TTrack<RawAnimation::JointTrack::Translations, PositionAdapter>
    TranslationTrack;
typedef TTrack<RawAnimation::JointTrack::Rotations, RotationAdapter>
    RotationTrack;
typedef TTrack<RawAnimation::JointTrack::Scales, ScaleAdapter> ScaleTrack;

class HillClimber {
 public:
  HillClimber(const AnimationOptimizer& _optimizer,
              const RawAnimation& _original, const Skeleton& _skeleton,
              RawAnimation* _output)
      : comparer_(_original, *_output, _skeleton),
        original_(_original),
        skeleton_(_skeleton) {
    // Checks output
    assert(_output->tracks.size() == original_.tracks.size());

    // Computes skeleton hierarchy specs, used to find initial settings.
    const HierarchyBuilder hierarchy(original_, _skeleton, _optimizer);

    // Setup virtual tracks and parameters.
    const int num_tracks = original_.num_tracks();
    translations_.reserve(num_tracks);
    rotations_.reserve(num_tracks);
    scales_.reserve(num_tracks);
    settings_.reserve(num_tracks);

    for (int i = 0; i < num_tracks; ++i) {
      const float kInitialFactor = .2f;

      // Using hierarchy spec for joint_length (aka comparing rotations)
      // is important as it prevents from optimizing too quickly joints at
      // the root. This length helps RotationAdapter at
      // computing/simulating the tolerance at hierarchy end. Hill
      // climbing will have the last word, figuring out if this decimation
      // is OK or not in the end.
      const float joint_length = hierarchy.specs[i].length;
      /*
      const int parent = _skeleton.joint_parents()[i];
      const float parent_scale = (parent != Skeleton::kNoParent)
                                 ? _hierarchy.specs[parent].scale
                                 : 1.f;*/
      const float parent_scale = 1.f;

      const AnimationOptimizer::Setting& setting =
          GetJointSetting(_optimizer, i);
      settings_.push_back(setting);

      const float initial = hierarchy.specs[i].tolerance * kInitialFactor;
      const RawAnimation::JointTrack& itrack = _original.tracks[i];
      RawAnimation::JointTrack& otrack = _output->tracks[i];

      {  // Translation track, translation is affected by parent scale.
        const PositionAdapter adap(parent_scale);
        const TranslationTrack track(itrack.translations, &otrack.translations,
                                     adap, initial, i);
        translations_.push_back(track);
      }
      {  // Rotation track, rotation affects children translations/length.
        const RotationAdapter adap(joint_length);
        const RotationTrack track(itrack.rotations, &otrack.rotations, adap,
                                  initial, i);
        rotations_.push_back(track);
      }
      {  // Scale track, scale affects children translations/length.
        const ScaleAdapter adap(joint_length);
        const ScaleTrack track(itrack.scales, &otrack.scales, adap, initial, i);
        scales_.push_back(track);
      }
    }

    // Collects all remaining tracks to process. Doing this at this point
    // ensures tracks won't be reallocated/moved.
    remainings_.reserve(num_tracks * 3);
    for (int i = 0; i < num_tracks; ++i) {
      remainings_.push_back(&translations_[i]);
      remainings_.push_back(&rotations_[i]);
      remainings_.push_back(&scales_[i]);
    }

    // Initialize all tracks with a decimation based on initial tolerance.
    for (size_t i = 0; i < remainings_.size(); ++i) {
      remainings_[i]->Initialize(comparer_, ozz::make_range(settings_));
    }
  }

  void operator()() {
    // Loops as long as there's still optimizable tracks to process.
    for (;;) {
      // Within a single loop over all remaining tracks, finds the one
      // that has the best bang for the buck.
      VTrack* best_track = NULL;
      float best_delta = 0.f;
      for (ozz::Vector<VTrack*>::Std::iterator it = remainings_.begin();
           it != remainings_.end();) {
        VTrack* track = *it;

        // Gets this track optimization delta.
        float delta = track->delta();

        // Track can be removed from those to process if it can't be
        // optimized anymore.
        if (delta >= 0) {
          it = remainings_.erase(it);
          continue;
        }

        // Remembers best option.
        if (delta < best_delta) {
          best_delta = delta;
          best_track = track;
        }

        ++it;  // Proceed with next track.
      }

      // Check if we reached end of possible options.
      assert(best_track != NULL || remainings_.empty());
      if (best_track == NULL) {
        break;
      }

      // Retains best track option
      best_track->Validate(comparer_, ozz::make_range(settings_));

      // Computes next candidate for track.
      best_track->Transition(ozz::make_range(settings_));

      // Updates all dependent tracks of the hierarchy.
      IterateJointsDF(
          skeleton_,
          IterateMemFun<HillClimber, &HillClimber::UpdateCandidateError>(*this),
          best_track->joint());
    }
  }

  void UpdateCandidateError(int _joint, int) {
    // !!!! TODO check if tracks that aren't part of the remaining ones
    // still need to be updated.
    // TODO check if all types of tracks really need to be updated
    // for the first joint, or if for example translations don't need when
    // optimized/modified track is a rotation.
    const ozz::Range<const AnimationOptimizer::Setting> settings =
        make_range(settings_);
    translations_[_joint].UpdateCandidateError(comparer_, settings);
    rotations_[_joint].UpdateCandidateError(comparer_, settings);
    scales_[_joint].UpdateCandidateError(comparer_, settings);
  }

 private:
  HillClimber(const HillClimber&);
  void operator=(const HillClimber&);

  Comparer comparer_;
  const RawAnimation& original_;
  const Skeleton& skeleton_;
  ozz::Vector<TranslationTrack>::Std translations_;
  ozz::Vector<RotationTrack>::Std rotations_;
  ozz::Vector<ScaleTrack>::Std scales_;
  ozz::Vector<AnimationOptimizer::Setting>::Std settings_;
  ozz::Vector<VTrack*>::Std remainings_;
};
}  // namespace

bool AnimationOptimizer::operator()(const RawAnimation& _input,
                                    const Skeleton& _skeleton,
                                    RawAnimation* _output) const {
  if (!_output) {
    return false;
  }
  // Reset output animation to default.
  *_output = RawAnimation();

  // Validate animation.
  if (!_input.Validate()) {
    return false;
  }

  const int num_tracks = _input.num_tracks();

  // Validates the skeleton matches the animation.
  if (num_tracks != _skeleton.num_joints()) {
    return false;
  }

  // Setups output animation.
  _output->name = _input.name;
  _output->duration = _input.duration;
  _output->tracks.resize(num_tracks);

  // Temporary vector used to store included keyframes during decimation.
  ozz::Vector<bool>::Std included;

  if (fast) {
    // First computes bone lengths, that will be used when filtering.
    const HierarchyBuilder hierarchy(_input, _skeleton, *this);

    for (int i = 0; i < num_tracks; ++i) {
      const RawAnimation::JointTrack& input = _input.tracks[i];
      RawAnimation::JointTrack& output = _output->tracks[i];

      // Gets joint specs back.
      const float joint_length = hierarchy.specs[i].length;
      const int parent = _skeleton.joint_parents()[i];
      const float parent_scale =
          (parent != Skeleton::kNoParent) ? hierarchy.specs[parent].scale : 1.f;
      const float tolerance = hierarchy.specs[i].tolerance;

      // Filters independently T, R and S tracks.
      // This joint translation is affected by parent scale.
      const PositionAdapter tadap(parent_scale);
      Decimate(input.translations, tadap, tolerance, &output.translations,
               &included);
      // This joint rotation affects children translations/length.
      const RotationAdapter radap(joint_length);
      Decimate(input.rotations, radap, tolerance, &output.rotations, &included);
      // This joint scale affects children translations/length.
      const ScaleAdapter sadap(joint_length);
      Decimate(input.scales, sadap, tolerance, &output.scales, &included);
    }
  } else {
    HillClimber climber(*this, _input, _skeleton, _output);
    climber();
  }

  // Output animation is always valid though.
  return _output->Validate();
}
}  // namespace offline
}  // namespace animation
}  // namespace ozz
