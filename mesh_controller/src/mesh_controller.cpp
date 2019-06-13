/*
 *  Copyright 2019, Sabrina Frohn
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *  authors:
 *    Sabrina Frohn <sfrohn@uni-osnabrueck.de>
 *
 */

//TODO sort out unnecessary headers

#include <pluginlib/class_list_macros.h>
#include <mesh_controller/mesh_controller.h>
#include <mbf_msgs/GetPathResult.h>
#include <mesh_map/util.h>
#include <lvr2/util/Meap.hpp>
#include <lvr2/geometry/HalfEdgeMesh.hpp>
#include <tf/transform_listener.h>
#include <std_msgs/Float32.h>



#include <fstream>
#include <iostream>

PLUGINLIB_EXPORT_CLASS(mesh_controller::MeshController, mbf_mesh_core::MeshController);

namespace mesh_controller{

    MeshController::MeshController()
    {

    }

    MeshController::~MeshController()
    {

    }

    uint32_t MeshController::computeVelocityCommands(
        const geometry_msgs::PoseStamped& pose,
        const geometry_msgs::TwistStamped& velocity,
        geometry_msgs::TwistStamped &cmd_vel,
        std::string &message
        ){
        if(current_plan.empty()){
            return mbf_msgs::GetPathResult::EMPTY_PATH;
        } else if (!goalSet){
            goal = current_plan.back();
            goalSet= true;
        }

        // set current face
        mesh_map::Vector pos_vec = poseToPositionVector(pose);
        setCurrentFace(pos_vec);
        updatePlanPos(pose, set_linear_velocity);

        // check if the robot is too far off the plan
        if (offPlan(pose)){
            // TODO see if NOT_INITIALIZED = 112 can be used
            return mbf_msgs::GetPathResult::FAILURE;
        }

        // variable that contains the planned / supposed orientation of the robot
        mesh_map::Vector plan_vec;
        if (config.useMeshGradient) {
            // use supposed orientation from mesh gradient
            if (current_face) {
                plan_vec = map_ptr->directionAtPosition(current_face.unwrap(), plan_vec);
            }
        } else {
            // use supposed orientation from calculated path
            plan_vec = poseToDirectionVector(plan_position);
        }

        // variable to store the new angular and linear velocities
        std::vector<float> values(2);


        // used to change type of controller
        switch (config.control_type) {
            case 0:
                values = naiveControl(pose, velocity, plan_vec);
                break;
            case 1:
                values = pidControl(pose, pose, velocity);
                break;
            default:
                return mbf_msgs::GetPathResult::NOT_INITIALIZED;
        }

        if (values[1] == std::numeric_limits<float>::max()){
            return mbf_msgs::GetPathResult::FAILURE;
        }
        // set velocities
        cmd_vel.twist.angular.z = values[0];
        cmd_vel.twist.linear.x = values[1];


        // RECORDING to measure "goodness" of travelled path
        if (record) {
            recordData(pose);
        }



        return mbf_msgs::GetPathResult::SUCCESS;
    }

    bool MeshController::isGoalReached(double dist_tolerance, double angle_tolerance)
    {
        // calculates the distance that is currently between the plan position and the goal
        tf::Pose plan_pose, goal_pose;
        tf::poseMsgToTF(plan_position.pose, plan_pose);
        tf::poseMsgToTF(goal.pose, goal_pose);

        float dist = plan_pose.getOrigin().distance(goal_pose.getOrigin());

        // test if robot is within tolerable distance to goal and if the heading has a tolerable distance to goal heading
        if (dist <= (float)dist_tolerance && angle <= (float)angle_tolerance){
            return true;
        } else {
            return false;
        }
    }

    bool  MeshController::setPlan(const std::vector<geometry_msgs::PoseStamped> &plan)
    {
        // checks if the given vector contains the plan
        if (!plan.empty()) {
            // assign given plan to current_plan variable to make it usable for navigation
            current_plan = plan;
            current_plan.erase(current_plan.begin());
            goal = current_plan.back();
            initial_dist = std::numeric_limits<float>::max();
            return true;
        }
        else {
            return false;
        }
    }

    bool  MeshController::cancel()
    {
        return false;
    }

