#include <iostream>
#include "AutoCalib.hpp"
#include "SeamDetection.hpp"
#include "SurroundView.hpp"
#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudawarping.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudafilters.hpp>


#include <omp.h>

static auto isstart = false;
static constexpr auto padding_warp = 20;

bool SurroundView::init(const std::vector<cv::cuda::GpuMat>& imgs){
	
	if (isInit){
	    std::cerr << "SurroundView already initialize...\n";
	    return false;
	}
	
	imgs_num = imgs.size();	

	if (imgs_num <= 1){
	    std::cerr << "Not enough images in imgs vector, must be >= 2...\n";
	    return false;
	}
	
	std::vector<cv::Mat> cpu_imgs(imgs_num);
	for (size_t i = 0; i < imgs_num; ++i){
	    imgs[i].download(cpu_imgs[i]);
	}


	AutoCalib autcalib(imgs_num);
	bool res = autcalib.init(cpu_imgs, true);
	if (!res){
	    std::cerr << "Error can't autocalibrate camera parameters...\n";
	    return false;
	}
	warped_image_scale = autcalib.get_warpImgScale();
	R = autcalib.getExtRotation();
	Ks_f = autcalib.getIntCameraParam();


	SeamDetector smd(imgs_num, warped_image_scale);
	res = smd.init(cpu_imgs, Ks_f, R);
	if (!res){
	    std::cerr << "Error can't seam masks for images...\n";
	    return false;
	}
	gpu_seam_masks = smd.getSeams();
	corners = smd.getCorners();
	sizes = smd.getSizes();
	texXmap = smd.getXmap();
	texYmap = smd.getYmap();

	if (cuBlender.get() == nullptr){
	    cuBlender = std::make_shared<CUDAMultiBandBlender>(numbands);
	    cuBlender->prepare(corners, sizes, gpu_seam_masks);
	}


        res = prepareCutOffFrame(cpu_imgs);
        if (!res){
            std::cerr << "Error can't prepare blending ROI rect...\n";
            return false;
        }


	isInit = true;


	return isInit;
}


bool SurroundView::initFromFile(const std::string& dirpath, const std::vector<cv::cuda::GpuMat>& imgs, const bool use_filewarp_pts)
{
    if (isInit){
        std::cerr << "SurroundView already initialize...\n";
        return false;
    }

    if (dirpath.empty()){
        std::cerr << "Invalid directory path...\n";
        return false;
    }

    imgs_num = imgs.size();

    if (imgs_num <= 1){
        std::cerr << "Not enough images in imgs vector, must be >= 2...\n";
        return false;
    }

    std::vector<cv::Mat> cpu_imgs(imgs_num);
    for (size_t i = 0; i < imgs_num; ++i){
        imgs[i].download(cpu_imgs[i]);
    }


    bool res = false;
    if (!isstart){ // delete after test
        res = getDataFromFile(dirpath, use_filewarp_pts);
        if (!res){
            std::cerr << "Error can't read camera parameters...\n";
            return false;
        }

        SeamDetector smd(imgs_num, warped_image_scale);
        res = smd.init(cpu_imgs, Ks_f, R);
        if (!res){
            std::cerr << "Error can't seam masks for images...\n";
            return false;
        }
        gpu_seam_masks = smd.getSeams();
        corners = smd.getCorners();
        sizes = smd.getSizes();
        texXmap = smd.getXmap();
        texYmap = smd.getYmap();
        isstart = true;
    }


    if (cuBlender.get() == nullptr){
        cuBlender = std::make_shared<CUDAMultiBandBlender>(numbands);
        cuBlender->prepare(corners, sizes, gpu_seam_masks);
    }

    if (!use_filewarp_pts){
        res = prepareCutOffFrame(cpu_imgs);
        if (!res){
            std::cerr << "Error can't prepare blending ROI rect...\n";
            return false;
        }
    }


    isInit = true;


    return isInit;
}


bool SurroundView::getDataFromFile(const std::string& dirpath, const bool use_filewarp_pts)
{
    Ks_f = std::move(std::vector<cv::Mat>(imgs_num));
    R = std::move(std::vector<cv::Mat>(imgs_num));
    auto fullpath = dirpath + "Camparam";
    for(auto i = 0; i < imgs_num; ++i){
           std::string KRpath{fullpath + std::to_string(i) + ".yaml"};
           cv::FileStorage KRfout(KRpath, cv::FileStorage::READ);
           warped_image_scale = KRfout["FocalLength"];
           if (!KRfout.isOpened()){
               std::cerr << "Error can't open camera param file: " << KRpath << "...\n";
               return false;
           }
           cv::Mat_<float> K_;
           KRfout["Intrisic"] >> K_;
           K_.convertTo(Ks_f[i], CV_32F);
           KRfout["Rotation"] >> R[i];
    }

    if (use_filewarp_pts){
          std::string WARP_PTS_path{dirpath + "corner_warppts.yaml"};
          cv::Point tl, tr, bl, br;
          cv::FileStorage WPTSfout(WARP_PTS_path, cv::FileStorage::READ);
          if (!WPTSfout.isOpened()){
              std::cerr << "Error can't open camera param file: " << WARP_PTS_path << "...\n";
              return false;
          }
          WPTSfout["tl"] >> tl; WPTSfout["tr"] >> tr;
          WPTSfout["bl"] >> bl; WPTSfout["br"] >> br;
          WPTSfout["res_size"] >> resSize;
          const auto width_ = resSize.width;
          const auto height_ = resSize.height;


          std::vector<cv::Point_<float>> src {tl, tr, bl, br};
          std::vector<cv::Point_<float>> dst {cv::Point(0, 0), cv::Point(width_, 0), cv::Point(0, height_), cv::Point(width_, height_)};
          transformM = cv::getPerspectiveTransform(src, dst);

          cv::cuda::buildWarpPerspectiveMaps(transformM, false, resSize, warpXmap, warpYmap);
    }



    return true;
}

