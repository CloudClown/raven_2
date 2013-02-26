/*
 * motor_input.cpp
 *
 *  Created on: Oct 10, 2012
 *      Author: benk
 */

#include <raven/control/input/motor_input.h>

#include "defines.h"

float&
MotorValuesInput::valueByOldType(int type) {
	int arm_ind;
	int joint_ind;
	getArmAndJointIndices(type,arm_ind,joint_ind);
	int arm_id = armSerialFromID(arm_ind);

	if (joint_ind == 3) {
		throw std::out_of_range("MVI::vBOT bad ind!");
	}
	if (joint_ind > 3) {
		joint_ind -= 1;
	}
	return armById(arm_id).value(joint_ind);
}

const float&
MotorValuesInput::valueByOldType(int type) const {
	int arm_ind;
	int joint_ind;
	getArmAndJointIndices(type,arm_ind,joint_ind);
	int arm_id = armSerialFromID(arm_ind);

	if (joint_ind == 3) {
		throw std::out_of_range("MVI::vBOT bad ind!");
	}
	if (joint_ind > 3) {
		joint_ind -= 1;
	}
	return armById(arm_id).value(joint_ind);
}

Eigen::VectorXf
MotorValuesInput::values() const {
	size_t numEl = 0;
	for (size_t i=0;i<arms_.size();i++) {
		numEl += arms_[i].values().rows();
	}
	Eigen::VectorXf v(numEl);
	size_t ind = 0;
	for (size_t i=0;i<arms_.size();i++) {
		size_t numElInArm = arms_[i].values().rows();
		v.segment(ind,numElInArm) = arms_[i].values();
		ind += numElInArm;
	}
	return v;
}

void
MotorPositionInput::setFrom(DevicePtr dev) {
	FOREACH_ARM_IN_DEVICE(arm_in,dev) {
		MotorArmData& arm_curr = armById(arm_in->id());
		for (size_t i=0;i<arm_curr.size();i++) {
			arm_curr.values()[i]= arm_in->motor(i)->position();
		}
	}
}

void
MotorVelocityInput::setFrom(DevicePtr dev) {
	FOREACH_ARM_IN_DEVICE(arm_in,dev) {
		MotorArmData& arm_curr = armById(arm_in->id());
		for (size_t i=0;i<arm_curr.size();i++) {
			arm_curr.values()[i]= arm_in->motor(i)->velocity();
		}
	}
}

void
MotorTorqueInput::setFrom(DevicePtr dev) {
	FOREACH_ARM_IN_DEVICE(arm_in,dev) {
		MotorArmData& arm_curr = armById(arm_in->id());
		for (size_t i=0;i<arm_curr.size();i++) {
			arm_curr.values()[i]= arm_in->motor(i)->torque();
		}
	}
}

/**************** single arm *******************/

void
SingleArmMotorPositionInput::setFrom(DevicePtr dev) {
	ArmPtr arm = dev->getArmById(id());
	size_t numMotors = Device::numMotorsOnArmById(id());
	data().values().resize(numMotors);
	for (size_t i=0;i<numMotors;i++) {
		data().values()[i]= arm->motor(i)->position();
	}
}

void
SingleArmMotorVelocityInput::setFrom(DevicePtr dev) {
	ArmPtr arm = dev->getArmById(id());
	size_t numMotors = Device::numMotorsOnArmById(id());
	data().values().resize(numMotors);
	for (size_t i=0;i<numMotors;i++) {
		data().values()[i]= arm->motor(i)->velocity();
	}
}

void
SingleArmMotorTorqueInput::setFrom(DevicePtr dev) {
	ArmPtr arm = dev->getArmById(id());
	size_t numMotors = Device::numMotorsOnArmById(id());
	data().values().resize(numMotors);
	for (size_t i=0;i<numMotors;i++) {
		data().values()[i]= arm->motor(i)->torque();
	}
}