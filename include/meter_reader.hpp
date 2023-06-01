#ifndef _METER_READER_HPP_
#define _METER_READER_HPP_

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

#include "config.hpp"
#include "detect.hpp"

class meterReader
{
    private:
        cv::Mat image; // current frame of the camera
        std::vector<DetObject> det_objs; // detected objects
        Detect detect; // detector

    public:
        meterReader(std::string const trt_model_det, std::string const trt_model_seg);
        ~meterReader();

        void read(std::vector<frameInfo> &frames);
        void crop_meters(std::vector<frameInfo> &frames);
        void read_meter(); // input this->meters, output this->readings
        void draw_boxes(std::vector<frameInfo> &images); // draw the bounding box of the detected objects
};

#endif