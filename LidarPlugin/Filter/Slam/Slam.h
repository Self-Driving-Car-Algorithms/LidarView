//=========================================================================
//
// Copyright 2018 Kitware, Inc.
// Author: Guilbert Pierre (spguilbert@gmail.com)
// Date: 03-27-2018
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
//=========================================================================

// This slam algorithm is inspired by the LOAM algorithm:
// J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
// Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

// The algorithm is composed of three sequential steps:
//
// - Keypoints extraction: this step consists of extracting keypoints over
// the points clouds. To do that, the laser lines / scans are trated indepently.
// The laser lines are projected onto the XY plane and are rescale depending on
// their vertical angle. Then we compute their curvature and create two class of
// keypoints. The edges keypoints which correspond to points with a hight curvature
// and planar points which correspond to points with a low curvature.
//
// - Ego-Motion: this step consists of recovering the motion of the lidar
// sensor between two frames (two sweeps). The motion is modelized by a constant
// velocity and angular velocity between two frames (i.e null acceleration).
// Hence, we can parameterize the motion by a rotation and translation per sweep / frame
// and interpolate the transformation inside a frame using the timestamp of the points.
// Since the points clouds generated by a lidar are sparses we can't design a
// pairwise match between keypoints of two successive frames. Hence, we decided to use
// a closest-point matching between the keypoints of the current frame
// and the geometrics features derived from the keypoints of the previous frame.
// The geometrics features are lines or planes and are computed using the edges keypoints
// and planar keypoints of the previous frame. Once the matching is done, a keypoint
// of the current frame is matched with a plane / line (depending of the
// nature of the keypoint) from the previous frame. Then, we recover R and T by
// minimizing the function f(R, T) = sum(d(point, line)^2) + sum(d(point, plane)^2).
// Which can be writen f(R, T) = sum((R*X+T-P).t*A*(R*X+T-P)) where:
// - X is a keypoint of the current frame
// - P is a point of the corresponding line / plane
// - A = (n*n.t) with n being the normal of the plane
// - A = (I - n*n.t).t * (I - n*n.t) with n being a director vector of the line
// Since the function f(R, T) is a non-linear mean square error function
// we decided to use the Levenberg-Marquardt algorithm to recover its argmin.
//
// - Mapping: This step consists of refining the motion recovered in the Ego-Motion
// step and to add the new frame in the environment map. Thanks to the ego-motion
// recovered at the previous step it is now possible to estimate the new position of
// the sensor in the map. We use this estimation as an initial point (R0, T0) and we
// perform an optimization again using the keypoints of the current frame and the matched
// keypoints of the map (and not only the previous frame this time!). Once the position in the
// map has been refined from the first estimation it is then possible to update the map by
// adding the keypoints of the current frame into the map.
//
// In the following programs : "slam" and "slam.cxx" the lidar
// coordinate system {L} is a 3D coordinate system with its origin at the
// geometric center of the lidar. The world coordinate system {W} is a 3D
// coordinate system which coinciding with {L] at the initial position. The
// points will be denoted by the ending letter L or W if they belong to
// the corresponding coordinate system

#ifndef SLAM_H
#define SLAM_H

// a new PCL Point is added so we need to recompile PCL
// to be able to use filters with this new type
#ifndef PCL_NO_PRECOMPILE
#define PCL_NO_PRECOMPILE
#endif

#include <pcl/kdtree/kdtree_flann.h>

#include <Eigen/Geometry>

#include "LidarPoint.h"
#include "SpinningSensorKeypointExtractor.h"
#include "KalmanFilter.h"
#include "KDTreePCLAdaptor.h"
#include "MotionModel.h"

#define SetMacro(name,type) void Set##name (type _arg) { name = _arg; }
#define GetMacro(name,type) type Get##name () const { return name; }

class RollingGrid;

enum MatchingMode
{
  EgoMotion = 0,
  Mapping = 1
};

enum WithinFrameTrajMode
{
  EgoMotionTraj = 0,
  MappingTraj = 1,
  UndistortionTraj = 2
};

struct Transform
{
  double time = 0;
  union
  {
    double position[3] = { 0.0, 0.0, 0.0 };
    struct {
      double x;
      double y;
      double z;
    };
  };
  union
  {
    double orientation[3] = { 0.0, 0.0, 0.0 };
    struct {
      double rx;
      double ry;
      double rz;
    };
  };

