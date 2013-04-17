/* Raven 2 Control - Control software for the Raven II robot
 * Copyright (C) 2005-2012  H. Hawkeye King, Blake Hannaford, and the University of Washington BioRobotics Laboratory
 *
 * This file is part of Raven 2 Control.
 *
 * Raven 2 Control is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Raven 2 Control is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Raven 2 Control.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * r2_kinematics.cpp
 *
 *  Created on: Jun 18, 2012
 *      Author: Hawkeye King
 *
 *
 *  Degenerate cases in inverse kinematics:
 *    All rotational joints have problems around +/-180.  (Unhandled exception)
 *    As tool tip approaches RCM, IK fails.  This case returns -2.
 */

#include <iostream>
#include <math.h>
#include <ros/ros.h>

#include "r2_kinematics.h"
#include "log.h"
#include "local_io.h"
#include "defines.h"
#include "kinematics/kinematics_defines.h"
#include "utils.h"

extern int NUM_MECH;

const double d2r = M_PI/180;
const double r2d = 180/M_PI;
const double eps = 1.0e-5;

using namespace std;

extern unsigned long int gTime;


// Robot constants
const double La12 = 75 * M_PI/180;
const double La23 = 52 * M_PI/180;
const double La3 = 0;
const double V = 0;
const double d4 = -0.47;  // m
const double Lw = 0.013;   // m
const double GM1 = sin(La12), GM2 = cos(La12), GM3 = sin(La23), GM4 = cos(La23);

// Pointers are set to DH table for left or right arm
double const* dh_alpha;
double const* dh_a;
double *dh_theta;
double *dh_d;

// DH Parameters.
//  Variable entries are marked "V" for clarity.
//  Entered in table form, so alphas[0][0] is alpha_0 and thetas[0][0] is theta_1.
const double alphas[2][6] = {{0,    La12, M_PI- La23, 0,   M_PI/2, M_PI/2},
                             {M_PI, La12, La23,       0,   M_PI/2, M_PI/2}};  // Left / Right
const double aas[2][6]    = {{0,    0,    0,          La3, 0,      Lw},
                             {0,    0,    0,          La3, 0,      Lw}};
double ds[2][6]           = {{0,    0,    V,          d4,  0,      0},
                             {0,    0,    V,          d4,  0,      0}};
double thetas[2][6]       = {{V,    V,    M_PI/2,     V,   V,      V},
                             {V,    V,    -M_PI/2,    V,   V,      V}};


int printIK = 0;
void print_btVector(btVector3 vv);
int check_solutions(double *thetas, ik_solution * iksol, int &out_idx, double &out_err);


//////////////////////////////////////////////////////////////////////////////////
//  Calculate a transform between two links
//////////////////////////////////////////////////////////////////////////////////

/**
 * getFKTransform (int a, int b)
 *
 *    Retrieve the forward kinematics transform from a to b, i.e., ^a_bT
 *
 *    Inputs: a, starting link frame
 *            b, ending link frame
 */
btTransform getFKTransform(int a, int b)
{
	btTransform xf;
	if ( (b <= a) || b==0 )
	{
		ROS_ERROR("Invalid start/end indices.");
	}

	double xx = cos(dh_theta[a]),                  xy = -sin(dh_theta[a]),                   xz =  0;
	double yx = sin(dh_theta[a])*cos(dh_alpha[a]), yy =  cos(dh_theta[a])*cos(dh_alpha[a]),  yz = -sin(dh_alpha[a]);
	double zx = sin(dh_theta[a])*sin(dh_alpha[a]), zy =  cos(dh_theta[a])*sin(dh_alpha[a]),  zz =  cos(dh_alpha[a]);

	double px =  dh_a[a];
	double py = -sin(dh_alpha[a])*dh_d[a];
	double pz =  cos(dh_alpha[a])*dh_d[a];

	xf.setBasis(btMatrix3x3(xx, xy, xz, yx, yy, yz, zx, zy, zz ));
	xf.setOrigin(btVector3(px, py, pz));

	// recursively find transforms for following links
	if (b > a+1)
		xf *= getFKTransform(a+1, b);

	return xf;
}



//////////////////////////////////////////////////////////////////////////////////
//  Forward kinematics
//////////////////////////////////////////////////////////////////////////////////

