/*
 * Copyright 2015 Fadri Furrer, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Michael Burri, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Mina Kamel, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Janosch Nikolic, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Markus Achtelik, ASL, ETH Zurich, Switzerland
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fcu_sim_plugins/gazebo_motor_model.h"

namespace gazebo {

GazeboMotorModel::~GazeboMotorModel() {
  event::Events::DisconnectWorldUpdateBegin(updateConnection_);
  if (node_handle_) {
    node_handle_->shutdown();
    delete node_handle_;
  }
}

void GazeboMotorModel::InitializeParams() {}

void GazeboMotorModel::Publish() {
  turning_velocity_msg_.data = joint_->GetVelocity(0);
  motor_velocity_pub_.publish(turning_velocity_msg_);
}

void GazeboMotorModel::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf) {
  model_ = _model;

  namespace_.clear();

  if (_sdf->HasElement("robotNamespace"))
    namespace_ = _sdf->GetElement("robotNamespace")->Get<std::string>();
  else
    gzerr << "[gazebo_motor_model] Please specify a robotNamespace.\n";
  node_handle_ = new ros::NodeHandle(namespace_);

  if (_sdf->HasElement("jointName"))
    joint_name_ = _sdf->GetElement("jointName")->Get<std::string>();
  else
    gzerr << "[gazebo_motor_model] Please specify a jointName, where the rotor is attached.\n";
  // Get the pointer to the joint.
  joint_ = model_->GetJoint(joint_name_);
  if (joint_ == NULL)
    gzthrow("[gazebo_motor_model] Couldn't find specified joint \"" << joint_name_ << "\".");


  if (_sdf->HasElement("linkName"))
    link_name_ = _sdf->GetElement("linkName")->Get<std::string>();
  else
    gzerr << "[gazebo_motor_model] Please specify a linkName of the rotor.\n";
  link_ = model_->GetLink(link_name_);
  if (link_ == NULL)
    gzthrow("[gazebo_motor_model] Couldn't find specified link \"" << link_name_ << "\".");


  if (_sdf->HasElement("motorNumber"))
    motor_number_ = _sdf->GetElement("motorNumber")->Get<int>();
  else
    gzerr << "[gazebo_motor_model] Please specify a motorNumber.\n";

  if (_sdf->HasElement("turningDirection")) {
    std::string turning_direction = _sdf->GetElement("turningDirection")->Get<std::string>();
    if (turning_direction == "cw")
      turning_direction_ = turning_direction::CW;
    else if (turning_direction == "ccw")
      turning_direction_ = turning_direction::CCW;
    else
      gzerr << "[gazebo_motor_model] Please only use 'cw' or 'ccw' as turningDirection.\n";
  }
  else
    gzerr << "[gazebo_motor_model] Please specify a turning direction ('cw' or 'ccw').\n";

  getSdfParam<std::string>(_sdf, "commandSubTopic", command_sub_topic_, "command");
  getSdfParam<std::string>(_sdf, "windSpeedSubTopic", wind_speed_sub_topic_, "wind");
  getSdfParam<std::string>(_sdf, "motorSpeedPubTopic", motor_speed_pub_topic_, "motors");

  getSdfParam<double>(_sdf, "rotorDragCoefficient", rotor_drag_coefficient_, 1.0e-4);
  getSdfParam<double>(_sdf, "rollingMomentCoefficient",rolling_moment_coefficient_, 1.3e-6);
  getSdfParam<double>(_sdf, "maxRotVelocity", max_rot_velocity_, 838.0);
  getSdfParam<double>(_sdf, "motorConstant", motor_constant_, 8.54858e-6);
  getSdfParam<double>(_sdf, "momentConstant", moment_constant_, 0.016);

  getSdfParam<double>(_sdf, "timeConstantUp", time_constant_up_, 1.0/80.0);
  getSdfParam<double>(_sdf, "timeConstantDown", time_constant_down_, 1.0/40.0);
  getSdfParam<double>(_sdf, "rotorVelocitySlowdownSim", rotor_velocity_slowdown_sim_, 10);
  getSdfParam<double>(_sdf, "initialRotorSpeed", ref_motor_rot_vel_, 0.0);

  // Set the maximumForce on the joint. This is deprecated from V5 on, and the joint won't move.
#if GAZEBO_MAJOR_VERSION < 5
  joint_->SetMaxForce(0, max_force_);
#endif
  // Listen to the update event. This event is broadcast every
  // simulation iteration.
  updateConnection_ = event::Events::ConnectWorldUpdateBegin(boost::bind(&GazeboMotorModel::OnUpdate, this, _1));

  command_sub_ = node_handle_->subscribe(command_sub_topic_, 1, &GazeboMotorModel::VelocityCallback, this);
  wind_speed_sub_ = node_handle_->subscribe(wind_speed_sub_topic_, 1, &GazeboMotorModel::WindSpeedCallback, this);
  motor_velocity_pub_ = node_handle_->advertise<std_msgs::Float32>(motor_speed_pub_topic_, 10);

  // Create the first order filter.
  rotor_velocity_filter_.reset(new FirstOrderFilter<double>(time_constant_up_, time_constant_down_, ref_motor_rot_vel_));
  joint_->SetMaxForce(0, 100);// the max force gazebo will use to set the velocity
}

// This gets called by the world update start event.
void GazeboMotorModel::OnUpdate(const common::UpdateInfo& _info) {
  sampling_time_ = _info.simTime.Double() - prev_sim_time_;
  prev_sim_time_ = _info.simTime.Double();
  UpdateForcesAndMoments();
  Publish();
}

void GazeboMotorModel::VelocityCallback(const std_msgs::Float64MultiArrayConstPtr& rot_velocities) {
  ROS_ASSERT_MSG(rot_velocities->data.size() > motor_number_,
                 "You tried to access index %d of the MotorSpeed message array which is of size %d.",
                 (int)motor_number_, (int)rot_velocities->data.size());
  ref_motor_rot_vel_ = std::min(rot_velocities->data[motor_number_], static_cast<double>(max_rot_velocity_));
}

void GazeboMotorModel::WindSpeedCallback(const geometry_msgs::Vector3ConstPtr& wind_speed) {
  // TODO(burrimi): Transform velocity to world frame if frame_id is set to something else.
  wind_speed_W_.x = wind_speed->x;
  wind_speed_W_.y = wind_speed->y;
  wind_speed_W_.z = wind_speed->z;
}

void GazeboMotorModel::UpdateForcesAndMoments() {
  // slow down response of motor
  actual_motor_rot_vel_ = rotor_velocity_filter_->updateFilter(ref_motor_rot_vel_, sampling_time_);
  double force = actual_motor_rot_vel_*actual_motor_rot_vel_ * motor_constant_;
  // Apply a force to the link.
  link_->AddRelativeForce(math::Vector3(0, 0, force));

  // Forces from Philppe Martin's and Erwan Salaün's
  // 2010 IEEE Conference on Robotics and Automation paper
  // The True Role of Accelerometer Feedback in Quadrotor Control
  // - \omega * \lambda_1 * V_A^{\perp}
  math::Vector3 joint_axis = joint_->GetGlobalAxis(0);
  math::Vector3 body_velocity_W = link_->GetWorldLinearVel();
  math::Vector3 relative_wind_velocity_W = body_velocity_W - wind_speed_W_;
  math::Vector3 body_velocity_perpendicular = relative_wind_velocity_W - (relative_wind_velocity_W.Dot(joint_axis) * joint_axis);
  math::Vector3 air_drag = -std::abs(actual_motor_rot_vel_) * rotor_drag_coefficient_ * body_velocity_perpendicular;
  link_->AddForce(air_drag);


  // Moments
  // Getting the parent link, such that the resulting torques can be applied to it.
  physics::Link_V parent_links = link_->GetParentJointsLinks();
  math::Pose pose_difference = link_->GetWorldCoGPose() - parent_links.at(0)->GetWorldCoGPose();
  math::Vector3 drag_torque(0, 0, -turning_direction_ * force * moment_constant_);
  math::Vector3 drag_torque_parent_frame = pose_difference.rot.RotateVector(drag_torque);
  parent_links.at(0)->AddRelativeTorque(drag_torque_parent_frame);

  math::Vector3 rolling_moment;
  // - \omega * \mu_1 * V_A^{\perp}
  rolling_moment = -std::abs(actual_motor_rot_vel_) * rolling_moment_coefficient_ * body_velocity_perpendicular;
  parent_links.at(0)->AddTorque(rolling_moment);

  // for animation, set the velocity of the rotor
  joint_->SetVelocity(0, turning_direction_ * actual_motor_rot_vel_ / rotor_velocity_slowdown_sim_);
}

GZ_REGISTER_MODEL_PLUGIN(GazeboMotorModel)
}
