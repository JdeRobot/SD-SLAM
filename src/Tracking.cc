/**
 *
 *  Copyright (C) 2017-2018 Eduardo Perdices <eperdices at gsyc dot es>
 *
 *  The following code is a derivative work of the code from the ORB-SLAM2 project,
 *  which is licensed under the GNU Public License, version 3. This code therefore
 *  is also licensed under the terms of the GNU Public License, version 3.
 *  For more information see <https://github.com/raulmur/ORB_SLAM2>.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "Tracking.h"
#include <iostream>
#include <mutex>
#include <unistd.h>
#include "ORBmatcher.h"
#include "Converter.h"
#include "Optimizer.h"
#include "ImageAlign.h"
#include "Config.h"
#include "extra/log.h"
#include "sensors/ConstantVelocity.h"
#include "sensors/IMU.h"
#include <math.h>

using namespace std;

namespace SD_SLAM {

Tracking::Tracking(System *pSys, Map *pMap, const int sensor):
  mState(NO_IMAGES_YET), mSensor(sensor), mpInitializer(static_cast<Initializer*>(NULL)),
  mpPatternDetector(), mpSystem(pSys), mpMap(pMap), mnLastRelocFrameId(0), mbOnlyTracking(false), madgwick_(Config::MadgwickGain()) {
  // Load camera parameters
  float fx = Config::fx();
  float fy = Config::fy();
  float cx = Config::cx();
  float cy = Config::cy();

  mK.setIdentity();
  mK(0, 0) = fx;
  mK(1, 1) = fy;
  mK(0, 2) = cx;
  mK(1, 2) = cy;

  cv::Mat DistCoef(4, 1, CV_32F);
  DistCoef.at<float>(0) = Config::k1();
  DistCoef.at<float>(1) = Config::k2();
  DistCoef.at<float>(2) = Config::p1();
  DistCoef.at<float>(3) = Config::p2();
  const float k3 = Config::k3();
  if (k3 != 0) {
    DistCoef.resize(5);
    DistCoef.at<float>(4) = k3;
  }
  DistCoef.copyTo(mDistCoef);

  mbf = Config::bf();

  float fps = Config::fps();
  if (fps == 0)
    fps=30;

  // Max/Min Frames to insert keyframes and to check relocalisation
  mMinFrames = 0;
  mMaxFrames = fps;

  cout << endl << "Camera Parameters: " << endl;
  cout << "- fx: " << fx << endl;
  cout << "- fy: " << fy << endl;
  cout << "- cx: " << cx << endl;
  cout << "- cy: " << cy << endl;
  cout << "- k1: " << DistCoef.at<float>(0) << endl;
  cout << "- k2: " << DistCoef.at<float>(1) << endl;
  if (DistCoef.rows==5)
    cout << "- k3: " << DistCoef.at<float>(4) << endl;
  cout << "- p1: " << DistCoef.at<float>(2) << endl;
  cout << "- p2: " << DistCoef.at<float>(3) << endl;
  cout << "- fps: " << fps << endl;

  // Load ORB parameters
  int nFeatures = Config::NumFeatures();
  float fScaleFactor = Config::ScaleFactor();
  int nLevels = Config::NumLevels();
  int fThFAST = Config::ThresholdFAST();

  mpORBextractorLeft = new ORBextractor(nFeatures, fScaleFactor,nLevels, fThFAST);

  if (sensor!=System::RGBD)
    mpIniORBextractor = new ORBextractor(2*nFeatures, fScaleFactor,nLevels, fThFAST);

  cout << endl  << "ORB Extractor Parameters: " << endl;
  cout << "- Number of Features: " << nFeatures << endl;
  cout << "- Scale Levels: " << nLevels << endl;
  cout << "- Scale Factor: " << fScaleFactor << endl;
  cout << "- Fast Threshold: " << fThFAST << endl;

  if (sensor==System::RGBD) {
    mThDepth = mbf*(float)Config::ThDepth()/fx;
    cout << endl << "Depth Threshold (Close/Far Points): " << mThDepth << endl;

    mDepthMapFactor = Config::DepthMapFactor();
    if (fabs(mDepthMapFactor)<1e-5)
      mDepthMapFactor=1;
    else
      mDepthMapFactor = 1.0f/mDepthMapFactor;
  }

  threshold_ = 32;  // 8
  usePattern = Config::UsePattern();
  align_image_ = true;

  if (usePattern)
    std::cout << "Use pattern for initialization" << std::endl;

  mpLoopClosing = nullptr;
  mpLocalMapper = nullptr;

  lastRelativePose_.setZero();

  // Set motion model
  Sensor * sensor_model;
  if (sensor == System::MONOCULAR_IMU)
    sensor_model = new IMU();
  else
    sensor_model = new ConstantVelocity();
  motion_model_ = new EKF(sensor_model);
  
  set_debug = false;
}

Eigen::Matrix4d Tracking::GrabImageRGBD(const cv::Mat &im, const cv::Mat &imD, const std::string filename) {
  cv::Mat imDepth = imD;

  // Image must be in gray scale
  assert(im.channels() == 1);

  if ((fabs(mDepthMapFactor-1.0f) > 1e-5) || imD.type() != CV_32F)
    imDepth.convertTo(imDepth, CV_32F, mDepthMapFactor);

  mCurrentFrame = Frame(im, imDepth, mpORBextractorLeft, mK, mDistCoef, mbf, mThDepth);

  Track();

  return mCurrentFrame.GetPose();
}


Eigen::Matrix4d Tracking::GrabImageMonocular(const cv::Mat &im, const std::string filename) {
  // Image must be in gray scale
  assert(im.channels() == 1);

  if (mState==NOT_INITIALIZED || mState==NO_IMAGES_YET)
    mCurrentFrame = Frame(im, mpIniORBextractor, mK, mDistCoef, mbf, mThDepth);
  else
    mCurrentFrame = Frame(im, mpORBextractorLeft, mK, mDistCoef, mbf, mThDepth);

  Track();

  return mCurrentFrame.GetPose();
}

Eigen::Matrix4d Tracking::GrabImageFusion(const cv::Mat &im, const double dt, const std::string filename) {
  // Image must be in gray scale 8UB
  // Img + Imu
  assert(im.channels() == 1);
  dt_ = dt;

  if (mState==NOT_INITIALIZED || mState==NO_IMAGES_YET)
    mCurrentFrame = Frame(im, mpIniORBextractor, mK, mDistCoef, mbf, mThDepth);
  else
    mCurrentFrame = Frame(im, mpORBextractorLeft, mK, mDistCoef, mbf, mThDepth);

  Track();

  return mCurrentFrame.GetPose();
}

Frame Tracking::CreateFrame(const cv::Mat &im) {
  return Frame(im, mpORBextractorLeft, mK, mDistCoef, mbf, mThDepth);
}

Frame Tracking::CreateFrame(const cv::Mat &im, const cv::Mat &imD) {
  cv::Mat imDepth = imD;

  if ((fabs(mDepthMapFactor-1.0f) > 1e-5) || imD.type() != CV_32F)
    imDepth.convertTo(imDepth, CV_32F, mDepthMapFactor);

  return Frame(im, imDepth, mpORBextractorLeft, mK, mDistCoef, mbf, mThDepth);
}

void Tracking::Track() {
  if (mState==NO_IMAGES_YET)
    mState = NOT_INITIALIZED;

  mLastProcessedState = mState;

  // Get Map Mutex -> Map cannot be changed
  unique_lock<mutex> lock(mpMap->mMutexMapUpdate);

  if (mState==NOT_INITIALIZED) {
    if (mSensor==System::RGBD)
      StereoInitialization();
    else {
      if (usePattern)
        PatternInitialization();
      else
        MonocularInitialization();
    }

    if (mState!=OK)
      return;
  } else {
    // System is initialized. Track Frame.
    bool bOK;

    // Initial camera pose estimation using motion model or relocalization (if tracking is lost)
    if (mState==OK) {
      // Local Mapping might have changed some MapPoints tracked in last frame
      CheckReplacedInLastFrame();

      if (!motion_model_->Started() || mCurrentFrame.mnId < mnLastRelocFrameId+2) {
        bOK = TrackReferenceKeyFrame();
      } else {
        if (mSensor == System::MONOCULAR_IMU_NEW || mSensor == System::FUSION_DATA_AND_GT)
          bOK = TrackWithNewIMUModel();
        else
          bOK = TrackWithMotionModel();
        if (!bOK) {
          LOGD("Failed Track TrackWithMotionModel. Trying to track with Reference key frame");
          bOK = TrackReferenceKeyFrame();
          motion_model_->Restart();
        }
      }
    } else {
      bOK = Relocalization();
      motion_model_->Restart();
    }

    mCurrentFrame.mpReferenceKF = mpReferenceKF;

    // If we have an initial estimation of the camera pose and matching. Track the local map.
    if (bOK)
      bOK = TrackLocalMap();

    if (bOK)
      mState = OK;
    else
      mState = LOST;

    // If tracking were good, check if we insert a keyframe
    if (bOK) {

      // Update motion sensor
      if (!mLastFrame.GetPose().isZero()){
        motion_model_->Update(mCurrentFrame.GetPose(), measurements_);
        madgwick_.set_orientation_from_frame(mCurrentFrame.GetPose());
      }

      else
        motion_model_->Restart();

      // Clean VO matches
      for (int i = 0; i<mCurrentFrame.N; i++) {
        MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];
        if (pMP)
          if (pMP->Observations()<1) {
            mCurrentFrame.mvbOutlier[i] = false;
            mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint*>(NULL);
          }
      }

      // Delete temporal MapPoints
      for (list<MapPoint*>::iterator lit = mlpTemporalPoints.begin(), lend =  mlpTemporalPoints.end(); lit!=lend; lit++) {
        MapPoint* pMP = *lit;
        delete pMP;
      }
      mlpTemporalPoints.clear();

      // Check if we need to insert a new keyframe
      if (NeedNewKeyFrame())
        CreateNewKeyFrame();

      // We allow points with high innovation (considererd outliers by the Huber Function)
      // pass to the new keyframe, so that bundle adjustment will finally decide
      // if they are outliers or not. We don't want next frame to estimate its position
      // with those points so we discard them in the frame.
      for (int i = 0; i<mCurrentFrame.N; i++) {
        if (mCurrentFrame.mvpMapPoints[i] && mCurrentFrame.mvbOutlier[i])
          mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint*>(NULL);
      }
    }

    // Reset if the camera get lost soon after initialization
    if (mState==LOST) {
      if (mpMap->KeyFramesInMap()<=5) {
        LOGD("Track lost soon after initialisation, reseting...");
        mpSystem->Reset();
        return;
      }
    }

    if (!mCurrentFrame.mpReferenceKF)
      mCurrentFrame.mpReferenceKF = mpReferenceKF;

    mLastFrame = Frame(mCurrentFrame);
  }

  // Store relative pose
  if (!mCurrentFrame.GetPose().isZero()) {
    Eigen::Matrix4d Tcr = mCurrentFrame.GetPose()*mCurrentFrame.mpReferenceKF->GetPoseInverse();
    lastRelativePose_ = Tcr;
  }
}

void Tracking::StereoInitialization() {
  if (mCurrentFrame.N>500) {
    // Set Frame pose to the origin
    mCurrentFrame.SetPose(Eigen::Matrix4d::Identity());

    // Create KeyFrame
    KeyFrame* pKFini = new KeyFrame(mCurrentFrame, mpMap);

    // Insert KeyFrame in the map
    mpMap->AddKeyFrame(pKFini);

    // Create MapPoints and asscoiate to KeyFrame
    for (int i = 0; i<mCurrentFrame.N; i++) {
      float z = mCurrentFrame.mvDepth[i];
      if (z > 0) {
        Eigen::Vector3d x3D = mCurrentFrame.UnprojectStereo(i);
        MapPoint* pNewMP = new MapPoint(x3D, pKFini, mpMap);
        pNewMP->AddObservation(pKFini, i);
        pKFini->AddMapPoint(pNewMP, i);
        pNewMP->ComputeDistinctiveDescriptors();
        pNewMP->UpdateNormalAndDepth();
        mpMap->AddMapPoint(pNewMP);

        mCurrentFrame.mvpMapPoints[i]=pNewMP;
      }
    }

    LOGD("New map created with %lu points", mpMap->MapPointsInMap());

    mpLocalMapper->InsertKeyFrame(pKFini);

    mnLastKeyFrameId = mCurrentFrame.mnId;
    mpLastKeyFrame = pKFini;

    mvpLocalKeyFrames.push_back(pKFini);
    mvpLocalMapPoints = mpMap->GetAllMapPoints();
    mpReferenceKF = pKFini;
    mCurrentFrame.mpReferenceKF = pKFini;

    mLastFrame = Frame(mCurrentFrame);

    mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

    mpMap->mvpKeyFrameOrigins.push_back(pKFini);

    mState = OK;
  }
}

void Tracking::MonocularInitialization() {
  if (!mpInitializer) {
    // Set Reference Frame
    if (mCurrentFrame.mvKeys.size()>100) {
      mInitialFrame = Frame(mCurrentFrame);
      mLastFrame = Frame(mCurrentFrame);
      mvbPrevMatched.resize(mCurrentFrame.mvKeysUn.size());
      for (size_t i = 0; i<mCurrentFrame.mvKeysUn.size(); i++)
        mvbPrevMatched[i] = mCurrentFrame.mvKeysUn[i].pt;

      if (mpInitializer)
        delete mpInitializer;

      mpInitializer =  new Initializer(mCurrentFrame, 1.0, 200);

      fill(mvIniMatches.begin(), mvIniMatches.end(),-1);

      return;
    }
  } else {
    // Try to initialize
    if ((int)mCurrentFrame.mvKeys.size()<=100) {
      delete mpInitializer;
      mpInitializer = static_cast<Initializer*>(NULL);
      fill(mvIniMatches.begin(), mvIniMatches.end(),-1);
      return;
    }

    // Find correspondences
    ORBmatcher matcher(0.9, true);
    int nmatches = matcher.SearchForInitialization(mInitialFrame, mCurrentFrame, mvbPrevMatched, mvIniMatches, 100);

    // Check if there are enough correspondences
    if (nmatches<100) {
      delete mpInitializer;
      mpInitializer = static_cast<Initializer*>(NULL);
      return;
    }

    Eigen::Matrix3d Rcw; // Current Camera Rotation
    Eigen::Vector3d tcw; // Current Camera Translation
    vector<bool> vbTriangulated; // Triangulated Correspondences (mvIniMatches)

    if (mpInitializer->Initialize(mCurrentFrame, mvIniMatches, Rcw, tcw, mvIniP3D, vbTriangulated)) {
      for (size_t i = 0, iend = mvIniMatches.size(); i < iend; i++) {
        if (mvIniMatches[i] >= 0 && !vbTriangulated[i]) {
          mvIniMatches[i]=-1;
          nmatches--;
        }
      }

      // Set Frame Poses
      mInitialFrame.SetPose(Eigen::Matrix4d::Identity());
      Eigen::Matrix4d Tcw;
      Tcw.setIdentity();
      Tcw.block<3, 3>(0, 0) = Rcw;
      Tcw.block<3, 1>(0, 3) = tcw;
      mCurrentFrame.SetPose(Tcw);

      CreateInitialMapMonocular();
    }
  }
}

void Tracking::CreateInitialMapMonocular() {
  // Create KeyFrames
  KeyFrame* pKFini = new KeyFrame(mInitialFrame, mpMap);
  KeyFrame* pKFcur = new KeyFrame(mCurrentFrame, mpMap);

  // Insert KFs in the map
  mpMap->AddKeyFrame(pKFini);
  mpMap->AddKeyFrame(pKFcur);

  // Create MapPoints and asscoiate to keyframes
  for (size_t i = 0; i<mvIniMatches.size(); i++) {
    if (mvIniMatches[i] < 0)
      continue;

    //Create MapPoint.
    Eigen::Vector3d worldPos(Converter::toVector3d(mvIniP3D[i]));

    MapPoint* pMP = new MapPoint(worldPos, pKFcur, mpMap);

    pKFini->AddMapPoint(pMP, i);
    pKFcur->AddMapPoint(pMP, mvIniMatches[i]);

    pMP->AddObservation(pKFini, i);
    pMP->AddObservation(pKFcur, mvIniMatches[i]);

    pMP->ComputeDistinctiveDescriptors();
    pMP->UpdateNormalAndDepth();

    //Fill Current Frame structure
    mCurrentFrame.mvpMapPoints[mvIniMatches[i]] = pMP;
    mCurrentFrame.mvbOutlier[mvIniMatches[i]] = false;

    //Add to Map
    mpMap->AddMapPoint(pMP);
  }

  // Update Connections
  pKFini->UpdateConnections();
  pKFcur->UpdateConnections();

  // Bundle Adjustment
  LOGD("New Map created with %lu points", mpMap->MapPointsInMap());

  Optimizer::GlobalBundleAdjustemnt(mpMap, 20);

  // Set median depth to 1
  float medianDepth = pKFini->ComputeSceneMedianDepth(2);
  float invMedianDepth = 1.0f/medianDepth;

  if (medianDepth < 0 || pKFcur->TrackedMapPoints(1)<100) {
    LOGE("Wrong initialization, reseting...");
    Reset();
    return;
  }

  // Scale initial baseline
  Eigen::Matrix4d Tc2w = pKFcur->GetPose();
  Tc2w.block<3, 1>(0, 3) = Tc2w.block<3, 1>(0, 3)*invMedianDepth;
  pKFcur->SetPose(Tc2w);

  // Scale points
  vector<MapPoint*> vpAllMapPoints = pKFini->GetMapPointMatches();
  for (size_t iMP = 0; iMP<vpAllMapPoints.size(); iMP++) {
    if (vpAllMapPoints[iMP]) {
      MapPoint* pMP = vpAllMapPoints[iMP];
      pMP->SetWorldPos(pMP->GetWorldPos()*invMedianDepth);
    }
  }

  mpLocalMapper->InsertKeyFrame(pKFini);
  mpLocalMapper->InsertKeyFrame(pKFcur);

  mCurrentFrame.SetPose(pKFcur->GetPose());
  mnLastKeyFrameId = mCurrentFrame.mnId;
  mpLastKeyFrame = pKFcur;

  mvpLocalKeyFrames.push_back(pKFcur);
  mvpLocalKeyFrames.push_back(pKFini);
  mvpLocalMapPoints = mpMap->GetAllMapPoints();
  mpReferenceKF = pKFcur;
  mCurrentFrame.mpReferenceKF = pKFcur;

  mLastFrame = Frame(mCurrentFrame);

  mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

  mpMap->mvpKeyFrameOrigins.push_back(pKFini);

  mState = OK;
}

void Tracking::PatternInitialization() {
  if (mCurrentFrame.N <= 500)
    return;

  // Search pattern
  if (mpPatternDetector.Detect(mCurrentFrame)) {

    // Set Frame pose from pattern
    Eigen::Matrix4d cam_pos = mpPatternDetector.GetRT();
    mCurrentFrame.SetPose(Eigen::Matrix4d::Identity());

    std::cout << "[INFO] Initial camera pose: [" << cam_pos(0, 3) << ", " << cam_pos(1, 3) << ", " << cam_pos(2, 3) << "]" << std::endl;

    // Create KeyFrame
    KeyFrame* pKFini = new KeyFrame(mCurrentFrame, mpMap);

    // Insert KeyFrame in the map
    mpMap->AddKeyFrame(pKFini);

    // Create MapPoints and asscoiate to keyframes
    vector<pair<int, Eigen::Vector3d>>& points = mpPatternDetector.GetPoints();
    for (auto p=points.begin(); p!=points.end(); p++) {
      int idx = p->first;

      // Calculate 3d point position from camera
      Eigen::Vector4d abspos(p->second(0), p->second(1), p->second(2), 1.0);
      Eigen::Vector4d relpos = cam_pos.inverse()*abspos;

      // Create MapPoint
      Eigen::Vector3d worldPos(relpos(0), relpos(1), relpos(2));
      MapPoint* pMP = new MapPoint(worldPos, pKFini, mpMap);
      pMP->AddObservation(pKFini, idx);
      pKFini->AddMapPoint(pMP, idx);
      pMP->ComputeDistinctiveDescriptors();
      pMP->UpdateNormalAndDepth();
      mpMap->AddMapPoint(pMP);

      // Fill Current Frame structure
      mCurrentFrame.mvpMapPoints[idx] = pMP;
    }

    LOGD("New map created from pattern with %lu points", mpMap->MapPointsInMap());

    mpLocalMapper->InsertKeyFrame(pKFini);

    mnLastKeyFrameId = mCurrentFrame.mnId;
    mpLastKeyFrame = pKFini;

    mvpLocalKeyFrames.push_back(pKFini);
    mvpLocalMapPoints = mpMap->GetAllMapPoints();
    mpReferenceKF = pKFini;
    mCurrentFrame.mpReferenceKF = pKFini;

    mLastFrame = Frame(mCurrentFrame);

    mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

    mpMap->mvpKeyFrameOrigins.push_back(pKFini);

    mState = OK;
  }
}

void Tracking::CheckReplacedInLastFrame() {
  for (int i  = 0; i<mLastFrame.N; i++) {
    MapPoint* pMP = mLastFrame.mvpMapPoints[i];

    if (pMP) {
      MapPoint* pRep = pMP->GetReplaced();
      if (pRep) {
        mLastFrame.mvpMapPoints[i] = pRep;
      }
    }
  }
}


bool Tracking::TrackReferenceKeyFrame() {
  ORBmatcher matcher(0.7, true);

  // Set same pose
  Eigen::Matrix4d last_pose = mLastFrame.GetPose();
  mCurrentFrame.SetPose(last_pose);

  LOGD("Last pose: [%.4f, %.4f, %.4f]", last_pose(0, 3), last_pose(1, 3), last_pose(2, 3));

  // Align current and last image
  if (align_image_) {
    ImageAlign image_align;
    if (!image_align.ComputePose(mCurrentFrame, mpReferenceKF)) {
      LOGE("Image align failed");
      mCurrentFrame.SetPose(last_pose);
    }
  }

  // Project points seen in reference keyframe
  fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint*>(NULL));
  int nmatches = matcher.SearchByProjection(mCurrentFrame, mpReferenceKF, threshold_, mSensor!=System::RGBD);

  // If few matches, ignores alignment and uses a wider window search
  if (nmatches<20) {
    LOGD("Not enough matches [%d], double threshold", nmatches);
    mCurrentFrame.SetPose(last_pose);
    fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint*>(NULL));
    nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, 2*threshold_, mSensor!=System::RGBD);
  }

  if (nmatches<20) {
    LOGD("Not enough matches [%d], tracking failed", nmatches);
    return false;
  }

  // Optimize frame pose with all matches
  Optimizer::PoseOptimization(&mCurrentFrame);

  // Discard outliers
  int nmatchesMap = 0;
  for (int i  = 0; i<mCurrentFrame.N; i++) {
    if (mCurrentFrame.mvpMapPoints[i]) {
      if (mCurrentFrame.mvbOutlier[i]) {
        MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];

        mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint*>(NULL);
        mCurrentFrame.mvbOutlier[i]=false;
        pMP->mbTrackInView = false;
        pMP->mnLastFrameSeen = mCurrentFrame.mnId;
        nmatches--;
      } else if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
        nmatchesMap++;
    }
  }

  if (nmatchesMap<10) {
    LOGD("Not enough inliers [%d], tracking failed", nmatchesMap);
    return false;
  }

  return true;
}

void Tracking::UpdateLastFrame() {
  // Update pose according to reference keyframe
  KeyFrame* pRef = mLastFrame.mpReferenceKF;
  Eigen::Matrix4d Tlr = lastRelativePose_;

  mLastFrame.SetPose(Tlr*pRef->GetPose());
}

bool Tracking::TrackVisual(Eigen::Matrix4d predicted_pose) {
  mCurrentFrame.SetPose(predicted_pose);

  ORBmatcher matcher(0.9, true);

  // Align current and last image
  if (align_image_) {
    ImageAlign image_align;
    if (!image_align.ComputePose(mCurrentFrame, mLastFrame)) {
      LOGE("Image align failed");
      mCurrentFrame.SetPose(predicted_pose);
    }
  }

  // Project points seen in previous frame
  fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint*>(NULL));
  int nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, threshold_, mSensor!=System::RGBD);
  first_proj = nmatches;

  // If few matches, ignores alignment and uses a wider window search
  if (nmatches<20) {
    LOGD("Not enough matches [%d], double threshold", nmatches);
    mCurrentFrame.SetPose(predicted_pose);
    fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint*>(NULL));
    nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, 2*threshold_, mSensor!=System::RGBD);
    second_proj = nmatches;
  }

  if (nmatches<20) {
    LOGD("Not enough matches [%d], tracking failed", nmatches);
    return false;
  }

  // Optimize frame pose with all matches
  Optimizer::PoseOptimization(&mCurrentFrame);

  // Discard outliers
  int nmatchesMap = 0;
  for (int i  = 0; i<mCurrentFrame.N; i++) {
    if (mCurrentFrame.mvpMapPoints[i]) {
      if (mCurrentFrame.mvbOutlier[i]) {
        MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];

        mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint*>(NULL);
        mCurrentFrame.mvbOutlier[i]=false;
        pMP->mbTrackInView = false;
        pMP->mnLastFrameSeen = mCurrentFrame.mnId;
        nmatches--;
      } else if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
        nmatchesMap++;
    }
  }

  inliers_on_pred = nmatchesMap;

  if (nmatchesMap<10) {
    LOGD("Not enough inliers [%d], tracking failed", nmatchesMap);
    return false;
  }

  return true;

}

bool Tracking::TrackWithNewIMUModel() {

  // Update last frame pose according to its reference keyframe
  UpdateLastFrame();

  // Predict pose with motion model
  Eigen::Matrix4d predicted_pose = motion_model_->Predict(mLastFrame.GetPose());
  LOGD("Predicted pose: [%.4f, %.4f, %.4f]", predicted_pose(0, 3), predicted_pose(1, 3), predicted_pose(2, 3));
  Matrix3d Rpred = predicted_pose.block<3,3>(0,0);
  // Update Madgwick
  madgwick_.update(imu_measurements_.acceleration(), imu_measurements_.angular_velocity(), dt_); // ADD DT
  Matrix3d Rimu = madgwick_.get_local_orientation().toRotationMatrix();

  // Check if movement is agresive or soft between t and t-1
  double movement_threshold = 0.02; // 0.04
  double angle = Quaterniond(mLastFrame.GetRotation()).angularDistance(madgwick_.get_local_orientation());


  // debug vars
  first_proj = 0, second_proj = 0, inliers_on_pred = 0, inliers_on_localmap = 0;
  stay_in_curve = angle > movement_threshold;
  // 

  int model = 1;
  
  /*
  Model 0: Monocular tracking
  Model 1: Reemplazar la rotacion de la prediccion por la de la IMU si estamos en curva
  Model 2: Reemplazar la rotacion de la prediccion por la de la IMU
  ----
  Using GT:  // ASEGURARSE QUE ESTAN EN EL MISMO SISTEMA DE COORDENADAS
  Model 3: Reemplazar la rotacion de la prediccion con la rotacion del GT -> ¿Tambien se pierde asi?
  Model 4: Usar como prediccion la traslacion del GT y la rotacion de la IMU -> No va a funcionar por la T
  */
 
  if ((model == 1 && stay_in_curve) || model == 2){
      LOGD("\t MODEL in curve!");
      predicted_pose.block<3,3>(0,0) = Rimu;
      //threshold_ = 32;
  }else{
    //threshold_ = 8;
  }


  bool vision = TrackVisual(predicted_pose);

  bool verbose = false;
  if (verbose){
    cout << "------- VERBOSE -------" << endl;

    // Disp matrix
    Matrix3d Rvisual = mCurrentFrame.GetRotation();
    Vector3d Apred = Rpred.eulerAngles(0,1,2);
    Vector3d Aimu  = Rimu.eulerAngles(0,1,2);
    Vector3d Avis  = Rvisual.eulerAngles(0,1,2);
    double roll, pitch, yaw;

    roll = Apred.x() * (180.0 / M_PI); pitch = Apred.y() * (180.0 / M_PI); yaw = Apred.z()* (180.0 / M_PI);
    printf("\nPrediction rotation: (%.2f, %.2f, %.2f). Matrix: \n", roll, pitch, yaw);
    cout << Rpred << endl;

    roll = Aimu.x() * (180.0 / M_PI); pitch = Aimu.y() * (180.0 / M_PI); yaw = Aimu.z()* (180.0 / M_PI);
    printf("\nIMU rotation:        (%.2f, %.2f, %.2f). Matrix: \n", roll, pitch, yaw);
    cout << Rimu << endl;

    roll = Avis.x() * (180.0 / M_PI); pitch = Avis.y() * (180.0 / M_PI); yaw = Avis.z()* (180.0 / M_PI);
    printf("\nVisual rotation:     (%.2f, %.2f, %.2f). Matrix: \n", roll, pitch, yaw);
    cout << Rvisual << endl;

    cout << "\nRotation Visual-IMU: \n" << Rvisual - Rimu << endl;
    cout << "\nRotation Visual-Pred: \n" << Rvisual - Rpred << endl;


    // Check if rotations have determinant 1 and rank 3 
    int rank, det;
    // Prediction
    FullPivLU<Matrix3d> lu_decomp_p(Rpred);
    rank = lu_decomp_p.rank();
    det = Rpred.determinant();
    if (rank != 3){LOGD("Prediction rotation rank is not 3. Its %d", rank); }
    if (det != 1) {LOGD("Prediction rotation determinant is not 1. Its %d", det); }

    // IMU
    FullPivLU<Matrix3d> lu_decomp(Rimu);
    rank = lu_decomp.rank();
    det = Rimu.determinant();
    if (rank != 3){LOGD("IMU rotation rank is not 3. Its %d", rank); }
    if (det != 1) {LOGD("IMU rotation determinant is not 1. Its %d", det); }

    // Visual
    FullPivLU<Matrix3d> lu_decomp_v(Rvisual);
    rank = lu_decomp_v.rank();
    det  = Rvisual.determinant();
    if (rank != 3){LOGD("Visual rotation rank is not 3. Its %d", rank); }
    if (det != 1) {LOGD("Visualrotation determinant is not 1. Its %d", det); }


    
    cout << "----------------------" << endl;
  }

  // save poses for create TFs
  pred_ctevel_q = Quaterniond(Rpred);
  pred_mad_q    = Quaterniond(Rimu);
  pred_vision_q = Quaterniond(mCurrentFrame.GetRotation());
  return vision;
}