    float MeshController::fadingFactor(){
        // calculate the distance between  the plan positions to get the plan length once
        if(initial_dist == std::numeric_limits<float>::max()){
            // transform the first pose of plan to a tf position vector
            tf::Vector3 tf_start_vec;
            geometry_msgs::PoseStamped start_pose = current_plan.at(0);
            mesh_map::Vector start_vec = poseToPositionVector(start_pose);
            tf_start_vec = {start_vec.x, start_vec.y, start_vec.z};

            tf::Vector3 tf_next_vec;
            float update_dist = 0.0;
            // go through plan and add up each distance between the current and its following position
            // to get the length of the whole path
            // TODO figure out why initial distance is sometimes smaller than actual path length!
            for(int i = 1; i < current_plan.size(); i++){
                ROS_INFO_STREAM("vector size : "<< current_plan.size()<<" current position : "<<i);
                // get the next pose of the plan and transform it to a tf position vector
                geometry_msgs::PoseStamped next_pose = current_plan.at(i);
                mesh_map::Vector next_vec = poseToPositionVector(next_pose);
                tf_next_vec = {next_vec.x, next_vec.y, next_vec.z};
                // calculate the distance between the two vectors
                update_dist = update_dist + tf_start_vec.distance(tf_next_vec);

                // overwrite start position with next position to calculate the difference
                // between the next and next-next position in next step
                tf_start_vec = tf_next_vec;
            }
            initial_dist = update_dist;
        }
        //ROS_INFO_STREAM("initial dist : "<<initial_dist);

        // transform the first pose of plan to a tf position vector
        tf::Vector3 tf_start_vec;
        geometry_msgs::PoseStamped start_pose = current_plan.at(0);
        mesh_map::Vector start_vec = poseToPositionVector(start_pose);
        tf_start_vec = {start_vec.x, start_vec.y, start_vec.z};

        float dist = 0.0;
        tf::Vector3 tf_next_vec;

        // add up distance of travelled path
        for(int i = 1; i <= plan_iter && i < current_plan.size(); i++){
            geometry_msgs::PoseStamped next_pose = current_plan.at(i);
            mesh_map::Vector next_vec = poseToPositionVector(next_pose);
            tf_next_vec = {next_vec.x, next_vec.y, next_vec.z};

            dist += tf_start_vec.distance(tf_next_vec);
            // overwrites start position with next position to calculate the difference
            // between the next and next-next position in next step
            tf_start_vec = tf_next_vec;

        }


        // compare the now travelled distance with the path length
        ROS_INFO_STREAM("plan position :"<<plan_iter<<" distance "<<dist);
        // in case the travelled distance is close to start position
        if (dist < config.fading) {
            //ROS_INFO("start fading");
            if (dist == 0.0) {
                // return small factor in case of initial position to enable movement
                // note: if max_velocity is zero, this factor will not matter
                last_fading = config.max_lin_velocity / 10;
                return last_fading;
            }
            // returns a factor slowly increasing to 1 while getting closer to the
            // distance from which the full velocity is driven
            return dist / config.fading;
        }
            // in case the travelled distance is close to goal position
        else if ((initial_dist - dist) < config.fading) {
            //ROS_INFO("end fading");
            // returns a factor slowly decreasing to 0 the end of the full velocity towards the goal position
            last_fading = (initial_dist - dist) / config.fading;
            return last_fading;
        }
        // for the part of the path when the velocity does not have to be influenced by a changing factor
        last_fading = 1.0;
        return last_fading;
    }

    mesh_map::Vector MeshController::poseToDirectionVector(const geometry_msgs::PoseStamped &pose){
        // define tf Pose for later assignment
        tf::Stamped<tf::Pose> tfPose;
        // transform pose to tf:Pose
        poseStampedMsgToTF(pose, tfPose);

        // get x as tf:Vector of transformed pose
        tf::Vector3 v = tfPose.getBasis()*tf::Vector3(1, 0, 0);
        // transform tf:Vector in mesh_map Vector and return
        return mesh_map::Vector(v.x(), v.y(), v.z());
    }

    mesh_map::Vector MeshController::poseToPositionVector(const geometry_msgs::PoseStamped &pose){
        return {(float)pose.pose.position.x, (float)pose.pose.position.y, (float)pose.pose.position.z};
    }