/***
 * rw_fwd_kin() - run the ravenII forward kinematics from
 */
int r2_fwd_kin(struct device *d0, int runlevel)
{
	l_r arm;
	btTransform xf;

	/// Do FK for each mechanism
	for (int m=0; m<NUM_MECH; m++)
	{
		/// get arm type and wrist actuation angle
		if (d0->mech[m].type == GOLD_ARM_SERIAL)
			arm = dh_left;
		else
			arm = dh_right;

		double wrist2          = (d0->mech[m].joint[GRASP2].jpos - d0->mech[m].joint[GRASP1].jpos) / 2.0;
		d0->mech[m].ori.grasp  = (d0->mech[m].joint[GRASP2].jpos + d0->mech[m].joint[GRASP1].jpos) * 1000;

		double joints[6] = {
				d0->mech[m].joint[SHOULDER].jpos,
				d0->mech[m].joint[ELBOW].jpos,
				d0->mech[m].joint[Z_INS].jpos,
				d0->mech[m].joint[TOOL_ROT].jpos,
				d0->mech[m].joint[WRIST].jpos,
				wrist2
		};

		// convert from joint angle representation to DH theta convention
		double lo_thetas[6];
		joint2theta(lo_thetas, joints, arm);

		/// execute FK
		fwd_kin(lo_thetas, arm, xf);

		d0->mech[m].pos.x = xf.getOrigin()[0] * (1000.0*1000.0);
		d0->mech[m].pos.y = xf.getOrigin()[1] * (1000.0*1000.0);
		d0->mech[m].pos.z = xf.getOrigin()[2] * (1000.0*1000.0);

		for (int i=0;i<3;i++)
			for (int j=0; j<3; j++)
				d0->mech[m].ori.R[i][j] = (xf.getBasis())[i][j];

	}

    if ((runlevel != RL_PEDAL_DN) && (runlevel != RL_INIT)) {
        // set cartesian pos_d = pos.
        // That way, if anything wonky happens during state transitions
        // there won't be any discontinuities.
        // Note: in init, this is done in setStartXYZ
        for (int m = 0; m < NUM_MECH; m++) {
          d0->mech[m].pos_d.x     = d0->mech[m].pos.x;
          d0->mech[m].pos_d.y     = d0->mech[m].pos.y;
          d0->mech[m].pos_d.z     = d0->mech[m].pos.z;
          d0->mech[m].ori_d.yaw   = d0->mech[m].ori.yaw;
          d0->mech[m].ori_d.pitch = d0->mech[m].ori.pitch;
          d0->mech[m].ori_d.roll  = d0->mech[m].ori.roll;
          d0->mech[m].ori_d.grasp = d0->mech[m].ori.grasp;

          for (int k = 0; k < 3; k++)
              for (int j = 0; j < 3; j++)
            	  d0->mech[m].ori_d.R[k][j] = d0->mech[m].ori.R[k][j];
        }
        updateMasterRelativeOrigin( d0 );   // Update the origin, to which master-side deltas are added.
    }

	return 0;
}

/** fwd_kin()
 *   Runs the Raven II forward kinematics to determine end effector position.
 *   Inputs:  6 element array of joint angles ( float j[] = {shoulder, elbow, ins, roll, wrist, grasp} )
 *            Arm type, left / right ( kin.armtype arm = left/right)
 *   Outputs: cartesian transform as 4x4 transformation matrix ( bullet transform.  WHAT'S THE SYNTAX FOR THAT???)
 *   Return: 0 on success, -1 on failure
 */
int fwd_kin (double in_j[6], l_r in_arm, btTransform &out_xform )
{
	dh_alpha = alphas[in_arm];
	dh_theta = thetas[in_arm];
	dh_a     = aas[in_arm];
	dh_d     = ds[in_arm];

	for (int i=0;i<6;i++)
	{
		if (i==2)
			dh_d[i] = in_j[i];
		else
			dh_theta[i] = in_j[i];// *M_PI/180;
	}

	int armId;
	if (in_arm == dh_left) {
		armId = GOLD_ARM_ID;
	} else {
		armId = GREEN_ARM_ID;
	}
	out_xform = actual_world_to_ik_world(armId)
							* Tw2b * getFKTransform(0,1);

	// rotate to match "tilted" base
	const static btTransform zrot_l = btTransform::getIdentity(); //( btMatrix3x3 (cos(25*d2r),-sin(25*d2r),0,  sin(25*d2r),cos(25*d2r),0,  0,0,1), btVector3 (0,0,0) );
	const static btTransform zrot_r = btTransform::getIdentity(); //( btMatrix3x3 (cos(-25*d2r),-sin(-25*d2r),0,  sin(-25*d2r),cos(-25*d2r),0,  0,0,1), btVector3 (0,0,0) );


	if (in_arm == dh_left)
	{
		out_xform = zrot_l * out_xform;
	}
	else
	{
		out_xform = zrot_r * out_xform;
	}
	return 0;
}