bool Tracking::TrackWithMotionModel() {
  ORBmatcher matcher(0.9, true);

  // Update last frame pose according to its reference keyframe
  UpdateLastFrame();

  // Predict initial pose with motion model
  Eigen::Matrix4d predicted_pose = motion_model_->Predict(mLastFrame.GetPose());
  mCurrentFrame.SetPose(predicted_pose);

  LOGD("Predicted pose: [%.4f, %.4f, %.4f]", predicted_pose(0, 3), predicted_pose(1, 3), predicted_pose(2, 3));

  // Align current and last image
  if (align_image_) {
    ImageAlign image_align;
    if (!image_align.ComputePose(mCurrentFrame, mLastFrame)) {
      LOGE("Image align failed");
      mCurrentFrame.SetPose(predicted_pose);
    }
  }

  // Project points seen in previous frame
  fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint*>(NULL));
  int nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, threshold_, mSensor!=System::RGBD);

  // If few matches, ignores alignment and uses a wider window search
  if (nmatches<20) {
    LOGD("Not enough matches [%d], double threshold", nmatches);
    mCurrentFrame.SetPose(predicted_pose);
    fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint*>(NULL));
    nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, 2*threshold_, mSensor!=System::RGBD);
  }

  if (nmatches<20) {
    LOGD("Not enough matches [%d], tracking failed", nmatches);
    return false;
  }

  // Optimize frame pose with all matches
  Optimizer::PoseOptimization(&mCurrentFrame);

  // Discard outliers
  int nmatchesMap = 0;
  for (int i  = 0; i<mCurrentFrame.N; i++) {
    if (mCurrentFrame.mvpMapPoints[i]) {
      if (mCurrentFrame.mvbOutlier[i]) {
        MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];

        mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint*>(NULL);
        mCurrentFrame.mvbOutlier[i]=false;
        pMP->mbTrackInView = false;
        pMP->mnLastFrameSeen = mCurrentFrame.mnId;
        nmatches--;
      } else if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
        nmatchesMap++;
    }
  }

  if (nmatchesMap<10) {
    LOGD("Not enough inliers [%d], tracking failed", nmatchesMap);
    return false;
  }

  return true;
}