    float MeshController::angleBetweenVectors(mesh_map::Vector robot_heading, mesh_map::Vector planned_heading){
        tf::Vector3 tf_robot(robot_heading.x, robot_heading.y, robot_heading.z);
        tf::Vector3 tf_planned(planned_heading.x, planned_heading.y, planned_heading.z);
        return tf_robot.angle(tf_planned);
    }

    float MeshController::tanValue(float max_hight, float max_width, float value){
        // as tangens goes to pos and neg infinity and never meets the width borders,
        // they have to be checked individually
        if (value >= max_width/2){
            return max_hight;
        } else if (value <= max_width/(-2)) {
            return -max_hight;
        }


        // to widen or narrow the curve
        float width = max_width/M_PI;
        // to compress tangens
        float compress = max_hight/(tan(value*width));

        // calculating corresponding y-value
        float result = compress*tan(value * width);
        // limit max and min return (just in case)
        if (result > max_hight){
            return max_hight;
        } else if (result < -max_hight) {
            return -max_hight;
        }
        return result;
    }

    float MeshController::linValue(float max_hight, float x_axis, float max_width, float value){
        if (value > max_width/2){
            return max_hight;
        } else if (value < -max_width/2){
            return -max_hight;
        }
        float incline = max_hight / (max_width/2);
        return abs(incline * (value + x_axis));
    }

    float MeshController::parValue(float max_hight, float max_width, float value){
        if (value > max_width/2){
            return max_hight;
        }
        float shape = max_hight / pow((max_width/2), 2);
        return shape*pow(value, 2);
    }

    float MeshController::gaussValue(float max_hight, float max_width, float value){
        // in case value lays outside width, the function goes to zero
        if(value > max_width/2){
            return 0.0;
        }

        //  calculating the standard deviation given the max_width
        // based on the fact that 99.7% of area lies between mü-3*sigma and mü+3*sigma
        float std_dev = pow(-max_width/6, 2);

        // calculating y value of given normal distribution
        // stretched to max_hight and desired width
        float y_value = max_hight * 1/(sqrtf(2*M_PI*std_dev))* pow(E, (-pow(value, 2) * pow((2*std_dev), 2)));

        return y_value;
    }

    float MeshController::direction(mesh_map::Vector& robot_heading, mesh_map::Vector& planned_heading){

        tf::Vector3 tf_robot(robot_heading.x, robot_heading.y, robot_heading.z);
        tf::Vector3 tf_planned(planned_heading.x, planned_heading.y, planned_heading.z);

        https://www.gamedev.net/forums/topic/508445-left-or-right-direction/
        tf::Vector3 tf_cross_prod = tf_robot.cross(tf_planned);

        tf::Vector3 tf_up = {0,0,-1};

        // use normal vector of face for dot product as "up" vector
        // => positive result = left, negative result = right,
        float tf_dot_prod = tf_cross_prod.dot(tf_up);

        if (tf_dot_prod < 0.0) {
            return 1.0;
        } else {
            return -1.0;
        }
    }

    bool MeshController::offPlan(const geometry_msgs::PoseStamped& robot_pose){
        // transform the first pose of plan to a tf position vector
        mesh_map::Vector robot_vec = poseToPositionVector(robot_pose);
        tf::Vector3 tf_robot_vec = {robot_vec.x, robot_vec.y, robot_vec.z};
        mesh_map::Vector plan_vec = poseToPositionVector(plan_position);
        tf::Vector3 tf_plan_vec = {plan_vec.x, plan_vec.y, plan_vec.z};

        if(tf_robot_vec.distance(tf_plan_vec) > config.off_plan){
            return true;
        }
        return false;
    }

    float MeshController::euclideanDistance(const geometry_msgs::PoseStamped& pose){
        // https://en.wikipedia.org/wiki/Euclidean_distance

        // transform position of poses to position vectors
        lvr2::BaseVector<float> pose_vector = {(float)pose.pose.position.x, (float)pose.pose.position.y, (float)pose.pose.position.z};
        lvr2::BaseVector<float> plan_vector = {(float)plan_position.pose.position.x, (float)plan_position.pose.position.y, (float)plan_position.pose.position.z};

        return euclideanDistance(pose_vector, plan_vector);
    }