  Transform() = default;
  Transform(double t, const Eigen::Matrix<double, 6, 1>& data)
    : time(t), x(data[0]), y(data[1]), z(data[2]),
      rx(data[3]), ry(data[4]), rz(data[5]) {}
};

class Slam
{
public:
  using Point = SpinningSensorKeypointExtractor::Point;
  Slam();
  void Reset();

  // Add a new frame to process to the slam algorithm
  // From this frame; keypoints will be computed and extracted
  // in order to recover the ego-motion of the lidar sensor
  // and to update the map using keypoints and ego-motion
  void AddFrame(pcl::PointCloud<Point>::Ptr pc, std::vector<size_t> laserIdMapping);

  // Get the computed world transform so far
  Transform GetWorldTransform();
  std::vector<double> GetTransformCovariance();

  std::unordered_map<std::string, double> GetDebugInformation();

  pcl::PointCloud<Point>::Ptr GetEdgesMap();
  pcl::PointCloud<Point>::Ptr GetPlanarsMap();
  pcl::PointCloud<Point>::Ptr GetBlobsMap();

  GetMacro(MaxDistBetweenTwoFrames, double)
  SetMacro(MaxDistBetweenTwoFrames, double)

  GetMacro(MaxDistanceForICPMatching, double)
  SetMacro(MaxDistanceForICPMatching, double)

  GetMacro(FastSlam, bool)
  SetMacro(FastSlam, bool)

  SetMacro(Undistortion, bool)
  GetMacro(Undistortion, bool)

  // Set RollingGrid Parameters
  void SetVoxelGridLeafSizeEdges(double size);
  void SetVoxelGridLeafSizePlanes(double size);
  void SetVoxelGridLeafSizeBlobs(double size);
  void SetVoxelGridSize(unsigned int size);
  void SetVoxelGridResolution(double resolution);

  // Get/Set EgoMotion
  GetMacro(EgoMotionLMMaxIter, unsigned int)
  SetMacro(EgoMotionLMMaxIter, unsigned int)

  GetMacro(EgoMotionICPMaxIter, unsigned int)
  SetMacro(EgoMotionICPMaxIter, unsigned int)

  GetMacro(EgoMotionLineDistanceNbrNeighbors, unsigned int)
  SetMacro(EgoMotionLineDistanceNbrNeighbors, unsigned int)

  GetMacro(EgoMotionMinimumLineNeighborRejection, unsigned int)
  SetMacro(EgoMotionMinimumLineNeighborRejection, unsigned int)

  GetMacro(EgoMotionLineDistancefactor, double)
  SetMacro(EgoMotionLineDistancefactor, double)

  GetMacro(EgoMotionPlaneDistanceNbrNeighbors, unsigned int)
  SetMacro(EgoMotionPlaneDistanceNbrNeighbors, unsigned int)

  GetMacro(EgoMotionPlaneDistancefactor1, double)
  SetMacro(EgoMotionPlaneDistancefactor1, double)

  GetMacro(EgoMotionPlaneDistancefactor2, double)
  SetMacro(EgoMotionPlaneDistancefactor2, double)

  GetMacro(EgoMotionMaxLineDistance, double)
  SetMacro(EgoMotionMaxLineDistance, double)

  GetMacro(EgoMotionMaxPlaneDistance, double)
  SetMacro(EgoMotionMaxPlaneDistance, double)

  GetMacro(EgoMotionInitLossScale, double)
  SetMacro(EgoMotionInitLossScale, double)

  GetMacro(EgoMotionFinalLossScale, double)
  SetMacro(EgoMotionFinalLossScale, double)

  // Get/Set Mapping
  GetMacro(MappingLMMaxIter, unsigned int)
  SetMacro(MappingLMMaxIter, unsigned int)

  GetMacro(MappingICPMaxIter, unsigned int)
  SetMacro(MappingICPMaxIter, unsigned int)

  GetMacro(MappingLineDistanceNbrNeighbors, unsigned int)
  SetMacro(MappingLineDistanceNbrNeighbors, unsigned int)

  GetMacro(MappingMinimumLineNeighborRejection, unsigned int)
  SetMacro(MappingMinimumLineNeighborRejection, unsigned int)

  GetMacro(MappingLineDistancefactor, double)
  SetMacro(MappingLineDistancefactor, double)

