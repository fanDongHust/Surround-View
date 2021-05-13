#include <opencv2/core/cuda.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/stitching/detail/util.hpp>
#include <cuda_runtime.h>

// ------------------------------- CUDABlender --------------------------------
class CUDABlender
{
private:
	cudaStream_t _cudaStreamImage;
	cudaStream_t _cudaStreamMask;	
public:
        CUDABlender();
        ~CUDABlender();

	void prepare(const std::vector<cv::Point> &corners, const std::vector<cv::Size> &sizes);
  

	void prepare(cv::Rect dst_roi);
  

        void feed(cv::cuda::GpuMat& img, cv::cuda::GpuMat& mask, cv::Point tl);
	

        void blend(cv::cuda::GpuMat &dst, cv::cuda::GpuMat &dst_mask, cv::cuda::Stream& streamObj);

public:
	cv::cuda::GpuMat dst_, dst_mask_;
	cv::Rect dst_roi_;
};


// ------------------------------- CUDAFeatherBlender --------------------------------
class CUDAFeatherBlender
{
private:
        cudaStream_t _cudaStreamDst;
        cudaStream_t _cudaStreamDst_weight;
        bool use_cache_weight_ = false;
public:
        CUDAFeatherBlender(const float sharpness = 0.02f);
        ~CUDAFeatherBlender();

        void prepare(const std::vector<cv::Point> &corners, const std::vector<cv::Size> &sizes);

        void prepare(const std::vector<cv::Point> &corners, const std::vector<cv::Size> &sizes, const std::vector<cv::cuda::GpuMat>& masks);

        void prepare(cv::Rect dst_roi);

        void feed(cv::cuda::GpuMat& img, cv::cuda::GpuMat& mask, const cv::Point& tl, cv::cuda::Stream& streamObj = cv::cuda::Stream::Null());

        void feed(cv::cuda::GpuMat& _img, cv::cuda::GpuMat& _mask, const cv::Point& tl, const int idx, cv::cuda::Stream& streamObj = cv::cuda::Stream::Null());

        void blend(cv::cuda::GpuMat &dst, cv::cuda::GpuMat &dst_mask, cv::cuda::Stream& streamObj = cv::cuda::Stream::Null());

private:
        void createWeightMap(const cv::cuda::GpuMat& mask, cv::cuda::GpuMat& weight_map, cv::cuda::Stream& streamObj = cv::cuda::Stream::Null());
public:
        cv::cuda::GpuMat dst_, dst_mask_;
        cv::cuda::GpuMat dst_weight_map_;
        cv::Rect dst_roi_;
        std::vector<cv::cuda::GpuMat> weight_maps_;
        float sharpness_;
};



class CUDAMultiBandBlender
{
protected:
        typedef struct Border_
        {
            int top;
            int left;
            int bottom;
            int right;
            Border_(int top_= 0, int left_ = 0, int bottom_ = 0, int right_ = 0) : top(top_), left(left_), bottom(bottom_), right(right_){}
        } Border;
        typedef struct TLBR_
        {
            cv::Point tl;
            cv::Point br;
            TLBR_(const cv::Point& tl_, const cv::Point& br_) : tl(tl_), br(br_) {}
        } TLBR;

private:
        cudaStream_t _cudaStreamDst;
        cudaStream_t _cudaStreamDst_weight;
public:
        CUDAMultiBandBlender(const int numbands_ = 1);
        ~CUDAMultiBandBlender();

        void prepare(const std::vector<cv::Point> &corners, const std::vector<cv::Size> &sizes, const std::vector<cv::cuda::GpuMat>& masks);

        void feed(cv::cuda::GpuMat& _img, cv::cuda::GpuMat& _mask, const int idx, cv::cuda::Stream& streamObj = cv::cuda::Stream::Null());

        void blend(cv::cuda::GpuMat &dst, cv::cuda::GpuMat &dst_mask, cv::cuda::Stream& streamObj = cv::cuda::Stream::Null());

private:
        void prepare_pyr(const cv::Rect& dst_roi);
        void prepare_roi(const std::vector<cv::Point> &corners, const std::vector<cv::Size> &sizes);
protected:
        cv::cuda::GpuMat dst_mask_;
        cv::Rect dst_roi_, dst_roi_final_;
        std::vector<cv::cuda::GpuMat> gpu_dst_pyr_laplace_;
        std::vector<cv::cuda::GpuMat> gpu_dst_band_weights_;
        std::vector<cv::cuda::GpuMat> gpu_imgs_with_border_;
        std::vector<Border> gpu_imgs_borders_;
        std::vector<TLBR> gpu_imgs_corners_;
        std::vector<std::vector<cv::cuda::GpuMat>> gpu_weight_pyr_gauss_vec_;
        std::vector<std::vector<cv::cuda::GpuMat>> gpu_src_pyr_laplace_vec_;
        std::vector<std::vector<cv::cuda::GpuMat>> gpu_ups_;
        int numbands;
};