#include <opencv2/highgui.hpp>
bool SurroundView::prepareCutOffFrame(const std::vector<cv::Mat>& cpu_imgs)
{
          cv::cuda::GpuMat gpu_result, warp_s, warp_img;
          for(size_t i = 0; i < imgs_num; ++i){
                  gpu_result.upload(cpu_imgs[i]);
                  cv::cuda::remap(gpu_result, warp_img, texXmap[i], texYmap[i], cv::INTER_LINEAR, cv::BORDER_REFLECT, cv::Scalar(), streamObj);
                  warp_img.convertTo(warp_s, CV_16S);
                  cuBlender->feed(warp_s, gpu_seam_masks[i], i);
          }


          cuBlender->blend(gpu_result, warp_img);
          cv::Mat result, thresh;
          gpu_result.download(result);
          warp_img.download(thresh);

          result.convertTo(result, CV_8U);
          cv::imshow("Cam0", result);
          return false;

          cv::threshold(thresh, thresh, 1, 255, cv::THRESH_BINARY);

          std::vector<std::vector<cv::Point>> cnts;

          cv::findContours(thresh, cnts, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);


          auto width_ = result.cols;
          auto height_ = result.rows;

          cv::Point tl(0, 0);
          cv::Point tr(0, 0);
          cv::Point bl = cnts[0][0];
          cv::Point br = cnts[0][0];
          /* constrain for right corner set manual -> start to find at middle of image*/
          const auto xr_constrain = result.cols - (sizes[sizes.size()- 1].width >> 1);
          /* find bottom-left and bottorm-right corners (or if another warping tl and tr)*/
          auto idx_tl = 0, idx_tr = 0, tot_idx = 0;
          for(const auto& pcnt : cnts){
              for (const auto& pt : pcnt){
                  if (bl.x >= pt.x){
                    bl = pt;
                    idx_tl = tot_idx;
                  }

                  /* cond. -> (bl.y < pt.y) depending of last mask seam */
                  if (br.x <= pt.x && bl.y < pt.y)
                    br = pt;  

                  tot_idx += 1;
                  if (pt.x == xr_constrain)
                    idx_tr = tot_idx;
              }
          }

          /* find top-left*/
          for(auto i = idx_tl; i >= 0; --i){
              const auto& pt = cnts[0][i + 1];
              const auto& prev_pt = cnts[0][i];
              auto dy = prev_pt.y - pt.y;
              if (bl.y >= pt.y && dy == 0){
                tl = pt;
                break;
              }
          }

          /* find top-right*/
          for(auto i = idx_tr; i >= 0; --i){
              const auto& pt = cnts[0][i];
              const auto& next_pt = cnts[0][i - 1];
              auto dy = next_pt.y - pt.y;
              // find corner point on seam of last frame
              auto dx = cnts[0][i - 2].x - next_pt.x;
              if (dy == 0 && dx == 0){
                tr = pt;
                break;
              }
          }


          resSize = result.size();
          /* add offset of coordinate corner points due to seam last frame */
          //save_warpptr("corner_warppts.yaml", resSize, tl, tr, bl, br);

          std::vector<cv::Point_<float>> src {tl, tr, bl, br};

          std::vector<cv::Point_<float>> dst {cv::Point(0, 0), cv::Point(width_, 0),
                                              cv::Point(0, height_), cv::Point(width_, height_)};
          transformM = cv::getPerspectiveTransform(src, dst);
          //cv::warpPerspective(result, result, transformM, resSize, cv::INTER_CUBIC, cv::BORDER_CONSTANT);

          cv::cuda::buildWarpPerspectiveMaps(transformM, false, resSize, warpXmap, warpYmap);

          return true;
}

void SurroundView::save_warpptr(const std::string& warpfile, const cv::Size& res_size,
                                const cv::Point& tl, const cv::Point& tr, const cv::Point& bl, const cv::Point& br)
{
    cv::FileStorage WPTSfout(warpfile, cv::FileStorage::WRITE);
    WPTSfout << "res_size" << res_size;
    WPTSfout << "tl" << tl;
    WPTSfout << "tr" << tr;
    WPTSfout << "bl" << bl;
    WPTSfout << "br" << br;

}


bool SurroundView::stitch(const std::vector<cv::cuda::GpuMat*>& imgs, cv::cuda::GpuMat& blend_img)
{
    if (!isInit){
        std::cerr << "SurroundView was not initialized...\n";
        return false;
    }

    cv::cuda::GpuMat gpuimg_warped_s, gpuimg_warped;
    cv::cuda::GpuMat stitch, mask_;

#ifndef NO_OMP
    #pragma omp parallel for default(none) shared(imgs) private(gpuimg_warped, gpuimg_warped_s)
#endif
    for(size_t i = 0; i < imgs_num; ++i){

          cv::cuda::remap(*imgs[i], gpuimg_warped, texXmap[i], texYmap[i], cv::INTER_LINEAR, cv::BORDER_REFLECT, cv::Scalar(), streamObj);

          gpuimg_warped.convertTo(gpuimg_warped_s, CV_16S, streamObj);

          cuBlender->feed(gpuimg_warped_s, gpu_seam_masks[i], i, streamObj);
    }

    cuBlender->blend(stitch, mask_, streamObj);

    cv::cuda::remap(stitch, blend_img, warpXmap, warpYmap, cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(), streamObj);

    return true;
}





