#include "stdafx.h"
#include "network.h"
#include "param_pool.h"
#include "yolo.h"
#include "box.h"
YoloModule::YoloModule(const XMLElement* element, Layer* l, TensorOrder order) {
	layer = l;
	x_desc = NULL;
	y_desc = NULL;
	input_width = 0;
	input_height = 0;
	GetPrevModules(element);
	threshold_ignore = element->FloatAttribute("ignore-thresh",0.5);
	threshold_thruth = element->FloatAttribute("truth-thresh", 1.0);
	focal_loss = element->BoolAttribute("focal-loss", false);

	const char* s = element->Attribute("anchor-masks");
	string anch_str(s ? s : "");
	
	vector<string> strs;
	split_string(strs, anch_str);
	AnchorBoxItem abi;
	for (string& s : strs) {
		abi.masked_index = atoi(s.c_str()); 
		if (GetNetwork().GetAnchor(abi.masked_index, abi.width, abi.height)) { 
			masked_anchors.push_back(abi);
		}
		
	}
	features = input_channels / masked_anchors.size();
	classes = features - 5;
	if (classes < 0) { // exceptional situation
		features = 6;
		classes = 1;
	}
}
YoloModule::~YoloModule() {
}
enum {
	INDEX_PROBILITY_X = 0,
	INDEX_PROBILITY_Y,  // 1
	INDEX_PROBILITY_W,  // 2
	INDEX_PROBILITY_H,  // 3
	INDEX_CONFIDENCE,   // 4
	INDEX_PROBILITY_CLASS_0,
	INDEX_PROBILITY_CLASS_1,
	INDEX_PROBILITY_CLASS_2,
	INDEX_PROBILITY_CLASS_3
	//...
};
bool debugging = false;
static Box get_yolo_box(const float *data, const AnchorBoxItem& anchor, int x, int y, float reciprocal_w, float reciprocal_h, int stride) {
	Box b;
	b.x = (*data + x) * reciprocal_w;// reciprocal_w == 1 / w 
	data += stride;
	b.y = (*data + y) * reciprocal_h;// reciprocal_h == 1 / h 
	data += stride;
	if (*data < 10.0) // to avoid overflow
		b.w = exp(*data) * anchor.width;
	else
		b.w = 0.00001;

	data += stride;
	if (*data < 10.0)
		b.h = exp(*data) * anchor.height;
	else
		b.h = 0.00001;
	return b;
}
 
