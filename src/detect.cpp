#include <opencv2/core/types.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/opencv.hpp>
#include <NvInfer.h>
#include <algorithm>
#include <string>
#include <vector>
#include <fstream>

#include "glog/logging.h"
#include "config.hpp"
#include "common.hpp"
#include "detect.hpp"

using namespace nvinfer1;

class Logger : public ILogger
{
    void log(Severity severity, const char *msg) noexcept override
    {
        // suppress info-level messages
        if (severity <= Severity::kWARNING)
            // std::cout << msg << std::endl;
            LOG(WARNING) << msg;
    }
} glogger;

// Constructor for the Detect class.
// Sets the input size to 640x640.
// Loads the engine from the engine file.
Detect::Detect(std::string const &engine_filename)
{
    std::ifstream file(ENGINE_PATH + engine_filename, std::ios::binary);
    LOG_ASSERT(file.good());   
    LOG(INFO) << "engine file opened: " << engine_filename;

    // read the enfine file into a buffer
    file.seekg(0, std::ios::end); // change the position of the stream to the end
    int size = file.tellg();            // get the size of the engine file
    file.seekg(0, std::ios::beg); // change the position of the stream to the beginning
    LOG(INFO) << "engine file size: " << size;
    char *trtModelStream = new char[size];    // allocate a buffer to store the engine file
    LOG_ASSERT(trtModelStream) << "Failed to allocate buffer for engine file";
    file.read(trtModelStream, size);   // read the engine file into the buffer
    file.close();

    LOG(INFO) << "engine file loaded into buffer";

    // set input size to 640x640
    this->width = IN_WIDTH;
    this->height = IN_HEIGHT;

    // load engine
    this->engine_path = ENGINE_PATH + engine_filename;
    this->runtime = createInferRuntime(glogger);
    LOG_ASSERT(this->runtime != nullptr) << "Failed to create infer runtime";

    // deserialize the engine file
    this->engine = this->runtime->deserializeCudaEngine(trtModelStream, size, nullptr);
    LOG_ASSERT(this->engine != nullptr) << "Failed to deserialize engine file " << engine_path;

    // create execution context
    this->context = this->engine->createExecutionContext();
    LOG_ASSERT(this->context != nullptr) << "Failed to create execution context";

    LOG_ASSERT(this->engine->getNbBindings() == 2) << "Invalid detection engine file: " << this->engine_path;

    LOG(INFO) << "Successfully loaded engine file " << engine_path;

    // // free the buffer
    // delete[] trtModelStream;

    cudaStreamCreate(&this->stream);
    this->num_bindings = this->engine->getNbBindings();
    LOG(INFO) << "num_bindings: " << this->num_bindings;

    //  get binding info
    for (int i = 0; i < this->num_bindings; ++i)
	{
		Binding binding;
		Dims dims;
		DataType dtype = this->engine->getBindingDataType(i);
		std::string name = this->engine->getBindingName(i);
		binding.name = name;
		binding.dsize = type_to_size(dtype);

		bool IsInput = engine->bindingIsInput(i);

		if (IsInput)
		{
			this->num_inputs += 1;
			dims = this->engine->getProfileDimensions(
				i,
				0,
				OptProfileSelector::kMAX);
			binding.size = get_size_by_dims(dims);
			binding.dims = dims;
			this->input_bindings.push_back(binding);
			// set max opt shape
			this->context->setBindingDimensions(i, dims);

		}
		else
		{
			dims = this->context->getBindingDimensions(i);
			binding.size = get_size_by_dims(dims);
			binding.dims = dims;
			this->output_bindings.push_back(binding);
			this->num_outputs += 1;
		}
	}

    LOG(INFO) << "num_inputs: " << this->num_inputs << ", num_outputs: " << this->num_outputs 
        << "input binding size: " << this->input_bindings[0].size << ", output binding size: " << this->output_bindings[0].size;
}

