#include "laser_slam/incremental_estimator.hpp"

#include <algorithm>
#include <utility>

#include <gtsam/nonlinear/GaussNewtonOptimizer.h>

using namespace gtsam;

namespace laser_slam {

// 初始化 iSAM2  laser_track  闭环噪声模型  闭环转换icp
IncrementalEstimator::IncrementalEstimator(const EstimatorParams& parameters,
                                           unsigned int n_laser_slam_workers) : params_(
                                               parameters),
                                               n_laser_slam_workers_(n_laser_slam_workers) {
  // Create the iSAM2 object.
  // 创建iSAM2对象
  ISAM2Params isam2_params;
  isam2_params.setRelinearizeSkip(1);
  isam2_params.setRelinearizeThreshold(0.001);
  isam2_ = ISAM2(isam2_params);

  // Create the laser tracks.
  // 创建激光轨迹
  for (size_t i = 0u; i < n_laser_slam_workers_; ++i) {
    std::shared_ptr<LaserTrack> laser_track(new LaserTrack(parameters.laser_track_params, i));
    laser_tracks_.push_back(std::move(laser_track));
  }

  // Create the loop closure noise model.
  // 创建闭环噪声模型
  // 是否采用柯西噪声模型的区别
  using namespace gtsam::noiseModel;
  if (params_.add_m_estimator_on_loop_closures) {
    LOG(INFO) << "Creating loop closure noise model with cauchy.";
    loop_closure_noise_model_  = gtsam::noiseModel::Robust::Create(
        gtsam::noiseModel::mEstimator::Cauchy::Create(1),
        gtsam::noiseModel::Diagonal::Sigmas(params_.loop_closure_noise_model));
  } else {
    loop_closure_noise_model_ =
        gtsam::noiseModel::Diagonal::Sigmas(params_.loop_closure_noise_model);
  }

	// 此处与所给噪声模型参数相同
  Eigen::Matrix<double,6,1> first_association_noise_model;
  first_association_noise_model[0] = 0.05;
  first_association_noise_model[1] = 0.05;
  first_association_noise_model[2] = 0.05;
  first_association_noise_model[3] = 0.015;
  first_association_noise_model[4] = 0.015;
  first_association_noise_model[5] = 0.015;
  first_association_noise_model_ =
      gtsam::noiseModel::Diagonal::Sigmas(first_association_noise_model);

  // Load the ICP configurations for adjusting the loop closure transformations.
  // TODO now using the same configuration as for the lidar odometry.
  // 加载用于调整闭环转换的ICP配置
  std::ifstream ifs_icp_configurations(params_.laser_track_params.icp_configuration_file.c_str());
  if (ifs_icp_configurations.good()) {
    LOG(INFO) << "Loading ICP configurations from: " <<
        params_.laser_track_params.icp_configuration_file;
    icp_.loadFromYaml(ifs_icp_configurations);
  } else {
    LOG(WARNING) << "Could not open ICP configuration file. Using default configuration.";
    icp_.setDefault();
  }
}

void IncrementalEstimator::processLoopClosure(const RelativePose& loop_closure) {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);

  // 如果是同一个worker
  // a为source，b为target
  if (loop_closure.track_id_a == loop_closure.track_id_b) {
	// a的时间戳小于b的时间戳
    CHECK_LT(loop_closure.time_a_ns, loop_closure.time_b_ns) << "Loop closure has invalid time.";
  }
  // 确保a、b的时间戳在对应路径上是有效的
  CHECK_GE(loop_closure.time_a_ns, laser_tracks_[loop_closure.track_id_a]->getMinTime()) <<
      "Loop closure has invalid time.";
  CHECK_LE(loop_closure.time_a_ns, laser_tracks_[loop_closure.track_id_a]->getMaxTime()) <<
      "Loop closure has invalid time.";
  CHECK_GE(loop_closure.time_b_ns, laser_tracks_[loop_closure.track_id_b]->getMinTime()) <<
      "Loop closure has invalid time.";
  CHECK_LE(loop_closure.time_b_ns, laser_tracks_[loop_closure.track_id_b]->getMaxTime()) <<
      "Loop closure has invalid time.";

  RelativePose updated_loop_closure = loop_closure;

  // Convert the reference frame of the loop closure transformation.
  // When applying the transformation w_T_a_b to the source cloud, it will align it with the
  // target cloud.
  // 转换闭环变换的参考系
  // 当把转换w_T_a_b应用到源云时，会将它与目标点云对齐
  SE3 w_T_a_b = loop_closure.T_a_b;
  SE3 T_w_a = laser_tracks_[loop_closure.track_id_a]->evaluate(loop_closure.time_a_ns);
  SE3 T_w_b = laser_tracks_[loop_closure.track_id_b]->evaluate(loop_closure.time_b_ns);
  SE3 a_T_a_b = T_w_a.inverse() * w_T_a_b * T_w_b;
  updated_loop_closure.T_a_b = a_T_a_b;

