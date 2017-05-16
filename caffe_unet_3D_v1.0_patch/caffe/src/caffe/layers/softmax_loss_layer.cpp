#include <algorithm>
#include <cfloat>
#include <vector>

#include "caffe/layers/softmax_loss_layer.hpp"
#include "caffe/util/math_functions.hpp"

namespace caffe {

template <typename Dtype>
void SoftmaxWithLossLayer<Dtype>::LayerSetUp(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  LossLayer<Dtype>::LayerSetUp(bottom, top);
  LayerParameter softmax_param(this->layer_param_);
  softmax_param.set_type("Softmax");
  softmax_layer_ = LayerRegistry<Dtype>::CreateLayer(softmax_param);
  softmax_bottom_vec_.clear();
  softmax_bottom_vec_.push_back(bottom[0]);
  softmax_top_vec_.clear();
  softmax_top_vec_.push_back(&prob_);
  softmax_layer_->SetUp(softmax_bottom_vec_, softmax_top_vec_);

  has_ignore_label_ =
    this->layer_param_.loss_param().has_ignore_label();
  if (has_ignore_label_) {
    ignore_label_ = this->layer_param_.loss_param().ignore_label();
  }
  if (!this->layer_param_.loss_param().has_normalization() &&
      this->layer_param_.loss_param().has_normalize()) {
    normalization_ = this->layer_param_.loss_param().normalize() ?
                     LossParameter_NormalizationMode_VALID :
                     LossParameter_NormalizationMode_BATCH_SIZE;
  } else {
    normalization_ = this->layer_param_.loss_param().normalization();
  }
}

template <typename Dtype>
void SoftmaxWithLossLayer<Dtype>::Reshape(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  LossLayer<Dtype>::Reshape(bottom, top);
  softmax_layer_->Reshape(softmax_bottom_vec_, softmax_top_vec_);
  softmax_axis_ =
      bottom[0]->CanonicalAxisIndex(this->layer_param_.softmax_param().axis());
  outer_num_ = bottom[0]->count(0, softmax_axis_);
  inner_num_ = bottom[0]->count(softmax_axis_ + 1);
  CHECK_EQ(outer_num_ * inner_num_, bottom[1]->count())
      << "Number of labels must match number of predictions; "
      << "e.g., if softmax axis == 1 and prediction shape is (N, C, H, W), "
      << "label count (number of labels) must be N*H*W, "
      << "with integer values in {0, 1, ..., C-1}.";
  if( bottom.size() == 3) {
    CHECK_EQ(outer_num_ * inner_num_, bottom[2]->count())
        << "Number of weights must match number of predictions; "
        << "e.g., if softmax axis == 1 and prediction shape is (N, C, H, W), "
        << "weight count (number of weights) must be N*H*W, "
        << "with positive float values";
  }
  if (top.size() >= 2) {
    // softmax output
    top[1]->ReshapeLike(*bottom[0]);
  }
}

template <typename Dtype>
Dtype SoftmaxWithLossLayer<Dtype>::get_normalizer(
    LossParameter_NormalizationMode normalization_mode, int valid_count) {
  Dtype normalizer;
  switch (normalization_mode) {
    case LossParameter_NormalizationMode_FULL:
      normalizer = Dtype(outer_num_ * inner_num_);
      break;
    case LossParameter_NormalizationMode_VALID:
      if (valid_count == -1) {
        normalizer = Dtype(outer_num_ * inner_num_);
      } else {
        normalizer = Dtype(valid_count);
      }
      break;
    case LossParameter_NormalizationMode_BATCH_SIZE:
      normalizer = Dtype(outer_num_);
      break;
    case LossParameter_NormalizationMode_NONE:
      normalizer = Dtype(1);
      break;
    default:
      LOG(FATAL) << "Unknown normalization mode: "
          << LossParameter_NormalizationMode_Name(normalization_mode);
  }
  // Some users will have no labels for some examples in order to 'turn off' a
  // particular loss in a multi-task setup. The max prevents NaNs in that case.
  return std::max(Dtype(1.0), normalizer);
}

template <typename Dtype>
void SoftmaxWithLossLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  // The forward pass computes the softmax prob values.
  softmax_layer_->Forward(softmax_bottom_vec_, softmax_top_vec_);
  const Dtype* prob_data = prob_.cpu_data();
  const Dtype* label = bottom[1]->cpu_data();
  int dim = prob_.count() / outer_num_;
  int max_label_value = -1;
  if(bottom.size() == 2 ) {
    // original version with equally weighted pixels
    int count = 0;
    Dtype loss = 0;
    for (int i = 0; i < outer_num_; ++i) {
      for (int j = 0; j < inner_num_; j++) {
	const int label_value = static_cast<int>(label[i * inner_num_ + j]);
	if (has_ignore_label_ && label_value == ignore_label_) {
	  continue;
	}
	DCHECK_GE(label_value, 0);
	DCHECK_LT(label_value, prob_.shape(softmax_axis_));
	loss -= log(std::max(prob_data[i * dim + label_value * inner_num_ + j],
			     Dtype(FLT_MIN)));
	++count;
	if (label_value>max_label_value)
	{
	  max_label_value = label_value;	
	}
      }
    }
    top[0]->mutable_cpu_data()[0] = loss / get_normalizer(normalization_, count);
  } else {
    // version with pixel-wise loss weights using a third input blob
    const Dtype* weight_data = bottom[2]->cpu_data();
    Dtype weightsum = 0;
    Dtype loss = 0;
    for (int i = 0; i < outer_num_; ++i) {
      for (int j = 0; j < inner_num_; j++) {
        const int label_value = static_cast<int>(label[i * inner_num_ + j]);
        if (has_ignore_label_ && label_value == ignore_label_) {
          continue;
        }
        DCHECK_GE(label_value, 0);
        DCHECK_LT(label_value, prob_.shape(softmax_axis_));
        loss -= weight_data[i * inner_num_ + j] *
            log(std::max(prob_data[i * dim + label_value * inner_num_ + j],
                             Dtype(FLT_MIN)));
        weightsum += weight_data[i * inner_num_ + j];
	if (label_value>max_label_value)
	{
	  max_label_value = label_value;	
	}
      }
    }
    if( weightsum == 0) {
      LOG(INFO) << this->type()
		<< " warning: sum of pixel wise loss weights is zero!";
      top[0]->mutable_cpu_data()[0] = 0;
    } else {
      top[0]->mutable_cpu_data()[0] = loss / get_normalizer(normalization_, weightsum);
    }
  }
  LOG(INFO) << this->type() << "max label value: " << max_label_value;
}