    float MeshController::euclideanDistance(lvr2::BaseVector<float>& current, lvr2::BaseVector<float>& planned){
        float power = 2.0;
        // calculate the euclidean distance of two position vectors
        float dist = sqrtf((pow((planned.x-current.x),power) + pow((planned.y-current.y),power) + pow((planned.z-current.z),power)));
        return dist;
    }

    void MeshController::updatePlanPos(const geometry_msgs::PoseStamped& pose, float velocity){
        if(last_call.isZero())
        {
            last_call = ros::Time::now();
            plan_iter = 0;
            return;
        }
        ros::Time now = ros::Time::now();
        ros::Duration time_delta = now - last_call;

        // the faster the robot, the further the robot might have travelled on the planned path
        double max_dist = velocity * time_delta.toSec();
        float min_dist = std::numeric_limits<float>::max();

        tf::Vector3 tf_robot_vec, tf_iter_vec;
        mesh_map::Vector robot_vec = poseToPositionVector(pose);
        mesh_map::Vector plan_vec = poseToPositionVector(plan_position);
        tf_robot_vec = {robot_vec.x, robot_vec.y, robot_vec.z};

        int iter, ret_iter;
        iter = plan_iter;

        float dist = 0;
        // look ahead
        do{
            geometry_msgs::PoseStamped iter_pose = current_plan[iter];
            mesh_map::Vector iter_vec = poseToPositionVector(iter_pose);
            tf_iter_vec = {iter_vec.x, iter_vec.y, iter_vec.z};
            dist = tf_robot_vec.distance(tf_iter_vec);
            if(dist < min_dist){
                ret_iter = iter;
                min_dist = dist;
            }
            iter++;
        } while(dist > max_dist && iter < current_plan.size());

        iter = plan_iter;
        dist = 0.0;

        do{
            geometry_msgs::PoseStamped iter_pose = current_plan[iter];
            mesh_map::Vector iter_vec = poseToPositionVector(iter_pose);
            tf_iter_vec = {iter_vec.x, iter_vec.y, iter_vec.z};
            dist = tf_robot_vec.distance(tf_iter_vec);
            if(dist < min_dist){
                ret_iter = iter;
                min_dist = dist;
            }
            iter--;
        } while(dist > max_dist && iter >= 0);

        plan_iter = ret_iter;
        plan_position = current_plan[plan_iter];
        last_call = now;
    }

    std::vector<float> MeshController::lookAhead(const geometry_msgs::PoseStamped& pose, float velocity)
    {
         // select how far to look ahead depending on velocity
         int steps = (int)tanValue(1000.0, 1.0, velocity);
         // max amount of steps that will be made (in case it is interrupted earlier)
         int max_steps = steps;
         // iterator value in case drastic speed reduction has to be made
         float accum_cost = 0.0;
         float accum_turn = 0.0;

         // look ahead when using the mesh gradient for navigation reference
         if (config.useMeshGradient){
             // transforming the position of the pose to a vector
             lvr2::BaseVector<float> position_ahead = {(float)pose.pose.position.x, (float)pose.pose.position.y, (float)pose.pose.position.z};
             // assign ahead_face the face of the current position to start from
             ahead_face = current_face;
             // transforming position of goal pose to position vector
             lvr2::BaseVector<float> goal_vec = {(float)goal.pose.position.x, (float)goal.pose.position.y, (float)goal.pose.position.z};
             // steps is divided by 3 as to account for the step size of the look ahead
             for (int j = 0; j < steps/3; j++){
                 // check if the goal is reachable in next step by look ahead
                 if(euclideanDistance(position_ahead, goal_vec) < 0.04){
                     steps = j;
                     break;
                 }
                 // find the next position
                 position_ahead = step_update(position_ahead, ahead_face.unwrap());
                 // TODO find out if position ahead is direction or position vector and how to transform to the other
                 // get cost of next position
                 float new_cost = cost(position_ahead);
                 // get direction difference between next position and current position
                 float future_turn = angleBetweenVectors(poseToDirectionVector(pose), position_ahead);
                 // accumulate cost and angle
                 accum_cost += new_cost;
                 accum_turn += future_turn;
             }
         }
         // look ahead when using the planned path for navigation reference
         else {
             // adds up cost of all steps ahead
             for (int i = 0; i < steps; i++) {
                 // in case look ahead extends planned path
                 if ((plan_iter + i) < current_plan.size()) {
                     steps = i;
                     break;
                 }
                 geometry_msgs::PoseStamped& pose_ahead = current_plan[plan_iter+i];
                 mesh_map::Vector pose_ahead_vec =  poseToPositionVector(pose_ahead);
                 // find cost of the future position
                 float new_cost = cost(pose_ahead_vec);
                 if (new_cost == std::numeric_limits<float>::infinity()){
                     steps = i;
                     break;
                 } else if (new_cost == -1.0){
                     // cost could not be accessed
                     // TODO check if a better solution exists
                     __throw_bad_function_call();
                 }
                 // get direction difference between current position and next position
                 float future_turn = angleBetweenVectors(poseToDirectionVector(pose), poseToDirectionVector(current_plan[plan_iter + i]));
                 // accumulate cost and angle
                 accum_cost += new_cost;
                 accum_turn += future_turn;
             }
         }


         //  take averages
         float av_cost = accum_cost / steps;
         float av_turn = accum_turn / steps;

         // calculate the difference between the current cost and average cost
         mesh_map::Vector current_vec = poseToDirectionVector(pose);
         float current_cost = cost(current_vec);
         float cost_difference = current_cost - av_cost;
         float cost_result;
         // check if a lethal vertex is ahead
         if (cost_difference != std::numeric_limits<float>::infinity()){
             cost_result = tanValue(config.max_lin_velocity, 2.0, cost_difference);
         } else {
             // if yes, set the linear velocity factor small depending on distance to the lethal vertex
             cost_result = gaussValue(config.max_lin_velocity, 2*max_steps, steps);
         }

         // TODO dirction of turn for angular vel
         // calculate the difference between the current angle and the average angle
         float turn_difference = angle - av_turn;
         float turn_result = tanValue(1.0, M_PI, turn_difference);


         return {turn_result, cost_result};
    }

