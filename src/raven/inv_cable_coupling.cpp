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


/*******************************
 *
 *  File: inv_cable_coupling.c
 *
 *  Calculate the inverse cable coupling from a Joint Space Pose,
 *  (th1, th2,d3) express the desired motor pose (m1, m2, m3)
 *
 **********************************/

#include "inv_cable_coupling.h"
#include "log.h"

extern struct DOF_type DOF_types[];
extern int NUM_MECH;
extern unsigned long int gTime;
/**
 * invCableCoupling - wrapper function that checks for correct runlevel
 *   and calls invMechCableCoupling for each mechanism in device

 *  Function should be called in all runlevels to ensure that mpos_d = jpos_d.
 *
 * \param device0 pointer to device struct
 * \param runlevel current runlevel
 *
 */
void invCableCoupling(struct device *device0, int runlevel)
{
  int i;

  //Run inverse cable coupling for each mechanism
  for (i = 0; i < NUM_MECH; i++)
    invMechCableCoupling(&(device0->mech[i]));
}



void invMechCableCoupling(struct mechanism *mech, int no_use_actual)
{
  if (mech->type != GOLD_ARM && mech->type != GREEN_ARM) {
  	log_msg("bad mech type!");
  	return;
  }

  float th1, th2, th3, th5, th6, th7;
  float d4;
  float m1, m2, m3, m4, m5, m6, m7;
  float tr1=0, tr2=0, tr3=0, tr4=0, tr5=0, tr6=0, tr7=0;

  th1 = mech->joint[SHOULDER].jpos_d;
  th2 = mech->joint[ELBOW].jpos_d;
  th3 = mech->joint[TOOL_ROT].jpos_d;
  d4 = 	mech->joint[Z_INS].jpos_d;
  th5 = mech->joint[WRIST].jpos_d;
  th6 = mech->joint[GRASP1].jpos_d;
  th7 = mech->joint[GRASP2].jpos_d;

  if (mech->type == GOLD_ARM){
	tr1 = DOF_types[SHOULDER_GOLD ].TR;
	tr2 = DOF_types[ELBOW_GOLD    ].TR;
	tr3 = DOF_types[TOOL_ROT_GOLD ].TR;
	tr4 = DOF_types[Z_INS_GOLD    ].TR;
	tr5 = DOF_types[WRIST_GOLD    ].TR;
	tr6 = DOF_types[GRASP1_GOLD   ].TR;
	tr7 = DOF_types[GRASP2_GOLD   ].TR;

  } else if (mech->type == GREEN_ARM){
	tr1 = DOF_types[SHOULDER_GREEN ].TR;
	tr2 = DOF_types[ELBOW_GREEN    ].TR;
	tr3 = DOF_types[TOOL_ROT_GREEN ].TR;
	tr4 = DOF_types[Z_INS_GREEN    ].TR;
	tr5 = DOF_types[WRIST_GREEN    ].TR;
	tr6 = DOF_types[GRASP1_GREEN   ].TR;
	tr7 = DOF_types[GRASP2_GREEN   ].TR;
  }

  m1 = tr1 * th1;
  m2 = tr2 * th2;
  m4 = tr4 * d4;

  // Use the current joint position for cable coupling
  float m4_actual = mech->joint[Z_INS].mpos;

  if (no_use_actual)
    m4_actual = m4;

  m3 = tr3 * th3 + m4_actual/GB_RATIO;
  m5 = tr5 * th5 + m4_actual/GB_RATIO;
//  m3 = tr3 * th3 + m4/GB_RATIO;
//  m5 = tr5 * th5 + m4/GB_RATIO;

  if (mech->tool_type == TOOL_GRASPER_10MM)
  {
	m6 = tr6 * th6 + m4_actual/GB_RATIO;
	m7 = tr7 * th7 + m4_actual/GB_RATIO;
  }
  else if (mech->tool_type == TOOL_GRASPER_8MM)
  {
  	// Note: sign of the last term changes for GOLD vs GREEN arm
    int sgn = (mech->type == GOLD_ARM) ? 1 : -1;
	m6 = tr6 * th6 + m4/GB_RATIO + sgn * (tr5 * th5) * (tr5/tr6);
	m7 = tr7 * th7 + m4/GB_RATIO - sgn * (tr5 * th5) * (tr5/tr6);
  }
  else  // (mech->tool_type == TOOL_NONE)
  {
	// coupling goes until the tool adapter pulleys
	m6 = tr6 * th6 + m4/GB_RATIO;
	m7 = tr7 * th7 + m4/GB_RATIO;
  }

  /*Now have solved for desired motor positions mpos_d*/
  mech->joint[SHOULDER].mpos_d 	= m1;
  mech->joint[ELBOW].mpos_d 	= m2;
  mech->joint[TOOL_ROT].mpos_d 	= m3;
  mech->joint[Z_INS].mpos_d 	= m4;
  mech->joint[WRIST].mpos_d 	= m5;
  mech->joint[GRASP1].mpos_d 	= m6;
  mech->joint[GRASP2].mpos_d 	= m7;

  return;
}