bool Tracking::TrackLocalMap() {
  // We have an estimation of the camera pose and some map points tracked in the frame.
  // We retrieve the local map and try to find matches to points in the local map.

  UpdateLocalMap();

  SearchLocalPoints();

  // Optimize Pose
  Optimizer::PoseOptimization(&mCurrentFrame);
  mnMatchesInliers = 0;

  // Update MapPoints Statistics
  for (int i = 0; i<mCurrentFrame.N; i++) {
    if (mCurrentFrame.mvpMapPoints[i]) {
      if (!mCurrentFrame.mvbOutlier[i]) {
        mCurrentFrame.mvpMapPoints[i]->IncreaseFound();
        if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
          mnMatchesInliers++;
      }
    }
  }

  inliers_on_localmap = mnMatchesInliers;
  // Decide if the tracking was succesful
  if (mnMatchesInliers<15) {
    LOGD("Not enough points tracked [%d], tracking failed", mnMatchesInliers);
    return false;
  }

  return true;
}


bool Tracking::NeedNewKeyFrame_test() {
  // If Local Mapping is freezed by a Loop Closure do not insert keyframes
  if (mpLocalMapper->isStopped() || mpLocalMapper->stopRequested())
    return false;

  const int nKFs = mpMap->KeyFramesInMap();

  bool temp = mCurrentFrame.mnId<mnLastRelocFrameId+(mMaxFrames / 2);
  LOGD("KeyFrame creation conditions")
  LOGD("\t1) Han pasado suficientes KF desde la ultima reloc? %s ", //([%lu] > [%d]?)", 
       temp ? "NO" : "SI");
       //mCurrentFrame.mnId, mnLastRelocFrameId+mMaxFrames);

  // Do not insert keyframes if not enough frames have passed from last relocalisation
  if (temp && nKFs>mMaxFrames){
    return false;
  }
  // Tracked MapPoints in the reference keyframe
  int nMinObs = 3;
  if (nKFs<=2)
    nMinObs=2;
  if (nKFs==1 && usePattern)
    nMinObs=1;
  int nRefMatches = mpReferenceKF->TrackedMapPoints(nMinObs);

  // Local Mapping accept keyframes?
  bool bLocalMappingIdle = mpLocalMapper->AcceptKeyFrames();

  // Check how many "close" points are being tracked and how many could be potentially created.
  int nNonTrackedClose = 0;
  int nTrackedClose= 0;
  if (mSensor==System::RGBD) {
    for (int i  = 0; i<mCurrentFrame.N; i++) {
      if (mCurrentFrame.mvDepth[i] > 0 && mCurrentFrame.mvDepth[i]<mThDepth) {
        if (mCurrentFrame.mvpMapPoints[i] && !mCurrentFrame.mvbOutlier[i])
          nTrackedClose++;
        else
          nNonTrackedClose++;
      }
    }
  }

  bool bNeedToInsertClose = (nTrackedClose<100) && (nNonTrackedClose>70);

  // Thresholds
  float thRefRatio = 0.75f;
  if (nKFs<2)
    thRefRatio = 0.4f;

  if (mSensor!=System::RGBD)
    thRefRatio = 0.9f;

  // Condition 1a: More than "MaxFrames" have passed from last keyframe insertion
  const bool c1a = mCurrentFrame.mnId >= mnLastKeyFrameId+mMaxFrames;
  // Condition 1b: More than "MinFrames" have passed and Local Mapping is idle
  const bool c1b = (mCurrentFrame.mnId >= mnLastKeyFrameId+mMinFrames && bLocalMappingIdle);
  //Condition 1c: tracking is weak
  const bool c1c =  mSensor==System::RGBD && (mnMatchesInliers<nRefMatches*0.25 || bNeedToInsertClose) ;
  //Condition 1d: stay in curve
  const bool c1d =  stay_in_curve && bLocalMappingIdle ;
  // Condition 2: Few tracked points compared to reference keyframe. Lots of visual odometry compared to map matches.
  const bool c2 = ((mnMatchesInliers<nRefMatches*thRefRatio|| bNeedToInsertClose) && mnMatchesInliers>15);

  bool cond_1 = (c1a || c1b || c1c || c1d);
  bool cond = cond_1 && c2;

  LOGD("\t2a)");
  LOGD("\t\t1) Han pasado el num MIN desde la ultima insercion? %s", mCurrentFrame.mnId >= mnLastKeyFrameId+mMinFrames ? "SI" : "NO");
  LOGD("\t\t2) El mapa local está en IDLE? %s", bLocalMappingIdle ? "SI" : "NO");
  LOGD("\t\t3) Han pasado el num MAX desde la ultima insercion? %s", c1a ? "SI" : "NO");
  LOGD("\t\t4) Estoy en una curva? %s", c1d ? "SI" : "NO");
  LOGD("\t\t** Resolución cond 2a ((1 AND 2) OR 3 OR (4 AND 2)) ->: [%s]", cond_1 ? "TRUE" : "FALSE");
  LOGD("\t2b)");
  LOGD("\t\t1) Num inliers [%d] es MAYOR a [15]? %s", mnMatchesInliers, mnMatchesInliers>15 ? "SI" : "NO");
  LOGD("\t\t2) Num inliers [%d] es MENOR que el %.2f de los puntos de ref [%f]? %s", 
       mnMatchesInliers, thRefRatio*100, nRefMatches*thRefRatio, mnMatchesInliers<nRefMatches*thRefRatio ? "SI" : "NO");
  LOGD("\t\t** Resolución cond 2b (1 AND 2) ->: [%s]", c2 ? "TRUE" : "FALSE");

  LOGD("** Necesario crear KF? [%s]\n",  cond_1 && c2 ? "SI" : "NO");

  if (c2) {
    // If the mapping accepts keyframes, insert keyframe.
    // Otherwise send a signal to interrupt BA
    if (bLocalMappingIdle) {
      return true;
    } else {
      mpLocalMapper->InterruptBA();

      if (mSensor==System::RGBD) {
        if (mpLocalMapper->KeyframesInQueue()<3)
          return true;
        else
          return false;
      } else
        return false;
    }
  } else
    return false;
}