    float MeshController::cost(mesh_map::Vector& pose_vec){
        // call function to get cost at position through corresponding face
        float ret_cost = map_ptr->costAtPosition(current_face.unwrap(), pose_vec);
        return ret_cost;
    }

    void MeshController::setCurrentFace(mesh_map::Vector& position_vec){
        // find a face to access cost later in case none has been found yet

        if(!current_face){
            current_face = map_ptr->getContainingFaceHandle(position_vec);
            if(!current_face){
                ROS_ERROR("searched through mesh - no face");
            }
        } else {
            current_face = searchNeighbourFaces(position_vec, current_face.unwrap());
            if(!current_face){
                setCurrentFace(position_vec);
            }
        }
    }

    lvr2::OptionalFaceHandle MeshController::searchNeighbourFaces(const mesh_map::Vector& pose_vec, const lvr2::FaceHandle face){

        std::list<lvr2::FaceHandle> possible_faces;
        possible_faces.push_back(face);
        std::list<lvr2::FaceHandle>::iterator current = possible_faces.begin();

        int cnt = 0;
        int max = 40; // TODO to config


        // as long as end of list is not reached or max steps are not overstepped
        while(possible_faces.end() != current && max != cnt++) {
            lvr2::FaceHandle work_face = *current;

            float u,v;
            // check if robot position is in the current face
            if (map_ptr->barycentricCoords(pose_vec, work_face, u, v)) {
                return work_face;
            } else {
                // add neighbour of neighbour, if we overstep a small face or the peak of it
                std::vector<lvr2::FaceHandle> nn_faces;
                map_ptr->mesh_ptr->getNeighboursOfFace(work_face, nn_faces);
                possible_faces.insert(possible_faces.end(), nn_faces.begin(), nn_faces.end());
            }
        }

        return lvr2::OptionalFaceHandle();
    }