int YoloModule::EntryIndex(int anchor, int loc, int entry) {
 
	return (anchor * features + entry) * output.Elements2D() + loc;
}
void YoloModule::DeltaBackground(float* data, float* delta, ObjectInfo* truths, int max_boxes, float& avg_anyobj) {
	float reciprocal_w = 1.0 / output.GetWidth();
	float reciprocal_h = 1.0 / output.GetHeight(); 
	for (int y = 0, offset = 0; y < output.GetHeight(); y++) {
		for (int x = 0; x < output.GetWidth(); x++, offset++) {
			for (int n = 0; n < masked_anchors.size(); n++) { // l.n == 3
															  // sigmoid(tx),sigmoid(ty),tw,th,Pr(Obj),Pr(Cls_0|Obj),Pr(Cls_0|Obj)...
				int box_index = EntryIndex(n, offset, INDEX_PROBILITY_X); 
				Box pred = get_yolo_box(data + box_index, masked_anchors[n],
					x, y, reciprocal_w, reciprocal_h, output.Elements2D());
				float best_iou = 0;
				int best_t = 0;
				for (int t = 0; t < max_boxes; t++) {
					ObjectInfo& truth = truths[t];
					if (truth.class_id < 0) break;
					float iou = BoxIoU(pred, Box(truth));
					if (iou > best_iou) { best_iou = iou; best_t = t; }
				}
				// class_index : point to Pr(Obj)
				int object_index = box_index + (INDEX_CONFIDENCE - INDEX_PROBILITY_X) * output.Elements2D();
				avg_anyobj += data[object_index]; 
				if (best_iou > threshold_ignore) {					
					delta[object_index] = 0.0f;//初始化全是背景，离目标较近的忽略，梯度为0
				} 
				else {
					delta[object_index] = 0.0f - data[object_index];
				}

				// 更近的视为目标(不过这一步效果不太好，所以阈值设为1，永远不执行)
				if (best_iou > threshold_thruth) {
					delta[object_index] = 1 - data[object_index];
					ObjectInfo& truth = truths[best_t];//network->Truth(b, best_t);
					int class_id = truth.class_id;
					int class_index = object_index + output.Elements2D(); // * (INDEX_PROBILITY_CLASS_0 - INDEX_CONFIDENCE) ;
					DeltaClass(data, delta, class_id, class_index);
					DeltaBox(data,delta,truth, masked_anchors[n], box_index, x, y);

				}
			} // masked_anchor
		} // x
	}// y 
}
void YoloModule::DeltaBox(float* data, float* delta, const ObjectInfo& truth, const AnchorBoxItem& anchor, int index, int x, int y) {

	float tx = (truth.x * output.GetWidth() - x);
	float ty = (truth.y * output.GetHeight() - y);
	float tw = log(truth.w / anchor.width);
	float th = log(truth.h / anchor.height);
	float scale = (2.0f - truth.w * truth.h);

	delta[index] = scale * (tx - data[index]);
	index += output.Elements2D();
	delta[index] = scale * (ty - data[index]);
	index += output.Elements2D();
	delta[index] = scale * (tw - data[index]);
	index += output.Elements2D();
	delta[index] = scale * (th - data[index]);
}
bool YoloModule::InitDescriptors(bool trainning) {
	return true;
}
void YoloModule::DeltaClass(float* data, float* delta, int class_id, int index, float* avg_cat) {

	int ti = index;
	if (class_id > 0)  ti += output.Elements2D() * class_id;
	if (delta[ti]) { //TOCHECK: always false?
		delta[ti] = 1 - data[ti];
		if(avg_cat) *avg_cat += data[ti];
		return;
	}
	// Focal loss
	if (focal_loss) {
		// Focal Loss
		float alpha = 0.5f;    // 0.25 or 0.5
							   //float gamma = 2;    // hardcoded in many places of the grad-formula


		float grad = 1.0f, pt = data[ti];
		// http://fooplot.com/#W3sidHlwZSI6MCwiZXEiOiItKDEteCkqKDIqeCpsb2coeCkreC0xKSIsImNvbG9yIjoiIzAwMDAwMCJ9LHsidHlwZSI6MTAwMH1d

		// http://blog.csdn.net/linmingan/article/details/77885832
		if (pt > 0.0f) grad = (pt - 1.0f) * (2.0f * pt * logf(pt) + pt - 1.0f);

		grad *= alpha;
		for (int n = 0; n < classes; n++, index += output.Elements2D()) {
			delta[index] = (((n == class_id) ? 1.0f : 0.0f) - data[index]);
			delta[index] *= grad;
			if (n == class_id) {
				if(avg_cat) *avg_cat += data[index];
			}
		}
	}
	else {
		// default
		for (int n = 0; n < classes; n++, index += output.Elements2D()) {
			delta[index] = ((n == class_id) ? 1 : 0) - data[index];
			if (n == class_id && avg_cat) {
				*avg_cat += data[index];
			}
		}
	}
}
 