bool Tracking::NeedNewKeyFrame() {
  // If Local Mapping is freezed by a Loop Closure do not insert keyframes
  if (mpLocalMapper->isStopped() || mpLocalMapper->stopRequested())
    return false;

  const int nKFs = mpMap->KeyFramesInMap();

  // Do not insert keyframes if not enough frames have passed from last relocalisation
  if (mCurrentFrame.mnId<mnLastRelocFrameId+mMaxFrames && nKFs>mMaxFrames)
    return false;

  // Tracked MapPoints in the reference keyframe
  int nMinObs = 3;
  if (nKFs<=2)
    nMinObs=2;
  if (nKFs==1 && usePattern)
    nMinObs=1;
  int nRefMatches = mpReferenceKF->TrackedMapPoints(nMinObs);

  // Local Mapping accept keyframes?
  bool bLocalMappingIdle = mpLocalMapper->AcceptKeyFrames();

  // Check how many "close" points are being tracked and how many could be potentially created.
  int nNonTrackedClose = 0;
  int nTrackedClose= 0;
  if (mSensor==System::RGBD) {
    for (int i  = 0; i<mCurrentFrame.N; i++) {
      if (mCurrentFrame.mvDepth[i] > 0 && mCurrentFrame.mvDepth[i]<mThDepth) {
        if (mCurrentFrame.mvpMapPoints[i] && !mCurrentFrame.mvbOutlier[i])
          nTrackedClose++;
        else
          nNonTrackedClose++;
      }
    }
  }

  bool bNeedToInsertClose = (nTrackedClose<100) && (nNonTrackedClose>70);

  // Thresholds
  float thRefRatio = 0.75f;
  if (nKFs<2)
    thRefRatio = 0.4f;

  if (mSensor!=System::RGBD)
    thRefRatio = 0.9f;

  // Condition 1a: More than "MaxFrames" have passed from last keyframe insertion
  const bool c1a = mCurrentFrame.mnId >= mnLastKeyFrameId+mMaxFrames;
  // Condition 1b: More than "MinFrames" have passed and Local Mapping is idle
  const bool c1b = (mCurrentFrame.mnId >= mnLastKeyFrameId+mMinFrames && bLocalMappingIdle);
  //Condition 1c: tracking is weak
  const bool c1c =  mSensor==System::RGBD && (mnMatchesInliers<nRefMatches*0.25 || bNeedToInsertClose) ;
  // Condition 2: Few tracked points compared to reference keyframe. Lots of visual odometry compared to map matches.
  const bool c2 = ((mnMatchesInliers<nRefMatches*thRefRatio|| bNeedToInsertClose) && mnMatchesInliers>15);

  if ((c1a||c1b||c1c)&&c2) {
    // If the mapping accepts keyframes, insert keyframe.
    // Otherwise send a signal to interrupt BA
    if (bLocalMappingIdle) {
      return true;
    } else {
      mpLocalMapper->InterruptBA();

      if (mSensor==System::RGBD) {
        if (mpLocalMapper->KeyframesInQueue()<3)
          return true;
        else
          return false;
      } else
        return false;
    }
  } else
    return false;
}

