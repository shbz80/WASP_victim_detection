/**
 * @file april_tags.cpp
 * @brief Example application for April tags library
 * @author: Shahbaz Khader
 * Adopted from Michael Kaess original code for april_tag detection.
 * Inits April tag detection
 * Subscribes to image feed
 * Displays marker in the image window on detection
 * Publishes detected tag positions in map frame
 * PUblishes marker message to rViz client
 * Implement reset detection service
 */

using namespace std;
#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <opencv2/highgui/highgui.hpp>
#include <cv_bridge/cv_bridge.h>
#include <iostream>
//#include <string>
#include <cstring>
#include <vector>
#include <list>
#include <sys/time.h>
#include "wasp_custom_msgs/object_loc.h"
#include <geometry_msgs/PointStamped.h>
#include <geometry_msgs/PoseStamped.h>
#include <tf/transform_listener.h>
#include <visualization_msgs/Marker.h>

// OpenCV library for easy access to USB camera and drawing of images
// on screen
#include "opencv2/opencv.hpp"

// April tags detector and various families that can be selected by command line option
#include "AprilTags/TagDetector.h"
#include "AprilTags/Tag16h5.h"
#include "AprilTags/Tag25h7.h"
#include "AprilTags/Tag25h9.h"
#include "AprilTags/Tag36h9.h"
#include "AprilTags/Tag36h11.h"
#include "task3_opencv/ResetDetection.h"

#define NUM_VICTIMS 5

// For Arduino: locally defined serial port access class

const char* windowName = "apriltags_demo";

cv_bridge::CvImagePtr cv_ptr;
cv::Mat image_new;
cv::Mat image_gray;

ros::Publisher object_location_pub;
ros::Publisher marker_pub;
tf::TransformListener* plistener=NULL;
visualization_msgs::Marker* pvicmarker=NULL;
int tag_flag = 0;
// utility function to provide current system time (used below in
// determining frame rate at which images are being processed)
double tic() {
  struct timeval t;
  gettimeofday(&t, NULL);
  return ((double)t.tv_sec + ((double)t.tv_usec)/1000000.);
}


#include <cmath>

#ifndef PI
const double PI = 3.14159265358979323846;
#endif
const double TWOPI = 2.0*PI;

/**
 * Normalize angle to be within the interval [-pi,pi].
 */
inline double standardRad(double t) {
  if (t >= 0.) {
    t = fmod(t+PI, TWOPI) - PI;
  } else {
    t = fmod(t-PI, -TWOPI) + PI;
  }
  return t;
}

/**
 * Convert rotation matrix to Euler angles
 */
void wRo_to_euler(const Eigen::Matrix3d& wRo, double& yaw, double& pitch, double& roll) {
    yaw = standardRad(atan2(wRo(1,0), wRo(0,0)));
    double c = cos(yaw);
    double s = sin(yaw);
    pitch = standardRad(atan2(-wRo(2,0), wRo(0,0)*c + wRo(1,0)*s));
    roll  = standardRad(atan2(wRo(0,2)*s - wRo(1,2)*c, -wRo(0,1)*s + wRo(1,1)*c));
}

//global variables to keep track of up to MAX_VICTIM number of tags
int detectedTags[NUM_VICTIMS]; 

bool updateDetected(int id){
	for (int i=0;i<NUM_VICTIMS;++i){
		if(detectedTags[i]==id) return false;
	}
	for (int i=0;i<NUM_VICTIMS;++i){
		if(detectedTags[i]==-1) {
		  detectedTags[i]=id;		
		  return true;
		}
	}
	return false;
 }
void resetTags(void){
	for (int i = 0; i < NUM_VICTIMS; ++i){
	  detectedTags[i] = -1;
	  }
}
class Demo {

  AprilTags::TagDetector* m_tagDetector;
  AprilTags::TagCodes m_tagCodes;

  bool m_draw; // draw image and April tag detections?
  bool m_arduino; // send tag detections to serial port?
  bool m_timing; // print timing information for each tag extraction call

  int m_width; // image size in pixels
  int m_height;
  double m_tagSize; // April tag side length in meters of square black frame
  double m_fx; // camera focal length in pixels
  double m_fy;
  double m_px; // camera principal point
  double m_py;

  int m_deviceId; // camera id (in case of multiple cameras)

  list<string> m_imgNames;

  cv::VideoCapture m_cap;

  int m_exposure;
  int m_gain;
  int m_brightness;

  //Serial m_serial;

public:

  // default constructor
  Demo() :
    // default settings, most can be modified through command line options (see below)
    m_tagDetector(NULL),
    m_tagCodes(AprilTags::tagCodes36h11),

    m_draw(true),
    m_arduino(false),
    m_timing(false),