/** getATransform()
 *   Runs the Raven II forward kinematics to determine the desired transform from frame A to frame B
 *   Inputs:  6 element array of joint angles ( float j[] = {shoulder, elbow, ins, roll, wrist, grasp} )
 *            Arm type, left / right ( kin.armtype arm = left/right)
 *   Outputs: cartesian transform as 4x4 transformation matrix ( bullet transform.  WHAT'S THE SYNTAX FOR THAT???)
 *   Return: 0 on success, -1 on failure
 */
int getATransform (struct mechanism &in_mch, btTransform &out_xform, int frameA, int frameB)
{
	l_r arm;

	/// get arm type and wrist actuation angle
	if (in_mch.type == GOLD_ARM_SERIAL)
			arm = dh_left;
		else
			arm = dh_right;

	double wrist2 = (in_mch.joint[GRASP2].jpos - in_mch.joint[GRASP1].jpos) / 2.0;

	double joints[6] = {
		in_mch.joint[SHOULDER].jpos,
		in_mch.joint[ELBOW].jpos,
		in_mch.joint[Z_INS].jpos,
		in_mch.joint[TOOL_ROT].jpos,
		in_mch.joint[WRIST].jpos,
		wrist2
	};

	// convert from joint angle representation to DH theta convention
	double lo_thetas[6];
	joint2theta(lo_thetas, joints, arm);

	dh_alpha = alphas[arm];
	dh_theta = thetas[arm];
	dh_a     = aas[arm];
	dh_d     = ds[arm];

	for (int i=0;i<6;i++)
	{
		if (i==2)
			dh_d[i] = lo_thetas[i];
		else
			dh_theta[i] = lo_thetas[i];// *M_PI/180;
	}

	out_xform = getFKTransform(frameA, frameB);

	// rotate to match "tilted" base
	// Needed?  Yes, for transform ^0_xT to get a frame aligned with base (instead of rotated 25 degrees to zero angle of shoulder joint)
	if (frameA == 0)
	{
		const static btTransform zrot_l( btMatrix3x3 (cos(25*d2r), -sin(25*d2r), 0,  sin(25*d2r), cos(25*d2r), 0,  0,0,1), btVector3 (0,0,0) );
		const static btTransform zrot_r( btMatrix3x3 (cos(-25*d2r),-sin(-25*d2r),0,  sin(-25*d2r),cos(-25*d2r),0,  0,0,1), btVector3 (0,0,0) );

		if (arm == dh_left)
		{
			out_xform = zrot_l * out_xform;
		}
		else
		{
			out_xform = zrot_r * out_xform;
		}
	}
	return 0;

}












//////////////////////////////////////////////////////////////////////////////////
//  Inverse kinematics
//////////////////////////////////////////////////////////////////////////////////

/***
 * rw_inv_kin() - run the ravenII inverse kinematics from device struct
 */
