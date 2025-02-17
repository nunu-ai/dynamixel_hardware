// Copyright 2020 Yutaka Kondo <yutaka.kondo@youtalk.jp>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dynamixel_hardware/dynamixel_hardware.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

#include "hardware_interface/types/hardware_interface_return_values.hpp"
#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "rclcpp/rclcpp.hpp"

namespace dynamixel_hardware
{
constexpr const char * kDynamixelHardware = "DynamixelHardware";
constexpr uint8_t kGoalPositionIndex = 0;
constexpr uint8_t kGoalVelocityIndex = 1;
constexpr uint8_t kPresentPositionVelocityCurrentIndex = 0;
constexpr const char * kGoalPositionItem = "Goal_Position";
constexpr const char * kGoalVelocityItem = "Goal_Velocity";
constexpr const char * kGoalCurrentItem = "Goal_Current";
constexpr const char * kMovingSpeedItem = "Moving_Speed";
constexpr const char * kPresentPositionItem = "Present_Position";
constexpr const char * kPresentVelocityItem = "Present_Velocity";
constexpr const char * kPresentSpeedItem = "Present_Speed";
constexpr const char * kPresentCurrentItem = "Present_Current";
constexpr const char * kPresentLoadItem = "Present_Load";

return_type DynamixelHardware::configure(const hardware_interface::HardwareInfo & info)
{
  RCLCPP_DEBUG(rclcpp::get_logger(kDynamixelHardware), "configure");
  if (configure_default(info) != return_type::OK) {
    return return_type::ERROR;
  }

  // Count joints and virtual joints
  int num_joints = 0;
  int num_virtual_joints = 0;
  for (const auto & joint : info_.joints) {
    if (joint.parameters.find("is_virtual") != joint.parameters.end()) {
      num_virtual_joints++;
    } else {
      num_joints++;
    }
  }

  joints_.resize(num_joints, Joint());
  virtual_joints_.resize(num_virtual_joints, Joint());
  joint_ids_.resize(num_joints, 0);

  int joint_index = 0;
  int virtual_joint_index = 0;
  for (uint i = 0; i < info_.joints.size(); i++) {
    if (
      info_.joints[i].parameters.find("is_virtual") != info_.joints[i].parameters.end() &&
      info_.joints[i].parameters.at("is_virtual") == "true") {
      // virtual joint
      virtual_joints_[virtual_joint_index].name = info_.joints[i].name;
      virtual_joints_[virtual_joint_index].state.position =
        std::numeric_limits<double>::quiet_NaN();
      virtual_joints_[virtual_joint_index].state.velocity =
        std::numeric_limits<double>::quiet_NaN();
      virtual_joints_[virtual_joint_index].state.effort = std::numeric_limits<double>::quiet_NaN();
      virtual_joints_[virtual_joint_index].command.position =
        std::numeric_limits<double>::quiet_NaN();
      virtual_joints_[virtual_joint_index].command.velocity =
        std::numeric_limits<double>::quiet_NaN();
      virtual_joints_[virtual_joint_index].command.effort =
        std::numeric_limits<double>::quiet_NaN();

      RCLCPP_INFO(
        rclcpp::get_logger(kDynamixelHardware), "virtual joint name %s",
        info_.joints[i].name.c_str());

      virtual_joint_index++;
    } else {
      // real joint
      joint_ids_[joint_index] = std::stoi(info_.joints[i].parameters.at("id"));
      joints_[joint_index].name = info_.joints[i].name;
      joints_[joint_index].state.position = std::numeric_limits<double>::quiet_NaN();
      joints_[joint_index].state.velocity = std::numeric_limits<double>::quiet_NaN();
      joints_[joint_index].state.effort = std::numeric_limits<double>::quiet_NaN();
      joints_[joint_index].command.position = std::numeric_limits<double>::quiet_NaN();
      joints_[joint_index].command.velocity = std::numeric_limits<double>::quiet_NaN();
      joints_[joint_index].command.effort = std::numeric_limits<double>::quiet_NaN();

      if (info_.joints[i].name == "gripper") {
        gripper_id_ = joint_ids_[joint_index];
        if (info_.joints[i].parameters.find("current_limit") != info_.joints[i].parameters.end()) {
          gripper_current_limit_ = std::stof(info_.joints[i].parameters.at("current_limit"));
          RCLCPP_INFO(
            rclcpp::get_logger(kDynamixelHardware), "gripper_current_limit: %.3f",
            gripper_current_limit_);
        } else {
          RCLCPP_WARN(
            rclcpp::get_logger(kDynamixelHardware),
            "current_limit is not set for gripper. Use default: %.3f", gripper_current_limit_);
        }
        RCLCPP_INFO(
          rclcpp::get_logger(kDynamixelHardware), "joint_id %d: %d is_gripper", i,
          joint_ids_[joint_index]);
      } else {
        RCLCPP_INFO(
          rclcpp::get_logger(kDynamixelHardware), "joint_id %d: %d", i, joint_ids_[joint_index]);
      }
      ++joint_index;
    }
  }

  if (
    info_.hardware_parameters.find("use_dummy") != info_.hardware_parameters.end() &&
    info_.hardware_parameters.at("use_dummy") == "true") {
    use_dummy_ = true;
    RCLCPP_INFO(rclcpp::get_logger(kDynamixelHardware), "dummy mode");
    status_ = hardware_interface::status::CONFIGURED;
    return return_type::OK;
  }

  auto usb_port = info_.hardware_parameters.at("usb_port");
  auto baud_rate = std::stoi(info_.hardware_parameters.at("baud_rate"));
  const char * log = nullptr;

  RCLCPP_INFO(rclcpp::get_logger(kDynamixelHardware), "usb_port: %s", usb_port.c_str());
  RCLCPP_INFO(rclcpp::get_logger(kDynamixelHardware), "baud_rate: %d", baud_rate);

  if (!dynamixel_workbench_.init(usb_port.c_str(), baud_rate, &log)) {
    RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
    return return_type::ERROR;
  }

  for (auto id : joint_ids_) {
    uint16_t model_number = 0;
    if (!dynamixel_workbench_.ping(id, &model_number, &log)) {
      RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
      return return_type::ERROR;
    }
  }

  enable_torque(false);
  set_control_mode(ControlMode::Position, true);
  if (
    info_.hardware_parameters.find("torque_off") == info_.hardware_parameters.end() ||
    info_.hardware_parameters.at("torque_off") != "true") {
    enable_torque(true);
  }

  const ControlItem * goal_position =
    dynamixel_workbench_.getItemInfo(joint_ids_[0], kGoalPositionItem);
  if (goal_position == nullptr) {
    return return_type::ERROR;
  }

  const ControlItem * goal_velocity =
    dynamixel_workbench_.getItemInfo(joint_ids_[0], kGoalVelocityItem);
  if (goal_velocity == nullptr) {
    goal_velocity = dynamixel_workbench_.getItemInfo(joint_ids_[0], kMovingSpeedItem);
  }
  if (goal_velocity == nullptr) {
    return return_type::ERROR;
  }

  const ControlItem * present_position =
    dynamixel_workbench_.getItemInfo(joint_ids_[0], kPresentPositionItem);
  if (present_position == nullptr) {
    return return_type::ERROR;
  }

  const ControlItem * present_velocity =
    dynamixel_workbench_.getItemInfo(joint_ids_[0], kPresentVelocityItem);
  if (present_velocity == nullptr) {
    present_velocity = dynamixel_workbench_.getItemInfo(joint_ids_[0], kPresentSpeedItem);
  }
  if (present_velocity == nullptr) {
    return return_type::ERROR;
  }

  const ControlItem * present_current =
    dynamixel_workbench_.getItemInfo(joint_ids_[0], kPresentCurrentItem);
  if (present_current == nullptr) {
    present_current = dynamixel_workbench_.getItemInfo(joint_ids_[0], kPresentLoadItem);
  }
  if (present_current == nullptr) {
    return return_type::ERROR;
  }

  control_items_[kGoalPositionItem] = goal_position;
  control_items_[kGoalVelocityItem] = goal_velocity;
  control_items_[kPresentPositionItem] = present_position;
  control_items_[kPresentVelocityItem] = present_velocity;
  control_items_[kPresentCurrentItem] = present_current;

  if (!dynamixel_workbench_.addSyncWriteHandler(
        control_items_[kGoalPositionItem]->address, control_items_[kGoalPositionItem]->data_length,
        &log)) {
    RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
    return return_type::ERROR;
  }

  if (!dynamixel_workbench_.addSyncWriteHandler(
        control_items_[kGoalVelocityItem]->address, control_items_[kGoalVelocityItem]->data_length,
        &log)) {
    RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
    return return_type::ERROR;
  }

  uint16_t start_address = std::min(
    control_items_[kPresentPositionItem]->address, control_items_[kPresentCurrentItem]->address);
  uint16_t read_length = control_items_[kPresentPositionItem]->data_length +
                         control_items_[kPresentVelocityItem]->data_length +
                         control_items_[kPresentCurrentItem]->data_length + 2;
  if (!dynamixel_workbench_.addSyncReadHandler(start_address, read_length, &log)) {
    RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
    return return_type::ERROR;
  }

  status_ = hardware_interface::status::CONFIGURED;
  return return_type::OK;
}

std::vector<hardware_interface::StateInterface> DynamixelHardware::export_state_interfaces()
{
  RCLCPP_DEBUG(rclcpp::get_logger(kDynamixelHardware), "export_state_interfaces");
  std::vector<hardware_interface::StateInterface> state_interfaces;
  // for (uint i = 0; i < info_.joints.size(); i++) {
  //   state_interfaces.emplace_back(hardware_interface::StateInterface(
  //     info_.joints[i].name, hardware_interface::HW_IF_POSITION, &joints_[i].state.position));
  //   state_interfaces.emplace_back(hardware_interface::StateInterface(
  //     info_.joints[i].name, hardware_interface::HW_IF_VELOCITY, &joints_[i].state.velocity));
  //   state_interfaces.emplace_back(hardware_interface::StateInterface(
  //     info_.joints[i].name, hardware_interface::HW_IF_EFFORT, &joints_[i].state.effort));
  // }
  for (auto & joint : joints_) {
    state_interfaces.emplace_back(hardware_interface::StateInterface(
      joint.name, hardware_interface::HW_IF_POSITION, &joint.state.position));
    state_interfaces.emplace_back(hardware_interface::StateInterface(
      joint.name, hardware_interface::HW_IF_VELOCITY, &joint.state.velocity));
    state_interfaces.emplace_back(hardware_interface::StateInterface(
      joint.name, hardware_interface::HW_IF_EFFORT, &joint.state.effort));
  }

  for (auto & joint : virtual_joints_) {
    state_interfaces.emplace_back(hardware_interface::StateInterface(
      joint.name, hardware_interface::HW_IF_POSITION, &joint.state.position));
    state_interfaces.emplace_back(hardware_interface::StateInterface(
      joint.name, hardware_interface::HW_IF_VELOCITY, &joint.state.velocity));
    state_interfaces.emplace_back(hardware_interface::StateInterface(
      joint.name, hardware_interface::HW_IF_EFFORT, &joint.state.effort));
  }

  return state_interfaces;
}

std::vector<hardware_interface::CommandInterface> DynamixelHardware::export_command_interfaces()
{
  RCLCPP_DEBUG(rclcpp::get_logger(kDynamixelHardware), "export_command_interfaces");
  std::vector<hardware_interface::CommandInterface> command_interfaces;
  // for (uint i = 0; i < info_.joints.size(); i++) {
  //   command_interfaces.emplace_back(hardware_interface::CommandInterface(
  //     info_.joints[i].name, hardware_interface::HW_IF_POSITION, &joints_[i].command.position));
  //   command_interfaces.emplace_back(hardware_interface::CommandInterface(
  //     info_.joints[i].name, hardware_interface::HW_IF_VELOCITY, &joints_[i].command.velocity));
  // }
  for (auto & joint : joints_) {
    command_interfaces.emplace_back(hardware_interface::CommandInterface(
      joint.name, hardware_interface::HW_IF_POSITION, &joint.command.position));
    command_interfaces.emplace_back(hardware_interface::CommandInterface(
      joint.name, hardware_interface::HW_IF_VELOCITY, &joint.command.velocity));
  }

  for (auto & joint : virtual_joints_) {
    command_interfaces.emplace_back(hardware_interface::CommandInterface(
      joint.name, hardware_interface::HW_IF_POSITION, &joint.command.position));
    command_interfaces.emplace_back(hardware_interface::CommandInterface(
      joint.name, hardware_interface::HW_IF_VELOCITY, &joint.command.velocity));
  }

  return command_interfaces;
}

return_type DynamixelHardware::start()
{
  RCLCPP_DEBUG(rclcpp::get_logger(kDynamixelHardware), "start");
  // for (uint i = 0; i < joints_.size(); i++) {
  //   if (use_dummy_ && std::isnan(joints_[i].state.position)) {
  //     joints_[i].state.position = 0.0;
  //     joints_[i].state.velocity = 0.0;
  //     joints_[i].state.effort = 0.0;
  //   }
  // }

  // for virtual joints, always set to 0 when starting
  for (auto & joint : virtual_joints_) {
    if (std::isnan(joint.state.position)) {
      joint.state.position = 0.0;
      joint.state.velocity = 0.0;
      joint.state.effort = 0.0;
    }
  }

  if (use_dummy_) {
    for (auto & joint : joints_) {
      if (std::isnan(joint.state.position)) {
        joint.state.position = 0.0;
        joint.state.velocity = 0.0;
        joint.state.effort = 0.0;
      }
    }
  }

  read();
  reset_command();
  write();

  status_ = hardware_interface::status::STARTED;
  return return_type::OK;
}

return_type DynamixelHardware::stop()
{
  RCLCPP_DEBUG(rclcpp::get_logger(kDynamixelHardware), "stop");
  status_ = hardware_interface::status::STOPPED;
  return return_type::OK;
}

return_type DynamixelHardware::read()
{
  if (use_dummy_) {
    return return_type::OK;
  }

  std::vector<uint8_t> ids(joints_.size(), 0);
  std::vector<int32_t> positions(joints_.size(), 0);
  std::vector<int32_t> velocities(joints_.size(), 0);
  std::vector<int32_t> currents(joints_.size(), 0);

  std::copy(joint_ids_.begin(), joint_ids_.end(), ids.begin());
  const char * log = nullptr;

  if (!dynamixel_workbench_.syncRead(
        kPresentPositionVelocityCurrentIndex, ids.data(), ids.size(), &log)) {
    RCLCPP_ERROR(rclcpp::get_logger(kDynamixelHardware), "%s", log);
  }

  if (!dynamixel_workbench_.getSyncReadData(
        kPresentPositionVelocityCurrentIndex, ids.data(), ids.size(),
        control_items_[kPresentCurrentItem]->address,
        control_items_[kPresentCurrentItem]->data_length, currents.data(), &log)) {
    RCLCPP_ERROR(rclcpp::get_logger(kDynamixelHardware), "%s", log);
  }

  if (!dynamixel_workbench_.getSyncReadData(
        kPresentPositionVelocityCurrentIndex, ids.data(), ids.size(),
        control_items_[kPresentVelocityItem]->address,
        control_items_[kPresentVelocityItem]->data_length, velocities.data(), &log)) {
    RCLCPP_ERROR(rclcpp::get_logger(kDynamixelHardware), "%s", log);
  }

  if (!dynamixel_workbench_.getSyncReadData(
        kPresentPositionVelocityCurrentIndex, ids.data(), ids.size(),
        control_items_[kPresentPositionItem]->address,
        control_items_[kPresentPositionItem]->data_length, positions.data(), &log)) {
    RCLCPP_ERROR(rclcpp::get_logger(kDynamixelHardware), "%s", log);
  }

  for (uint i = 0; i < ids.size(); i++) {
    joints_[i].state.position = dynamixel_workbench_.convertValue2Radian(ids[i], positions[i]);
    joints_[i].state.velocity = dynamixel_workbench_.convertValue2Velocity(ids[i], velocities[i]);
    joints_[i].state.effort = dynamixel_workbench_.convertValue2Current(currents[i]);
  }

  return return_type::OK;
}

return_type DynamixelHardware::write()
{
  // for virtual joints, just copy command to state
  for (auto & joint : virtual_joints_) {
    joint.state.position = joint.command.position;
    joint.state.velocity = joint.command.velocity;
    joint.state.effort = joint.command.effort;
  }

  if (use_dummy_) {
    for (auto & joint : joints_) {
      joint.state.position = joint.command.position;
    }

    return return_type::OK;
  }

  std::vector<uint8_t> ids(joints_.size(), 0);
  std::vector<int32_t> commands(joints_.size(), 0);

  std::copy(joint_ids_.begin(), joint_ids_.end(), ids.begin());
  const char * log = nullptr;

  if (std::any_of(
        joints_.cbegin(), joints_.cend(), [](auto j) { return j.command.velocity != 0.0; })) {
    // Velocity control
    set_control_mode(ControlMode::Velocity);
    for (uint i = 0; i < ids.size(); i++) {
      commands[i] = dynamixel_workbench_.convertVelocity2Value(
        ids[i], static_cast<float>(joints_[i].command.velocity));
    }
    if (!dynamixel_workbench_.syncWrite(
          kGoalVelocityIndex, ids.data(), ids.size(), commands.data(), 1, &log)) {
      RCLCPP_ERROR(rclcpp::get_logger(kDynamixelHardware), "%s", log);
    }
    return return_type::OK;
  } else if (std::any_of(
               joints_.cbegin(), joints_.cend(), [](auto j) { return j.command.effort != 0.0; })) {
    // Effort control
    RCLCPP_ERROR(rclcpp::get_logger(kDynamixelHardware), "Effort control is not implemented");
    return return_type::ERROR;
  }

  // Position control
  set_control_mode(ControlMode::Position);
  for (uint i = 0; i < ids.size(); i++) {
    commands[i] = dynamixel_workbench_.convertRadian2Value(
      ids[i], static_cast<float>(joints_[i].command.position));
  }
  if (!dynamixel_workbench_.syncWrite(
        kGoalPositionIndex, ids.data(), ids.size(), commands.data(), 1, &log)) {
    RCLCPP_ERROR(rclcpp::get_logger(kDynamixelHardware), "%s", log);
  }

  return return_type::OK;
}

return_type DynamixelHardware::enable_torque(const bool enabled)
{
  const char * log = nullptr;

  if (enabled && !torque_enabled_) {
    for (uint i = 0; i < joints_.size(); ++i) {
      if (!dynamixel_workbench_.torqueOn(joint_ids_[i], &log)) {
        RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
        return return_type::ERROR;
      }
    }
    reset_command();
    RCLCPP_INFO(rclcpp::get_logger(kDynamixelHardware), "Torque enabled");
  } else if (!enabled && torque_enabled_) {
    for (uint i = 0; i < joints_.size(); ++i) {
      if (!dynamixel_workbench_.torqueOff(joint_ids_[i], &log)) {
        RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
        return return_type::ERROR;
      }
    }
    RCLCPP_INFO(rclcpp::get_logger(kDynamixelHardware), "Torque disabled");
  }

  torque_enabled_ = enabled;
  return return_type::OK;
}

return_type DynamixelHardware::set_control_mode(const ControlMode & mode, const bool force_set)
{
  const char * log = nullptr;

  if (mode == ControlMode::Velocity && (force_set || control_mode_ != ControlMode::Velocity)) {
    bool torque_enabled = torque_enabled_;
    if (torque_enabled) {
      enable_torque(false);
    }

    for (uint i = 0; i < joint_ids_.size(); ++i) {
      if (!dynamixel_workbench_.setVelocityControlMode(joint_ids_[i], &log)) {
        RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
        return return_type::ERROR;
      }
    }
    RCLCPP_INFO(rclcpp::get_logger(kDynamixelHardware), "Velocity control");
    control_mode_ = ControlMode::Velocity;

    if (torque_enabled) {
      enable_torque(true);
    }
  } else if (
    mode == ControlMode::Position && (force_set || control_mode_ != ControlMode::Position)) {
    bool torque_enabled = torque_enabled_;
    if (torque_enabled) {
      enable_torque(false);
    }

    for (uint i = 0; i < joint_ids_.size(); ++i) {
      if (!dynamixel_workbench_.setPositionControlMode(joint_ids_[i], &log)) {
        RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
        return return_type::ERROR;
      }
    }
    RCLCPP_INFO(rclcpp::get_logger(kDynamixelHardware), "Position control");
    control_mode_ = ControlMode::Position;

    if (torque_enabled) {
      enable_torque(true);
    }
  } else if (control_mode_ != ControlMode::Velocity && control_mode_ != ControlMode::Position) {
    RCLCPP_FATAL(
      rclcpp::get_logger(kDynamixelHardware), "Only position/velocity control are implemented");
    return return_type::ERROR;
  }

  // set current-based position control mode for gripper
  if (
    gripper_id_ != 255 &&
    (force_set || gripper_control_mode_ != ControlMode::CurrentBasedPosition)) {
    bool torque_enabled = torque_enabled_;
    if (torque_enabled) {
      enable_torque(false);
    }

    if (!dynamixel_workbench_.setCurrentBasedPositionControlMode(gripper_id_, &log)) {
      RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
      return return_type::ERROR;
    }
    int32_t current =
      dynamixel_workbench_.convertCurrent2Value(gripper_id_, gripper_current_limit_);
    if (!dynamixel_workbench_.itemWrite(gripper_id_, kGoalCurrentItem, current, &log)) {
      RCLCPP_FATAL(rclcpp::get_logger(kDynamixelHardware), "%s", log);
      return return_type::ERROR;
    }

    RCLCPP_INFO(
      rclcpp::get_logger(kDynamixelHardware), "Current-based position control for gripper");
    gripper_control_mode_ = ControlMode::CurrentBasedPosition;

    if (torque_enabled) {
      enable_torque(true);
    }
  }

  return return_type::OK;
}

return_type DynamixelHardware::reset_command()
{
  for (uint i = 0; i < joints_.size(); i++) {
    joints_[i].command.position = joints_[i].state.position;
    joints_[i].command.velocity = 0.0;
    joints_[i].command.effort = 0.0;
  }

  for (uint i = 0; i < virtual_joints_.size(); i++) {
    virtual_joints_[i].command.position = virtual_joints_[i].state.position;
    virtual_joints_[i].command.velocity = 0.0;
    virtual_joints_[i].command.effort = 0.0;
  }

  return return_type::OK;
}

}  // namespace dynamixel_hardware

#include "pluginlib/class_list_macros.hpp"

PLUGINLIB_EXPORT_CLASS(dynamixel_hardware::DynamixelHardware, hardware_interface::SystemInterface)