void Tracking::CreateNewKeyFrame() {
  if (!mpLocalMapper->SetNotStop(true))
    return;

  KeyFrame* pKF = new KeyFrame(mCurrentFrame, mpMap);

  mpReferenceKF = pKF;
  mCurrentFrame.mpReferenceKF = pKF;

  /*
  if (mSensor==System::MONOCULAR_IMU_NEW) {
    madgwick_.set_orientation_from_frame(mCurrentFrame.GetPose()); 
  }
  */
  if (mSensor==System::RGBD) {
    mCurrentFrame.UpdatePoseMatrices();

    // We sort points by the measured depth by the stereo/RGBD sensor.
    // We create all those MapPoints whose depth < mThDepth.
    // If there are less than 100 close points we create the 100 closest.
    vector<pair<float, int> > vDepthIdx;
    vDepthIdx.reserve(mCurrentFrame.N);
    for (int i = 0; i<mCurrentFrame.N; i++) {
      float z = mCurrentFrame.mvDepth[i];
      if (z > 0) {
        vDepthIdx.push_back(make_pair(z, i));
      }
    }

    if (!vDepthIdx.empty()) {
      sort(vDepthIdx.begin(), vDepthIdx.end());

      int nPoints = 0;
      for (size_t j = 0; j < vDepthIdx.size(); j++) {
        int i = vDepthIdx[j].second;

        bool bCreateNew = false;

        MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];
        if (!pMP)
          bCreateNew = true;
        else if (pMP->Observations()<1) {
          bCreateNew = true;
          mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint*>(NULL);
        }

        if (bCreateNew) {
          Eigen::Vector3d x3D = mCurrentFrame.UnprojectStereo(i);
          MapPoint* pNewMP = new MapPoint(x3D, pKF, mpMap);
          pNewMP->AddObservation(pKF, i);
          pKF->AddMapPoint(pNewMP, i);
          pNewMP->ComputeDistinctiveDescriptors();
          pNewMP->UpdateNormalAndDepth();
          mpMap->AddMapPoint(pNewMP);

          mCurrentFrame.mvpMapPoints[i]=pNewMP;
          nPoints++;
        } else {
          nPoints++;
        }

        if (vDepthIdx[j].first>mThDepth && nPoints>100)
          break;
      }
    }
  }

  mpLocalMapper->InsertKeyFrame(pKF);

  mpLocalMapper->SetNotStop(false);

  mnLastKeyFrameId = mCurrentFrame.mnId;
  mpLastKeyFrame = pKF;
}