  // Apply an ICP step if desired.
  // 如果需要，执行ICP（更新帧间关系）
  if (params_.do_icp_step_on_loop_closures) {
    // Get the initial guess.
    PointMatcher::TransformationParameters initial_guess =
        updated_loop_closure.T_a_b.getTransformationMatrix().cast<float>();

    LOG(INFO) << "Creating the submaps for loop closure ICP.";
    Clock clock;
    DataPoints sub_map_a;
    DataPoints sub_map_b;
    laser_tracks_[updated_loop_closure.track_id_a]->buildSubMapAroundTime(
        loop_closure.time_a_ns, params_.loop_closures_sub_maps_radius, &sub_map_a);
    laser_tracks_[updated_loop_closure.track_id_b]->buildSubMapAroundTime(
        loop_closure.time_b_ns, params_.loop_closures_sub_maps_radius, &sub_map_b);
    clock.takeTime();
    LOG(INFO) << "Took " << clock.getRealTime() << " ms to create loop closures sub maps.";

    LOG(INFO) << "Creating loop closure ICP.";
    clock.start();
    PointMatcher::TransformationParameters icp_solution = icp_.compute(sub_map_b, sub_map_a,
                                                                       initial_guess);
    clock.takeTime();
    LOG(INFO) << "Took " << clock.getRealTime() <<
        " ms to compute the icp_solution for the loop closure.";

    updated_loop_closure.T_a_b = convertTransformationMatrixToSE3(icp_solution);
  }

  // 将更新的闭环变换和经验历史关系加入因子图中
  LOG(INFO) << "Creating loop closure factor.";

  NonlinearFactorGraph new_factors, new_associations_factors;
  Expression<SE3> exp_T_w_b(laser_tracks_[loop_closure.track_id_b]->getValueExpression(
      updated_loop_closure.time_b_ns));
  Expression<SE3> exp_T_w_a(laser_tracks_[loop_closure.track_id_a]->getValueExpression(
      updated_loop_closure.time_a_ns));
  Expression<SE3> exp_T_a_w(kindr::minimal::inverse(exp_T_w_a));
  // exp_relative包含了里程计优化后的a_w和w_b
  Expression<SE3> exp_relative(kindr::minimal::compose(exp_T_a_w, exp_T_w_b));
  // updated_loop_closure是
  ExpressionFactor<SE3> new_factor(loop_closure_noise_model_, updated_loop_closure.T_a_b,
                                   exp_relative);
  new_factors.push_back(new_factor);

  ExpressionFactor<SE3> new_association_factor(first_association_noise_model_,
                                               updated_loop_closure.T_a_b, exp_relative);

  new_associations_factors.push_back(new_association_factor);

  // 重新估计闭环轨迹
  LOG(INFO) << "Estimating the trajectories.";
  std::vector<unsigned int> affected_worker_ids;
  affected_worker_ids.push_back(loop_closure.track_id_a);
  affected_worker_ids.push_back(loop_closure.track_id_b);
  Values new_values;
  Values result = estimateAndRemove(new_factors, new_associations_factors,
                                    new_values, affected_worker_ids,
                                    updated_loop_closure.time_b_ns);

  // 闭环后更新轨迹
  LOG(INFO) << "Updating the trajectories after LC.";
  for (auto& track: laser_tracks_) {
    track->updateFromGTSAMValues(result);
  }
  LOG(INFO) << "Updating the trajectories after LC done.";
}

// 
Values IncrementalEstimator::estimate(const gtsam::NonlinearFactorGraph& new_factors,
                                      const gtsam::Values& new_values,
                                      laser_slam::Time timestamp_ns) {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);
  // Update and force relinearization.
  isam2_.update(new_factors, new_values).print();
  // TODO Investigate why these two subsequent update calls are needed.
  isam2_.update();
  isam2_.update();

  Values result(isam2_.calculateEstimate());
  return result;
}

