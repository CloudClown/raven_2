/***************************
 *
 * FILE: stateEstimate.c
 * Created May 2010 by H. Hawkeye King
 *
 *    I apply filters or whatever to get an estimate of the state
 * (position and velocity) of the system.
 *
 */

#include "state_estimate.h"

extern struct DOF_type DOF_types[];
extern int NUM_MECH;

void getStateLPF(struct DOF *joint);

/*
 * stateEstimate()
 */

void stateEstimate(struct robot_device *device0)
{
    struct DOF *_joint;
    int i,j;

    //Loop through all joints
    for (i = 0; i < NUM_MECH; i++)
    {
        for (j = 0; j < MAX_DOF_PER_MECH; j++)
        {
            _joint = &(device0->mech[i].joint[j]);

//            if (_joint->type == TOOL_ROT_GOLD || _joint->type == TOOL_ROT_GOLD)
//                encToMPos(_joint);
//            else
                getStateLPF(_joint);

        }
    }
}

/*
 * filterPosition()
 *
 *  Apply an LPF to the motor position to eliminate
 * high frequency content in the control loop.  The HF
 * will drive the cable transmission unstable.
 *
 */
void getStateLPF(struct DOF *joint)
{
    // 50 HZ 3rd order butterworth
//    float B[] = {0.0029,  0.0087,  0.0087,  0.0029};
//    float A[] = {1.0000,  2.3741, -1.9294,  0.5321};
    // 75 Hz 3rd order butterworth
//    float B[] = {0.00859,  0.0258,  0.0258,  0.00859};
//    float A[] = {1.0000,   2.0651, -1.52,  0.3861};
    //  20 Hz 3rd order butterworth
//    float B[] = {0.0002196,  0.0006588,  0.0006588,  0.0002196};
//    float A[] = {1.0000,   2.7488, -2.5282,  0.7776};
    //  120 Hz 3rd order butterworth
    float B[] = {0.02864,  0.08591,  0.08591,  0.02864};
    float A[] = {1.0000,   1.5189, -0.9600,  0.2120};
//    float B[] = {1.0, 0,0,0};
//    float A[] = {0,0,0,0};

    float *oldPos = DOF_types[joint->type].old_mpos;
    float *oldFiltPos= DOF_types[joint->type].old_filtered_mpos;
    float filtPos = 0;
    float f_enc_val = joint->enc_val;

#ifdef RAVEN_II
    if ( (joint->type == SHOULDER_GOLD) ||
         (joint->type == ELBOW_GOLD)    ||
         (joint->type == Z_INS_GOLD)    ||
         (joint->type == TOOL_ROT_GOLD) ||
         (joint->type == WRIST_GOLD)    ||
         (joint->type == GRASP1_GOLD)   ||
         (joint->type == GRASP2_GOLD)
         )
         f_enc_val *= -1;
#endif

    // Calculate motor angle from encoder value
    float motorPos = (2.0*PI) * (1.0/((float)ENC_CNTS_PER_REV)) * (f_enc_val - (float)joint->enc_offset);

    // Initialize filter to steady state
    if (!DOF_types[joint->type].filterRdy)
    {
        oldPos[2] = motorPos;
        oldPos[1] = motorPos;
        oldPos[0] = motorPos;
        oldFiltPos[2] = motorPos;
        oldFiltPos[1] = motorPos;
        oldFiltPos[0] = motorPos;
        //    rt_printk("FILTER BEING READIED %d\n", gTime);
        DOF_types[joint->type].filterRdy = TRUE;
    }

    //Compute filtered motor angle
    filtPos =
        B[0] * motorPos  +
        B[1] * oldPos[0] +
        B[2] * oldPos[1] +
        B[3] * oldPos[2] +
        A[1] * oldFiltPos[0] +
        A[2] * oldFiltPos[1] +
        A[3] * oldFiltPos[2];

    // Compute velocity from first difference
    // This is safe b/c noise is removed by LPF
    joint->mvel = (filtPos - oldFiltPos[0]) / STEP_PERIOD;
    joint->mpos = filtPos;


    // Update old values for filter
    oldPos[2] = oldPos[1];
    oldPos[1] = oldPos[0];
    oldPos[0] = motorPos;
    oldFiltPos[2] = oldFiltPos[1];
    oldFiltPos[1] = oldFiltPos[0];
    oldFiltPos[0] = filtPos;

    return;
}

void resetFilter(struct DOF* _joint)
{
    //reset filter
    for (int i=0;i<=2;i++)
    {
        DOF_types[_joint->type].old_mpos[i] = _joint->mpos_d;
        DOF_types[_joint->type].old_filtered_mpos[i] = _joint->mpos_d;
    }
}

/*
  if (joint->type == SHOULDER_B) {
    if (gTime % 100 == 0)
      printk("Filter state(%d):%d\t\t%d\t%d\t%d\t%d \t\t %d\t%d\t%d\n",
	     jiffies,
	     (int)(filtPos*100),
	     (int)(motorPos*100),
	     (int)(oldPos[0]*100) ,
	     (int)(oldPos[1]*100) ,
	     (int)(oldPos[2]*100) ,
	     (int)(oldFiltPos[0]*100) ,
	     (int)(oldFiltPos[1]*100) ,
	     (int)(oldFiltPos[2]*100));
	     }
*/