int r2_inv_kin(struct device *d0, int runlevel)
{
	l_r arm;
	btTransform xf;
	struct orientation * ori_d;
	struct position    * pos_d;

	/// Do FK for each mechanism
	for (int m=0; m<NUM_MECH; m++)
	{
		/// get arm type and wrist actuation angle
		if (d0->mech[m].type == GOLD_ARM_SERIAL)
			arm = dh_left;
		else
			arm = dh_right;

		ori_d = &(d0->mech[m].ori_d);
		pos_d = &(d0->mech[m].pos_d);

		// copy R matrix
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				(xf.getBasis())[i][j] = ori_d->R[i][j];

		xf.setBasis( btMatrix3x3(   ori_d->R[0][0], ori_d->R[0][1], ori_d->R[0][2],
									ori_d->R[1][0], ori_d->R[1][1], ori_d->R[1][2],
									ori_d->R[2][0], ori_d->R[2][1], ori_d->R[2][2]  ) );
		xf.setOrigin( btVector3(pos_d->x/(1000.0*1000.0),pos_d->y/(1000.0*1000.0), pos_d->z/(1000.0*1000.0)));




		const static btTransform zrot_l( btMatrix3x3 (cos(25*d2r),-sin(25*d2r),0,  sin(25*d2r),cos(25*d2r),0,  0,0,1), btVector3 (0,0,0) );
		const static btTransform zrot_r( btMatrix3x3 (cos(-25*d2r),-sin(-25*d2r),0,  sin(-25*d2r),cos(-25*d2r),0,  0,0,1), btVector3 (0,0,0) );


		if (arm == dh_left)
		{
			xf = zrot_l.inverse() * xf;
		}
		else
		{
			xf = zrot_r.inverse() * xf;
		}

		//		DO IK
		ik_solution iksol[8] = {{},{},{},{},{},{},{},{}};
		int ret = inv_kin(xf, arm, iksol);
		if (ret < 0)
			cout << "failed gracefully (arm:"<< arm <<" ret:" << ret<< ") j=\t\t(" << thetas[0] << ",\t" << thetas[1] << ",\t" << thetas[2] << ",\t" << thetas[3] << ",\t" << thetas[4] << ",\t" << thetas[5] << ")"<<endl;

		// Check solutions - compare IK solutions to current joint angles...
		double wrist2 = (d0->mech[m].joint[GRASP2].jpos - d0->mech[m].joint[GRASP1].jpos) / 2.0; // grep "
		double joints[6] = {
				d0->mech[m].joint[SHOULDER].jpos,
				d0->mech[m].joint[ELBOW   ].jpos,
				d0->mech[m].joint[Z_INS   ].jpos,
				d0->mech[m].joint[TOOL_ROT].jpos,
				d0->mech[m].joint[WRIST   ].jpos,
				wrist2
		};

		/// convert from joint angle representation to DH theta convention
		double thetas[6];
		joint2theta(thetas, joints, arm);
		int sol_idx=0;
		double sol_err;
		if ( check_solutions(thetas, iksol, sol_idx, sol_err) < 0)
		{
//			cout << "IK failed\n";
			return -1;
		}

		double Js[6];
		double gangle = double(d0->mech[m].ori_d.grasp) / 1000.0;
		theta2joint(iksol[sol_idx], Js);
		d0->mech[m].joint[SHOULDER].jpos_d = Js[0];
		d0->mech[m].joint[ELBOW   ].jpos_d = Js[1];
		d0->mech[m].joint[Z_INS   ].jpos_d = Js[2];
		d0->mech[m].joint[TOOL_ROT].jpos_d = Js[3];
		d0->mech[m].joint[WRIST   ].jpos_d = Js[4];
		d0->mech[m].joint[GRASP1  ].jpos_d = -Js[5] +  gangle / 2;
		d0->mech[m].joint[GRASP2  ].jpos_d =  Js[5] +  gangle / 2;

		if (printIK !=0 )// && d0->mech[m].type == GREEN_ARM_SERIAL )
		{
			log_msg("All IK solutions for mechanism %d.  Chosen solution:%d:",m, sol_idx);
			log_msg("Current     :\t( %3f,\t %3f,\t %3f,\t %3f,\t %3f,\t %3f (\t %3f/\t %3f))",
					joints[0] * r2d,
					joints[1] * r2d,
					joints[2],
					joints[3] * r2d,
					joints[4] * r2d,
					joints[5] * r2d,
					d0->mech[m].joint[GRASP2].jpos  * r2d,
					d0->mech[m].joint[GRASP2].jpos * r2d
			);
			for (int i=0; i<6; i++)
			{
				theta2joint(iksol[i], Js);
				log_msg("ik_joints[%d]:\t( %3f,\t %3f,\t %3f,\t %3f,\t %3f,\t %3f)",i,
						Js[0] * r2d,
						Js[1] * r2d,
						Js[2],
						Js[3] * r2d,
						Js[4] * r2d,
						Js[5] * r2d
						);
//			log_msg("re_thetas: [%d]\t( %3f,\t %3f,\t %3f,\t %3f,\t %3f,\t %3f)",m,
//					thetas[0] * r2d,
//					thetas[1] * r2d,
//					thetas[2],
//					thetas[3] * r2d,
//					thetas[4] * r2d,
//					thetas[5] * r2d
//					);
//			log_msg("ik_thetas: [%d]\t( %3f,\t %3f,\t %3f,\t %3f,\t %3f,\t %3f)", m,
//					iksol[sol_idx].th1 * r2d,
//					iksol[sol_idx].th2 * r2d,
//					iksol[sol_idx].d3,
//					iksol[sol_idx].th4 * r2d,
//					iksol[sol_idx].th5 * r2d,
//					iksol[sol_idx].th6 * r2d
//					);
			}
		}
	}

	printIK=0;

	return 0;
}

