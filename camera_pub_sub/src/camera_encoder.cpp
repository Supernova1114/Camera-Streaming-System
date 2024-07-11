#include <sstream>  
#include <string.h>
#include <string>
#include <vector>
#include <thread>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/header.hpp"
#include "std_srvs/srv/set_bool.hpp"
#include "std_srvs/srv/trigger.hpp"
#include "image_transport/image_transport.hpp"

#include "cv_bridge/cv_bridge.h"
#include "opencv2/core/mat.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/videoio.hpp"

#include "custom_interfaces/srv/set_resolution.hpp"
#include "custom_interfaces/srv/set_frame_rate.hpp"
#include "usb_device.h"


rclcpp::Node::SharedPtr node;

std::ostringstream gstreamer_api;
cv::VideoCapture videoCapture;

std::string camera_name;
std::string device_path;
std::string compression_format;
std::string hostMachine;

int imageSendWidth;
int imageSendHeight;
int imageSendFPS;

int cameraCapWidth;
int cameraCapHeight;
int cameraCapFPS;


void build_gstreamer_api()
{
    // GStreamer pipeline for capturing from the camera, used by OpenCV
    if (hostMachine == "jetson")
    {
        gstreamer_api << "v4l2src device=" << device_path << " io-mode=2"<< " ! "
        << "image/jpeg,width=" << cameraCapWidth << ","
        << "height=" << cameraCapHeight << ","
        << "framerate=" << cameraCapFPS << "/1 ! "
        << "jpegdec" << " ! "
       	<< "video/x-raw ! appsink";
    }
    else if (hostMachine == "amd64")
    {
        gstreamer_api << "v4l2src device=" << device_path << " ! "
        << "image/jpeg,width=" << cameraCapWidth << ","
        << "height=" << cameraCapHeight << ","
        << "framerate=" << cameraCapFPS << "/1,"
        << "format=(string)" << compression_format << " ! "
        << "decodebin ! appsink";
    }

}

bool set_resolution(int width, int height)
{
    if (width <= cameraCapWidth && height <= cameraCapHeight)
    {
        imageSendWidth = width;
        imageSendHeight = height;

        return true;
    }
    
    return false;
}

bool set_framerate(int fps)
{
    if (fps <= cameraCapFPS)
    {
        imageSendFPS = fps;
        return true;
    }

    return false;
}

bool toggle_camera(bool enableCamera)
{   
    bool success = true;

    if (enableCamera)
    {
        if (!videoCapture.isOpened())
        {
            success = videoCapture.open(gstreamer_api.str(), cv::CAP_GSTREAMER);
        }
    }
    else
    {
        if (videoCapture.isOpened())
        {
            videoCapture.release();

            if (videoCapture.isOpened())
                success = false;
        }
        
    }

    if (!success)
    {
        std::cout << "ERROR! Unable to open camera: {name: "
        << camera_name << "}" << std::endl;
    }

    return success;
}

void toggle_camera_srv_process(const std::shared_ptr<std_srvs::srv::SetBool::Request> request,
          std::shared_ptr<std_srvs::srv::SetBool::Response> response)
{
    response->success = toggle_camera(request->data);
}

void set_resolution_srv_process(const std::shared_ptr<custom_interfaces::srv::SetResolution::Request> request,
          std::shared_ptr<custom_interfaces::srv::SetResolution::Response> response)
{
    response->success = set_resolution(request->width, request->height);
}

void set_framerate_srv_process(const std::shared_ptr<custom_interfaces::srv::SetFrameRate::Request> request,
          std::shared_ptr<custom_interfaces::srv::SetFrameRate::Response> response)
{
    response->success = set_framerate(request->frame_rate);
}

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);

    rclcpp::NodeOptions options;
    node = rclcpp::Node::make_shared("camera_publisher", options);

    node->declare_parameter("camera_name", "camera1");
    node->declare_parameter("serial_ID", "");

    // Must be supported by physical camera device.
    node->declare_parameter("camera_cap_width", 1280);
    node->declare_parameter("camera_cap_height", 720);
    node->declare_parameter("camera_cap_fps", 30);

    // Ability to resize to something custom before sending.
    // These values should be equal to or less than camera_cap parameters.
    node->declare_parameter("image_send_width", 640);
    node->declare_parameter("image_send_height", 480);
    node->declare_parameter("image_send_fps", 5);

    node->declare_parameter("auto_enable_camera", false);
    node->declare_parameter("compression_format", "MJPG");

    // Currently implemented: amd64, jetson
    node->declare_parameter("host_machine", "jetson");

    cameraCapWidth = node->get_parameter("camera_cap_width").as_int();
    cameraCapHeight = node->get_parameter("camera_cap_height").as_int();
    cameraCapFPS = node->get_parameter("camera_cap_fps").as_int();

    imageSendWidth = node->get_parameter("image_send_width").as_int();
    imageSendHeight = node->get_parameter("image_send_height").as_int();
    imageSendFPS = node->get_parameter("image_send_fps").as_int();

    bool autoEnableCamera = node->get_parameter("auto_enable_camera").as_bool();

    std::string serial_ID = node->get_parameter("serial_ID").as_string();
    compression_format = node->get_parameter("compression_format").as_string();
    camera_name = node->get_parameter("camera_name").as_string();

    hostMachine = node->get_parameter("host_machine").as_string();

    std::string base_topic = camera_name + "/transport";
    std::string toggle_srv_name = camera_name + "/toggle_camera";
    // std::string request_image_srv_name = camera_name + "/request_image";
    std::string request_image_pub_name = camera_name + "/request_image_pub";
    std::string set_resolution_srv_name = camera_name + "/set_resolution";
    std::string set_framerate_srv_name = camera_name + "/set_framerate";

    // TODO - Switch to image_transport::CameraPublisher to get access to qos
    image_transport::ImageTransport transport(node);
    image_transport::Publisher publisher = transport.advertise(base_topic, 1);

    auto toggle_camera_srv = 
        node->create_service<std_srvs::srv::SetBool>(toggle_srv_name, &toggle_camera_srv_process);

    auto set_resolution_srv =
        node->create_service<custom_interfaces::srv::SetResolution>(set_resolution_srv_name, &set_resolution_srv_process);

    auto set_framerate_srv =
        node->create_service<custom_interfaces::srv::SetFrameRate>(set_framerate_srv_name, &set_framerate_srv_process);

    device_path = get_device_path(serial_ID);

    if (device_path.empty()) {
        std::cout << "ERROR! Device not found: {name: "
        << camera_name << ", serial_ID: " 
        << serial_ID << "}" << std::endl;
        return 1;
    }
    else
    {   
        std::cout << "Device found: {name: "
        << camera_name << ", device_path: " 
        << device_path << "}" << std::endl;
    }

    build_gstreamer_api();

    if (autoEnableCamera)
    {
        toggle_camera(true);
    }

    cv::Mat frame;
    cv::Mat resizedFrame;
    std_msgs::msg::Header header;
    sensor_msgs::msg::Image::SharedPtr msg;
    
    while (rclcpp::ok()) 
    {
        if (videoCapture.isOpened())
        {
            videoCapture >> frame;

            if (!frame.empty()) 
            {
                cv::resize(frame, resizedFrame, cv::Size(imageSendWidth, imageSendHeight), 0.0, 0.0, cv::INTER_AREA);
                msg = cv_bridge::CvImage(header, "bgr8", resizedFrame).toImageMsg();
                publisher.publish(msg);
            }
        }
        
        rclcpp::spin_some(node);
        std::this_thread::sleep_for(std::chrono::milliseconds((1000 / imageSendFPS)));
    }

    return 0;
}