Detect::~Detect()
{
    this->context->destroy();
	this->engine->destroy();
	this->runtime->destroy();
	cudaStreamDestroy(this->stream);
	for (auto& ptr : this->device_ptrs)
	{
		CUDA_CHECK(cudaFree(ptr));
	}

	for (auto& ptr : this->host_ptrs)
	{
		CUDA_CHECK(cudaFreeHost(ptr));
	}
}

// preprocess the input image
void Detect::letterbox(const cv::Mat& image, cv::Mat& nchw)
{
    // set the affine transformation matrix
    LOG(INFO) << "making letterbox";
    LOG(INFO) << "image size: " << image.cols << "x" << image.rows;

    float scale = std::min(float(this->width) / image.cols, float(this->height) / image.rows);
    float delta_x = (-scale * image.cols + this->width) / 2;
    float delta_y = (-scale * image.rows + this->height) / 2;
    LOG(INFO) << "scale: " << scale << ", delta_x: " << delta_x << ", delta_y: " << delta_y;

    // M = [[scale, 0, delta_x], [0, scale, delta_y]]
    this->M = cv::Mat::zeros(2, 3, CV_32FC1);
    this->M.at<float>(0, 0) = scale;
    this->M.at<float>(1, 1) = scale;
    this->M.at<float>(0, 2) = delta_x;
    this->M.at<float>(1, 2) = delta_y;
    LOG(INFO) << "M: " << this->M;

    // apply the affine transformation (letterbox)
    cv::Size size(this->width, this->height);
    cv::warpAffine(image, 
        nchw,
        this->M,
        size, 
        cv::INTER_LINEAR, 
        cv::BORDER_CONSTANT, 
        cv::Scalar(114, 114, 114)
    );
    cv::invertAffineTransform(this->M, this->IM);

    cv::imwrite("letterbox.png", nchw);

    // blobFromImage:
    // 1. BGR to RGB
    // 2. /255.0, normalize to [0, 1]
    // 3. H,W,C to C,H,W
    nchw = cv::dnn::blobFromImage(
        nchw, 
        1.0f / 255.0f, 
        nchw.size(), 
        cv::Scalar(0.0f, 0.0f, 0.0f), 
        true, 
        false, 
        CV_32F
    );

    LOG(INFO) << "input size after preprocess: "
              << "[" << nchw.size[0] << ", " << nchw.size[1]
              << ", " << nchw.size[2] << ", " << nchw.size[3] << "]";
}

float Detect::iou(cv::Rect &rect1, cv::Rect &rect2)
{
    int x1 = std::max(rect1.x, rect2.x);
    int y1 = std::max(rect1.y, rect2.y);
    int x2 = std::min(rect1.x + rect1.width, rect2.x + rect2.width);
    int y2 = std::min(rect1.y + rect1.height, rect2.y + rect2.height);

    int intersection = std::max(0, x2 - x1) * std::max(0, y2 - y1);
    int union_ = rect1.width * rect1.height + rect2.width * rect2.height - intersection;

    return float(intersection) / union_;
}

void Detect::nonMaxSupression(std::vector<detObject> &results)
{
    // sort the results by confidence in descending order
    std::sort(results.begin(), results.end(), [](detObject &a, detObject &b) { return a.conf > b.conf; });

    std::vector<bool> keep(results.size(), true);
    for (int i = 0; i < results.size(); i++)
    {
        if (keep[i])
        {
            for (int j = i + 1; j < results.size(); j++)
            {
                if (keep[j])
                {
                    if (this->iou(results[i].rect, results[j].rect) > NMS_THRESH)
                    {
                        keep[j] = false;
                    }
                }
            }
        }
    }

    for (int i = 0; i < results.size(); i++)
    {
        if (!keep[i])
        {
            results.erase(results.begin() + i);
            i--;
        }
    }
    LOG(INFO) << "postprocess (nms) done";
}