/**
 * inv_kin() -
 *   Runs the Raven II INVERSE kinematics to determine end effector position.
 *   Inputs:  cartesian transform as 4x4 transformation matrix ( bullit transform.  WHAT'S THE SYNTAX FOR THAT???)
 *            Arm type, left / right ( kin.armtype arm = left/right)
 *   Outputs: 6 element array of joint angles ( float j[] = {shoulder, elbow, ins, roll, wrist, grasp} )

 *   Return:
 *   	0  : success,
 *   	-1 : bad arm
 *   	-2 : too close to RCM.
 */
int  __attribute__ ((optimize("0"))) inv_kin(btTransform in_T06, l_r in_arm, ik_solution iksol[8])
{
	dh_theta = thetas[in_arm];
	dh_d     = ds[in_arm];
	dh_alpha = alphas[in_arm];
	dh_a     = aas[in_arm];
	for (int i=0;i<8;i++)
		iksol[i] = ik_zerosol;

	if  ( in_arm  >= dh_l_r_last)
	{
		ROS_ERROR("BAD ARM IN IK!!!");
		return -1;
	}

	for (int i=0;i<8;i++)    iksol[i].arm = in_arm;

	/////
	///  Step 1, Compute P5
	btTransform  T60 = in_T06.inverse();
	btVector3    p6rcm = T60.getOrigin();
	btVector3    p05[8];

	p6rcm[2]=0;    // take projection onto x-y plane
	for (int i= 0; i<2; i++)
	{
		btVector3 p65 = (-1+2*i) * Lw * p6rcm.normalize();
		p05[4*i] = p05[4*i+1] = p05[4*i+2] = p05[4*i+3] = in_T06 * p65;
	}

	/////
	///  Step 2, compute displacement of prismatic joint d3
	for (int i=0;i<2;i++)
	{
		double insertion = 0;
		insertion += p05[4*i].length();  // Two step process avoids compiler optimization problem. (Yeah, right. It was the compiler's problem...)

		if (insertion <= Lw)
		{
			cerr << "WARNING: mechanism at RCM singularity(Lw:"<< Lw <<"ins:" << insertion << ").  IK failing.\n";
			iksol[4*i + 0].invalid = iksol[4*i + 1].invalid = ik_invalid;
			iksol[4*i + 2].invalid = iksol[4*i + 3].invalid = ik_invalid;
			return -2;
		}
		iksol[4*i + 0].d3 = iksol[4*i + 1].d3 = -d4 - insertion;
		iksol[4*i + 2].d3 = iksol[4*i + 3].d3 = -d4 + insertion;
	}

	/////
	///  Step 3, calculate theta 2
	for (int i=0; i<8; i+=2) // p05 solutions
	{
		double z0p5 = p05[i][2];

		double d = iksol[i].d3 + d4;
		double cth2=0;

		if (in_arm  == dh_left)
			cth2 = 1 / (GM1*GM3) * ((-z0p5 / d) - GM2*GM4);
		else
			cth2 = 1 / (GM1*GM3) * ((z0p5 / d) + GM2*GM4);

		// Smooth roundoff errors at +/- 1.
		if      (cth2 > 1  && cth2 <  1+eps) cth2 =  1;
		else if (cth2 < -1 && cth2 > -1-eps) cth2 = -1;

		if (cth2>1 || cth2 < -1) {
//			 cout << setprecision(3) << fixed;;
//			 cout << "invalid solution ["<<i<<"] arm(" << in_arm << ") : " <<  cth2 <<" = 1 / "<< (GM1*GM3) << " *  ((" <<z0p5 <<" / "<< d <<") + "<< GM2*GM4 <<")";
//			 cout << setprecision(3) << fixed;;
//			 cout << endl;
			iksol[i].invalid = iksol[i+1].invalid = ik_invalid;
		}
		else
		{
			iksol[ i ].th2 =  acos( cth2 );
			iksol[i+1].th2 = -acos( cth2 );
		}
	}

	/////
	///  Step 4: Compute theta 1
	for (int i=0;i<8;i++)
	{
		if (iksol[i].invalid == ik_invalid)
			continue;

		double cth2 = cos(iksol[i].th2);
		double sth2 = sin(iksol[i].th2);
		double d    = iksol[i].d3 + d4;
		double BB1 = sth2*GM3;
		double BB2=0;
		btMatrix3x3 Bmx;     // using 3 vector and matrix bullet types for convenience.
		btVector3   xyp05(p05[i]);
		xyp05[2]=0;

		if (in_arm == dh_left)
		{
			BB2 = cth2*GM2*GM3 - GM1*GM4;
			Bmx.setValue(BB1,  BB2,0,   -BB2, BB1,0,   0,    0,  1 );
		}
		else
		{
			BB2 = cth2*GM2*GM3 + GM1*GM4;
			Bmx.setValue( BB1, BB2,0,   BB2,-BB1,0,    0,   0,  1 );
		}

		btVector3 scth1 = Bmx.inverse() * xyp05 * (1/d);
		iksol[i].th1 = atan2(scth1[1],scth1[0]);
	}

	/////
	///  Step 5: get theta 4, 5, 6
	for (int i=0; i<8;i++)
	{
		if (iksol[i].invalid == ik_invalid)
			continue;

		// compute T03:
		dh_theta[0] = iksol[i].th1;
		dh_theta[1] = iksol[i].th2;
		dh_d[2]     = iksol[i].d3;
		btTransform T03 = getFKTransform(0, 3);
		btTransform T36 = T03.inverse() * in_T06;

		double c5 = -T36.getBasis()[2][2];
		double s5 = (T36.getOrigin()[2]-d4)/Lw;

		// Compute theta 4:
		double c4, s4;
		if (fabs(c5) > eps)
		{
			c4 =T36.getOrigin()[0] / (Lw * c5);
			s4 =T36.getOrigin()[1] / (Lw * c5);
		}
		else
		{
			c4 = T36.getBasis()[0][2] / s5;
			s4 = T36.getBasis()[1][2] / s5;
		}
		iksol[i].th4 = atan2(s4,c4);

		// Compute theta 5:
		iksol[i].th5 = atan2(s5, c5);


		// Compute theta 6:
		double s6, c6;
		if (fabs(s5) > eps)
		{
			c6 =  T36.getBasis()[2][0] / s5;
			s6 = -T36.getBasis()[2][1] / s5;
		}
		else
		{
			dh_theta[3] = iksol[i].th4;
			dh_theta[4] = iksol[i].th5;
			btTransform T05 = T03 * getFKTransform(3, 5);
			btTransform T56 = T05.inverse() * in_T06;
			c6 =T56.getBasis()[0][0];
			s6 =T56.getBasis()[2][0];
		}
		iksol[i].th6 = atan2(s6, c6);

		if (gTime%1000 == 0 && in_arm == dh_left )
		{
//			log_msg("dh_iksols: [%d]\t( %3f,\t %3f,\t %3f,\t %3f,\t %3f,\t %3f)",0,
//					iksol[i].th1 * r2d,
//					iksol[i].th2 * r2d,
//					iksol[i].d3,
//					iksol[i].th4 * r2d,
//					iksol[i].th5 * r2d,
//					iksol[i].th6 * r2d
//					);
		}


	}
	return 0;
}


