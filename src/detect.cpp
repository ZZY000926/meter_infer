#include <opencv2/opencv.hpp>
#include <cuda_runtime_api.h>
#include <NvInfer.h> 
#include <string>
#include <fstream>

#include "glog/logging.h"
#include "config.hpp"
#include "detect.hpp"

using namespace nvinfer1;

class Logger : public ILogger           
{
    void log(Severity severity, const char* msg) noexcept override
    {
        // suppress info-level messages
        if (severity <= Severity::kWARNING)
            // std::cout << msg << std::endl;
            LOG(WARNING) << msg;
    }
} logger;

// Constructor for the Detect class.
// Sets the input size to 640x640.
// Loads the engine from the engine file.
Detect::Detect(std::string const &engine_filename)
{
    // set input size to 640x640
    this->width = IN_WIDTH;
    this->height = IN_HEIGHT;

    // load engine
    this->engine_path = ENGINE_PATH + engine_filename;
    this->runtime = createInferRuntime(logger);

    std::ifstream engine_file(engine_path, std::ios::binary);
    if (!engine_file.good())
    {
        LOG(ERROR) << "Failed to open engine file " << engine_path;
        return;
    }

    // read the enfine file into a buffer
    size_t size = 0;
	engine_file.seekg(0, engine_file.end);  // change the position of the stream to the end
	size = engine_file.tellg();	// get the size of the engine file
	engine_file.seekg(0, engine_file.beg); // change the position of the stream to the beginning
	char *modelStream = new char[size]; // allocate a buffer to store the engine file
	engine_file.read(modelStream, size); // read the engine file into the buffer
	engine_file.close();

    // deserialize the engine file
    this->engine = this->runtime->deserializeCudaEngine(modelStream, size, nullptr);
    if (!this->engine)
    {
        LOG(ERROR) << "Failed to deserialize engine file " << engine_path;
        return;
    }

    // create execution context
    this->context = this->engine->createExecutionContext();
    if (!this->context)
    {
        LOG(ERROR) << "Failed to create execution context";
        return;
    }

    LOG_IF(FATAL, this->engine->getNbBindings() != 2) << "Invalid detection engine file: " << this->engine_path;

    LOG(WARNING) << "Successfully loaded engine file " << engine_path;

    // free the buffer
    delete[] modelStream;
}

Detect::~Detect()
{
    this->context->destroy();
    this->engine->destroy();
    this->runtime->destroy();
}

cv::Mat Detect::processInput(cv::Mat &image)
{
    // set the affine transformation matrix
    float scale = std::min(float(this->width) / image.cols, float(this->height) / image.rows);
    float Ox = (-scale * image.cols + this->width) / 2;
    float Oy = (-scale * image.rows + this->height) / 2;
    LOG(INFO) << "scale: " << scale << ", Ox: " << Ox << ", Oy: " << Oy;
    this->M = cv::Mat::zeros(2, 3, CV_32FC1);
    this->M.at<float>(0, 0) = scale;
    this->M.at<float>(1, 1) = scale;
    this->M.at<float>(0, 2) = Ox;
    this->M.at<float>(1, 2) = Oy;
    LOG(INFO) << "M: " << this->M;

    // apply the affine transformation (letterbox)
    cv::Mat image_processed;
    cv::warpAffine(image, image_processed, this->M, cv::Size(this->width, this->height), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));
    cv::invertAffineTransform(this->M, this->IM);

    cv::imshow("image_affine", image_processed);
    cv::waitKey(0);

    // blobFromImage:
    // 1. BGR to RGB
    // 2. /255.0, normalize to [0, 1]
    // 3. H,W,C to C,H,W
    image_processed =  cv::dnn::blobFromImage(image_processed, 1.0f/255.0f, image_processed.size(), cv::Scalar(0.0f, 0.0f, 0.0f), true, false, CV_32F);
    
    LOG(INFO) << "image_processed: " << "[" << image_processed.size[0] << ", " << image_processed.size[1] \
    << ", " << image_processed.size[2] << ", " << image_processed.size[3] << "]";

    return image_processed;
}

void Detect::processOutput(cv::Mat &image)
{
}

void CHECK(cudaError_t status) {
    
    if (status != 0)
    { 
        LOG(FATAL) << "Cuda failure: " << status;
    }
}

void Detect::Infer(cv::Mat &image)
{
    const int inputIndex = this->engine->getBindingIndex(INPUT_NAME); 
    const int outputIndex = this->engine->getBindingIndex(OUTPUT_NAME0);
    LOG(INFO) << "inputIndex: " << inputIndex << ", outputIndex: " << outputIndex;

    void* buffers[2]; 
    cudaMalloc(&buffers[inputIndex], BATCH_SIZE * IN_CHANNEL * IN_WIDTH * IN_HEIGHT * sizeof(float)); 
    cudaMalloc(&buffers[outputIndex], batchSize * 3 * IN_H * IN_W /4 * sizeof(float));
}