void Tracking::SearchLocalPoints() {
  // Do not search map points already matched
  for (vector<MapPoint*>::iterator vit = mCurrentFrame.mvpMapPoints.begin(), vend = mCurrentFrame.mvpMapPoints.end(); vit!=vend; vit++) {
    MapPoint* pMP = *vit;
    if (pMP) {
      if (pMP->isBad()) {
        *vit = static_cast<MapPoint*>(NULL);
      } else {
        pMP->IncreaseVisible();
        pMP->mnLastFrameSeen = mCurrentFrame.mnId;
        pMP->mbTrackInView = false;
      }
    }
  }

  int nToMatch = 0;

  // Project points in frame and check its visibility
  for (vector<MapPoint*>::iterator vit = mvpLocalMapPoints.begin(), vend = mvpLocalMapPoints.end(); vit!=vend; vit++) {
    MapPoint* pMP = *vit;
    if (pMP->mnLastFrameSeen == mCurrentFrame.mnId)
      continue;
    if (pMP->isBad())
      continue;
    // Project (this fills MapPoint variables for matching)
    if (mCurrentFrame.isInFrustum(pMP, 0.5)) {
      pMP->IncreaseVisible();
      nToMatch++;
    }
  }

  if (nToMatch > 0) {
    ORBmatcher matcher(0.8);
    int th = 1;
    if (mSensor==System::RGBD)
      th=3;
    // If the camera has been relocalised recently, perform a coarser search
    if (mCurrentFrame.mnId<mnLastRelocFrameId+2)
      th=5;
    matcher.SearchByProjection(mCurrentFrame, mvpLocalMapPoints, th);
  }
}

