/***************************************
 *
 * File: local_io.c
 *

Local_io keeps its own copy of DS1 for incorporating new
network-layer and toolkit updates.

The local DS1 copy is protected by a mutex.

This transient copy should then be read to another copy for
active use.

***************************************/

///TODO: Modify the guts of local comm and network layer
#ifndef _GNU_SOURCE
#define _GNU_SOURCE //For realtime posix support. see http://www.gnu.org/s/libc/manual/html_node/Feature-Test-Macros.html
#endif

#include <string.h>
#include <pthread.h>
#include <ros/ros.h>
#include <tf/transform_datatypes.h>

#include "log.h"
#include "local_io.h"
#include "utils.h"
#include "mapping.h"
#include "itp_teleoperation.h"

extern int NUM_MECH;
extern USBStruct USBBoards;
extern unsigned long int gTime;

static struct param_pass data1;
pthread_mutexattr_t data1MutexAttr;
pthread_mutex_t data1Mutex;

volatile int isUpdated; //volatile int instead of atomic_t ///Should we use atomic builtins? http://gcc.gnu.org/onlinedocs/gcc-4.1.2/gcc/Atomic-Builtins.html

btQuaternion Q_ori[2];

// initialize data arrays to zero
// create mutex
int initLocalioData(void)
{
    int i;
    pthread_mutexattr_init(&data1MutexAttr);
    pthread_mutexattr_setprotocol(&data1MutexAttr,PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&data1Mutex,&data1MutexAttr);

    pthread_mutex_lock(&data1Mutex);
    for (i=0;i<NUM_MECH;i++)
    {
        data1.xd[i].x = 0;
        data1.xd[i].y = 0;
        data1.xd[i].z = 0;
        data1.rd[i].yaw = 0;
        data1.rd[i].pitch = 0;
        data1.rd[i].roll = 0;
        data1.rd[i].grasp = 0;
        Q_ori[i] = Q_ori[i].getIdentity();
    }
    data1.surgeon_mode=0;
    pthread_mutex_unlock(&data1Mutex);
    return 0;
}

// --------------------------- //
// - Recieve userspace data  - //
//---------------------------- //
//int recieveUserspace(unsigned int fifo)
int recieveUserspace(void *u,int size)
{
    if (size==sizeof(struct u_struct))
    {
        isUpdated = TRUE;
        teleopIntoDS1((struct u_struct*)u);
    }
    return 0;
}

// teleopIntoDS1()
//
//   Input from the master is put into DS1 as pos_d.
//
void teleopIntoDS1(struct u_struct *t)
{
    struct position p;
    int i, armidx, armserial;
    pthread_mutex_lock(&data1Mutex);
    btQuaternion q_temp;
    btMatrix3x3 rot_mx_temp;

    for (i=0;i<NUM_MECH;i++)
    {
        armserial = USBBoards.boards[i]==GREEN_ARM_SERIAL ? GREEN_ARM_SERIAL : GOLD_ARM_SERIAL;
        armidx    = USBBoards.boards[i]==GREEN_ARM_SERIAL ? 1 : 0;

        // apply mapping to teleop data
        p.x = t->delx[armidx];
        p.y = t->dely[armidx];
        p.z = t->delz[armidx];

        //set local quaternion from teleop quaternion data
        q_temp.setX( t->Qx[armidx] );
        q_temp.setY( t->Qy[armidx] );
        q_temp.setZ( t->Qz[armidx] );
        q_temp.setW( t->Qw[armidx] );

        fromITP(&p, q_temp, armserial);

        data1.xd[i].x += p.x;
        data1.xd[i].y += p.y;
        data1.xd[i].z += p.z;
//        data1.rd[i].grasp = t->buttonstate[armidx];
        const int graspmax = (M_PI/2 * 1000);
        const int graspmin = (-20.0 * 1000.0 DEG2RAD);
        if (armserial == GOLD_ARM_SERIAL)
        {
            data1.rd[i].grasp -= t->grasp[armidx];
            if (data1.rd[i].grasp>graspmax) data1.rd[i].grasp=graspmax;
            else if(data1.rd[i].grasp<graspmin) data1.rd[i].grasp=graspmin;
        }
        else
        {
            data1.rd[i].grasp += t->grasp[armidx];
            if (data1.rd[i].grasp < -graspmax) data1.rd[i].grasp = -graspmax;
            else if(data1.rd[i].grasp > -graspmin) data1.rd[i].grasp = -graspmin;
        }


        //Add quaternion increment
        Q_ori[armidx]= q_temp*Q_ori[armidx];
        rot_mx_temp.setRotation(Q_ori[armidx]);

        // Set rotation command
        for (int j=0;j<3;j++)
            for (int k=0;k<3;k++)
                data1.rd[i].R[j][k] = rot_mx_temp[j][k];
    }

//    log_msg("updated d1.xd to: (%d,%d,%d)/(%d,%d,%d)",
//           data1.xd[0].x, data1.xd[0].y, data1.xd[0].z,
//           data1.xd[1].x, data1.xd[1].y, data1.xd[1].z);

    data1.surgeon_mode = t->surgeon_mode;
    pthread_mutex_unlock(&data1Mutex);
}