    std::vector<float> MeshController::naiveControl(const geometry_msgs::PoseStamped& pose, const geometry_msgs::TwistStamped& velocity, mesh_map::Vector plan_vec) {
        mesh_map::Vector dir_vec = poseToDirectionVector(pose);
        mesh_map::Vector position_vec = poseToPositionVector(pose);

        // ANGULAR MOVEMENT
        // calculate angle between orientation vectors
        // angle will never be negative and smaller or equal to pi
        angle = angleBetweenVectors(dir_vec, plan_vec);

        // output: angle publishing
        std_msgs::Float32 angle32;
        angle32.data = angle * 180 / M_PI;
        angle_pub.publish(angle32);

        // to determine in which direction to turn (neg for left, pos for right)
        float leftRight = direction(dir_vec, plan_vec);

        // calculate a direction velocity depending on the turn direction and difference angle
        float final_ang_vel = leftRight * linValue(config.max_ang_velocity, 0.0, 2 * M_PI, angle);

        // LINEAR movement
        float lin_vel_by_ang = gaussValue(config.max_lin_velocity, 2 * M_PI, angle);
        float final_lin_vel;
        // check the size of the angle. If it is not more than about 35 degrees, integrate position costs to linear velocity
        if (angle < 0.6) {
            float cost_lin_vel = cost(position_vec);
            // in case current vertex is a lethal vertex
            if (cost_lin_vel == std::numeric_limits<float>::max()) {
                final_lin_vel = cost_lin_vel;
            } else {
                // basic linear velocity depending on angle difference between robot pose and plan and the cost at position
                float lin_factor_by_cost = linValue(config.max_lin_velocity/10,  0.0, 2.0, cost_lin_vel);
                lin_vel_by_ang -= lin_factor_by_cost;
                if (lin_vel_by_ang < 0.0) {
                    lin_vel_by_ang = 0.0;
                } else if (lin_vel_by_ang > config.max_lin_velocity) {
                    ROS_INFO_STREAM("larger than max vel through cost "<<lin_factor_by_cost);
                    lin_vel_by_ang = config.max_lin_velocity;
                }
            }
        }
        final_lin_vel = lin_vel_by_ang * fadingFactor();
        // store new velocity to use as previous velocity
        set_linear_velocity = final_lin_vel;
        return {final_ang_vel, final_lin_vel};

/*
        // ADDITIONAL factors
        // look ahead
        std::vector<float> ahead_factor = lookAhead(pose, velocity.twist.linear.x);

        // dynamic obstacle avoidance

        // slow start
        float start = startVelocityFactor(pose);
        // slower towards end
        float end = endVelocityFactor(pose);

        // adding or subtracting percentage of previously set angular velocity (turn_angle diff) depending on angular ahead factor
        // combine the basic velocity calculations with the look ahead
        float ahead_ang;
        if(ahead_factor[0] < 0){
            ahead_ang = turn_angle_diff - (1-(ahead_factor[0] / turn_angle_diff));
        } else if (ahead_factor[0] > 0){
            ahead_ang = turn_angle_diff + (1-(ahead_factor[0] / turn_angle_diff));
        } else {
            ahead_ang = turn_angle_diff;
        }

        // adding or subtracting percentage of previously set linear velocity (vel_given_angle) depending on linear ahead factor
        float ahead_lin;
        if (ahead_factor[1] < 0){
            ahead_lin = vel_given_angle - (1-(ahead_factor[1] / vel_given_angle));
        } else if (ahead_factor[1] > 0){
            ahead_lin = vel_given_angle + (1-(ahead_factor[1] / vel_given_angle));
        } else{
            ahead_lin = vel_given_angle;
        }

        // setting linear velocity depending on robot position in relation to start / goal
        float new_vel = start * end * ahead_lin;

        return {ahead_ang, new_vel};
*/
    }

    std::vector<float> MeshController::pidControl(const geometry_msgs::PoseStamped& setpoint, const geometry_msgs::PoseStamped& pv, const geometry_msgs::TwistStamped& velocity){
        // LINEAR movement
        float linear_vel = pidControlDistance(setpoint, pv);
        // ANGULAR movement
        const mesh_map::Vector& angular_sp = poseToDirectionVector(setpoint);
        const mesh_map::Vector& angular_pv = poseToDirectionVector(pv);
        float angular_vel = pidControlDir(angular_sp, angular_pv, pv);

        // ADDITIONAL factors
        // to regulate linear velocity depending on angular velocity (higher angular vel => lower linear vel)
        float vel_given_angle = linear_vel - ((angular_vel/config.max_ang_velocity) * linear_vel);

        std::vector<float> ahead = lookAhead(pv, velocity.twist.linear.x);


        // adding or subtracting percentage of previously set angular velocity (turn_angle diff) depending on angular ahead factor
        // combine the basic velocity calculations with the look ahead
        float ahead_ang;
        if(ahead[0] < 0){
            ahead_ang = angular_vel - (1-(ahead[0] / angular_vel));
        } else if (ahead[0] > 0){
            ahead_ang = angular_vel + (1-(ahead[0] / angular_vel));
        } else {
            ahead_ang = angular_vel;
        }

        // adding or subtracting percentage of previously set linear velocity (vel_given_angle) depending on linear ahead factor
        float ahead_lin;
        if (ahead[1] < 0){
            ahead_lin = vel_given_angle - (1-(ahead[1] / vel_given_angle));
        } else if (ahead[1] > 0){
            ahead_lin = vel_given_angle + (1-(ahead[1] / vel_given_angle));
        } else{
            ahead_lin = vel_given_angle;
        }

        return {ahead_ang, ahead_lin};
    }