/*********
 * Check_solutions
 *
 * Input: set of inverse kinematics solutions
 * Output: the correct solution
 */
int check_solutions(double *thetas, ik_solution * iksol, int &out_idx, double &out_err)
{
	double minerr = 32765;
	int minidx=-1;
	double invalids = 0;
	double eps = 10;
	for (int i=0; i<8;i++)
	{
		if (iksol[i].invalid == ik_invalid)
		{
			invalids++;
			continue;
		}

		// check for rollover on tool roll
		if ( fabs(thetas[3] - iksol[i].th4) > 340 * d2r )
		{
			if (thetas[3] > iksol[i].th4)
				iksol[i].th4 += 2 * M_PI;
			else
				iksol[i].th4 -= 2 * M_PI;
		}

		double s2err = 0;
		s2err += pow(thetas[0] - iksol[i].th1, 2);
		s2err += pow(thetas[1] - iksol[i].th2, 2);
		s2err += pow( 100*(thetas[2] - iksol[i].d3) , 2);
		s2err += pow(thetas[3] - iksol[i].th4, 2);
		s2err += pow(thetas[4] - iksol[i].th5, 2);
		s2err += pow(thetas[5] - iksol[i].th6, 2);
		if (s2err < minerr)
		{
			minerr=s2err;
			minidx=i;
		}
	}

	if (minerr>eps)
	{
		minidx=9;
		minerr = 0;
		if (gTime %100 == 0 && iksol[minidx].arm == dh_left)
		{
			cout << "failed (err>eps) on j=\t\t(" << thetas[0] * r2d << ",\t" << thetas[1] *r2d << ",\t" << thetas[2] << ",\t" << thetas[3] * r2d << ",\t" << thetas[4] * r2d << ",\t" << thetas[5] * r2d << ")"<<endl;
			for (int idx=0;idx<8;idx++)
			{
				double s2err = 0;
				s2err += pow(thetas[0] - iksol[idx].th1*r2d, 2);
				s2err += pow(thetas[1] - iksol[idx].th2*r2d, 2);
				s2err += pow(thetas[2] - iksol[idx].d3, 2);
				s2err += pow(thetas[3] - iksol[idx].th4*r2d, 2);
				s2err += pow(thetas[4] - iksol[idx].th5*r2d, 2);
				s2err += pow(thetas[5] - iksol[idx].th6*r2d, 2);
//				cout << "sol[" << idx<<"]: err:"<<   s2err << "\t\t" << iksol[idx].th1*r2d << ",\t" << iksol[idx].th2*r2d << ",\t" <<iksol[idx].d3 << ",\t" <<iksol[idx].th4*r2d << ",\t" <<iksol[idx].th5*r2d << ",\t" <<iksol[idx].th6*r2d << ",\n";
			}
		}
		return -1;
	}

	out_idx=minidx;
	out_err=minerr;
	return 0;
}



