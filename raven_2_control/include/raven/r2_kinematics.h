/*
 * r2_kinematics.h
 *
 *  Created on: Jun 18, 2012
 *      Author: biorobotics
 */

#ifndef R2_KINEMATICS_H_
#define R2_KINEMATICS_H_

#include <tf/transform_datatypes.h>
#include "DS0.h"


enum l_r {
	dh_left = 0,
	dh_right = 1,
	dh_l_r_last=2
};
enum ik_valid_sol {
	ik_valid = 0,
	ik_invalid = 1,
	ik_valid_sol_last = 2,
};
typedef struct {
	int invalid;
	l_r arm;
	double th1;
	double th2;
	double d3;
	double th4;
	double th5;
	double th6;
} ik_solution;

const ik_solution ik_zerosol={ik_valid,dh_left, 0,0,0, 0,0,0};

void print_btTransform(btTransform);
void print_btVector(btVector3 vv);
btTransform getFKTransform(int a, int b);


void showInverseKinematicsSolutions(struct device *d0, int runlevel);

int r2_fwd_kin(struct device *d0, int runlevel);
int getATransform (struct mechanism &in_mch, btTransform &out_xform, int frameA, int frameB);

/** fwd_kin()
 *   Runs the Raven II forward kinematics to determine end effector position.
 *   Inputs:  6 element array of joint angles ( float j[] = {shoulder, elbow, ins, roll, wrist, grasp} )
 *            Arm type, left / right ( kin.armtype arm = left/right)
 *   Outputs: cartesian transform as 4x4 transformation matrix ( bullit transform.  WHAT'S THE SYNTAX FOR THAT???)
 *   Return: 0 on success, -1 on failure
 */
int __attribute__((optimize("0"))) fwd_kin( double in_j[6], l_r in_armtype, btTransform &out_xform);




int r2_inv_kin(struct device *d0, int runlevel);

/** inv_kin()
 *   Runs the Raven II INVERSE kinematics to determine end effector position.
 *   Inputs:  cartesian transform as 4x4 transformation matrix ( bullit transform.  WHAT'S THE SYNTAX FOR THAT???)
 *            Arm type, left / right ( kin.armtype arm = left/right)
 *   Outputs: 6 element array of joint angles ( float j[] = {shoulder, elbow, ins, roll, wrist, grasp} )
 *   Return: 0 on success, -1 on failure
 */
int inv_kin (btTransform in_xf, l_r in_arm, ik_solution iksol[8]);



void joint2theta(double *out_iktheta, double *in_J, l_r);
void theta2joint(ik_solution in_iktheta, double *out_J);

#endif /* R2_KINEMATICS_H_ */
