/*
All modification made by Intel Corporation: © 2016 Intel Corporation

All contributions by the University of California:
Copyright (c) 2014, 2015, The Regents of the University of California (Regents)
All rights reserved.

All other contributions:
Copyright (c) 2014, 2015, the respective contributors
All rights reserved.
For the list of contributors go to https://github.com/BVLC/caffe/blob/master/CONTRIBUTORS.md


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef MKL2017_SUPPORTED
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layer.hpp"
#include "caffe/layers/mkl_layers.hpp"
#include "caffe/util/performance.hpp"
#include "mkl_service.h"

#ifdef USE_MLSL
using namespace MLSL;
#endif

static int getMKLBuildDate() {
  static int build = 0;
  if (build == 0) {
    MKLVersion v;
    mkl_get_version(&v);
    build = atoi(v.Build);
  }
  return build;
}

#define START_TIMER() {if (need_log) {timer.Start();}}
#define STOP_TIMER(component) {if (need_log) {double elapsed = timer.MicroSeconds(); LOG(ERROR) << component << ": " << elapsed / 1000. << " ms";}}

namespace caffe {
template <typename Dtype>
MKLConvolutionLayer<Dtype>::MKLConvolutionLayer(
  const LayerParameter& param)
      : ConvolutionLayer<Dtype>(param),
        fwd_bottom_data(new MKLData<Dtype>()),
        fwd_top_data(new MKLData<Dtype>()),
        fwd_filter_data(new MKLData<Dtype>()),
        fwd_bias_data(new MKLData<Dtype>()),
        convolutionFwd(NULL),
        bwdd_top_diff(new MKLDiff<Dtype>()),
        bwdd_bottom_diff(new MKLDiff<Dtype>()),
        bwdd_filter_data(new MKLData<Dtype>()),
        convolutionBwdData(static_cast<dnnPrimitive_t>(NULL)),
        bwdf_top_diff(new MKLDiff<Dtype>()),
        bwdf_filter_diff(new MKLDiff<Dtype>()),
        bwdf2fwd_filter_diff(new MKLDiff<Dtype>()),
        bwdf_bottom_data(new MKLData<Dtype>()),
        convolutionBwdFilter(static_cast<dnnPrimitive_t>(NULL)),
        bwdb_top_diff(new MKLDiff<Dtype>()),
        bwdb_bias_diff(new MKLDiff<Dtype>()),
        convolutionBwdBias(static_cast<dnnPrimitive_t>(NULL)),
        bwdf_filter_diff_iter(new MKLDiff<Dtype>()),
        bwdb_bias_diff_iter(new MKLDiff<Dtype>()) {
          layer_name = param.name();
          reinit_times = 0;
          LOG(ERROR) << layer_name << " MKL";
        }

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::compute_output_shape() {
  ConvolutionLayer<Dtype>::compute_output_shape();
  this->height_out_ = (this->height_ + 2 * this->pad_h_ - this->kernel_h_)
      / this->stride_h_ + 1;
  this->width_out_ = (this->width_ + 2 * this->pad_w_ - this->kernel_w_)
      / this->stride_w_ + 1;
}

template <typename Dtype>
MKLConvolutionLayer<Dtype>::~MKLConvolutionLayer() {
    dnnDelete<Dtype>(convolutionFwd);
    dnnDelete<Dtype>(convolutionBwdData);
    dnnDelete<Dtype>(convolutionBwdFilter);
    if (this->bias_term_)
        dnnDelete<Dtype>(convolutionBwdBias);
}

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Init(
      const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  this->width_ = bottom[0]->width();
  this->height_ = bottom[0]->height();
  this->num_ = bottom[0]->num();

  // TODO: clean up this
  kernel_w_ = this->kernel_shape_.cpu_data()[1];
  kernel_h_ = this->kernel_shape_.cpu_data()[0];
  stride_w_ = this->stride_.cpu_data()[1];
  stride_h_ = this->stride_.cpu_data()[0];
  pad_w_ = this->pad_.cpu_data()[1];
  pad_h_ = this->pad_.cpu_data()[0];

  this->bottom_shape_ = &bottom[0]->shape();
  compute_output_shape();
  int status;
  size_t n, g;
  size_t iw, ih, ic;
  size_t ow, oh, oc;
  size_t kw, kh; /* filter */
  size_t dimension = 4;

  g  = std::max(this->group_, 1);
  n  = this->num_;
  iw = this->width_;
  ih = this->height_;
  ic = this->channels_;

  ow = this->width_out_;
  oh = this->height_out_;
  oc = this->num_output_;

  kw = this->kernel_w_;
  kh = this->kernel_h_;

  size_t bdata_sizes[4] = {iw, ih, ic, n};
  size_t bdata_strides[4] = {1, iw, iw*ih, iw*ih*ic};

  /* starting with MKL 2017 Gold in case of groups filter layout
   * becomes 5D, i.e. groups become a separate dimension */
  size_t g_mkl2017 = g;
  size_t f_dimension = dimension + (g != 1);
  if (getMKLBuildDate() < 20160701) {
      g_mkl2017 = 1;
      f_dimension = dimension;
  }

  size_t fdata_sizes[5] = {kw, kh, ic/g, oc/g_mkl2017, g_mkl2017};
  size_t fdata_strides[5]  = {1, kw, kw*kh, kw*kh*ic/g, kw*kh*ic/g*oc/g};

  size_t bias_sizes[1] = {oc};
  size_t bias_strides[1] = {1};

  size_t tdata_sizes[4] = {ow, oh, oc, n};
  size_t tdata_strides[4]  = {1, ow, ow*oh, ow*oh*oc};

  size_t convolutionStrides[2] = {this->stride_w_, this->stride_h_};
  int    inputOffset[2] = {-this->pad_w_, -this->pad_h_};

  // Names are for debugging purposes only.
  fwd_bottom_data ->name = "fwd_bottom_data   @ " + this->layer_param_.name();
  fwd_top_data    ->name = "fwd_top_data      @ " + this->layer_param_.name();
  fwd_filter_data ->name = "fwd_filter_data   @ " + this->layer_param_.name();
  fwd_bias_data   ->name = "fwd_bias_data     @ " + this->layer_param_.name();
  bwdd_top_diff   ->name = "bwdd_top_diff     @ " + this->layer_param_.name();
  bwdd_bottom_diff->name = "bwdd_bottom_diff  @ " + this->layer_param_.name();
  bwdd_filter_data->name = "bwdd_filter_data  @ " + this->layer_param_.name();
  bwdf_top_diff   ->name = "bwdf_top_diff     @ " + this->layer_param_.name();
  bwdf_bottom_data->name = "bwdf_bottom_data  @ " + this->layer_param_.name();
  bwdf_filter_diff->name = "bwdf_filter_diff  @ " + this->layer_param_.name();
  bwdf2fwd_filter_diff->name =
                       "bwdf2fwd_filter_diff  @ " + this->layer_param_.name();
  bwdb_top_diff   ->name = "bwdb_top_diff     @ " + this->layer_param_.name();
  bwdb_bias_diff  ->name = "bwdb_bias_diff    @ " + this->layer_param_.name();

  // bool need_log = !layer_name.compare("conv1") || !layer_name.compare("rfcn_cls");
  bool need_log = false;
  double elapsed;

  if (need_log) {
        timer.Start();
  }
  // Free MKL primitives
  dnnDelete<Dtype>(convolutionFwd);
  if (need_log) {
        elapsed = timer.MicroSeconds();
        LOG(ERROR) << "delete primitives: " << elapsed / 1000.0 << " ms";
        LOG(ERROR) << "bias term: " << this->bias_term_;
         timer.Start();
  }

  if (this->bias_term_) {
    status = dnnGroupsConvolutionCreateForwardBias<Dtype>(
      &convolutionFwd,
      NULL,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  } else {
    status = dnnGroupsConvolutionCreateForward<Dtype>(
      &convolutionFwd,
      NULL,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  }
  if (need_log) {
      elapsed = timer.MicroSeconds();
      LOG(ERROR) << "create forward: " << elapsed / 1000.0 << " ms";
      timer.Start();
  }

  CHECK_EQ(status, 0)
          << "Failed dnnCreateConvolution<Dtype>(dnnForward) with status "
          << status << "\n";

  fwd_bottom_data->create_layouts(convolutionFwd, dnnResourceSrc, dimension,
                                  bdata_sizes, bdata_strides);
  fwd_top_data   ->create_layouts(convolutionFwd, dnnResourceDst, dimension,
                                  tdata_sizes, tdata_strides);
  fwd_filter_data->create_layouts(convolutionFwd, dnnResourceFilter,
                                  f_dimension, fdata_sizes, fdata_strides);

  if (this->bias_term_)
    fwd_bias_data->create_layouts(convolutionFwd, dnnResourceBias, 1,
                                  bias_sizes, bias_strides);

  if (need_log) {
      elapsed = timer.MicroSeconds();
      LOG(ERROR) << "create forward layout: " << elapsed / 1000.0 << " ms";
      timer.Start();
  }
/*
 * Backward by data layer setup
 */
  dnnDelete<Dtype>(convolutionBwdData);
  status = dnnGroupsConvolutionCreateBackwardData<Dtype>(
    &convolutionBwdData,
    NULL,
    dnnAlgorithmConvolutionDirect,
    g,
    dimension,
    bdata_sizes,
    tdata_sizes,
    fdata_sizes,
    convolutionStrides,
    inputOffset,
    dnnBorderZeros);
  CHECK_EQ(status, 0)
          << "Failed dnnConvolutionCreateBackwardData with status "
          << status << "\n";
  if (need_log) {
      elapsed = timer.MicroSeconds();
      LOG(ERROR) << "create backward: " << elapsed / 1000.0 << " ms";
      timer.Start();
  }

  bwdd_bottom_diff->create_layouts(convolutionBwdData, dnnResourceDiffSrc,
                                   dimension, bdata_sizes, bdata_strides);
  bwdd_top_diff   ->create_layouts(convolutionBwdData, dnnResourceDiffDst,
                                   dimension, tdata_sizes, tdata_strides);
  bwdd_filter_data->create_layouts(convolutionBwdData, dnnResourceFilter,
                                   f_dimension, fdata_sizes, fdata_strides);

  if (need_log) {
      elapsed = timer.MicroSeconds();
      LOG(ERROR) << "create backward layout: " << elapsed / 1000. << " ms";
      timer.Start();
  }

/*
 * Backward by filter layer setup
 */
  dnnDelete<Dtype>(convolutionBwdFilter);
  status = dnnGroupsConvolutionCreateBackwardFilter<Dtype>(
    &convolutionBwdFilter,
    NULL,
    dnnAlgorithmConvolutionDirect,
    g,
    dimension,
    bdata_sizes,
    tdata_sizes,
    fdata_sizes,
    convolutionStrides,
    inputOffset,
    dnnBorderZeros);
  CHECK_EQ(status, 0)
          << "Failed dnnConvolutionCreateBackwardFilter with status "
          << status << "\n";
  if (need_log) {
      elapsed = timer.MicroSeconds();
      LOG(ERROR) << "create backward filter: " << elapsed / 1000. << " ms";
      timer.Start();
  }

  bwdf_bottom_data->create_layouts(convolutionBwdFilter, dnnResourceSrc,
                                   dimension, bdata_sizes, bdata_strides);
  bwdf_top_diff   ->create_layouts(convolutionBwdFilter, dnnResourceDiffDst,
                                   dimension, tdata_sizes, tdata_strides);
  bwdf_filter_diff->create_layouts(convolutionFwd, dnnResourceFilter,
                                   f_dimension, fdata_sizes, fdata_strides);
  // support for (iter_size > 1) requires additional buffer
  bwdf_filter_diff_iter->create_layouts(convolutionFwd, dnnResourceFilter,
                                   f_dimension, fdata_sizes, fdata_strides);

  if (need_log) {
      elapsed = timer.MicroSeconds();
      LOG(ERROR) << "create filter layout: " << elapsed / 1000. << " ms";
      timer.Start();
  }

  // Note: this caused some trouble for older MKL
  if (getMKLBuildDate() > 20160701) {
    // bwdf2fwd_filter_diff:
    // layout_int = internal layout of weight diff
    // layout_usr = internal layout of weight data on forward convolution
    bwdf2fwd_filter_diff->create_internal_layout(convolutionBwdFilter,
        dnnResourceDiffFilter);
    bwdf2fwd_filter_diff->remove_user_layout();
    status = dnnLayoutCreateFromPrimitive<Dtype>(
        &bwdf2fwd_filter_diff->layout_usr, convolutionFwd, dnnResourceFilter);
    CHECK_EQ(status, 0) << "Failed dnnLayoutCreateFromPrimitive with status "
            << status << "\n";

    bwdf2fwd_filter_diff->create_conversions();
  }

  if (need_log) {
      elapsed = timer.MicroSeconds();
      LOG(ERROR) << "create diff: " << elapsed / 1000. << " ms";
      timer.Start();
  }

/*
 * Backward by bias layer setup
 */
  if (this->bias_term_) {
    dnnDelete<Dtype>(convolutionBwdBias);
    status = dnnGroupsConvolutionCreateBackwardBias<Dtype>(
      &convolutionBwdBias,
      NULL,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      tdata_sizes);
    CHECK_EQ(status, 0)
            << "Failed dnnConvolutionCreateBackwardBias with status "
            << status << "\n";

    bwdb_top_diff->create_layouts(convolutionBwdBias, dnnResourceDiffDst,
                                  dimension, tdata_sizes, tdata_strides);
    bwdb_bias_diff->create_layouts(convolutionBwdBias, dnnResourceDiffBias,
                                   1, bias_sizes, bias_strides);
    // support for (iter_size > 1) requires additional buffer
    bwdb_bias_diff_iter->create_layouts(convolutionBwdBias, dnnResourceDiffBias,
                                        1, bias_sizes, bias_strides);
  }

#ifdef USE_MLSL

  if (!this->layerOp) {
    DataType dt = (sizeof(Dtype) == 4)? DT_FLOAT : DT_DOUBLE;
    ComputeOpRegInfo *myRegInfo;
    myRegInfo = new ComputeOpRegInfo(COMP_OP_TYPE_CC);
    myRegInfo->SetName(this->layer_param_.name().c_str());
    myRegInfo->AddInputFeatureMap(ic, iw*ih, dt);
    myRegInfo->AddOutputFeatureMap(oc, ow*oh, dt);
    myRegInfo->AddWeights(ic*oc/g, kw*kh, dt, DISTRIBUTED_WEIGHT_UPDATE);

    if (this->bias_term_) {
      myRegInfo->AddWeights(oc, 1, dt, false /* no make sense to do distributed update for bias */);
    }

    myRegInfo->Validate();
    this->layerOp = new ComputeOp(myRegInfo, caffe::internode::data_parallelism);
    delete myRegInfo;

    for (int idx = 0; idx < this->blobs_.size(); idx++) {
      LOG_LAYER(this) << "LayerSetUp: this->blobs_[idx]->count() " << this->blobs_[idx]->count();
      LOG_LAYER(this) << "LayerSetUp: wt idx " << idx
                      << ", local weight len " << this->layerOp->GetWeights(idx)->LocalLen() * this->layerOp->GetWeights(idx)->WTSize()
                      << ", owned weight len " << this->layerOp->GetWeights(idx)->OwnedLen() * this->layerOp->GetWeights(idx)->WTSize()
                      << ", wtsize " << this->layerOp->GetWeights(idx)->WTSize();
    }
  }

#endif /* USE_MLSL */

  if (need_log) {
      elapsed = timer.MicroSeconds();
      LOG(ERROR) << "create backward bias: " << elapsed / 1000. << " ms";
  }
}

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::LayerSetUp(
      const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  ConvolutionLayer<Dtype>::LayerSetUp(bottom, top);

  Init(bottom, top);
}

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  bool reinitialize = (this->width_ == bottom[0]->width() &&
                       this->height_ == bottom[0]->height() &&
                       this->channels_ == bottom[0]->channels() &&
                       this->num_ == bottom[0]->num()) ? false : true; 

  // bool need_log = !layer_name.compare("conv1") || !layer_name.compare("rfcn_cls");
  bool need_log = false;

  if (need_log) {
	  LOG(ERROR) << "layer name: " << layer_name;
	  LOG(ERROR) << "image num: " << bottom[0]->num() <<  " width: " << bottom[0]->width() << " height: " << bottom[0]->height() << " channel: " << bottom[0]->channels();
	  LOG(ERROR) << "layer conf num: " << this->num_ << " width: " << this->width_ << " height: " << this->height_ << " channel: " << this->channels_;
  }

  BaseConvolutionLayer<Dtype>::Reshape(bottom, top);

  /*
  if (need_log) {
	  timer.Start();
  }
  */
  if (reinitialize == true) {
    if (reinit_times >= 1) {
      LOG(FATAL) << "Pls use same size image input for performance seek";
    }
    if (need_log) {
      LOG(ERROR) << "re-initialize";
    }
    Init(bottom, top);
    reinit_times++;
  }
  /*
  if (need_log) {
     double elapsed = timer.MicroSeconds();
     LOG(ERROR) << "Reshape time: " << elapsed / 1000. << " ms";
  }
  */
}