// checkLocalUpdates()
//
//  returns true if updates have been recieved from master or toolkit since last module update
//  returns false otherwise
//  Also, checks the last time updates were made from master.  If it has been to long
//  surgeon_mode state is set to pedal-up.
int checkLocalUpdates()
{
    static unsigned long int lastUpdated;

    if (isUpdated || lastUpdated == 0)
    {
        lastUpdated = gTime;
    }
    else if (((gTime-lastUpdated) > MASTER_CONN_TIMEOUT) && ( data1.surgeon_mode ))
    {
        // if timeout period is expired, set surgeon_mode "DISENGAGED" if currently "ENGAGED"
        log_msg("Master connection timeout.  surgeon_mode -> up.\n");
        data1.surgeon_mode = SURGEON_DISENGAGED;
        lastUpdated = gTime;
        isUpdated = TRUE;
    }

    return isUpdated;
}

// Give the latest updated DS1 to the caller.
// Precondition: d1 is a pointer to allocated memory
// Postcondition: memory location of d1 contains latest DS1 Data from network/toolkit.
struct param_pass * getRcvdParams(struct param_pass* d1)
{
    ///TODO: Check performance of trylock / default priority inversion scheme
    if (pthread_mutex_trylock(&data1Mutex)!=0)   //Use trylock since this function is called form rt-thread. return immediately with old values if unable to lock
        return d1;
    //pthread_mutex_lock(&data1Mutex); //Priority inversion enabled. Should force completion of other parts and enter into this section.
    memcpy(d1, &data1, sizeof(struct param_pass));
    isUpdated = 0;
    pthread_mutex_unlock(&data1Mutex);
    return d1;
}