    m_width(640),
    m_height(360),
    m_tagSize(0.099),
    m_fx(623.709),
    m_fy(582.226),
    m_px(m_width/2),
    m_py(m_height/2),

    m_exposure(-1),
    m_gain(-1),
    m_brightness(-1),

    m_deviceId(0)
  {}

  // changing the tag family
  void setTagCodes(string s) {
    if (s=="16h5") {
      m_tagCodes = AprilTags::tagCodes16h5;
    } else if (s=="25h7") {
      m_tagCodes = AprilTags::tagCodes25h7;
    } else if (s=="25h9") {
      m_tagCodes = AprilTags::tagCodes25h9;
    } else if (s=="36h9") {
      m_tagCodes = AprilTags::tagCodes36h9;
    } else if (s=="36h11") {
      m_tagCodes = AprilTags::tagCodes36h11;
    } else {
      cout << "Invalid tag family specified" << endl;
      exit(1);
    }
  }


  void setup() {
    m_tagDetector = new AprilTags::TagDetector(m_tagCodes);

    // prepare window for drawing the camera images
    if (m_draw) {
      cv::namedWindow(windowName, 1);
    }

  }

  

  void print_detection(AprilTags::TagDetection& detection) const {
    cout << "  Id: " << detection.id
         << " (Hamming: " << detection.hammingDistance << ")";
    
    // recovering the relative pose of a tag:

    // NOTE: for this to be accurate, it is necessary to use the
    // actual camera parameters here as well as the actual tag size
    // (m_fx, m_fy, m_px, m_py, m_tagSize)

    Eigen::Vector3d translation;
    Eigen::Matrix3d rotation;
    detection.getRelativeTranslationRotation(m_tagSize, m_fx, m_fy, m_px, m_py,
                                             translation, rotation);

    Eigen::Matrix3d F;
    F <<
      1, 0,  0,
      0,  -1,  0,
      0,  0,  1;
    Eigen::Matrix3d fixed_rot = F*rotation;
    double yaw, pitch, roll;
    wRo_to_euler(fixed_rot, yaw, pitch, roll);
    

  /*
    
    cout << "  distance=" << translation.norm()
         << "m, x=" << translation(0)
         << ", y=" << translation(1)
         << ", z=" << translation(2)
         << ", yaw=" << yaw
         << ", pitch=" << pitch
         << ", roll=" << roll
         << endl;
	*/
    // Also note that for SLAM/multi-view application it is better to
    // use reprojection error of corner points, because the noise in
    // this relative pose is very non-Gaussian; see iSAM source code
    // for suitable factors.

    geometry_msgs::PointStamped tag_point;
   // tag_point.header.frame_id = "camera_rgb_optical_frame";
    tag_point.header.frame_id = "base_footprint";
    tag_point.header.stamp = ros::Time();
    tag_point.point.x = translation(0);
    tag_point.point.y = translation(1);
    tag_point.point.z = translation(2);
    geometry_msgs::PointStamped victim_point;
    try{
	    
	    plistener->transformPoint("map", tag_point, victim_point);

	    ROS_INFO("camera_optical_frame: (%.2f, %.2f. %.2f) -----> map: (%.2f, %.2f, %.2f) at time %.2f",
		tag_point.point.x, tag_point.point.y, tag_point.point.z,
		victim_point.point.x, victim_point.point.y, victim_point.point.z, victim_point.header.stamp.toSec());
       }
	  catch(tf::TransformException& ex){
	    ROS_ERROR("Received an exception trying to transform a point from \"base_footprint\" to \"map\": %s", ex.what());
	  }
    //Message to publish the APril tag ID's collected
    //double f = 23.43;
    //std::string sss = std::to_string(f);
    //std::string id_string = std::to_string(detection.id);
    //std::to_string(10);
    //char id_string[10];
    //id_string = itoa(detection.id);
	
	string id_string;//string which will contain the result

	stringstream convert; // stringstream used for the conversion

	convert << detection.id;//add the value of Number to the characters in the stream

	id_string = convert.str();//set Result to the content of the stream
    geometry_msgs::PoseStamped location;
    location.header.frame_id = id_string;
    location.pose.position.x = victim_point.point.x;
    location.pose.position.y = victim_point.point.y;
    location.pose.position.z = victim_point.point.z;
    object_location_pub.publish(location);

   //marker publish
    pvicmarker->header.frame_id = "/map";
    pvicmarker->header.stamp = ros::Time::now();

    // Set the namespace and id for this marker.  This serves to create a unique ID
    // Any marker sent with the same namespace and id will overwrite the old one
    pvicmarker->ns = "basic_shapes";
    pvicmarker->id = detection.id;

    // Set the marker type.  Initially this is CUBE, and cycles between that and SPHERE, ARROW, and CYLINDER
    pvicmarker->type = visualization_msgs::Marker::CYLINDER;

    // Set the marker action.  Options are ADD, DELETE, and new in ROS Indigo: 3 (DELETEALL)
    pvicmarker->action = visualization_msgs::Marker::ADD;

    // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
    pvicmarker->pose.position.x = location.pose.position.x;
    pvicmarker->pose.position.y = location.pose.position.y;
    pvicmarker->pose.position.z = 0;
    pvicmarker->pose.orientation.x = 0.0;
    pvicmarker->pose.orientation.y = 0.0;
    pvicmarker->pose.orientation.z = 0.0;
    pvicmarker->pose.orientation.w = 1.0;

    // Set the scale of the marker -- 1x1x1 here means 1m on a side
    pvicmarker->scale.x = 0.2;
    pvicmarker->scale.y = 0.2;
    pvicmarker->scale.z = 0.2;

    // Set the color -- be sure to set alpha to something non-zero!
    pvicmarker->color.r = 0.0f;
    pvicmarker->color.g = 1.0f;
    pvicmarker->color.b = 0.0f;
    pvicmarker->color.a = 1.0;

    pvicmarker->lifetime = ros::Duration();
    marker_pub.publish(*pvicmarker);
  }
   


	
  void processImage(cv::Mat& image, cv::Mat& image_gray) {
    // alternative way is to grab, then retrieve; allows for
    // multiple grab when processing below frame rate - v4l keeps a
    // number of frames buffered, which can lead to significant lag
    //      m_cap.grab();
    //      m_cap.retrieve(image);

    // detect April tags (requires a gray scale image)

    cv::cvtColor(image, image_gray, CV_BGR2GRAY);

    double t0;
    if (m_timing) {
      t0 = tic();
    }
    vector<AprilTags::TagDetection> detections = m_tagDetector->extractTags(image_gray);

    if (m_timing) {
      double dt = tic()-t0;
      cout << "Extracting tags took " << dt << " seconds." << endl;
    }

    // print out each detection
    //cout << detections.size() << " tags detected:" << endl;
    
    /*
    for (int i=0; i<detections.size(); i++) {
      print_detection(detections[i]);
    }*/
    
 // print april detection results
    for (int i=0; i<detections.size(); i++) {
    	int id = detections[i].id;   
    	if(updateDetected(id)) {
	  print_detection(detections[i]);
	  ROS_INFO("New victim detected, id: %d", id);
	}
	else {
	  ROS_INFO("No new victim detected, id: %d", id);
	}
    }
    
    // show the current image including any detections
    if (m_draw) {
      for (int i=0; i<detections.size(); i++) {
        // also highlight in the image
        detections[i].draw(image);
      }
      imshow(windowName, image); // OpenCV call
      cv::waitKey(1);
    }

  }
}; // Demo