    float MeshController::pidControlDistance(const geometry_msgs::PoseStamped& setpoint, const geometry_msgs::PoseStamped& pv){
        // setpoint is desired position, pv is actual position
        // https://gist.github.com/bradley219/5373998

        float error = euclideanDistance(setpoint);

        // proportional part
        float proportional = config.prop_dis_gain * error;

        // integral part
        int_dis_error += (error * config.int_time);
        float integral = config.int_dis_gain * int_dis_error;

        // derivative part
        float derivative = config.deriv_dis_gain * ((error - prev_dis_error) / config.int_time);

        float linear = proportional + integral + derivative;

        // TODO check if max and min output useful
        //if( output > _max )
        //    output = _max;
        //else if( output < _min )
        //    output = _min;

        prev_dis_error = error;
        return linear;
    }

    float MeshController::pidControlDir(const mesh_map::Vector& setpoint, const mesh_map::Vector& pv, const geometry_msgs::PoseStamped& pv_pose){
        // setpoint is desired direction, pv is actual direction
        // https://gist.github.com/bradley219/5373998

        float dir_error = angleBetweenVectors(setpoint, pv);

        // proportional part
        float proportional = config.prop_dir_gain * dir_error;

        // integral part
        int_dir_error += (dir_error * config.int_time);
        float integral = config.int_dir_gain * int_dir_error;

        // derivative part
        float derivative = config.deriv_dir_gain * ((dir_error - prev_dir_error) / config.int_time);

        float angular = proportional + integral + derivative;

        // TODO check if max and min output useful
        //if( output > _max )
        //    output = _max;
        //else if( output < _min )
        //    output = _min;

        prev_dir_error = dir_error;

        // to determine in which direction to turn (neg for left, pos for right)
        //float leftRight = direction(pv_pose, setpoint);

        return angular;//*leftRight;
    }

    void MeshController::recordData(const geometry_msgs::PoseStamped& robot_pose){
        float distance = euclideanDistance(robot_pose);
        string output = std::to_string(distance) + "\n";

        string info_msg = string("distance: ");

        const char* filename = "Paths.txt";
        fstream appendFileToWorkWith;

        appendFileToWorkWith.open(filename, std::fstream::in | std::fstream::out | std::fstream::app);


        // If file does not exist, Create new file
        if (!appendFileToWorkWith )
        {
            appendFileToWorkWith.open(filename,  fstream::in | fstream::out | fstream::trunc);
            appendFileToWorkWith <<info_msg;
            appendFileToWorkWith <<output;
            appendFileToWorkWith.close();

        }
        else
        {    // use existing file
            appendFileToWorkWith <<output;
            appendFileToWorkWith.close();

        }
    }