//////////////////////////////////////////////////////////////////////////////////
// Utility functions to print out transforms
//////////////////////////////////////////////////////////////////////////////////

/**
 * print_btTransform() -
 *      print a btTransform
 */
void print_btTransform(btTransform xf)
{
	btMatrix3x3 rr = xf.getBasis();
	btVector3 vv = xf.getOrigin();
	std::stringstream ss;

	cout << fixed;
	for (int i=0; i<3; i++)
	{
		for (int j=0; j<3; j++)
			ss << rr[i][j] << "\t ";
		ss << vv[i] << endl;
	}
	ss << "0\t 0\t 0\t 1\n";
	log_msg("\n%s", ss.str().c_str());
}


/**
 * print_btVector() -
 *      print a btVector
 */
void print_btVector(btVector3 vv)
{
	std::stringstream ss;

	for (int j=0; j<3; j++)
		ss << vv[j] << ",\t ";
	log_msg("(%s)", ss.str().c_str());
}




//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////
// Conversion of J to Theta
//  J represents the physical robot joint angles.
//  Theta is used by the kinematics.
//  Theta convention was easier to solve the equations, while J was already coded in software.
//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////

const static double TH1_J0_L = -180;//-205;   //add this to J0 to get \theta1 (in deg)
const static double TH2_J1_L = -180;   //add this to J1 to get \theta2 (in deg)
const static double D3_J2_L = 0.45;    //add this to J2 to get d3 (in meters????)
const static double TH4_J3_L = 0;      //add this to J3 to get \theta4 (in deg)
const static double TH5_J4_L = -90; //90;     //add this to J4 to get \theta5 (in deg)
const static double TH6A_J5_L = 0;     //add this to J5 to get \theta6a (in deg)
const static double TH6B_J6_L = 0;     //add this to J6 to get \theta6b (in deg)

