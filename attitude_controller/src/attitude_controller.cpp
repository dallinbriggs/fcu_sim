#include "attitude_controller/attitude_controller.h"
#include <iostream>

namespace attitude_controller
{

attitudeController::attitudeController() :
  nh_(ros::NodeHandle()),
  nh_private_(ros::NodeHandle("~")),
  multicopter_()
{
  // get robot namespace
  std::string robot_namespace;
  nh_private_.param<std::string>("robot_namespace", robot_namespace, "/");
  ROS_INFO_STREAM("robot namespace " << robot_namespace);
  ros::NodeHandle robot_nh(("/" + robot_namespace).c_str());

  // retrieve multirotor configuration
  multicopter_.loadfromParam(robot_nh);
  robot_nh.param<double>("max_roll", max_roll_, 50*M_PI/180.0);
  robot_nh.param<double>("max_pitch", max_pitch_, 50*M_PI/180.0);
  robot_nh.param<double>("max_yaw_rate", max_yaw_rate_, 200*M_PI/180.0);
  robot_nh.param<double>("max_thrust", max_throttle_, 60.0);

  // dynamic reconfigure
  func_ = boost::bind(&attitudeController::gainCallback, this, _1, _2);
  server_.setCallback(func_);

  // retrieve topic names
  nh_private_.param<std::string>("odometry_topic", odometry_topic_, "ground_truth/odometry");
  nh_private_.param<std::string>("command_topic", command_topic_, "command");
  nh_private_.param<std::string>("motor_speed_command_topic", motor_speed_command_topic_, "command/motor_speed");



  // Setup publishers and subscribers
  odometry_subscriber_ = nh_.subscribe(odometry_topic_, 1, &attitudeController::odometryCallback, this);
  command_subscriber_ = nh_.subscribe(command_topic_, 1, &attitudeController::commandCallback, this);
  actuators_publisher_ = nh_.advertise<std_msgs::Float64MultiArray>(motor_speed_command_topic_, 1);

  // set time and outputs to zero
  dt_ = 0;
  time_of_last_control_ = 0;
  thrust_c_ = 0.0;
  roll_c_ = 0.0;
  pitch_c_ = 0.0;
  psidot_c_ = 0.0;

  // initialize command vectors
  rotor_velocities_.resize(multicopter_.num_rotors);
  desired_forces_.resize(4);
}


void attitudeController::commandCallback(const fcu_common::CommandConstPtr& msg){
  roll_c_ = max_roll_*msg->normalized_roll;
  pitch_c_ = max_pitch_*msg->normalized_pitch;
  psidot_c_ = max_yaw_rate_*msg->normalized_yaw;
  thrust_c_ = max_throttle_*msg->normalized_throttle;
}


void attitudeController::odometryCallback(const nav_msgs::OdometryConstPtr &msg){
  ROS_INFO("OCB");
  tf::Quaternion current_orientation;
  tf::quaternionMsgToTF(msg->pose.pose.orientation,current_orientation);
  tf::Matrix3x3(current_orientation).getRPY(phi_, theta_,psi_);

  theta_ *= -1.0; // NWU to NED
  psi_ *= -1.0; // NWU to NED

  p_ = msg->twist.twist.angular.x;
  q_ = -1.0*msg->twist.twist.angular.y; // NWU to NED
  r_ = -1.0*msg->twist.twist.angular.z; // NWU to NED

  // update loop time
  double current_time = ros::Time::now().toSec();
  if (time_of_last_control_ != 0){
    dt_ = current_time - time_of_last_control_;
  }
  time_of_last_control_ = ros::Time::now().toSec();

  if(dt_ > 0.001){
    // update control
    if(controller_type_ == PID_CONTROL){
      desired_forces_ = updatePIDControl();
    }else if(controller_type_ == HINF_CONTROL){
      desired_forces_ = updateHInfControl();
    }

    std::cout << "\ndesired forces: \n";
    std::cout << desired_forces_(0) << ", " << roll_c_ << ", " << phi_ << std::endl;
    std::cout << desired_forces_(1) << ", " << pitch_c_ << ", " <<  theta_ << std::endl;
    std::cout << desired_forces_(2) <<  ", " << psidot_c_ << ", " <<  r_ << std::endl;

    // mix output
    multicopter_.mixOutput(&rotor_velocities_, &desired_forces_);
    std_msgs::Float64MultiArray command;
    for(int i=0; i<multicopter_.num_rotors; i++){
      // saturate command
      rotor_velocities_[i] = sqrt((rotor_velocities_[i]<0.0)?0.0:rotor_velocities_[i]);
      rotor_velocities_[i] = (rotor_velocities_[i]>multicopter_.rotors[i].max_rotor_speed)?multicopter_.rotors[i].max_rotor_speed:rotor_velocities_[i];
      command.data.push_back(rotor_velocities_[i]);
      std::cout << rotor_velocities_[i] << ", ";
    }
    std::cout << "\n\n";
    actuators_publisher_.publish(command);
  }else{
    ROS_INFO_STREAM("small dt " << dt_);
  }
}


Eigen::Vector4d attitudeController::updatePIDControl(){
  Eigen::Vector4d desired_forces;
  desired_forces(0) = pid_roll_.computePID(roll_c_, phi_, dt_); // l
  desired_forces(1) = pid_pitch_.computePID(pitch_c_, theta_, dt_);// m
  desired_forces(2) = pid_yaw_rate_.computePID(psidot_c_, r_, dt_); // n
  desired_forces(3) = thrust_c_; // fz
  return desired_forces;
}


Eigen::Vector4d attitudeController::updateHInfControl(){
  Eigen::Vector3d eta, eta_r, etadot, etadot_r, etaddot_r, moments, omega;
  Eigen::Vector4d desired_forces;
  eta << phi_, theta_, psi_;
  eta_r << roll_c_, pitch_c_, yaw_c_;
  Eigen::Matrix3d R;
  R << 1, sin(phi_)*tan(theta_), cos(phi_)*tan(theta_),
       0, cos(phi_), -sin(phi_),
       0, sin(phi_)*1/cos(theta_), cos(phi_)*1/cos(theta_);
  omega << p_, q_, r_;
  etadot = R*omega;
  etadot_r << phidot_c_, thetadot_c_, psidot_c_;
  etaddot_r << phiddot_c_, thetaddot_c_, psiddot_c_;
  moments = h_inf_.computeHInfinityControl(eta, eta_r, etadot, etadot_r, etaddot_r,dt_);
  desired_forces << moments(0), moments(1), moments(2), thrust_c_;
  return desired_forces;
}

void attitudeController::gainCallback(attitude_controller::GainConfig &config, uint32_t level)
{
  controller_type_ = config.controller;
  // intialize proper controller
  if(controller_type_ == PID_CONTROL){
    ROS_WARN("using PID Control");
    ROS_WARN_STREAM("New Gains => Roll: { P: = " << config.roll_P << " I: = " << config.roll_I << " D: = " << config.roll_D);
    ROS_WARN_STREAM("New Gains => Pitch: { P: = " << config.pitch_P << " I: = " << config.pitch_I << " D: = " << config.pitch_D);
    ROS_WARN_STREAM("New Gains => yaw: { P: = " << config.yaw_rate_P << " I: = " << config.yaw_rate_I << " D: = " << config.yaw_rate_D);
    // set PID gains
    pid_roll_.setGains(config.roll_P, config.roll_I, config.roll_D);
    pid_pitch_.setGains(config.pitch_P, config.pitch_I, config.pitch_D);
    pid_yaw_rate_.setGains(config.yaw_rate_P, config.yaw_rate_I, config.yaw_rate_D);
    pid_roll_.clearIntegrator();
    pid_pitch_.clearIntegrator();
    pid_yaw_rate_.clearIntegrator();
  }else if(controller_type_ == HINF_CONTROL){
    ROS_WARN("using H-Inf Control");
    h_inf_.setOmega(config.w1,config.w2,config.w3,config.wu);
    h_inf_.setMassMatrix(multicopter_.inertia_matrix);
    h_inf_.resetIntegrator();
  }else{
    ROS_ERROR_STREAM("Improper controller type: " << controller_type_);
  }
}



























} // namespace attitude_controller