void Tracking::UpdateLocalMap() {
  // This is for visualization
  mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

  // Update
  UpdateLocalKeyFrames();
  UpdateLocalPoints();
}

void Tracking::UpdateLocalPoints() {
  mvpLocalMapPoints.clear();

  for (vector<KeyFrame*>::const_iterator itKF = mvpLocalKeyFrames.begin(), itEndKF = mvpLocalKeyFrames.end(); itKF!=itEndKF; itKF++) {
    KeyFrame* pKF = *itKF;
    const vector<MapPoint*> vpMPs = pKF->GetMapPointMatches();

    for (vector<MapPoint*>::const_iterator itMP=vpMPs.begin(), itEndMP=vpMPs.end(); itMP!=itEndMP; itMP++) {
      MapPoint* pMP = *itMP;
      if (!pMP)
        continue;
      if (pMP->mnTrackReferenceForFrame == mCurrentFrame.mnId)
        continue;
      if (!pMP->isBad()) {
        mvpLocalMapPoints.push_back(pMP);
        pMP->mnTrackReferenceForFrame = mCurrentFrame.mnId;
      }
    }
  }
}


void Tracking::UpdateLocalKeyFrames() {

  // Each map point vote for the keyframes in which it has been observed
  map<KeyFrame*, int> keyframeCounter;
  for (int i = 0; i<mCurrentFrame.N; i++) {
    if (mCurrentFrame.mvpMapPoints[i]) {
      MapPoint* pMP = mCurrentFrame.mvpMapPoints[i];
      if (!pMP->isBad()) {
        const map<KeyFrame*, size_t> observations = pMP->GetObservations();
        for (map<KeyFrame*, size_t>::const_iterator it=observations.begin(), itend=observations.end(); it!=itend; it++)
          keyframeCounter[it->first]++;
      } else {
        mCurrentFrame.mvpMapPoints[i]=NULL;
      }
    }
  }

  if (keyframeCounter.empty())
    return;

  int max = 0;
  KeyFrame* pKFmax= static_cast<KeyFrame*>(NULL);

  mvpLocalKeyFrames.clear();
  mvpLocalKeyFrames.reserve(3*keyframeCounter.size());

  // All keyframes that observe a map point are included in the local map. Also check which keyframe shares most points
  for (map<KeyFrame*, int>::const_iterator it=keyframeCounter.begin(), itEnd=keyframeCounter.end(); it!=itEnd; it++) {
    KeyFrame* pKF = it->first;

    if (pKF->isBad())
      continue;

    if (it->second>max) {
      max=it->second;
      pKFmax=pKF;
    }

    mvpLocalKeyFrames.push_back(it->first);
    pKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;
  }

  // Include also some not-already-included keyframes that are neighbors to already-included keyframes
  for (vector<KeyFrame*>::const_iterator itKF = mvpLocalKeyFrames.begin(), itEndKF = mvpLocalKeyFrames.end(); itKF!=itEndKF; itKF++) {
    // Limit the number of keyframes
    if (mvpLocalKeyFrames.size()>80)
      break;

    KeyFrame* pKF = *itKF;

    const vector<KeyFrame*> vNeighs = pKF->GetBestCovisibilityKeyFrames(10);

    for (vector<KeyFrame*>::const_iterator itNeighKF=vNeighs.begin(), itEndNeighKF=vNeighs.end(); itNeighKF!=itEndNeighKF; itNeighKF++) {
      KeyFrame* pNeighKF = *itNeighKF;
      if (!pNeighKF->isBad()) {
        if (pNeighKF->mnTrackReferenceForFrame != mCurrentFrame.mnId) {
          mvpLocalKeyFrames.push_back(pNeighKF);
          pNeighKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;
          break;
        }
      }
    }

    const set<KeyFrame*> spChilds = pKF->GetChilds();
    for (set<KeyFrame*>::const_iterator sit = spChilds.begin(), send = spChilds.end(); sit != send; sit++) {
      KeyFrame* pChildKF = *sit;
      if (!pChildKF->isBad()) {
        if (pChildKF->mnTrackReferenceForFrame != mCurrentFrame.mnId) {
          mvpLocalKeyFrames.push_back(pChildKF);
          pChildKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;
          break;
        }
      }
    }

    KeyFrame* pParent = pKF->GetParent();
    if (pParent) {
      if (pParent->mnTrackReferenceForFrame != mCurrentFrame.mnId) {
        mvpLocalKeyFrames.push_back(pParent);
        pParent->mnTrackReferenceForFrame = mCurrentFrame.mnId;
        break;
      }
    }

  }

  if (pKFmax) {
    mpReferenceKF = pKFmax;
    mCurrentFrame.mpReferenceKF = mpReferenceKF;
  }
}