template <typename Dtype>
void SoftmaxWithLossLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
    const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  if (propagate_down[1]) {
    LOG(FATAL) << this->type()
               << " Layer cannot backpropagate to label inputs.";
  }
  if (propagate_down[0]) {
    Dtype* bottom_diff = bottom[0]->mutable_cpu_diff();
    const Dtype* prob_data = prob_.cpu_data();
    caffe_copy(prob_.count(), prob_data, bottom_diff);
    const Dtype* label = bottom[1]->cpu_data();
    int dim = prob_.count() / outer_num_;
    if( bottom.size() == 2) {
      // original version with equally weighted pixels
      int count = 0;
      for (int i = 0; i < outer_num_; ++i) {
	for (int j = 0; j < inner_num_; ++j) {
	  const int label_value = static_cast<int>(label[i * inner_num_ + j]);
	  if (has_ignore_label_ && label_value == ignore_label_) {
	    for (int c = 0; c < bottom[0]->shape(softmax_axis_); ++c) {
	      bottom_diff[i * dim + c * inner_num_ + j] = 0;
	    }
	  } else {
	    bottom_diff[i * dim + label_value * inner_num_ + j] -= 1;
	    ++count;
	  }
	}
      }
      // Scale gradient
      Dtype loss_weight = top[0]->cpu_diff()[0] /
	get_normalizer(normalization_, count);
      caffe_scal(prob_.count(), loss_weight, bottom_diff);
    } else {
      // version with pixel-wise loss weights using a third input blob
      const Dtype* weight_data = bottom[2]->cpu_data();
      Dtype weightsum = 0;
      for (int i = 0; i < outer_num_; ++i) {
        for (int j = 0; j < inner_num_; ++j) {
          const int label_value = static_cast<int>(label[i * inner_num_ + j]);
          if (has_ignore_label_ && label_value == ignore_label_) {
            for (int c = 0; c < bottom[0]->shape(softmax_axis_); ++c) {
              bottom_diff[i * dim + c * inner_num_ + j] = 0;
            }
          } else {
            bottom_diff[i * dim + label_value * inner_num_ + j] -= 1;
            Dtype weight = weight_data[i * inner_num_ + j];
            for (int c = 0; c < bottom[0]->shape(softmax_axis_); ++c) {
              bottom_diff[i * dim + c * inner_num_ + j] *= weight;
            }
            weightsum += weight;
          }
        }
      }
      // Scale gradient
      const Dtype loss_weight = top[0]->cpu_diff()[0];
      if( weightsum == 0) {
	LOG(INFO) << this->type()
		  << " warning: sum of pixel wise loss weights is zero!";
	//top[0]->mutable_cpu_data()[0] = 0;
      } else {
          caffe_scal(prob_.count(), loss_weight/weightsum, bottom_diff);
      }
    }
  }
}

#ifdef CPU_ONLY
STUB_GPU(SoftmaxWithLossLayer);
#endif

INSTANTIATE_CLASS(SoftmaxWithLossLayer);
REGISTER_LAYER_CLASS(SoftmaxWithLoss);

}  // namespace caffe