#ifdef USE_MLSL

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::pack_buffer(FeatureMap *fm, Dtype *to, const Dtype *from) {
      for (int i = 0; i < fm->NumPackBlocks(); i++) {
          BlockInfo * bi = fm->GetPackBlock(i);
          int bMBLen = bi->MBLen();
          int bMBStart = bi->MBStart();
          int bFMLen = bi->FMLen();
          int bFMStart = bi->FMStart();
          Dtype *src = (Dtype*) from;
          Dtype *dst = (Dtype*) (to + bi->BufOffset());
          for (int mb = 0; mb < bMBLen; mb++) {
              for (int fm = 0; fm < bFMLen; fm++) {
                  for (int s = 0 ; s < bi->FMSize(); s++) {
                    dst[(fm*bMBLen + mb)*bi->FMSize() + s] = src[s*bFMLen*bMBLen + (bFMStart+fm)*bMBLen + (bMBStart+mb)];
                  }
              }
          }
      }
  }

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::unpack_buffer(FeatureMap *fm, const Dtype *from, Dtype *to) {
      for (int i = 0; i < fm->NumUnpackBlocks(); i++) {
          BlockInfo * bi = fm->GetUnpackBlock(i);
          int bMBLen = bi->MBLen();
          int bMBStart = bi->MBStart();
          int bFMLen = bi->FMLen();
          int bFMStart = bi->FMStart();
          Dtype *dst = (Dtype*) to;
          Dtype *src = (Dtype*) (from + bi->BufOffset());
          for (int mb = 0; mb < bMBLen; mb++) {
              for (int fm = 0; fm < bFMLen; fm++) {
                  for (int s = 0 ; s < bi->FMSize(); s++) {
                    dst[s*bFMLen*bMBLen + (bFMStart+fm)*bMBLen + (bMBStart+mb)] = src[(fm*bMBLen + mb)*bi->FMSize() + s];
                  }
              }
          }
      }
}