    lvr2::BaseVector<float> MeshController::step_update(mesh_map::Vector& vec, lvr2::FaceHandle face){
        // clear vector field map
        vector_map.clear();

        const auto& face_normals = map_ptr->faceNormals();

        bool foundConnectedFace = false;
        std::list<lvr2::FaceHandle> possible_faces;
        std::vector<lvr2::FaceHandle> neighbour_faces;
        map_ptr->mesh_ptr->getNeighboursOfFace(face, neighbour_faces);
        possible_faces.insert(possible_faces.end(), neighbour_faces.begin(), neighbour_faces.end());
        std::list<lvr2::FaceHandle>::iterator current = possible_faces.begin();
        mesh_map::Vector dir;
        float step_width = 0.03;

        // Set start distance to zero
        // add start vertex to priority queue
        for(auto vH : map_ptr->mesh_ptr->getVerticesOfFace(face))
        {
            const mesh_map::Vector diff = vec - map_ptr->mesh_ptr->getVertexPosition(vH);
            vector_map.insert(vH, diff);
        }

        int cnt = 0;
        int max = 40; // TODO to config

        while(possible_faces.end() != current && max != cnt++)
        {
            lvr2::FaceHandle fH = *current;
            auto vertices = map_ptr->mesh_ptr->getVertexPositionsOfFace(fH);
            auto face_vertices = map_ptr->mesh_ptr->getVerticesOfFace(fH);
            float u, v;

            // Projection onto the triangle plane
            mesh_map::Vector tmp_vec = mesh_map::projectVectorOntoPlane(vec, vertices[0], face_normals[fH]);

            // Check if the projected point lies in the current testing face
            if(vector_map.containsKey(face_vertices[0]) && vector_map.containsKey(face_vertices[1]) && vector_map.containsKey(face_vertices[2])
               && mesh_map::barycentricCoords(tmp_vec, vertices[0], vertices[1], vertices[2], u, v))
            {
                foundConnectedFace = true;
                // update ahead_face as face of the new vector
                ahead_face = fH;
                vec = tmp_vec;
                float w = 1 - u - v;
                dir = ( vector_map[face_vertices[0]]*u + vector_map[face_vertices[1]]*v + vector_map[face_vertices[2]]*w ).normalized() * step_width ;
                break;
            }
            else
            {
                // add neighbour of neighbour, if we overstep a small face or the peak of it
                std::vector<lvr2::FaceHandle> nn_faces;
                map_ptr->mesh_ptr->getNeighboursOfFace(fH, nn_faces);
                possible_faces.insert(possible_faces.end(), nn_faces.begin(), nn_faces.end());
            }
            current++;
        }

        if(!foundConnectedFace){
            return lvr2::BaseVector<float>();
        }

        return vec + dir;
    }

    /*
    void MeshController::obstacleAvoidance(pose, mesh_map, path){
        // make new measurement + make mesh layer of it
        // compare new measurement to old one - find new lethal faces
        // check if new lethal faces are on path
        // check if new lethal faces are on path
            // if on path: reduce velocity + make new plan

    }
    */

    void MeshController::reconfigureCallback(mesh_controller::MeshControllerConfig& cfg, uint32_t level)
    {

        ROS_INFO("Reconfigure Request: %f %f %f %f %f %f %s %f %f %f %f %i ",
                config.prop_dis_gain,
                config.int_dis_gain,
                config.deriv_dis_gain,
                config.prop_dir_gain,
                config.int_dir_gain,
                config.deriv_dir_gain,
                config.useMeshGradient?"True":"False",
                config.max_lin_velocity,
                config.max_ang_velocity,
                config.fading,
                config.int_time,
                config.control_type);

        if (first_config)
        {
            config = cfg;
            first_config = false;
        }

        config = cfg;
    }

    bool  MeshController::initialize(
            const std::string& plugin_name,
            const boost::shared_ptr<tf2_ros::Buffer>& tf_ptr,
            const boost::shared_ptr<mesh_map::MeshMap>& mesh_map_ptr){

        goalSet = false;
        name = plugin_name;
        private_nh = ros::NodeHandle("~/"+name);

        ROS_INFO_STREAM("Namespace of the controller: " << private_nh.getNamespace());
        // all for mesh plan
        map_ptr = mesh_map_ptr;


        // for PID
        // initialize integral error for PID
        int_dis_error = 0.0;
        int_dir_error = 0.0;

        haveStartFace = false;

        set_linear_velocity = 0.0;

        // for recording - true = record, false = no record
        record = false;

        reconfigure_server_ptr = boost::shared_ptr<dynamic_reconfigure::Server<mesh_controller::MeshControllerConfig> > (
                new dynamic_reconfigure::Server<mesh_controller::MeshControllerConfig>(private_nh));

        config_callback = boost::bind(&MeshController::reconfigureCallback, this, _1, _2);
        reconfigure_server_ptr->setCallback(config_callback);

        angle_pub = private_nh.advertise<std_msgs::Float32>("current_angle", 1);


        return true;

    }
} /* namespace mesh_controller */