// Reset writable copy of DS1
void updateMasterRelativeOrigin(struct device *device0)
{
    int armidx;
    struct orientation *_ori;
    btMatrix3x3 tmpmx;

    // update data1 (network position desired) to device0.position_desired (device position desired)
    //   This eliminates accumulation of deltas from network while robot is idle.
    pthread_mutex_lock(&data1Mutex);
    for (int i=0;i<NUM_MECH;i++)
    {
        data1.xd[i].x = device0->mech[i].pos_d.x;
        data1.xd[i].y = device0->mech[i].pos_d.y;
        data1.xd[i].z = device0->mech[i].pos_d.z;
        _ori = &(device0->mech[i].ori_d);
        data1.rd[i].grasp = _ori->grasp;
        for (int j=0;j<3;j++)
            for (int k=0;k<3;k++)
                data1.rd[i].R[j][k] = _ori->R[j][k];

        // Set the local quaternion orientation rep.
        armidx = USBBoards.boards[i]==GREEN_ARM_SERIAL ? 1 : 0;
        tmpmx.setValue(_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
                        _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
                        _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
        tmpmx.getRotation(Q_ori[armidx]);

    }
    pthread_mutex_unlock(&data1Mutex);
    isUpdated = TRUE;

    return;
}

///
/// PUBLISH ROS DATA
///
#include <tf/transform_datatypes.h>
#include <raven_2/raven_state.h>
#include <raven_2/joint_command.h>
#include <visualization_msgs/Marker.h>
#include <sensor_msgs/JointState.h>


void publish_joints(struct robot_device*);
void publish_marker(struct robot_device*);

using namespace raven_2;
// Global publisher for raven data
ros::Publisher pub_ravenstate;
ros::Publisher joint_publisher;
ros::Publisher vis_pub1;
ros::Publisher vis_pub2;
ros::Subscriber joint_sub1;
ros::Subscriber joint_sub2;

/**
*    init_ravenstate_publishing()  - initialize publishing data to ros.
*/
int init_ravenstate_publishing(ros::NodeHandle &n){
    pub_ravenstate = n.advertise<raven_state>("ravenstate", 1000);
    joint_publisher = n.advertise<sensor_msgs::JointState>("joint_states", 1);
    vis_pub1 = n.advertise<visualization_msgs::Marker>( "visualization_marker1", 0 );
    vis_pub2 = n.advertise<visualization_msgs::Marker>( "visualization_marker2", 0 );
    return 0;
}

extern device* device0ptr;
void jointCallback1(const joint_command::ConstPtr& joint_cmd) {
  printf("joint callback!\n");
  for (int i=0; i < 8; i++) {
    device0ptr->mech[0].joint[i].tau_d = joint_cmd->torque[i];
  }
}


void init_joint_subs(ros::NodeHandle &n) {
    joint_sub1 = n.subscribe("joint_cmd1", 1, jointCallback1);
    //joint_sub2 = n.subscribe("joint_cmd2", 2, jointCallback2);    
}

/*
* publish_ravenstate_ros()
*
*   Copy robot data over to ros message type and publish.
*/
void publish_ravenstate_ros(struct robot_device *dev,u_08 runlevel,u_08 sublevel){
    static int count=0;
    static raven_state msg_ravenstate;  // satic variables to minimize memory allocation calls
    static ros::Time t1;
    static ros::Time t2;
    static ros::Duration d;

    if (count == 0){
        t1 = t1.now();
    }
    count ++;
    t2 = t2.now();
    d = t2-t1;

//    if (d.toSec()<0.01)
//        return;

    msg_ravenstate.dt=d;
    t1=t2;

    publish_joints(dev);

    // Copy the robot state to the output datastructure.
    int numdof=8;
    for (int j=0; j<NUM_MECH; j++){
        msg_ravenstate.type[j]    = dev->mech[j].type;
        msg_ravenstate.pos[j*3]   = dev->mech[j].pos.x;
        msg_ravenstate.pos[j*3+1] = dev->mech[j].pos.y;
        msg_ravenstate.pos[j*3+2] = dev->mech[j].pos.z;
        msg_ravenstate.pos_d[j*3]   = dev->mech[j].pos_d.x;
        msg_ravenstate.pos_d[j*3+1] = dev->mech[j].pos_d.y;
        msg_ravenstate.pos_d[j*3+2] = dev->mech[j].pos_d.z;
        for (int i=0; i<numdof; i++){
            int jtype = dev->mech[j].joint[i].type;
            msg_ravenstate.encVals[jtype]    = dev->mech[j].joint[i].enc_val;
            msg_ravenstate.tau[jtype]        = dev->mech[j].joint[i].tau_d;
            msg_ravenstate.mpos[jtype]       = dev->mech[j].joint[i].mpos RAD2DEG;
            msg_ravenstate.jpos[jtype]       = dev->mech[j].joint[i].jpos RAD2DEG;
            msg_ravenstate.mvel[jtype]       = dev->mech[j].joint[i].mvel RAD2DEG;
            msg_ravenstate.jvel[jtype]       = dev->mech[j].joint[i].jvel RAD2DEG;
            msg_ravenstate.jpos_d[jtype]     = dev->mech[j].joint[i].jpos_d RAD2DEG;
            msg_ravenstate.mpos_d[jtype]     = dev->mech[j].joint[i].mpos_d RAD2DEG;
            msg_ravenstate.encoffsets[jtype] = dev->mech[j].joint[i].enc_offset;
        }
    }
    msg_ravenstate.f_secs = d.toSec();
    msg_ravenstate.runlevel=runlevel;
    msg_ravenstate.sublevel=sublevel;

    // Publish the raven data to ROS
    pub_ravenstate.publish(msg_ravenstate);
}

/**
*   publish_joints() - publish joint angles to visualization
*/
void publish_joints(struct robot_device* device0){

    static int count=0;
    static ros::Time t1;
    static ros::Time t2;
    static ros::Duration d;

    if (count == 0){
        t1 = t1.now();
    }
    count ++;
    t2 = t2.now();
    d = t2-t1;

    if (d.toSec()<0.030)
        return;
    t1=t2;

    publish_marker(device0);

    sensor_msgs::JointState joint_state;
    //update joint_state
    joint_state.header.stamp = ros::Time::now();
    joint_state.name.resize(28);
    joint_state.position.resize(28);
//    joint_state.name.resize(14);
//    joint_state.position.resize(14);
    int left, right;
    if (device0->mech[0].type == GOLD_ARM)
    {
        left = 0;
        right = 1;
    }
    else
    {
        left = 1;
        right = 0;
    }
//======================LEFT ARM===========================
    joint_state.name[0] ="shoulder_L";
    joint_state.position[0] = device0->mech[left].joint[0].jpos;
    joint_state.name[1] ="elbow_L";
    joint_state.position[1] = device0->mech[left].joint[1].jpos;
    joint_state.name[2] ="insertion_L";
    joint_state.position[2] = device0->mech[left].joint[2].jpos;
    joint_state.name[3] ="tool_roll_L";
    joint_state.position[3] = device0->mech[left].joint[4].jpos;
    joint_state.name[4] ="wrist_joint_L";
    joint_state.position[4] = device0->mech[left].joint[5].jpos;
    joint_state.name[5] ="grasper_joint_1_L";
    joint_state.position[5] = device0->mech[left].joint[6].jpos;
    joint_state.name[6] ="grasper_joint_2_L";
    joint_state.position[6] = device0->mech[left].joint[7].jpos;
//======================RIGHT ARM===========================
    joint_state.name[7] ="shoulder_R";
    joint_state.position[7] = device0->mech[right].joint[0].jpos;
    joint_state.name[8] ="elbow_R";
    joint_state.position[8] = device0->mech[right].joint[1].jpos;
    joint_state.name[9] ="insertion_R";
    joint_state.position[9] = device0->mech[right].joint[2].jpos;
    joint_state.name[10] ="tool_roll_R";
    joint_state.position[10] = device0->mech[right].joint[4].jpos;
    joint_state.name[11] ="wrist_joint_R";
    joint_state.position[11] = device0->mech[right].joint[5].jpos;
    joint_state.name[12] ="grasper_joint_1_R";
    joint_state.position[12] = device0->mech[right].joint[6].jpos;
    joint_state.name[13] ="grasper_joint_2_R";
    joint_state.position[13] = device0->mech[right].joint[7].jpos;
//======================LEFT ARM===========================
    joint_state.name[14] ="shoulder_L2";
    joint_state.position[14] = device0->mech[left].joint[0].jpos_d;
    joint_state.name[15] ="elbow_L2";
    joint_state.position[15] = device0->mech[left].joint[1].jpos_d;
    joint_state.name[16] ="insertion_L2";
    joint_state.position[16] = device0->mech[left].joint[2].jpos_d;

    joint_state.name[17] ="tool_roll_L2";
    joint_state.position[17] = device0->mech[left].joint[4].jpos_d;
    joint_state.name[18] ="wrist_joint_L2";
    joint_state.position[18] = device0->mech[left].joint[5].jpos_d;
    joint_state.name[19] ="grasper_joint_1_L2";
    joint_state.position[19] = device0->mech[left].joint[6].jpos_d;
    joint_state.name[20] ="grasper_joint_2_L2";
    joint_state.position[20] = device0->mech[left].joint[7].jpos_d;
//======================RIGHT ARM===========================
    joint_state.name[21] ="shoulder_R2";
    joint_state.position[21] = device0->mech[right].joint[0].jpos_d;
    joint_state.name[22] ="elbow_R2";
    joint_state.position[22] = device0->mech[right].joint[1].jpos_d;
    joint_state.name[23] ="insertion_R2";
    joint_state.position[23] = device0->mech[right].joint[2].jpos_d;
    joint_state.name[24] ="tool_roll_R2";
    joint_state.position[24] = device0->mech[right].joint[4].jpos_d;
    joint_state.name[25] ="wrist_joint_R2";
    joint_state.position[25] = device0->mech[right].joint[5].jpos_d;
    joint_state.name[26] ="grasper_joint_1_R2";
    joint_state.position[26] = device0->mech[right].joint[6].jpos_d;
    joint_state.name[27] ="grasper_joint_2_R2";
    joint_state.position[27] = device0->mech[right].joint[7].jpos_d;

    //Publish the joint states
    joint_publisher.publish(joint_state);

}


void publish_marker(struct robot_device* device0)
{
    visualization_msgs::Marker marker1, marker2;
    geometry_msgs::Point p, px,py,pz;
    btQuaternion bq;
    struct orientation* _ori;
    btMatrix3x3 xform;

    visualization_msgs::Marker axes[3];
    btQuaternion axq, oriq;

    int left,right;

    if (device0->mech[0].type == GOLD_ARM)
    {
        left = 0;
        right = 1;
    }
    else
    {
        left = 1;
        right = 0;
    }


    // setup marker
    marker1.type = visualization_msgs::Marker::SPHERE;
    marker1.action = visualization_msgs::Marker::ADD;   // Set the marker action.  Options are ADD and DELETE
    marker1.header.stamp = ros::Time::now();
    marker1.ns = "RCM_marker";
    marker1.lifetime = ros::Duration();
    // Set the scale of the marker -- 1x1x1 here means 1m on a side
    marker1.scale.x = 0.020;
    marker1.scale.y = 0.020;
    marker1.scale.z = 0.020;


    // DRAW TEH SPHERE
    int draw_L_sphere=0;
    if (draw_L_sphere)
    {
        marker1.type = visualization_msgs::Marker::SPHERE;
        // Set the frame ID and timestamp.  See the TF tutorials for information on these.
        marker1.header.frame_id = "/base_link_L";
        marker1.id = 0;
        // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
        p.x = device0->mech[left].pos.x/1e6;
        p.y = device0->mech[left].pos.y/1e6;
        p.z = device0->mech[left].pos.z/1e6;
        marker1.pose.position.x = device0->mech[left].pos.x/1e6;
        marker1.pose.position.y = device0->mech[left].pos.y/1e6;
        marker1.pose.position.z = device0->mech[left].pos.z/1e6;

        // Get quaternion representation of rotation
        _ori = &(device0->mech[left].ori);
        xform.setValue(_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
                       _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
                       _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
        xform.getRotation(bq);
        marker1.pose.orientation.x = bq.getX();
        marker1.pose.orientation.y = bq.getY();
        marker1.pose.orientation.z = bq.getZ();
        marker1.pose.orientation.w = bq.getW();

        // Set the color -- be sure to set alpha to something non-zero!
        marker1.color.r = 1.0f;
        marker1.color.g = 0.0f;
        marker1.color.b = 0.0f;
        marker1.color.a = 1.0;
        // Publish the marker
        vis_pub1.publish(marker1);
    }


    int draw_L_axes=1;
    if (draw_L_axes)
    {
        for (int i=0;i<3;i++)
        {
            axes[i].type = visualization_msgs::Marker::ARROW;
            axes[i].action = visualization_msgs::Marker::ADD;   // Set the marker action.  Options are ADD and DELETE
            axes[i].header.stamp = ros::Time::now();
            axes[i].ns = "RCM_marker";
            axes[i].lifetime = ros::Duration();
            // Set the scale of the marker -- 1x1x1 here means 1m on a side
            axes[i].scale.x = 0.020;
            axes[i].scale.y = 0.020;
            axes[i].scale.z = 0.020;

            // Set the frame ID and timestamp.  See the TF tutorials for information on these.
            axes[i].header.frame_id = "/base_link_L";
            axes[i].id = 10+i;
            // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
            axes[i].pose.position.x = device0->mech[left].pos.x/1e6;
            axes[i].pose.position.y = device0->mech[left].pos.y/1e6;
            axes[i].pose.position.z = device0->mech[left].pos.z/1e6;

            // Set the color -- be sure to set alpha to something non-zero!
            axes[i].color.r = 0.0f;
            axes[i].color.g = 0.0f;
            axes[i].color.b = 0.0f;
            axes[i].color.a = 1.0;

        }

        // get the transform rotation
        _ori = &(device0->mech[left].ori);
//        xform.setValue(_ori->R[0][0], _ori->R[1][0], _ori->R[2][0],
//                            _ori->R[0][1], _ori->R[1][1], _ori->R[2][1],
//                            _ori->R[0][2], _ori->R[1][2], _ori->R[2][2]);
        xform.setValue(_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
                       _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
                       _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
        xform.getRotation(bq);

        // draw the axes
        xform.setValue(1,0,0,   0,1,0,    0,0,1);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[0].pose.orientation.x = oriq.getX();
        axes[0].pose.orientation.y = oriq.getY();
        axes[0].pose.orientation.z = oriq.getZ();
        axes[0].pose.orientation.w = oriq.getW();
        axes[0].color.r = 1.0f;

        xform.setValue(0,-1,0,    1,0,0,     0,0,1);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[1].pose.orientation.x = oriq.getX();
        axes[1].pose.orientation.y = oriq.getY();
        axes[1].pose.orientation.z = oriq.getZ();
        axes[1].pose.orientation.w = oriq.getW();
        axes[1].color.g = 1.0f;

        xform.setValue(0,0,-1,   0,1,0,    1,0,0);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[2].pose.orientation.x = oriq.getX();
        axes[2].pose.orientation.y = oriq.getY();
        axes[2].pose.orientation.z = oriq.getZ();
        axes[2].pose.orientation.w = oriq.getW();
        axes[2].color.b = 1.0f;

        // Publish the marker
        vis_pub1.publish(axes[0]);
        vis_pub1.publish(axes[1]);
        vis_pub1.publish(axes[2]);
    }

    int draw_R_sphere=0;
    if (draw_R_sphere)
    {
        marker1.type = visualization_msgs::Marker::SPHERE;

        // Set the frame ID and timestamp.  See the TF tutorials for information on these.
        marker1.header.frame_id = "/base_link_R";
        marker1.id = 1;
        // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
        marker1.pose.position.x = device0->mech[right].pos.x/1e6;
        marker1.pose.position.y = device0->mech[right].pos.y/1e6;
        marker1.pose.position.z = device0->mech[right].pos.z/1e6;

        // Get quaternion representation of rotation
        _ori = &(device0->mech[right].ori);
        xform.setValue(_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
                       _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
                       _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
        xform.getRotation(bq);
        marker1.pose.orientation.x = bq.getX();
        marker1.pose.orientation.y = bq.getY();
        marker1.pose.orientation.z = bq.getZ();
        marker1.pose.orientation.w = bq.getW();

        // Set the color -- be sure to set alpha to something non-zero!
        marker1.color.r = 0.0f;
        marker1.color.g = 1.0f;
        marker1.color.b = 0.0f;
        marker1.color.a = 1.0;
        // Publish the marker
        vis_pub1.publish(marker1);
    }



    int draw_R_axes=1;
    if (draw_R_axes)
    {
        for (int i=0;i<3;i++)
        {
            axes[i].type = visualization_msgs::Marker::ARROW;
            axes[i].action = visualization_msgs::Marker::ADD;   // Set the marker action.  Options are ADD and DELETE
            axes[i].header.stamp = ros::Time::now();
            axes[i].ns = "RCM_marker";
            axes[i].lifetime = ros::Duration();
            // Set the scale of the marker -- 1x1x1 here means 1m on a side
            axes[i].scale.x = 0.020;
            axes[i].scale.y = 0.020;
            axes[i].scale.z = 0.020;

            // Set the frame ID and timestamp.  See the TF tutorials for information on these.
            axes[i].header.frame_id = "/base_link_R";
            axes[i].id = 30+i;
            // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
            axes[i].pose.position.x = device0->mech[right].pos.x/1e6;
            axes[i].pose.position.y = device0->mech[right].pos.y/1e6;
            axes[i].pose.position.z = device0->mech[right].pos.z/1e6;

            // Set the color -- be sure to set alpha to something non-zero!
            axes[i].color.r = 0.0f;
            axes[i].color.g = 0.0f;
            axes[i].color.b = 0.0f;
            axes[i].color.a = 1.0;

        }

        // get the transform rotation
        _ori = &(device0->mech[right].ori);
//        xform.setValue(_ori->R[0][0], _ori->R[1][0], _ori->R[2][0],
//                            _ori->R[0][1], _ori->R[1][1], _ori->R[2][1],
//                            _ori->R[0][2], _ori->R[1][2], _ori->R[2][2]);
        xform.setValue(_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
                       _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
                       _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
        xform.getRotation(bq);

        // draw the axes
        xform.setValue(1,0,0,   0,1,0,    0,0,1);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[0].pose.orientation.x = oriq.getX();
        axes[0].pose.orientation.y = oriq.getY();
        axes[0].pose.orientation.z = oriq.getZ();
        axes[0].pose.orientation.w = oriq.getW();
        axes[0].color.r = 1.0f;

        xform.setValue(0,-1,0,    1,0,0,     0,0,1);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[1].pose.orientation.x = oriq.getX();
        axes[1].pose.orientation.y = oriq.getY();
        axes[1].pose.orientation.z = oriq.getZ();
        axes[1].pose.orientation.w = oriq.getW();
        axes[1].color.g = 1.0f;

        xform.setValue(0,0,-1,   0,1,0,    1,0,0);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[2].pose.orientation.x = oriq.getX();
        axes[2].pose.orientation.y = oriq.getY();
        axes[2].pose.orientation.z = oriq.getZ();
        axes[2].pose.orientation.w = oriq.getW();
        axes[2].color.b = 1.0f;

        // Publish the marker
        vis_pub1.publish(axes[0]);
        vis_pub1.publish(axes[1]);
        vis_pub1.publish(axes[2]);
    }

    int draw_L2_sphere = 0;
    if (draw_L2_sphere)
    {
        // Set the frame ID and timestamp.  See the TF tutorials for information on these.
        marker1.header.frame_id = "/base_link_L2";
        marker1.id = 2;
        // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
        marker1.pose.position.x = device0->mech[left].pos_d.x/1e6;
        marker1.pose.position.y = device0->mech[left].pos_d.y/1e6;
        marker1.pose.position.z = device0->mech[left].pos_d.z/1e6;
        _ori = &(device0->mech[left].ori_d);
        xform.setValue(_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
                       _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
                       _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
        xform.getRotation(bq);
        marker1.pose.orientation.x = bq.getX();
        marker1.pose.orientation.y = bq.getY();
        marker1.pose.orientation.z = bq.getZ();
        marker1.pose.orientation.w = bq.getW();

        // Set the color -- be sure to set alpha to something non-zero!
        marker1.color.r = 1.0f;
        marker1.color.g = 0.5f;
        marker1.color.b = 0.0f;
        marker1.color.a = 1.0;
        // Publish the marker
        vis_pub2.publish(marker1);
    }

    int draw_L2_axes = 1;
    if (draw_L2_axes)
    {
        for (int i=0;i<3;i++)
        {
            axes[i].type = visualization_msgs::Marker::ARROW;
            axes[i].action = visualization_msgs::Marker::ADD;   // Set the marker action.  Options are ADD and DELETE
            axes[i].header.stamp = ros::Time::now();
            axes[i].ns = "RCM_marker";
            axes[i].lifetime = ros::Duration();
            // Set the scale of the marker -- 1x1x1 here means 1m on a side
            axes[i].scale.x = 0.020;
            axes[i].scale.y = 0.020;
            axes[i].scale.z = 0.020;

            // Set the frame ID and timestamp.  See the TF tutorials for information on these.
            axes[i].header.frame_id = "/base_link_L2";
            axes[i].id = 50+i;
            // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
            axes[i].pose.position.x = device0->mech[left].pos_d.x/1e6;
            axes[i].pose.position.y = device0->mech[left].pos_d.y/1e6;
            axes[i].pose.position.z = device0->mech[left].pos_d.z/1e6;

            // Set the color -- be sure to set alpha to something non-zero!
            axes[i].color.r = 0.0f;
            axes[i].color.g = 0.0f;
            axes[i].color.b = 0.0f;
            axes[i].color.a = 1.0;

        }
        // Get the device transform
        _ori = &(device0->mech[left].ori_d);
        xform.setValue(_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
                       _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
                       _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
        xform.getRotation(bq);
        // Draw the axes
        xform.setValue(1,0,0,   0,1,0,    0,0,1);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[0].pose.orientation.x = oriq.getX();
        axes[0].pose.orientation.y = oriq.getY();
        axes[0].pose.orientation.z = oriq.getZ();
        axes[0].pose.orientation.w = oriq.getW();
        axes[0].color.r = 1.0f;

        xform.setValue(0,-1,0,    1,0,0,     0,0,1);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[1].pose.orientation.x = oriq.getX();
        axes[1].pose.orientation.y = oriq.getY();
        axes[1].pose.orientation.z = oriq.getZ();
        axes[1].pose.orientation.w = oriq.getW();
        axes[1].color.g = 1.0f;

        xform.setValue(0,0,-1,   0,1,0,    1,0,0);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[2].pose.orientation.x = oriq.getX();
        axes[2].pose.orientation.y = oriq.getY();
        axes[2].pose.orientation.z = oriq.getZ();
        axes[2].pose.orientation.w = oriq.getW();
        axes[2].color.b = 1.0f;

        // Publish the marker
        vis_pub2.publish(axes[0]);
        vis_pub2.publish(axes[1]);
        vis_pub2.publish(axes[2]);
    }

    int draw_R2_sphere = 0;
    if (draw_R2_sphere)
    {
        // Set the frame ID and timestamp.  See the TF tutorials for information on these.
        marker1.header.frame_id = "/base_link_R2";
        marker1.id = 3;
        // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
        marker1.pose.position.x = device0->mech[right].pos_d.x/1e6;
        marker1.pose.position.y = device0->mech[right].pos_d.y/1e6;
        marker1.pose.position.z = device0->mech[right].pos_d.z/1e6;
        _ori = &(device0->mech[right].ori_d);
        xform.setValue(_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
                       _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
                       _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
        xform.getRotation(bq);
        marker1.pose.orientation.x = bq.getX();
        marker1.pose.orientation.y = bq.getY();
        marker1.pose.orientation.z = bq.getZ();
        marker1.pose.orientation.w = bq.getW();
        // Set the color -- be sure to set alpha to something non-zero!
        marker1.color.r = 0.5f;
        marker1.color.g = 1.0f;
        marker1.color.b = 0.0f;
        marker1.color.a = 1.0;
        // Publish the marker
        vis_pub2.publish(marker1);
    }

    int draw_R2_axes = 1;
    if (draw_R2_axes)
    {
        for (int i=0;i<3;i++)
        {
            axes[i].type = visualization_msgs::Marker::ARROW;
            axes[i].action = visualization_msgs::Marker::ADD;   // Set the marker action.  Options are ADD and DELETE
            axes[i].header.stamp = ros::Time::now();
            axes[i].ns = "RCM_marker";
            axes[i].lifetime = ros::Duration();
            // Set the scale of the marker -- 1x1x1 here means 1m on a side
            axes[i].scale.x = 0.020;
            axes[i].scale.y = 0.020;
            axes[i].scale.z = 0.020;

            // Set the frame ID and timestamp.  See the TF tutorials for information on these.
            axes[i].header.frame_id = "/base_link_R2";
            axes[i].id = 40+i;
            // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
            axes[i].pose.position.x = device0->mech[right].pos_d.x/1e6;
            axes[i].pose.position.y = device0->mech[right].pos_d.y/1e6;
            axes[i].pose.position.z = device0->mech[right].pos_d.z/1e6;

            // Set the color -- be sure to set alpha to something non-zero!
            axes[i].color.r = 0.0f;
            axes[i].color.g = 0.0f;
            axes[i].color.b = 0.0f;
            axes[i].color.a = 1.0;

        }
        // Get the device transform
        _ori = &(device0->mech[right].ori_d);
        xform.setValue(_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
                       _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
                       _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
        xform.getRotation(bq);
        // Draw the axes
        xform.setValue(1,0,0,   0,1,0,    0,0,1);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[0].pose.orientation.x = oriq.getX();
        axes[0].pose.orientation.y = oriq.getY();
        axes[0].pose.orientation.z = oriq.getZ();
        axes[0].pose.orientation.w = oriq.getW();
        axes[0].color.r = 1.0f;

        xform.setValue(0,-1,0,    1,0,0,     0,0,1);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[1].pose.orientation.x = oriq.getX();
        axes[1].pose.orientation.y = oriq.getY();
        axes[1].pose.orientation.z = oriq.getZ();
        axes[1].pose.orientation.w = oriq.getW();
        axes[1].color.g = 1.0f;

        xform.setValue(0,0,-1,   0,1,0,    1,0,0);
        xform.getRotation(axq);
        oriq = bq * axq;
        axes[2].pose.orientation.x = oriq.getX();
        axes[2].pose.orientation.y = oriq.getY();
        axes[2].pose.orientation.z = oriq.getZ();
        axes[2].pose.orientation.w = oriq.getW();
        axes[2].color.b = 1.0f;

        // Publish the marker
        vis_pub2.publish(axes[0]);
        vis_pub2.publish(axes[1]);
        vis_pub2.publish(axes[2]);
    }

    int draw_xx_axes=0;
    if (draw_xx_axes)
    {
        for (int i=0;i<3;i++)
        {
            axes[i].type = visualization_msgs::Marker::ARROW;
            axes[i].action = visualization_msgs::Marker::ADD;   // Set the marker action.  Options are ADD and DELETE
            axes[i].header.stamp = ros::Time::now();
            axes[i].ns = "RCM_marker";
            axes[i].lifetime = ros::Duration();
            // Set the scale of the marker -- 1x1x1 here means 1m on a side
            axes[i].scale.x = 0.020;
            axes[i].scale.y = 0.020;
            axes[i].scale.z = 0.020;

            // Set the frame ID and timestamp.  See the TF tutorials for information on these.
            axes[i].header.frame_id = "/link3_L2";
            axes[i].id = 20+i;
            // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
            axes[i].pose.position.x = 0;
            axes[i].pose.position.y = 0;
            axes[i].pose.position.z = 0;

            // Set the color -- be sure to set alpha to something non-zero!
            axes[i].color.r = 0.0f;
            axes[i].color.g = 0.0f;
            axes[i].color.b = 0.0f;
            axes[i].color.a = 1.0;

        }

        // get the transform rotation
        _ori = &(device0->mech[left].ori);

        // draw the axes
        xform.setValue(1,0,0,   0,1,0,    0,0,1);
        xform.getRotation(axq);
        oriq = axq;
        axes[0].pose.orientation.x = oriq.getX();
        axes[0].pose.orientation.y = oriq.getY();
        axes[0].pose.orientation.z = oriq.getZ();
        axes[0].pose.orientation.w = oriq.getW();
        axes[0].color.r = 1.0f;

        xform.setValue(0,-1,0,    1,0,0,     0,0,1);
        xform.getRotation(axq);
        oriq = axq;
        axes[1].pose.orientation.x = oriq.getX();
        axes[1].pose.orientation.y = oriq.getY();
        axes[1].pose.orientation.z = oriq.getZ();
        axes[1].pose.orientation.w = oriq.getW();
        axes[1].color.g = 1.0f;

        xform.setValue(0,0,-1,   0,1,0,    1,0,0);
        xform.getRotation(axq);
        oriq = axq;
        axes[2].pose.orientation.x = oriq.getX();
        axes[2].pose.orientation.y = oriq.getY();
        axes[2].pose.orientation.z = oriq.getZ();
        axes[2].pose.orientation.w = oriq.getW();
        axes[2].color.b = 1.0f;

        // Publish the marker
        vis_pub2.publish(axes[0]);
        vis_pub2.publish(axes[1]);
        vis_pub2.publish(axes[2]);
    }



    int draw_xxx_axes=0;
    if (draw_xxx_axes)
    {
        for (int i=0;i<3;i++)
        {
            axes[i].type = visualization_msgs::Marker::ARROW;
            axes[i].action = visualization_msgs::Marker::ADD;   // Set the marker action.  Options are ADD and DELETE
            axes[i].header.stamp = ros::Time::now();
            axes[i].ns = "RCM_marker";
            axes[i].lifetime = ros::Duration();
            // Set the scale of the marker -- 1x1x1 here means 1m on a side
            axes[i].scale.x = 0.020;
            axes[i].scale.y = 0.020;
            axes[i].scale.z = 0.020;

            // Set the frame ID and timestamp.  See the TF tutorials for information on these.
            axes[i].header.frame_id = "/link3_R2";
            axes[i].id = 20+i;
            // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
            axes[i].pose.position.x = 0;
            axes[i].pose.position.y = 0;
            axes[i].pose.position.z = 0;

            // Set the color -- be sure to set alpha to something non-zero!
            axes[i].color.r = 0.0f;
            axes[i].color.g = 0.0f;
            axes[i].color.b = 0.0f;
            axes[i].color.a = 1.0;

        }

        // get the transform rotation
        _ori = &(device0->mech[left].ori);

        // draw the axes
        xform.setValue(1,0,0,   0,1,0,    0,0,1);
        xform.getRotation(axq);
        oriq = axq;
        axes[0].pose.orientation.x = oriq.getX();
        axes[0].pose.orientation.y = oriq.getY();
        axes[0].pose.orientation.z = oriq.getZ();
        axes[0].pose.orientation.w = oriq.getW();
        axes[0].color.r = 1.0f;

        xform.setValue(0,-1,0,    1,0,0,     0,0,1);
        xform.getRotation(axq);
        oriq = axq;
        axes[1].pose.orientation.x = oriq.getX();
        axes[1].pose.orientation.y = oriq.getY();
        axes[1].pose.orientation.z = oriq.getZ();
        axes[1].pose.orientation.w = oriq.getW();
        axes[1].color.g = 1.0f;

        xform.setValue(0,0,-1,   0,1,0,    1,0,0);
        xform.getRotation(axq);
        oriq = axq;
        axes[2].pose.orientation.x = oriq.getX();
        axes[2].pose.orientation.y = oriq.getY();
        axes[2].pose.orientation.z = oriq.getZ();
        axes[2].pose.orientation.w = oriq.getW();
        axes[2].color.b = 1.0f;

        // Publish the marker
        vis_pub2.publish(axes[0]);
        vis_pub2.publish(axes[1]);
        vis_pub2.publish(axes[2]);
    }


}



//
//    marker1.type = visualization_msgs::Marker::LINE_LIST;
//    marker1.action = visualization_msgs::Marker::ADD;   // Set the marker action.  Options are ADD and DELETE
//    marker1.header.stamp = ros::Time::now();
//    marker1.ns = "RCM_marker";
//    marker1.lifetime = ros::Duration();
//    // Set the scale of the marker -- 1x1x1 here means 1m on a side
//    marker1.scale.x = 0.020;
//    marker1.scale.y = 0.020;
//    marker1.scale.z = 0.020;
//
//    // Set the frame ID and timestamp.  See the TF tutorials for information on these.
//    marker1.header.frame_id = "/base_link_L";
//    marker1.id = 0;
//
//    // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
//
//    p.x = device0->mech[left].pos.x/1e6;
//    p.y = device0->mech[left].pos.y/1e6;
//    p.z = device0->mech[left].pos.z/1e6;
//    marker1.points.push_back(p);
//    px=p;
//    px.x+=0.3;
//    marker1.points.push_back(px);
//    marker1.points.push_back(p);
//    py=p;
//    py.y+=0.3;
//    marker1.points.push_back(py);
//    marker1.points.push_back(p);
//    pz=p;
//    pz.z+=0.3;
//    marker1.points.push_back(pz);
//
////    marker1.pose.position.x = device0->mech[left].pos.x/1e6;
////    marker1.pose.position.y = device0->mech[left].pos.y/1e6;
////    marker1.pose.position.z = device0->mech[left].pos.z/1e6;
//
//    // Get quaternion representation of rotation
//    struct orientation* _ori = &(device0->mech[left].ori);
//    btMatrix3x3 xform (_ori->R[0][0], _ori->R[0][1], _ori->R[0][2],
//                        _ori->R[1][0], _ori->R[1][1], _ori->R[1][2],
//                        _ori->R[2][0], _ori->R[2][1], _ori->R[2][2]);
//    btQuaternion bq;
//    xform.getRotation(bq);
//    marker1.pose.orientation.x = bq.getX();
//    marker1.pose.orientation.y = bq.getY();
//    marker1.pose.orientation.z = bq.getZ();
//    marker1.pose.orientation.w = bq.getW();
//
//    // Set the color -- be sure to set alpha to something non-zero!
//    marker1.color.r = 1.0f;
//    marker1.color.g = 0.0f;
//    marker1.color.b = 0.0f;
//    marker1.color.a = 1.0;
//    // Publish the marker
//    vis_pub1.publish(marker1);




// ------------------------- //
// - Recieve toolkit data  - //
////-------------------------- //
//int recieveToolkit(int fifo) //was unsigned int fifo
//{
//    struct param_pass ds1Recv;
//    int ret;
//
//    ret=read(fifo, &ds1Recv, sizeof(struct param_pass));
//    if ( ret != sizeof( struct param_pass) )
//    {
//        log_msg("Bogus toolkit data on FIFO Rcv.  Resetting fifo.\n");
//        return -1;
//    }
//
//    updateDS1(&ds1Recv);
//
//    return 0;
//}


// updateDS1()
//
//   update DS1 from toolkit update.
//
//void updateDS1(struct param_pass *d1)
//{
//    int i;
//
//    pthread_mutex_lock(&data1Mutex);
//
//    // on entrance into RL=0, reset pos_d.
//    for (i=0;i<NUM_MECH;i++)
//    {
//        if (d1->runlevel==0)
//        {
//            data1.xd[i].x = 0;
//            data1.xd[i].y = 0;
//            data1.xd[i].z = 0;
//            data1.rd[i].yaw   =0;
//            data1.rd[i].pitch =0;
//            data1.rd[i].roll  =0;
//            data1.rd[i].grasp =0;
//        }
//        // copy position data to new DS1 (rec'd from toolkit)
//        //  The objective is to keep current position/orientation,
//        //  while receiving commands (gains, trajectories, etc) from toolkit
//        d1->xd[i].x = data1.xd[i].x;
//        d1->xd[i].y = data1.xd[i].y;
//        d1->xd[i].z = data1.xd[i].z;
//        d1->rd[i].yaw   = data1.rd[i].yaw;
//        d1->rd[i].pitch = data1.rd[i].pitch;
//        d1->rd[i].roll  = data1.rd[i].roll;
//        d1->rd[i].grasp  = data1.rd[i].grasp;
//    }
//    d1->surgeon_mode = data1.surgeon_mode;
//
//    // copy new DS to local store
//    memcpy(&data1, d1, sizeof(struct param_pass));
//    isUpdated = TRUE;
//
//    pthread_mutex_unlock(&data1Mutex);
//}