/*Create a global object so that the image callback can access its functions*/
Demo demo;

//Call Back function for camera subsciber
void imageCallback(const sensor_msgs::ImageConstPtr& msg){
cv_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
  //cv::imshow("view", cv_ptr->image);
  //cv::waitKey(1);
  cv::cvtColor(cv_ptr->image, image_gray, CV_BGR2GRAY);
  demo.processImage(cv_ptr->image, image_gray);
}


// reset detection service routine
bool reset(task3_opencv::ResetDetection::Request  &req,
         task3_opencv::ResetDetection::Response &res)
{
  ROS_INFO("Reset request received: %d", (int)req.resetFlag);
  if (req.resetFlag==1){
	  resetTags();
	  res.resetDone = 1;
	  ROS_INFO("Reset confirmed: %d", (int)res.resetDone);
  }	
  
  return true;
}

// here is were everything begins
int main(int argc, char* argv[]) {
  ros::init(argc, argv, "Tag_Detector");
  ros::NodeHandle nh;
  //init tf listener object
  plistener = new (tf::TransformListener);
  // init marker object
  pvicmarker = new (visualization_msgs::Marker);
  image_transport::ImageTransport it(nh);
  cv::startWindowThread();
  resetTags();
  demo.setup();
  cout << "Initial setup executed"<<endl;
  // image_transport::Subscriber sub = it.subscribe("/ardrone/image_raw", 1, imageCallback);
  // subscribe to the image feed
  image_transport::Subscriber sub = it.subscribe("/camera/rgb/image_rawe", 1, imageCallback);
  // advertise exploration result message
  object_location_pub = nh.advertise<geometry_msgs::PoseStamped>("WASP_planner/Explore_result", 1);
  // advertise the marker message
  marker_pub = nh.advertise<visualization_msgs::Marker>("visualization_marker", 1);
  cout << "Image Subscriber executed"<<endl;
  // advertise the reset detection service
  ros::ServiceServer service = nh.advertiseService("reset_tag_detection", reset);
  ROS_INFO("Ready to detect tags");
  ros::spin();

  return 0;
}