  GetMacro(MappingPlaneDistanceNbrNeighbors, unsigned int)
  SetMacro(MappingPlaneDistanceNbrNeighbors, unsigned int)

  GetMacro(MappingPlaneDistancefactor1, double)
  SetMacro(MappingPlaneDistancefactor1, double)

  GetMacro(MappingPlaneDistancefactor2, double)
  SetMacro(MappingPlaneDistancefactor2, double)

  GetMacro(MappingMaxLineDistance, double)
  SetMacro(MappingMaxLineDistance, double)

  GetMacro(MappingMaxPlaneDistance, double)
  SetMacro(MappingMaxPlaneDistance, double)

  GetMacro(MappingLineMaxDistInlier, double)
  SetMacro(MappingLineMaxDistInlier, double)

  GetMacro(MappingInitLossScale, double)
  SetMacro(MappingInitLossScale, double)

  GetMacro(MappingFinalLossScale, double)
  SetMacro(MappingFinalLossScale, double)

  void SetKeyPointsExtractor(std::shared_ptr<SpinningSensorKeypointExtractor> extractor) { this->KeyPointsExtractor = extractor; }
  std::shared_ptr<SpinningSensorKeypointExtractor> GetKeyPointsExtractor() { return this->KeyPointsExtractor; }

private:
  std::vector<Transform> Trajectory;

  // Array used only for debug purposes
  double EgoMotionEdgesPointsUsed;
  double EgoMotionPlanesPointsUsed;
  double MappingEdgesPointsUsed;
  double MappingPlanesPointsUsed;
  double MappingBlobsPointsUsed;
  double MappingVarianceError;

  // Mapping between keypoints and their corresponding
  // index in the vtk input frame
  std::vector<int> EdgePointRejectionEgoMotion;
  std::vector<int> PlanarPointRejectionEgoMotion;
  std::vector<int> EdgePointRejectionMapping;
  std::vector<int> PlanarPointRejectionMapping;

  // If set to true the mapping planars keypoints used
  // will be the same than the EgoMotion one. If set to false
  // all points that are not set to invalid will be used
  // as mapping planars points.
  bool FastSlam = true;

  // Should the algorithm undistord the frame or not
  // The undistortion will improve the accuracy but
  // the computation speed will decrease
  bool Undistortion = false;

  // Represents estimated samples of the trajectory
  // of the sensor within a lidar frame. The orientation
  // and position of the sensor at a random time t can then
  // be obtained using an interpolation
  SampledSensorPath WithinFrameTrajectory;

  // keypoints extracted
  pcl::PointCloud<Point>::Ptr CurrentEdgesPoints;
  pcl::PointCloud<Point>::Ptr CurrentPlanarsPoints;
  pcl::PointCloud<Point>::Ptr CurrentBlobsPoints;
  pcl::PointCloud<Point>::Ptr PreviousEdgesPoints;
  pcl::PointCloud<Point>::Ptr PreviousPlanarsPoints;
  pcl::PointCloud<Point>::Ptr PreviousBlobsPoints;

  // keypoints local map
  std::shared_ptr<RollingGrid> EdgesPointsLocalMap;
  std::shared_ptr<RollingGrid> PlanarPointsLocalMap;
  std::shared_ptr<RollingGrid> BlobsPointsLocalMap;

  // Number of frame that have been processed
  unsigned int NbrFrameProcessed = 0;

  // The max distance allowed between two frames
  // If the distance is over this limit, the ICP
  // matching will not match point and the odometry
  // will fail. It has to be setted according to the
  // maximum speed of the vehicule used
  // Represent the distance that the lidar has made during one sweep
  // if it is moving at a speed of 90 km/h and spinning at a rpm
  // of 600 rotation per minute
  double MaxDistBetweenTwoFrames = (90.0 / 3.6) * (60.0 / 600.0);

  // Maximum number of iteration
  // in the ego motion optimization step
  unsigned int EgoMotionLMMaxIter = 15;

  // Maximum number of iteration
  // in the mapping optimization step
  unsigned int MappingLMMaxIter = 15;

  // During the Levenberg-Marquardt algoritm
  // keypoints will have to be match with planes
  // and lines of the previous frame. This parameter
  // indicates how many times we want to do the
  // the ICP matching
  unsigned int EgoMotionICPMaxIter = 4;
  unsigned int MappingICPMaxIter = 3;