bool Tracking::Relocalization() {
  ORBmatcher matcher(0.75, true);
  int nmatches, nGood;

  // Compare to all keyframes starting from the last one
  vector<KeyFrame*> kfs = mpMap->GetAllKeyFrames();
  for (auto it=kfs.rbegin(); it != kfs.rend(); it++) {
    KeyFrame* kf = *it;

    mCurrentFrame.SetPose(kf->GetPose());

    // Try to align current frame and candidate keyframe
    ImageAlign image_align;
    if (!image_align.ComputePose(mCurrentFrame, kf, true))
      continue;

    fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint*>(NULL));

    // Project points seen in previous frame
    nmatches = matcher.SearchByProjection(mCurrentFrame, kf, threshold_, mSensor!=System::RGBD);
    if (nmatches < 20)
      continue;

    // Optimize frame pose with all matches
    nGood = Optimizer::PoseOptimization(&mCurrentFrame);
    if (nGood < 10)
      continue;

    mnLastRelocFrameId = mCurrentFrame.mnId;
    return true;
  }

  return false;
}

void Tracking::Reset() {

  LOGD("System Reseting");

  // Reset Local Mapping
  LOGD("Reseting Local Mapper...");
  mpLocalMapper->RequestReset();

  // Reset Loop Closing
  if (mpLoopClosing) {
    LOGD("Reseting Loop Closing...");
    mpLoopClosing->RequestReset();
  }

  // Clear Map (this erase MapPoints and KeyFrames)
  mpMap->clear();

  KeyFrame::nNextId = 0;
  Frame::nNextId = 0;
  mState = NO_IMAGES_YET;

  if (mpInitializer) {
    delete mpInitializer;
    mpInitializer = static_cast<Initializer*>(NULL);
  }

  lastRelativePose_.setZero();
  motion_model_->Restart();
}

void Tracking::InformOnlyTracking(const bool &flag) {
  mbOnlyTracking = flag;
}

void Tracking::PatternCellSize(double w, double h){
	mpPatternDetector.SetCellSizeW(w);
	mpPatternDetector.SetCellSizeH(h);
}

}  // namespace SD_SLAM