bool YoloModule::Forward(ForwardContext& context) {
	if (!InferenceModule::Forward(context)) return false;
	int b, n;
	output = input;

	if (shortcut_delta.SameDememsion(input))
		shortcut_delta = 0.0f;
	else if (!shortcut_delta.InitFrom(input))
		return false;

	float* output_gpu = output.GetMem();
 
	bool ret = true;
	for (b = 0; b < output.GetBatch(); b++) {
		for (n = 0; n < masked_anchors.size(); n++) {
			int index = EntryIndex(n, 0, INDEX_PROBILITY_X);
			if (!activate_array_ongpu(output_gpu + index, 2 * output.Elements2D(), LOGISTIC)) {
				return false;
			}
			index = EntryIndex( n, 0, INDEX_CONFIDENCE);
			if (!activate_array_ongpu(output_gpu + index, (1 + classes) * output.Elements2D(), LOGISTIC))
				return false;
		}
		output_gpu += output.Elements3D();
	} 
	if (!context.training) return true;

	float cost = 0.0;
	float avg_iou = 0, recall = 0, recall75 = 0, avg_cat = 0;
	float avg_obj = 0, avg_anyobj = 0;
	int count = 0, class_count = 0;
	float reciprocal_w = 1.0f / input.GetWidth();
	float reciprocal_h = 1.0f / output.GetHeight();

#if 0
	char filename[MAX_PATH];
	sprintf(filename, "E:\\AI\\Data\\debugging\\RQNet\\%s.forward.yolo.activation.bin", layer->GetName().c_str());
	ofstream of(filename, ios::binary | ios::trunc);
	if (of.is_open()) {
		of.write(reinterpret_cast<char*>(output_cpu), output.Elements3D() * sizeof(float));
		of.close();
	}
#endif
	int  max_boxes = context.max_truths_per_batch;
	ObjectInfo* batch_truths = context.truths;
	float* delta_cpu = New float[output.Elements3D()];
	float* output_cpu = New float[output.Elements3D()]; 

	size_t batch_bytes = output.Elements3D() * sizeof(float);
	
	for (int b = 0; b < output.GetBatch(); b++, batch_truths += max_boxes) {
		if (!output.Get3DData(b, output_cpu)) {
			delete[]delta_cpu;
			delete[]output_cpu;
			return false;
		}
		debugging = (12 == b);
		memset(delta_cpu, 0, batch_bytes); 
		DeltaBackground(output_cpu, delta_cpu, batch_truths, max_boxes, avg_anyobj);
		for (int t = 0; t < max_boxes; t++) {
			ObjectInfo& truth = batch_truths[t];
			if (truth.class_id < 0) break;
			float best_iou = 0;
			int n, best_n = 0, mask_n = -1;
			int x = (truth.x * output.GetWidth());
			int y = (truth.y * output.GetHeight());
			
			Box truth_shift(0, 0, truth.w, truth.h);
			float w, h;
			for (n = 0; n < GetNetwork().GetAnchorCount(); n++) {
				GetNetwork().GetAnchor(n, w, h);
				Box pred(0, 0, w, h);
				float iou = BoxIoU(pred, truth_shift);
				if (iou > best_iou) {
					best_iou = iou;
					best_n = n;
				}
			}
			for (n = 0; n < masked_anchors.size(); n++) {
				if (best_n == masked_anchors[n].masked_index) {
					mask_n = n;
					break;
				}
			}
			if (mask_n >= 0) { // found matched anchor
				int offset = y * output.GetWidth() + x;
				int box_index = EntryIndex( mask_n, offset, INDEX_PROBILITY_X);
				DeltaBox(output_cpu, delta_cpu, truth, masked_anchors[mask_n], box_index, x, y);

				int object_index = box_index + (INDEX_CONFIDENCE - INDEX_PROBILITY_X) * output.Elements2D();
				avg_obj += output_cpu[object_index];
				delta_cpu[object_index] = 1 - output_cpu[object_index];

				int class_id = truth.class_id;//context.truths[t*(4 + 1) + b*l.truths + 4];
 
				int class_index = object_index + output.Elements2D() /* *(INDEX_PROBILITY_CLASS_0 - INDEX_CONFIDENCE) */;
				DeltaClass(output_cpu, delta_cpu, class_id, class_index, &avg_cat);

				count++;
				class_count++;

				Box pred = get_yolo_box(output_cpu + box_index, masked_anchors[n],
					x, y, reciprocal_w, reciprocal_h, output.Elements2D());
				float iou = BoxIoU(pred, Box(truth));

				if (iou > 0.5f) recall += 1.0;
				if (iou > 0.75f) recall75 += 1.0;
				avg_iou += iou;
			}
		}
		shortcut_delta.Set3DData(b, delta_cpu, true);
		cost += square_sum_array(delta_cpu, output.Elements3D());
#if 0
		if (12 == b) {
			char filename[MAX_PATH];
			sprintf(filename, "E:\\AI\\Data\\debugging\\RQNet\\%s_delta_one_c.txt", layer->GetName().c_str());
			char line[20];
			ofstream of(filename, ios::trunc);
			int index = 0;
			if (of.is_open()) {
				for (int c = 0; c < output.GetChannels(); c++) {
					of << "c - " << c << "\n";
					for (int y = 0; y < output.GetHeight(); y++) {
						for (int x = 0; x < output.GetWidth(); x++) {
							sprintf(line, "%.6f ", delta_cpu[index++]);
							of << line;
						}
						of << "\n";
					}

				}
				of.close();
			}
		}
#endif
	}

	delete[]output_cpu;
	delete[]delta_cpu;
	GetNetwork().RegisterLoss(cost);
	 
	ostringstream oss;
	avg_anyobj /= output.MemElements();
	oss << "Layer<"<< layer->GetIndex() <<">: " << count << " objects." ;
	if (count > 0) {
		avg_iou /= count;
		avg_obj /= count;
		recall /= count;
		recall75 /= count;
		
		if (class_count > 0) {
			avg_cat /= class_count;			
			oss << setprecision(4) << " Avg IoU: " << avg_iou << ", cls: " << avg_cat << ", obj:" << avg_obj 
			 << ", No Obj: "<< avg_anyobj <<", Recall: "<< recall <<"(50%), "<< recall75 <<"(75%).\n" ;
		}
		else {
			oss << setprecision(4) << " Avg IoU: " << avg_iou << ", obj:" << avg_obj
				<< ", No Obj: " << avg_anyobj << ", Recall: " << recall << "(50%), " << recall75 << "(75%).\n";
			 
			 
		}
		cout << oss.str();
	}
	else
		cout << "Layer<" << layer->GetIndex() << ">: " << count << " objects.\n";

	return true;
}
bool YoloModule::Backward(FloatTensor4D& delta) {
	delta = shortcut_delta;
// 	char filename[300];
// 	sprintf(filename, "E:\\AI\\Data\\debugging\\RQNet\\%s.backward.bin", layer->GetName().c_str());
// 	delta.SaveBatchData(filename, -1);
	return true;
}