  // When computing the point<->line and point<->plane distance
  // in the ICP, the kNearest edges/planes points of the current
  // points are selected to approximate the line/plane using a PCA
  // If the one of the k-nearest points is too far the neigborhood
  // is rejected. We also make a filter upon the ratio of the eigen
  // values of the variance-covariance matrix of the neighborhood
  // to check if the points are distributed upon a line or a plane
  unsigned int MappingLineDistanceNbrNeighbors = 10;
  unsigned int MappingMinimumLineNeighborRejection = 4;
  double MappingLineDistancefactor = 5.0;

  unsigned int MappingPlaneDistanceNbrNeighbors = 5;
  double MappingPlaneDistancefactor1 = 35.0;
  double MappingPlaneDistancefactor2 = 8.0;

  double MappingMaxPlaneDistance = 0.2;
  double MappingMaxLineDistance = 0.2;
  double MappingLineMaxDistInlier = 0.2;

  unsigned int EgoMotionLineDistanceNbrNeighbors = 8;
  unsigned int EgoMotionMinimumLineNeighborRejection = 3;
  double EgoMotionLineDistancefactor = 5.;

  unsigned int EgoMotionPlaneDistanceNbrNeighbors = 5;
  double EgoMotionPlaneDistancefactor1 = 35.0;
  double EgoMotionPlaneDistancefactor2 = 8.0;

  double EgoMotionMaxPlaneDistance = 0.2;
  double EgoMotionMaxLineDistance = 0.2;

  std::shared_ptr<SpinningSensorKeypointExtractor> KeyPointsExtractor =
      std::make_shared<SpinningSensorKeypointExtractor>();

  // Use or not blobs
  bool UseBlob = false;

  // The max distance allowed between two frames
  // If the distance is over this limit, the ICP
  // matching will not match point and the odometry
  // will fail. It has to be setted according to the
  // maximum speed of the vehicule used
  double MaxDistanceForICPMatching = 20.0;

  // Transformation to map the current pointcloud
  // in the referential of the previous one
  Eigen::Matrix<double, 6, 1> Trelative;
  Eigen::VectorXd MotionParametersEgoMotion;

  // Transformation to map the current pointcloud
  // in the world (i.e first frame) one
  Eigen::Matrix<double, 6, 1> Tworld = Eigen::Matrix<double, 6, 1>::Zero();
  Eigen::Matrix<double, 6, 1> PreviousTworld = Eigen::Matrix<double, 6, 1>::Zero();
  Eigen::VectorXd MotionParametersMapping;

  // Variance-Covariance matrix that estimates the
  // estimation error about the 6-DoF parameters
  Eigen::Matrix<double, 6, 6> TworldCovariance = Eigen::Matrix<double, 6, 6>::Identity();

  // Computed trajectory of the sensor
  // i.e the list of transforms computed
  std::vector<Eigen::Matrix<double, 6, 1> > TworldList;

  // To recover the ego-motion we have to minimize the function
  // f(R, T) = sum(d(point, line)^2) + sum(d(point, plane)^2). In both
  // case the distance between the point and the line / plane can be
  // writen (R*X+T - P).t * A * (R*X+T - P). Where X is the key point
  // P is a point on the line / plane. A = (n*n.t) for a plane with n
  // being the normal and A = (I - n*n.t)^2 for a line with n being
  // a director vector of the line
  // - Avalues will store the A matrix
  // - Pvalues will store the P points
  // - Xvalues will store the W points
  // - residualCoefficient will attenuate the distance function for outliers
  // - TimeValues store the time acquisition
  std::vector<Eigen::Matrix3d > Avalues;
  std::vector<Eigen::Vector3d > Pvalues;
  std::vector<Eigen::Vector3d > Xvalues;
  std::vector<double> residualCoefficient;
  std::vector<double> TimeValues;

  // Histogram of the ICP matching rejection causes
  std::vector<double> MatchRejectionHistogramPlane;
  std::vector<double> MatchRejectionHistogramLine;
  std::vector<double> MatchRejectionHistogramBlob;
  int NrejectionCauses = 7;
  void ResetDistanceParameters();

  // Display information about the keypoints - neighborhood
  // mathching rejections
  void RejectionInformationDisplay();

  // Add a default point to the trajectories
  void AddDefaultPoint(double x, double y, double z, double rx, double ry, double rz, double t);