#endif /* USE_MLSL */

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Forward_cpu(
  const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  int status;
  size_t n, g;
  size_t iw, ih, ic;
  size_t ow, oh, oc;

  // LOG(INFO) << "use mkl conv";
  g  = this->group_;
  n  = this->num_;
  iw = this->width_;
  ih = this->height_;
  ic = this->channels_/g;

  CHECK(bottom[0]->width()    == iw &&
        bottom[0]->height()   == ih &&
        bottom[0]->channels() == ic*g &&
        bottom[0]->num()      == n)
          << "Inclompatible shape of bottom with layer";

  ow = this->width_out_;
  oh = this->height_out_;
  oc = this->num_output_/g;
  CHECK(top[0]->width()    == ow &&
        top[0]->height()   == oh &&
        top[0]->channels() == oc*g &&
        top[0]->num()      == n) << "Inclompatible shape of bottom with layer";


  // bool need_log = !layer_name.compare("conv1") || !layer_name.compare("rfcn_cls");
  bool need_log = false;

  if (need_log) {
  	LOG(ERROR) << "input image number: " << n << " width: " << iw << " height: " << ih << " channel: " << ic;
	LOG(ERROR) << "output image width: " << ow << " height: " << oh << " channel: " << oc;
  }

  if (need_log) {
  	timer.Start();
  }

  void *res_convolutionFwd[dnnResourceNumber];
  res_convolutionFwd[dnnResourceSrc] =
    fwd_bottom_data->get_converted_prv(bottom[0], false);
  res_convolutionFwd[dnnResourceFilter] =
    fwd_filter_data->get_converted_prv(this->blobs_[0].get(), true);
  if (this->bias_term_) {
    res_convolutionFwd[dnnResourceBias] =
      fwd_bias_data->get_converted_prv(this->blobs_[1].get(), true);
  }

  double elapsed;
  if (need_log) {
      elapsed = timer.MicroSeconds();
      LOG(ERROR) << "timer 1: " << elapsed << " us";
      timer.Start();
  }

  if (fwd_top_data->conversion_needed()) {
    if (need_log) {
        LOG(ERROR) << "convert layout";
    }
    top[0]->set_prv_data_descriptor(fwd_top_data);
    res_convolutionFwd[dnnResourceDst] =
            reinterpret_cast<void *>(top[0]->mutable_prv_data());
  } else {
    res_convolutionFwd[dnnResourceDst] = top[0]->mutable_cpu_data();
  }

  if (need_log) {
    elapsed = timer.MicroSeconds();
    LOG(ERROR) << "timer 2: " << elapsed << " us";
    timer.Start();
  }


  status = dnnExecute<Dtype>(convolutionFwd, res_convolutionFwd);
  CHECK_EQ(status, 0) << "Forward convolution failed with status " << status;

  // dump conv output
#if 0
  static int cnt = 0;
  if (cnt == 0) {
    FILE *fp = fopen("./conv1_mkl.txt", "wb");
    const Dtype* top_data = top[0]->cpu_data();
    int i = 0;
    for (int n = 0; n < top[0]->num(); n++) {
      for (int c = 0; c < 1; c++) {
        for (int h = 0; h < top[0]->height(); h++) {
          for (int w = 0; w < top[0]->width(); w++) {
            fprintf(fp, "%.2f, ", top_data[i]);
            i++;
          }
        }
      }
    }
   fclose(fp);
  }
  cnt++;
#endif


  if (need_log) {
    elapsed = timer.MicroSeconds();
    LOG(ERROR) << "timer 3: " << elapsed << " us";
  }
}

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Backward_cpu(
  const vector<Blob<Dtype>*>& top, const vector<bool>& propagate_down,
  const vector<Blob<Dtype>*>& bottom) {
  int status;
  size_t n, g;
  size_t iw, ih, ic;
  size_t ow, oh, oc;

  g  = this->group_;
  n  = this->num_;
  iw = this->width_;
  ih = this->height_;
  ic = this->channels_/g;

  CHECK(bottom[0]->width()    == iw &&
        bottom[0]->height()   == ih &&
        bottom[0]->channels() == ic*g &&
        bottom[0]->num()      == n)
          << "Incompatible shape of bottom with layer";

  ow = this->width_out_;
  oh = this->height_out_;
  oc = this->num_output_/g;
  CHECK(top[0]->width()    == ow &&
        top[0]->height()   == oh &&
        top[0]->channels() == oc*g &&
        top[0]->num()      == n) << "Incompatible shape of bottom with layer";

  // bool need_log = !layer_name.compare("conv1") || !layer_name.compare("rfcn_cls");
  bool need_log = false;
  if (need_log) {
  	LOG(ERROR) << layer_name << " data back propagation: " << propagate_down[0];
  }
  START_TIMER();
  if (propagate_down[0]) {
    void *res_convolutionBwdData[dnnResourceNumber];

    Timer a;
    if (need_log) {
        a.Start();
    }
    res_convolutionBwdData[dnnResourceDiffDst] =
      bwdd_top_diff->get_converted_prv(top[0], true);
    // Currently this conversion adds padding to weights.
    // We don't want that to be stored in the weights prv_ptr_
    res_convolutionBwdData[dnnResourceFilter]  =
      bwdd_filter_data->get_converted_prv(this->blobs_[0].get(), false);

    if (need_log) {
        LOG(ERROR) << "get prv data: " << a.MicroSeconds() / 1000. << " ms";
        LOG(ERROR) << "diff convert: " << bwdd_bottom_diff->conversion_needed();
        a.Start();
    }

    if (bwdd_bottom_diff->conversion_needed()) {
      bottom[0]->set_prv_diff_descriptor(bwdd_bottom_diff);
      res_convolutionBwdData[dnnResourceDiffSrc] =
              bottom[0]->mutable_prv_diff();
    } else {
      res_convolutionBwdData[dnnResourceDiffSrc] =
              bottom[0]->mutable_cpu_diff();
    }

    if (need_log) {
        LOG(ERROR) << "bottom diff data: " << a.MicroSeconds() / 1000. << " ms";
        a.Start();
    }

    status = dnnExecute<Dtype>(convolutionBwdData, res_convolutionBwdData);
    CHECK_EQ(status, 0) << "Backward Data conv failed with status " << status;

    if (need_log) {
        LOG(ERROR) << "execute: " << a.MicroSeconds() / 1000. << " ms";
    }
  }
  STOP_TIMER("data back propagation");

  if (need_log) {
      LOG(ERROR) << "weight back propagation: " << this->param_propagate_down(0);
  }
  START_TIMER();
  if (this->param_propagate_down(0)) {
    void *res_convolutionBwdFilter[dnnResourceNumber];

    Timer b;

    if (need_log) {
      b.Start();
    }
    res_convolutionBwdFilter[dnnResourceDiffDst] =
            bwdf_top_diff->get_converted_prv(top[0], true);
    // The last get_converted_prv() argument is a hack for reusing conversion
    // done already in the forward direction.
    res_convolutionBwdFilter[dnnResourceSrc] =
            bwdf_bottom_data->get_converted_prv(bottom[0], false,
            fwd_bottom_data.get());

    if (bwdf_filter_diff->conversion_needed()) {
      this->blobs_[0]->set_prv_diff_descriptor(bwdf_filter_diff);
    }
    if (bwdf2fwd_filter_diff->conversion_needed()) {
      // Different layouts in fwd filters vs bwd diffs
      res_convolutionBwdFilter[dnnResourceDiffFilter] =
              reinterpret_cast<void *>(bwdf2fwd_filter_diff->prv_ptr());
    } else {
      if (Caffe::iter_size() > 1) {
        // if (iter_size > 1) then diffs are accumulated across iterations
        res_convolutionBwdFilter[dnnResourceDiffFilter] =
              bwdf_filter_diff_iter->prv_ptr();
      } else {
        if (bwdf_filter_diff->conversion_needed()) {
          res_convolutionBwdFilter[dnnResourceDiffFilter] =
                this->blobs_[0]->mutable_prv_diff();
        } else {
        res_convolutionBwdFilter[dnnResourceDiffFilter] =
              this->blobs_[0]->mutable_cpu_diff();
        }
      }
    }
    status = dnnExecute<Dtype>(convolutionBwdFilter, res_convolutionBwdFilter);
    CHECK_EQ(status, 0) << "Backward Filter conv failed with status " << status;

    if (need_log) {
      LOG(ERROR) << "bwd: " << b.MicroSeconds() / 1000. << " ms";
      b.Start();
    }

    // LOG(ERROR) << "iter size: " << Caffe::iter_size();
    if (bwdf2fwd_filter_diff->conversion_needed()) {
      // Different layouts in fwd filters vs bwd diffs
      void *convert_resources[dnnResourceNumber];
      convert_resources[dnnResourceFrom] = bwdf2fwd_filter_diff->prv_ptr();

      if (Caffe::iter_size() > 1) {
        // if (iter_size > 1) then diffs are accumulated across iterations
        convert_resources[dnnResourceTo] =
              bwdf_filter_diff_iter->prv_ptr();
        if (bwdf_filter_diff->conversion_needed())
          DLOG(INFO) << "convert priv => priv  " << bwdf2fwd_filter_diff->name
                     << " => " << bwdf_filter_diff->name;
        else
          DLOG(INFO) << "convert priv =>       " << bwdf2fwd_filter_diff->name
                     << " =>";
      } else {
        if (bwdf_filter_diff->conversion_needed()) {
          convert_resources[dnnResourceTo] =
                this->blobs_[0]->mutable_prv_diff();
          DLOG(INFO) << "convert priv => priv  " << bwdf2fwd_filter_diff->name
                     << " => " << bwdf_filter_diff->name;
        } else {
          convert_resources[dnnResourceTo] =
                this->blobs_[0]->mutable_cpu_diff();
          DLOG(INFO) << "convert priv =>       " << bwdf2fwd_filter_diff->name
                     << " =>";
        }
      }

      status = dnnExecute<Dtype>(bwdf2fwd_filter_diff->convert_from_int,
              convert_resources);
      CHECK_EQ(status, 0) << "Conversion failed with status " << status;
    }

    if (need_log) {
        LOG(ERROR) << "convert weight diff to user: " << b.MicroSeconds() / 1000. << " ms";
        b.Start();
    }

    if (Caffe::iter_size() > 1) {
      // if (iter_size > 1) then diffs are accumulated across iterations
      if (bwdf_filter_diff->conversion_needed()) {
        caffe_axpy<Dtype>((const int)this->blobs_[0]->prv_diff_count(), 1,
              reinterpret_cast<Dtype*>(bwdf_filter_diff_iter->prv_ptr()),
              this->blobs_[0]->mutable_prv_diff());
      } else {
        caffe_axpy<Dtype>((const int)this->blobs_[0]->count(), 1,
              reinterpret_cast<Dtype*>(bwdf_filter_diff_iter->prv_ptr()),
              this->blobs_[0]->mutable_cpu_diff());
      }
    }
    if (need_log) {
        LOG(ERROR) << "need filter diff conversion:  " << bwdf_filter_diff->conversion_needed();
        LOG(ERROR) << "filter smooth takes: " << b.MicroSeconds() / 1000. << " ms";
    }
  }
  STOP_TIMER("weight back propagation");


  if (need_log) {
      LOG(ERROR) << "bias back propagation: " << this->param_propagate_down(1);
  }
  START_TIMER();
  if (this->param_propagate_down(1)) {
    void *res_convolutionBwdBias[dnnResourceNumber];

    res_convolutionBwdBias[dnnResourceDiffDst] =
            bwdb_top_diff->get_converted_prv(top[0], true);
    if (Caffe::iter_size() > 1) {
      // if (iter_size > 1) then diffs are accumulated across iterations
      res_convolutionBwdBias[dnnResourceDiffBias] =
            bwdb_bias_diff_iter->prv_ptr();
    } else {
      if (bwdb_bias_diff->conversion_needed()) {
        this->blobs_[1]->set_prv_diff_descriptor(bwdb_bias_diff);
          res_convolutionBwdBias[dnnResourceDiffBias] =
              reinterpret_cast<void *>(this->blobs_[1]->mutable_prv_diff());

      } else {
        res_convolutionBwdBias[dnnResourceDiffBias] =
            reinterpret_cast<void *>(this->blobs_[1]->mutable_cpu_diff());
      }
    }

    status = dnnExecute<Dtype>(convolutionBwdBias, res_convolutionBwdBias);
    CHECK_EQ(status, 0) << "Backward Bias failed with status " << status;

    if (Caffe::iter_size() > 1) {
      // if (iter_size > 1) then diffs are accumulated across iterations
      if (bwdb_bias_diff->conversion_needed()) {
        caffe_axpy<Dtype>((const int)this->blobs_[1]->prv_diff_count(), 1,
              reinterpret_cast<Dtype*>(bwdb_bias_diff_iter->prv_ptr()),
              this->blobs_[1]->mutable_prv_diff());
      } else {
        caffe_axpy<Dtype>((const int)this->blobs_[1]->count(), 1,
              reinterpret_cast<Dtype*>(bwdb_bias_diff_iter->prv_ptr()),
              this->blobs_[1]->mutable_cpu_diff());
      }
    }
  }
  STOP_TIMER("bias back propagation");
}

#ifdef CPU_ONLY
STUB_GPU(MKLConvolutionLayer);
#else
template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Forward_gpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top)
  {NOT_IMPLEMENTED;}
template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Backward_gpu(
    const vector<Blob<Dtype>*>& top, const vector<bool>& propagate_down,
    const vector<Blob<Dtype>*>& bottom)
  {NOT_IMPLEMENTED;}
#endif

INSTANTIATE_CLASS(MKLConvolutionLayer);
}  // namespace caffe
#endif  // #ifdef MKL2017_SUPPORTED