void Detect::processOutput(float *output, std::vector<detObject> &results)
{
    for (int i = 0; i < BATCH_SIZE; i++)
    {
        for (int k = 0; k < DET_OUT_CHANNEL1; k++)
        {
            int index[DET_OUT_CHANNEL0];
            for (int j = 0; j < DET_OUT_CHANNEL0; j++)
            {
                index[j] = k + DET_OUT_CHANNEL1 * (j + DET_OUT_CHANNEL0 * i);
            }

            float conf = output[index[4]];

            if (conf > CONF_THRESH)
            {
                detObject result;
                float cx = output[index[0]];
                float cy = output[index[1]];
                float w = output[index[2]];
                float h = output[index[3]];
                result.rect = cv::Rect2i(int(cx - w / 2.0), int(cy - h / 2.0), int(w), int(h));
                result.conf = conf;
                result.class_id = int(output[index[5]]);
                results.push_back(result);
            }
        }
    }

    LOG(INFO) << results.size() << " results before nms";
    this->nonMaxSupression(results);
}

void Detect::makePipe(bool warmup)
{

	for (auto& bindings : this->input_bindings)
	{
		void* d_ptr; // device pointer
		CUDA_CHECK(cudaMallocAsync(
			&d_ptr,
			bindings.size * bindings.dsize,
			this->stream)
		);
		this->device_ptrs.push_back(d_ptr);
	}

	for (auto& bindings : this->output_bindings)
	{
		void* d_ptr, * h_ptr; // device pointer, host pointer
		size_t size = bindings.size * bindings.dsize;
		CUDA_CHECK(cudaMallocAsync(
			&d_ptr,
			size,
			this->stream)
		);
		CUDA_CHECK(cudaHostAlloc(
			&h_ptr,
			size,
			0)
		);
		this->device_ptrs.push_back(d_ptr);
		this->host_ptrs.push_back(h_ptr);
	}

	if (warmup)
	{
		for (int i = 0; i < 10; i++)
		{
			for (auto& bindings : this->input_bindings)
			{
				size_t size = bindings.size * bindings.dsize;
				void* h_ptr = malloc(size);
				memset(h_ptr, 0, size);
				CUDA_CHECK(cudaMemcpyAsync(
					this->device_ptrs[0],
					h_ptr,
					size,
					cudaMemcpyHostToDevice,
					this->stream)
				);
				free(h_ptr);
			}
			this->infer();
		}
		printf("model warmup 10 times\n");

	}
}

void Detect::infer()
{
    this->context->enqueueV2(
		this->device_ptrs.data(),
		this->stream,
		nullptr
	);
	for (int i = 0; i < this->num_outputs; i++)
	{
		size_t osize = this->output_bindings[i].size * this->output_bindings[i].dsize;
		CUDA_CHECK(cudaMemcpyAsync(this->host_ptrs[i],
			this->device_ptrs[i + this->num_inputs],
			osize,
			cudaMemcpyDeviceToHost,
			this->stream)
		);

	}
	cudaStreamSynchronize(this->stream);
}

void Detect::copyFromMat(cv::Mat& nchw)
{
	this->context->setBindingDimensions(
		0,
		Dims
			{
				4,
				{ 1, 3, this->height, this->width }
			}
	);
    LOG(INFO) << "binding dimensions set";

	CUDA_CHECK(cudaMemcpyAsync(
		this->device_ptrs[0],
		nchw.ptr<float>(),
		nchw.total() * nchw.elemSize(),
		cudaMemcpyHostToDevice,
		this->stream)
	);
}

// run detection on the image
void Detect::detect(cv::Mat &image, std::vector<detObject> &results)
{
    // preprocess input
    cv::Mat nchw;
    this->letterbox(image, nchw);
    LOG(INFO) << "image processed";

    // make pipe
    this->makePipe(true);
    LOG(INFO) << "pipe made";

    // copy to device
    this->copyFromMat(nchw);
    LOG(INFO) << "image copied to device";

    // run inference
    this->infer();
    LOG(INFO) << "inference done";

    // postprocess output
    // this->processOutput(output, results);
}