  // Saturation properties
  double EgoMotionInitLossScale = 2.0 ; // Saturation around 5 meters
  double EgoMotionFinalLossScale = 0.2 ; // Saturation around 1.5 meters
  double MappingInitLossScale = 0.7; // Saturation around 2.5 meters
  double MappingFinalLossScale = 0.05; // // Saturation around 0.4 meters

  // Find the ego motion of the sensor between
  // the current frame and the next one using
  // the keypoints extracted.
  void ComputeEgoMotion();

  // Map the position of the sensor from
  // the current frame in the world referential
  // using the map and the keypoints extracted.
  void Mapping();

  // Transform the input point already undistort into Tworld.
  void TransformToWorld(Point& p);

  // Match the current keypoint with its neighborhood in the map / previous
  // frames. From this match we compute the point-to-neighborhood distance
  // function:
  // (R * X + T - P).t * A * (R * X + T - P)
  // Where P is the mean point of the neighborhood and A is the symmetric
  // variance-covariance matrix encoding the shape of the neighborhood
  int ComputeLineDistanceParameters(KDTreePCLAdaptor& kdtreePreviousEdges, Eigen::Matrix3d& R,
                                    Eigen::Vector3d& dT, Point p, MatchingMode matchingMode);
  int ComputePlaneDistanceParameters(KDTreePCLAdaptor& kdtreePreviousPlanes, Eigen::Matrix3d& R,
                                     Eigen::Vector3d& dT, Point p, MatchingMode matchingMode);
  int ComputeBlobsDistanceParameters(pcl::KdTreeFLANN<Point>::Ptr kdtreePreviousBlobs, Eigen::Matrix3d& R,
                                     Eigen::Vector3d& dT, Point p, MatchingMode matchingMode);

  // Instead of taking the k-nearest neigbors in the odometry
  // step we will take specific neighbor using the particularities
  // of the lidar sensor
  void GetEgoMotionLineSpecificNeighbor(std::vector<int>& nearestValid, std::vector<float>& nearestValidDist,
                                        unsigned int nearestSearch, KDTreePCLAdaptor& kdtreePreviousEdges, Point p);

  // Instead of taking the k-nearest neighbors in the mapping
  // step we will take specific neighbor using a sample consensus
  // model
  void GetMappingLineSpecificNeigbbor(std::vector<int>& nearestValid, std::vector<float>& nearestValidDist, double maxDistInlier,
                                        unsigned int nearestSearch, KDTreePCLAdaptor& kdtreePreviousEdges, Point p);

  // All points of the current frame has been
  // acquired at a different timestamp. The goal
  // is to express them in a same referential
  // This can be done using estimated egomotion and assuming
  // a constant angular velocity and velocity during a sweep
  // or any other motion model

  // Express the provided point into the referential of the sensor
  // at time tf. The referential at time of acquisition t is estimated
  // using the constant velocity hypothesis and the provided sensor
  // position estimation
  void ExpressPointInOtherReferencial(Point& p);
  void ExpressPointCloudInOtherReferencial(pcl::PointCloud<Point>::Ptr pointcloud);

  // Compute the trajectory of the sensor within a frame
  // according to the sensor motion model.
  // for the EgoMotion part it is just an interpolation
  // between Id and Trelative
  // for the mapping part it is an interpolation between indentity
  // and the incremental transform between TworldPrevious and Tworld
  void CreateWithinFrameTrajectory(SampledSensorPath& path, WithinFrameTrajMode mode);

  // Update the world transformation by integrating
  // the relative motion recover and the previous
  // world transformation
  void UpdateTworldUsingTrelative();

  // Update the maps by populate the rolling grids
  // using the current keypoints expressed in the
  // world reference frame coordinate system
  void UpdateMapsUsingTworld();

  // Update the current keypoints by expressing
  // them in the reference coordinate system that
  // correspond to the one attached to the sensor
  // at the time of the end of the frame
  void UpdateCurrentKeypointsUsingTworld();

  // Set the lidar maximun range
  void SetLidarMaximunRange(const double maxRange);

  // Indicate if we are in display mode or not
  // Display mode will add arrays showing some
  // results of the slam algorithm such as
  // the keypoints extracted, curvature etc
  bool DisplayMode = false;

  // Identity matrix
  Eigen::Matrix3d I3 = Eigen::Matrix3d::Identity();
};

#endif // SLAM_H