const static double TH1_J0_R = 0;//-25;    //add this to J0 to get \theta1 (in deg)
const static double TH2_J1_R = 0;      //add this to J1 to get \theta2 (in deg)
const static double D3_J2_R = 0.45;    //add this to J2 to get d3 (in meters???)
const static double TH4_J3_R = -0;     //add this to J3 to get \theta4 (in deg)
const static double TH5_J4_R = -90; //90;     //add this to J4 to get \theta5 (in deg)
const static double TH6A_J5_R = 0;     //add this to J5 to get \theta6a (in deg)
const static double TH6B_J6_R = 0;     //add this to J6 to get \theta6b (in deg)

//void joint2thetaCallback(const sensor_msgs::JointStateConstPtr joint_state)
void joint2theta(double *out_iktheta, double *in_J, l_r in_arm)
{
	//convert J to theta
	if (in_arm == dh_left)
	{
		//======================LEFT ARM===========================
		out_iktheta[0] = in_J[0] + TH1_J0_L * M_PI/180.0;
		out_iktheta[1] = in_J[1] + TH2_J1_L * M_PI/180.0;
		out_iktheta[2] = in_J[2] + D3_J2_L;
		out_iktheta[3] = in_J[3] + TH4_J3_L * M_PI/180.0;
		out_iktheta[4] = in_J[4] + TH5_J4_L * M_PI/180.0;
		out_iktheta[5] = in_J[5] + TH6A_J5_L * M_PI/180.0;
	}

	else
	{
	//======================RIGHT ARM===========================
		out_iktheta[0] = in_J[0] + TH1_J0_R * M_PI/180.0;
		out_iktheta[1] = in_J[1] + TH2_J1_R * M_PI/180.0;
		out_iktheta[2] = in_J[2] + D3_J2_R;
		out_iktheta[3] = in_J[3] + TH4_J3_R * M_PI/180.0;
		out_iktheta[4] = in_J[4] + TH5_J4_R * M_PI/180.0;
		out_iktheta[5] = in_J[5] + TH6A_J5_R * M_PI/180.0;
	}

	// bring to range {-pi , pi}
	for (int i=0;i<6;i++)
	{
		while(out_iktheta[i] > M_PI)
			out_iktheta[i] -= 2*M_PI;

		while(out_iktheta[i] < -M_PI)
			out_iktheta[i] += 2*M_PI;
	}
}


//void joint2thetaCallback(const sensor_msgs::JointStateConstPtr joint_state)
void theta2joint(ik_solution in_iktheta, double *out_J)
{
	//convert J to theta
	if (in_iktheta.arm == dh_left)
	{
		//======================LEFT ARM===========================
		out_J[0] = in_iktheta.th1 - TH1_J0_L * M_PI/180.0;
		out_J[1] = in_iktheta.th2 - TH2_J1_L * M_PI/180.0;
		out_J[2] = in_iktheta.d3  - D3_J2_L;
		out_J[3] = in_iktheta.th4 - TH4_J3_L * M_PI/180.0;
		out_J[4] = in_iktheta.th5 - TH5_J4_L * M_PI/180.0;
		out_J[5] = in_iktheta.th6 - TH6A_J5_L * M_PI/180.0;
	}

	else
	{
	//======================RIGHT ARM===========================
		out_J[0] = in_iktheta.th1 - TH1_J0_R * M_PI/180.0;
		out_J[1] = in_iktheta.th2 - TH2_J1_R * M_PI/180.0;
		out_J[2] = in_iktheta.d3  - D3_J2_R;
		out_J[3] = in_iktheta.th4 - TH4_J3_R * M_PI/180.0;
		out_J[4] = in_iktheta.th5 - TH5_J4_R * M_PI/180.0;
		out_J[5] = in_iktheta.th6 - TH6A_J5_R * M_PI/180.0;
	}

	// bring to range {-pi , pi}
	for (int i=0;i<6;i++)
	{
		while(out_J[i] > M_PI)
			out_J[i] -= 2*M_PI;

		while(out_J[i] < -M_PI)
			out_J[i] += 2*M_PI;
	}

}



void showInverseKinematicsSolutions(struct device *d0, int runlevel)
{
	printIK = 1;
	r2_inv_kin(d0, runlevel);
}
