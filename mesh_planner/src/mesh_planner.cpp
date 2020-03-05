/*
 *  Copyright 2019, Sebastian Pütz
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
 *    Sebastian Pütz <spuetz@uni-osnabrueck.de>
 *
 */

// TODO fix lvr2 missing imports
#include <lvr2/geometry/Handles.hpp>
using namespace std;
#include <unordered_set>

#include <lvr2/util/Meap.hpp>
#include <lvr_ros/colors.h>
#include <mbf_msgs/GetPathResult.h>
#include <mesh_map/util.h>
#include <pluginlib/class_list_macros.h>

#include <mesh_planner/mesh_planner.h>

PLUGINLIB_EXPORT_CLASS(mesh_planner::MeshPlanner, mbf_mesh_core::MeshPlanner);

namespace mesh_planner {

MeshPlanner::MeshPlanner() {}

MeshPlanner::~MeshPlanner() {}

uint32_t MeshPlanner::makePlan(const geometry_msgs::PoseStamped &start,
                               const geometry_msgs::PoseStamped &goal,
                               double tolerance,
                               std::vector<geometry_msgs::PoseStamped> &plan,
                               double &cost, std::string &message) {
  const auto &mesh = mesh_map->mesh();
  std::list<std::pair<mesh_map::Vector, lvr2::FaceHandle>> path;

  //mesh_map->combineVertexCosts(); // TODO should be outside the planner

  ROS_INFO("start wave front propagation.");

  mesh_map::Vector goal_vec = mesh_map::toVector(goal.pose.position);
  mesh_map::Vector start_vec = mesh_map::toVector(start.pose.position);

  uint32_t outcome = waveFrontPropagation(goal_vec, start_vec, path);

  path.reverse();

  std_msgs::Header header;
  header.stamp = ros::Time::now();
  header.frame_id = mesh_map->mapFrame();

  cost = 0;
  if (!path.empty()) {
    mesh_map::Vector vec = path.front().first;
    lvr2::FaceHandle fH = path.front().second;
    path.pop_front();

    const auto &face_normals = mesh_map->faceNormals();

    for (auto &next : path) {
      geometry_msgs::PoseStamped pose;
      pose.header = header;
      pose.pose = mesh_map::calculatePoseFromPosition(vec, next.first,
                                                      face_normals[fH]);
      vec = next.first;
      fH = next.second;
      plan.push_back(pose);
    }
  }

  nav_msgs::Path path_msg;
  path_msg.poses = plan;
  path_msg.header = header;

  path_pub.publish(path_msg);
  mesh_map->publishVertexCosts(potential, "Potential");

  if(publish_vector_field)
  {
    mesh_map->publishVectorField(
        "vector_field", vector_map, cutting_faces, publish_face_vectors);
  }

  return outcome;
}

bool MeshPlanner::cancel() {
  cancel_planning = true;
  return true;
}

bool MeshPlanner::initialize(
    const std::string &plugin_name,
    const boost::shared_ptr<mesh_map::MeshMap> &mesh_map_ptr) {
  mesh_map = mesh_map_ptr;
  name = plugin_name;
  map_frame = mesh_map->mapFrame();
  private_nh = ros::NodeHandle("~/" + name);

  private_nh.param("publish_vector_field", publish_vector_field, false);
  private_nh.param("publish_face_vectors", publish_face_vectors, false);

  path_pub = private_nh.advertise<nav_msgs::Path>("path", 1, true);
  const auto &mesh = mesh_map->mesh();
  direction = lvr2::DenseVertexMap<float>(mesh.nextVertexIndex(), 0);
  // TODO check all map dependencies! (loaded layers etc...)

  reconfigure_server_ptr = boost::shared_ptr<
      dynamic_reconfigure::Server<mesh_planner::MeshPlannerConfig>>(
      new dynamic_reconfigure::Server<mesh_planner::MeshPlannerConfig>(
          private_nh));

  config_callback =
      boost::bind(&MeshPlanner::reconfigureCallback, this, _1, _2);
  reconfigure_server_ptr->setCallback(config_callback);

  return true;
}

lvr2::DenseVertexMap<mesh_map::Vector> MeshPlanner::getVectorMap() {
  return vector_map;
}

void MeshPlanner::reconfigureCallback(mesh_planner::MeshPlannerConfig &cfg,
                                      uint32_t level) {
  ROS_INFO_STREAM("New height diff layer config through dynamic reconfigure.");
  if (first_config) {
    config = cfg;
    first_config = false;
    return;
  }
  config = cfg;
}

void MeshPlanner::computeVectorMap() {
  const auto &mesh = mesh_map->mesh();
  const auto &face_normals = mesh_map->faceNormals();


  for (auto v3 : mesh.vertices()) {
    // if(vertex_costs[v3] > config.cost_limit || !predecessors.containsKey(v3))
    // continue;

    const lvr2::VertexHandle &v1 = predecessors[v3];

    // if predecessor is pointing to it self, continue with the next vertex.
    if (v1 == v3)
      continue;

    // get the cut face
    const auto &optFh = cutting_faces.get(v3);
    // if no cut face, continue with the next vertex
    if (!optFh)
      continue;

    const lvr2::FaceHandle &fH = optFh.get();

    const auto &vec3 = mesh.getVertexPosition(v3);
    const auto &vec1 = mesh.getVertexPosition(v1);

    // compute the direction vector and rotate it by theta, which is stored in
    // the direction vertex map
    const auto dirVec = (vec1 - vec3).rotated(face_normals[fH], direction[v3]);
    // store the normalized rotated vector in the vector map
    vector_map.insert(v3, dirVec.normalized());
  }
  mesh_map->setVectorMap(vector_map);
}

uint32_t MeshPlanner::waveFrontPropagation(
    const mesh_map::Vector &start, const mesh_map::Vector &goal,
    std::list<std::pair<mesh_map::Vector, lvr2::FaceHandle>> &path){
  return waveFrontPropagation(start, goal, mesh_map->edgeDistances(),
                              mesh_map->vertexCosts(), path, potential,
                              predecessors);
}

/*
inline bool MeshPlanner::waveFrontUpdate2(
    lvr2::DenseVertexMap<float> &distances,
    const lvr2::DenseEdgeMap<float> &edge_weights, const lvr2::VertexHandle &v1,
    const lvr2::VertexHandle &v2, const lvr2::VertexHandle &v3) {
  const auto &mesh = mesh_map->mesh();
  const auto &vertex_costs = mesh_map->vertexCosts();

  const double u1 = distances[v1];
  const double u2 = distances[v2];
  double u3 = distances[v3]; // new

  const lvr2::OptionalEdgeHandle e12h = mesh.getEdgeBetween(v1, v2);
  const float c = edge_weights[e12h.unwrap()];
  const float c_sq = c * c;

  const lvr2::OptionalEdgeHandle e13h = mesh.getEdgeBetween(v1, v3);
  const float b = edge_weights[e13h.unwrap()];
  const float b_sq = b * b;

  const lvr2::OptionalEdgeHandle e23h = mesh.getEdgeBetween(v2, v3);
  const float a = edge_weights[e23h.unwrap()];
  const float a_sq = a * a;

  float dot = (a_sq + b_sq - c_sq) / (2 * a * b);
  float cost = vertex_costs[v3] > config.cost_limit ? config.cost_limit : vertex_costs[v3];
  float weight = 1 + cost/config.cost_limit;

  float u3tmp = mesh_map::computeUpdateSethianMethod(u1, u2, a, b, dot, 1.0);

  if (u3tmp < u3) {
    u3 = distances[v3] = u3tmp;

    float u1_sq = u1*u1;
    float u2_sq = u2*u2;
    float u3_sq = u3*u3;

    float t1a = (u3_sq + b_sq - u1_sq) / (2*u3*b);
    float t2a = (a_sq + u3_sq - u2_sq) / (2*a*u3);

    bool os2 = std::fabs(t2a) > 1;
    bool os1 = std::fabs(t1a) > 1;

    float theta0 = acos(dot);
    float theta1 = acos(t1a);
    float theta2 = acos(t2a);

    if(!std::isfinite(theta0 + theta1 + theta2)){
      ROS_ERROR_STREAM("------------------");
      if(std::isnan(theta0)) ROS_ERROR_STREAM("Theta0 is NaN!");
      if(std::isnan(theta1)) ROS_ERROR_STREAM("Theta1 is NaN!");
      if(std::isnan(theta2)) ROS_ERROR_STREAM("Theta2 is NaN!");
      if(std::isinf(theta2)) ROS_ERROR_STREAM("Theta2 is inf!");
      if(std::isinf(theta2)) ROS_ERROR_STREAM("Theta2 is inf!");
      if(std::isinf(theta2)) ROS_ERROR_STREAM("Theta2 is inf!");
      if(std::isnan(t1a)) ROS_ERROR_STREAM("t1a is NaN!");
      if(std::isnan(t2a)) ROS_ERROR_STREAM("t2a is NaN!");
      if(std::fabs(t2a) > 1) ROS_ERROR_STREAM("|t2a| is > 1: " << t2a);
      if(std::fabs(t1a) > 1) ROS_ERROR_STREAM("|t1a| is > 1: " << t1a);
    }
    bool left = theta2 > theta0;
    bool right = theta1 > theta0;

    if(left && theta2 < theta1) ROS_ERROR_STREAM("theta2 smaller than theta1");
    if(right && theta1 < theta2) ROS_ERROR_STREAM("theta1 smaller than theta2");

    if (distances[v1] < distances[v2]) {
      predecessors[v3] = v1;
      direction[v3] = !left? theta1 : -theta1;
    }
    else // right face check
    {
      predecessors[v3] = v2;
      direction[v3] = !right? -theta2 : theta2;
    }

    cutting_faces.insert(v3, mesh.getFaceBetween(v1, v2, v3).unwrap());
    return vertex_costs[v3] <= config.cost_limit;
  }
  else return false;
}

*/

inline bool MeshPlanner::waveFrontUpdate(
    lvr2::DenseVertexMap<float> &distances,
    const lvr2::DenseEdgeMap<float> &edge_weights, const lvr2::VertexHandle &v1,
    const lvr2::VertexHandle &v2, const lvr2::VertexHandle &v3) {
  const auto &mesh = mesh_map->mesh();

  const double u1 = distances[v1];
  const double u2 = distances[v2];
  const double u3 = distances[v3];

  const lvr2::OptionalEdgeHandle e12h = mesh.getEdgeBetween(v1, v2);
  const double c = edge_weights[e12h.unwrap()];
  const double c_sq = c * c;

  const lvr2::OptionalEdgeHandle e13h = mesh.getEdgeBetween(v1, v3);
  const double b = edge_weights[e13h.unwrap()];
  const double b_sq = b * b;

  const lvr2::OptionalEdgeHandle e23h = mesh.getEdgeBetween(v2, v3);
  const float a = edge_weights[e23h.unwrap()];
  const float a_sq = a * a;

  const float u1sq = u1 * u1;
  const float u2sq = u2 * u2;

  const float A = sqrt(std::max<double>(
      (-u1 + u2 + c) * (u1 - u2 + c) * (u1 + u2 - c) * (u1 + u2 + c), 0));
  const float B = sqrt(std::max<double>(
      (-a + b + c) * (a - b + c) * (a + b - c) * (a + b + c), 0));

  //const double A = std::fabs((-u1 + u2 + c) * (u1 - u2 + c) * (u1 + u2 - c) * (u1 + u2 + c));
  //const double B = std::fabs((-a + b + c) * (a - b + c) * (a + b - c) * (a + b + c));

  const float sx = (c_sq + u1sq - u2sq) / (2 * c);
  // const double sx_sq = sx * sx;

  const float sy = -A / (2 * c);
  // const double sy_sq = sy * sy;

  const float p = (-a_sq + b_sq + c_sq) / (2 * c);
  //const double hc = B / (2 * c);

  const float dy = (A + B) / (2 * c);
  // const double dx = (u2sq - u1sq + b_sq - a_sq) / (2*c);
  const float dx = p - sx;

  // const double x = dx != sx ? (A*sx - B*p)/((A + B)*c) : dx/c;
  // const double dy = hc-sy;

  const double u3tmp_sq = dx * dx + dy * dy;
  double u3tmp = sqrt(u3tmp_sq);

  if (!std::isfinite(u3tmp)){
    ROS_ERROR_STREAM("u3 tmp is not finite!");
  }
  if(u3tmp < u3) {
    const double u3_sq = u3tmp * u3tmp;
    const double u2_sq = u2 * u2;
    const double u1_sq = u1 * u1;

    const double t0a = (a_sq + b_sq - c_sq) / (2*a*b);
    const double t1a = (u3_sq + b_sq - u1_sq) / (2*u3tmp*b);
    const double t2a = (a_sq + u3_sq - u2_sq) / (2*a*u3tmp);

    const double theta0 = acos(t0a);
    const double theta1 = acos(t1a);
    const double theta2 = acos(t2a);

    if(!std::isfinite(theta0 + theta1 + theta2)){
      ROS_ERROR_STREAM("------------------");
      if(std::isnan(theta0)) ROS_ERROR_STREAM("Theta0 is NaN!");
      if(std::isnan(theta1)) ROS_ERROR_STREAM("Theta1 is NaN!");
      if(std::isnan(theta2)) ROS_ERROR_STREAM("Theta2 is NaN!");
      if(std::isinf(theta2)) ROS_ERROR_STREAM("Theta2 is inf!");
      if(std::isinf(theta2)) ROS_ERROR_STREAM("Theta2 is inf!");
      if(std::isinf(theta2)) ROS_ERROR_STREAM("Theta2 is inf!");
      if(std::isnan(t1a)) ROS_ERROR_STREAM("t1a is NaN!");
      if(std::isnan(t2a)) ROS_ERROR_STREAM("t2a is NaN!");
      if(std::fabs(t2a) > 1) ROS_ERROR_STREAM("|t2a| is > 1: " << t2a);
      if(std::fabs(t1a) > 1) ROS_ERROR_STREAM("|t1a| is > 1: " << t1a);
    }

    /*
    const bool left = theta2 > theta0;
    const bool right = theta1 > theta0;

    if(left && theta2 < theta1) ROS_ERROR_STREAM("theta2 smaller than theta1");
    if(right && theta1 < theta2) ROS_ERROR_STREAM("theta1 smaller than theta2");
    */

    if(theta1 + theta2 < theta0)
    {
      auto fH = mesh.getFaceBetween(v1, v2, v3).unwrap();
      cutting_faces.insert(v3, fH);
      distances[v3] = static_cast<float>(u3tmp);
      if (theta1 < theta2) {
        predecessors[v3] = v1;
        direction[v3] = theta1;
      }
      thetelse
      {
        predecessors[v3] = v2;
        direction[v3] = -theta2;
      }
      return true;
    }
    else if (theta1 < theta2)
    {
      u3tmp = distances[v1] + b;
      if(u3tmp < u3)
      {
        cutting_faces.insert(v3, mesh.getFaceBetween(v1, v2, v3).unwrap());
        predecessors[v3] = v1;
        distances[v3] = static_cast<float>(u3tmp);
        direction[v3] = 0;
        return true;
      }
      return false;
    }
    else
    {
      u3tmp = distances[v2] + a;
      if(u3tmp < u3)
      {
        cutting_faces.insert(v3, mesh.getFaceBetween(v1, v2, v3).unwrap());
        predecessors[v3] = v2;
        distances[v3] = static_cast<float>(u3tmp);
        direction[v3] = 0;
        return true;
      }
      return false;
    }
  }
  return false;
}


uint32_t MeshPlanner::waveFrontPropagation(
    const mesh_map::Vector &original_start,
    const mesh_map::Vector &original_goal,
    const lvr2::DenseEdgeMap<float> &edge_weights,
    const lvr2::DenseVertexMap<float> &costs,
    std::list<std::pair<mesh_map::Vector, lvr2::FaceHandle>> &path,
    lvr2::DenseVertexMap<float> &distances,
    lvr2::DenseVertexMap<lvr2::VertexHandle> &predecessors) {
  ROS_INFO_STREAM("Init wave front propagation.");

  const auto &mesh = mesh_map->mesh();

  const auto &vertex_costs = mesh_map->vertexCosts();

  auto & invalid = mesh_map->invalid;

  //mesh_map->publishDebugPoint(original_start, mesh_map::color(0, 1, 0), "start_point");
  //mesh_map->publishDebugPoint(original_goal, mesh_map::color(1, 0, 0), "goal_point");

  mesh_map::Vector start = original_start;
  mesh_map::Vector goal = original_goal;

  // Find the containing faces of start and goal
  const auto &start_opt = mesh_map->getContainingFace(start, 0.2);
  const auto &goal_opt = mesh_map->getContainingFace(goal, 0.2);

  // reset cancel planning
  cancel_planning = false;

  if (!start_opt)
    return mbf_msgs::GetPathResult::INVALID_START;
  if (!goal_opt)
    return mbf_msgs::GetPathResult::INVALID_GOAL;

  const auto &start_face = start_opt.unwrap();
  const auto &goal_face = goal_opt.unwrap();

  //mesh_map->publishDebugFace(goal_face, mesh_map::color(1, 0, 0), "goal_face");
  //mesh_map->publishDebugFace(start_face, mesh_map::color(0, 1, 0), "start_face");

  path.clear();
  distances.clear();
  predecessors.clear();

  // TODO in face planning for a single face
  if (goal_face == start_face) {
    return true;
  }

  lvr2::DenseVertexMap<bool> fixed(mesh.nextVertexIndex(), false);

  // clear vector field map
  vector_map.clear();

  ros::WallTime t_start, t_end;
  t_start = ros::WallTime::now();

  // initialize distances with infinity
  // initialize predecessor of each vertex with itself
  for (auto const &vH : mesh.vertices()) {
    distances.insert(vH, std::numeric_limits<float>::infinity());
    predecessors.insert(vH, vH);
  }

  lvr2::Meap<lvr2::VertexHandle, float> pq;
  // Set start distance to zero
  // add start vertex to priority queue
  for (auto vH : mesh.getVerticesOfFace(start_face)) {
    const mesh_map::Vector diff = start - mesh.getVertexPosition(vH);
    const float dist = diff.length();
    distances[vH] = dist;
    vector_map.insert(vH, diff);
    cutting_faces.insert(vH, start_face);
    fixed[vH] = true;
    pq.insert(vH, dist);
  }

  bool goal_face_fixed = false;

  ROS_INFO_STREAM("Start wave front propagation");

  while (!pq.isEmpty() && !cancel_planning && !goal_face_fixed) {
    lvr2::VertexHandle current_vh = pq.popMin().key();

    // check if already fixed
    // if(fixed[current_vh]) continue;
    fixed[current_vh] = true;

    std::vector<lvr2::VertexHandle> neighbours;
    try{
      mesh.getNeighboursOfVertex(current_vh, neighbours);
    }
    catch (lvr2::PanicException exception)
    {
      ROS_ERROR_STREAM("Found non manifold vertex!");
      continue;
    }
    for(auto nh : neighbours){

      if(goal_face_fixed)
        break;

      if(invalid[nh])
        continue;

      std::vector<lvr2::FaceHandle> faces;
      try{
        mesh.getFacesOfVertex(nh, faces);
      }
      catch(lvr2::PanicException exception){
        ROS_ERROR_STREAM("Found non manifold vertex!");
        continue;
      }

      for (auto fh : faces) {
        const auto vertices = mesh.getVerticesOfFace(fh);
        const lvr2::VertexHandle &a = vertices[0];
        const lvr2::VertexHandle &b = vertices[1];
        const lvr2::VertexHandle &c = vertices[2];

        if(invalid[a] || invalid[b] || invalid[c]){
          ROS_ERROR_STREAM("Found non manifold vertex!");
          continue;
        }

        // We are looking for a face where exactly
        // one vertex is not in the fixed set
        if (fixed[a] && fixed[b] && fixed[c]) {
          if(fh == goal_face)
          {
            // if all vertices are fixed and we reached the goal face,
            // stop the wave front propagation.
            ROS_INFO_STREAM("Wave front reached the goal!");
            goal_face_fixed = true;
            break;
          }
          // The face's vertices are already optimal
          // with respect to the distance
          continue;
        }

        try{
          if (fixed[a] && fixed[b] && !fixed[c]) {
            // c is free
            //if(vertex_costs[c] >= config.cost_limit)
            //  continue;
            if (waveFrontUpdate(distances, edge_weights, a, b, c))
              pq.insert(c, distances[c]);
          } else if (fixed[a] && !fixed[b] && fixed[c]) {
            // b is free
            //if(vertex_costs[b] >= config.cost_limit)
            //  continue;
            if (waveFrontUpdate(distances, edge_weights, c, a, b))
              pq.insert(b, distances[b]);
          } else if (!fixed[a] && fixed[b] && fixed[c]) {
            // a if free
            //if(vertex_costs[a] >= config.cost_limit)
            //  continue;
            if (waveFrontUpdate(distances, edge_weights, b, c, a))
              pq.insert(a, distances[a]);
          } else {
            // two free vertices -> skip that face
            continue;
          }
        }
        catch(lvr2::PanicException exception)
        {
          ROS_ERROR_STREAM("Found non manifold vertex!");
          continue;
        }
      }
    }
  }

  t_end = ros::WallTime::now();
  double execution_time = (t_end - t_start).toNSec() * 1e-6;
  ROS_INFO_STREAM("Execution time (ms): " << execution_time << " for "
                                          << mesh.numVertices()
                                          << " num vertices in the mesh.");

  if (cancel_planning) {
    ROS_WARN_STREAM("Wave front propagation has been canceled!");
    return mbf_msgs::GetPathResult::CANCELED;
  }

  ROS_INFO_STREAM("Finished wave front propagation.");

  /*
   * Sampling the path by backtracking the vector field
   */

  ROS_INFO_STREAM("Compute vector map");
  computeVectorMap();

  bool path_exists = false;

  for (auto goal_vertex : mesh.getVerticesOfFace(goal_face)) {
    if (goal_vertex != predecessors[goal_vertex]) {
      path_exists = true;
      break;
    }
  }

  if (!path_exists) {
    ROS_WARN("Predecessor of the goal is not set! No path found!");
    return mbf_msgs::GetPathResult::NO_PATH_FOUND;
  }

  ROS_INFO_STREAM("Start vector field back tracking!");
  constexpr float step_width = 0.03; // step width of 3 cm

  lvr2::FaceHandle current_face = goal_face;
  mesh_map::Vector current_pos = goal;
  path.push_front(
      std::pair<mesh_map::Vector, lvr2::FaceHandle>(current_pos, current_face));

  while (current_pos.distance2(start) > step_width && !cancel_planning) {
    // move current pos ahead on the surface following the vector field,
    // updates the current face if necessary
    if (mesh_map->meshAhead(current_pos, current_face, step_width)) {
      path.push_front(std::pair<mesh_map::Vector, lvr2::FaceHandle>(
          current_pos, current_face));
    } else {
      ROS_WARN_STREAM(
          "Could not find a valid path, while back-tracking from the goal");
      return mbf_msgs::GetPathResult::NO_PATH_FOUND;
    }
  }
  path.push_front(
      std::pair<mesh_map::Vector, lvr2::FaceHandle>(start, start_face));

  if (cancel_planning) {
    ROS_WARN_STREAM("Wave front propagation has been canceled!");
    return mbf_msgs::GetPathResult::CANCELED;
  }

  ROS_INFO_STREAM("Successfully finished vector field back tracking!");

  return mbf_msgs::GetPathResult::SUCCESS;

  /*
  lvr2::VertexHandle prev = predecessors[goal_vertex];

  if(prev == goal_vertex)
  {
    ROS_WARN("Predecessor of goal not set!");
    return false;
  }

  while(prev != start_vertex)
  {
    path.push_front(prev);
    if(predecessors[prev] == prev){
      ROS_WARN_STREAM("No path found!");
      return false;
    }
    prev = predecessors[prev];
  }
  path.push_front(start_vertex);
*/
}


} /* namespace mesh_planner */