Values IncrementalEstimator::estimateAndRemove(
    const gtsam::NonlinearFactorGraph& new_factors,
    const gtsam::NonlinearFactorGraph& new_associations_factors,
    const gtsam::Values& new_values,
    const std::vector<unsigned int>& affected_worker_ids,
    laser_slam::Time timestamp_ns) {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);

  CHECK_EQ(affected_worker_ids.size(), 2u);

  // Find and update the factor indices to remove.
  // 查找并更新要删除的因子索引
  // ？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？
  std::vector<size_t> factor_indices_to_remove;
  
  std::string linked_workers_print = "";
  for (size_t i = 0u; i < linked_workers_.size(); ++i) {
      linked_workers_print += "Group " + std::to_string(i) + ": ";
      for (const auto& worker_id : linked_workers_[i]) {
          linked_workers_print +=  std::to_string(worker_id) + " ";
      }
  }
  LOG(INFO) << "linked_workers before " << linked_workers_print;
  
  // No factor to remove if the ids are the same.
  // 如果id相同，则不需要删除因子
  const unsigned int first_worker_id = affected_worker_ids.at(0u);
  const unsigned int second_worker_id = affected_worker_ids.at(1u);
  unsigned int worker_id_being_removed;
  if (first_worker_id != second_worker_id) {
      // Check whether the trajectories are already linked.
	  // 检查轨迹是否已经连接
      std::vector<std::vector<unsigned int> >::iterator it_first_worker_group, it_second_worker_group;
      bool workers_are_already_linked = false;
      for (std::vector<std::vector<unsigned int> >::iterator it = linked_workers_.begin();
          it != linked_workers_.end(); ++it) {
            bool first_worker_found = false;
            bool second_worker_found = false;
            if (std::find(it->begin(), it->end(), first_worker_id) != it->end()) {
                it_first_worker_group = it;
                first_worker_found = true;
            }
            if (std::find(it->begin(), it->end(), second_worker_id) != it->end()) {
                it_second_worker_group = it;
                second_worker_found = true;
            }
            if (first_worker_found && second_worker_found) {
                workers_are_already_linked = true;
            }
      }

      if (!workers_are_already_linked) {
          // Check which group should be removed (if one contains ID 0 it should be kept). 
		  // 检查应该删除哪个组(如果其中一个组包含ID 0，则应该保留该组)
		  // ？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？
		  // it_first_worker_group和it_second_worker_group是一样的？
          std::vector<std::vector<unsigned int> >::iterator it_group_to_keep, it_group_to_remove;
          if (std::find(it_first_worker_group->begin(), it_first_worker_group->end(), 0u) 
                    != it_first_worker_group->end()) {
              it_group_to_keep = it_first_worker_group;
              it_group_to_remove = it_second_worker_group;
          } else {
              it_group_to_keep = it_second_worker_group;
              it_group_to_remove = it_first_worker_group;
          }
          // Find the factor id of the prior of the group to remove and link the groups.
		  // 找到先验的组的因子id，删除并链接组
          for (const auto& worker_id : *it_group_to_remove) {
              if (factor_indices_to_remove_.count(worker_id) == 1u) {
                  factor_indices_to_remove.push_back(factor_indices_to_remove_.at(worker_id));
                  factor_indices_to_remove_.erase(worker_id);
                  worker_id_being_removed = worker_id;
              }
              it_group_to_keep->push_back(worker_id);
              
          }
          CHECK_EQ(factor_indices_to_remove.size(), 1u);

          // Erase the group to remove.
          linked_workers_.erase(it_group_to_remove);
      }
  }
  
  if (!factor_indices_to_remove.empty()) {
      LOG(INFO) << "Removing prior on worker id " << worker_id_being_removed;
  }
  
  linked_workers_print = "";
  for (size_t i = 0u; i < linked_workers_.size(); ++i) {
      linked_workers_print += "Group " + std::to_string(i) + ": ";
      for (const auto& worker_id : linked_workers_[i]) {
          linked_workers_print +=  std::to_string(worker_id) + " ";
      }
  }
  LOG(INFO) << "linked_workers after " << linked_workers_print;
  gtsam::NonlinearFactorGraph new_factors_to_add;
  if (!factor_indices_to_remove.empty()) {
    new_factors_to_add = new_associations_factors;
  } else {
    new_factors_to_add = new_factors;
  }
  isam2_.update(new_factors_to_add, new_values, factor_indices_to_remove).print();

  // TODO Investigate why these two subsequent update calls are needed.
  // ？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？
  // isam对象如何更新
  isam2_.update();
  isam2_.update();

  Values result(isam2_.calculateEstimate());
  return result;
}

gtsam::Values IncrementalEstimator::registerPrior(const gtsam::NonlinearFactorGraph& new_factors,
                                                  const gtsam::Values& new_values,
                                                  const unsigned int worker_id) {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);
  ISAM2Result update_result = isam2_.update(new_factors, new_values);

  CHECK_EQ(update_result.newFactorsIndices.size(), 1u);
  if (worker_id > 0u) {
    factor_indices_to_remove_.insert(
        std::make_pair(worker_id, update_result.newFactorsIndices.at(0u)));
  }
  std::vector<unsigned int> new_linked_worker;
  new_linked_worker.push_back(worker_id);
  linked_workers_.push_back(new_linked_worker);
  
  // Each call to iSAM2 update(*) performs one iteration of the iterative nonlinear solver.
  // If accuracy is desired at the expense of time, update(*) can be called additional times
  // to perform multiple optimizer iterations every step.
  // 每次调用iSAM2的update()都会执行迭代非线性求解器的一次迭代
  // 如果需要以时间为代价的准确性，可以额外调用update(*)以在每个步骤中执行多个优化程序迭代

  isam2_.update();
  isam2_.update();
  Values result(isam2_.calculateEstimate());
  return result;
}

std::shared_ptr<LaserTrack> IncrementalEstimator::getLaserTrack(unsigned int laser_track_id) {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);
  CHECK_GE(laser_track_id, 0u);
  CHECK_LT(laser_track_id, laser_tracks_.size());
  return laser_tracks_[laser_track_id];
}

std::vector<std::shared_ptr<LaserTrack> > IncrementalEstimator::getAllLaserTracks() {
  std::lock_guard<std::recursive_mutex> lock(full_class_mutex_);
  return laser_tracks_;
}

} // namespace laser_slam
