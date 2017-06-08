/*

  Prequisitions:

    $ ./scripts/download_model_binary.py models/bvlc_reference_caffenet
    $ ./data/ilsvrc12/get_ilsvrc_aux.sh


  Usage:

      ./build/examples/cpp_classification/classifier.bin \
      models/bvlc_reference_caffenet/deploy.prototxt \
      models/bvlc_reference_caffenet/bvlc_reference_caffenet.caffemodel \
      data/ilsvrc12/imagenet_mean.binaryproto \
      data/ilsvrc12/synset_words.txt \
      examples/images/cat.jpg

*/


#include <string>
#include <vector>
#include <iostream>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "caffe/caffe.hpp"

// pair (label, confidence) representing a prediction
typedef std::pair<std::string, float> Prediction;

using namespace caffe;


static bool pair_compare (const std::pair<float, int>& lhs,
                          const std::pair<float, int>& rhs)
{
    return lhs.first > rhs.first;
}


/*
   Return the indices of the top N values of vector v.
*/
static std::vector<int> argmax (const std::vector<float>& v, int N)
{
    std::vector<std::pair<float, int> > pairs;
    for (size_t i = 0; i < v.size(); ++i)
        pairs.push_back (std::make_pair(v[i], i));

    std::partial_sort (pairs.begin(), pairs.begin() + N, pairs.end(), pair_compare);

    std::vector<int> result;
    for (int i = 0; i < N; ++i)
        result.push_back (pairs[i].second);

    return result;
}


class Classifier
{
    public:
        Classifier (const std::string& model_file,
                    const std::string& trained_file,
                    const std::string& mean_file,
                    const std::string& label_file);
        std::vector<Prediction> classify ( const cv::Mat& image, int N = 5 );

    private:
        int num_channels_;
        cv::Mat mean_;
        cv::Size input_geometry_;
        std::vector<std::string> labels_;
        shared_ptr<Net<float> > net_;


        void set_mean (const std::string& mean_file);
        std::vector<float> predict (const cv::Mat& image);
        void wrap_input_layer (std::vector<cv::Mat>* input_channels); 
        void preprocess (const cv::Mat& image, std::vector<cv::Mat>* input_channels);

};


Classifier::Classifier (const std::string& model_file,
                        const std::string& trained_file,
                        const std::string& mean_file,
                        const std::string& label_file)
{
    #ifdef CPU_ONLY
        Caffe::set_mode (Caffe::CPU);
    #else
        Caffe::set_mode (Caffe::GPU);
    #endif

    // load the network
    net_.reset (new Net<float>(model_file, TEST));

    // load the trained_file
    net_->CopyTrainedLayersFrom (trained_file);
    CHECK_EQ (net_->num_inputs(), 1) << "Network should have exactly one input.";
    CHECK_EQ (net_->num_outputs(), 1) << "Network should have exactly one output.";
    // blob
    Blob<float>* input_layer = net_->input_blobs()[0];
    num_channels_ = input_layer->channels();
    CHECK ( num_channels_ == 3 || num_channels_ == 1 ) << "Input layer should have 1 or 3 channels";
    // input_geometry
    input_geometry_ = cv::Size (input_layer->width(), input_layer->height());

    // load the mean file
    set_mean (mean_file);

    // load labels
    std::ifstream labels (label_file.c_str());
    CHECK (labels) << "Unable to open labels file" << label_file;
    std::string line;
    while (std::getline (labels, line))
        labels_.push_back (string(line));

    Blob<float>* output_layer = net_->output_blobs()[0];
    CHECK_EQ (labels_.size(), output_layer->channels())
            << "Number of labels is different from the output layer dimension.";
}


void Classifier::set_mean (const std::string& mean_file)
{
    BlobProto blob_proto;
    ReadProtoFromBinaryFileOrDie (mean_file.c_str(), &blob_proto);

    // convert from BlobProto to Blob<float>
    Blob<float> mean_blob;
    mean_blob.FromProto (blob_proto);
    CHECK_EQ (mean_blob.channels(), num_channels_)
             << "Number of channels of mean file doesn't match input layer.";
    // the format of the mean file is plannar 32-bit float BGR or grayscale
    std::vector<cv::Mat> channels;
    float* data = mean_blob.mutable_cpu_data();
    for (int i = 0; i < num_channels_; ++i)
    {
        // extract an individual channel
        cv::Mat channel (mean_blob.height(), mean_blob.width(), CV_32FC1, data);
        // std::cout << "channel: " << channel << std::endl;
        channels.push_back (channel);
        data += mean_blob.height() * mean_blob.width();
    }

    // merge the separate channels into a single image
    cv::Mat mean;
    cv::merge (channels, mean);

    // compute the global mean pixel value and create a mean image
    // filled with this value
    cv::Scalar channel_mean = cv::mean (mean);
    mean_ = cv::Mat (input_geometry_, mean.type(), channel_mean);
}


std::vector<Prediction> Classifier::classify ( const cv::Mat& image, int N )
{
    std::vector<float> output = predict (image);

    std::vector<Prediction> predictions;
    N = std::min<int>(labels_.size(), N);
    std::vector<int> maxN = argmax (output, N);
    std::cout << "N: " << N << std::endl;
    for (int i = 0; i < N; ++i)
    {
        int idx = maxN[i];
        std::cout << "output[idx]: " << output[idx] << std::endl;
        predictions.push_back (std::make_pair(labels_[idx], output[idx]));
    }

    return predictions;
}


std::vector<float> Classifier::predict (const cv::Mat& image)
{

    // input layer
    Blob<float>* input_layer = net_->input_blobs()[0];
    input_layer->Reshape (1, num_channels_, input_geometry_.height, input_geometry_.width);
    // forward dimension change to all layers
    net_->Reshape();

    // input_channels
    std::vector<cv::Mat> input_channels;
    wrap_input_layer (&input_channels);

    // preprocess
    preprocess (image, &input_channels);

    // forward
    net_->Forward();

    // copy the output layer to a std::vector
    Blob<float>* output_layer = net_->output_blobs()[0];
    const float* begin = output_layer->cpu_data();
    const float* end = begin + output_layer->channels();

    return std::vector<float>(begin,end);
}


/*
   Wrap the input layer of the network in separate cv::Mat objects (one per channel).
   This way we save one memcpy operatoion and we don't need to rely on cudaMemcpy2D.
   The last preprocessing operationi will write the separate channels directly to the
   input layer.
*/
void Classifier::wrap_input_layer (std::vector<cv::Mat>* input_channels)
{
    Blob<float>* input_layer = net_->input_blobs()[0];

    int width = input_layer->width();
    int height = input_layer->height();
    float* input_data = input_layer->mutable_cpu_data();

    for (int i = 0; i < input_layer->channels(); ++i)
    {
        cv::Mat channel (height, width, CV_32FC1, input_data);
        input_channels->push_back (channel);
        input_data += width * height;
    }
}


void Classifier::preprocess (const cv::Mat& image, std::vector<cv::Mat>* input_channels)
{
    // convert the input image to the input image format of the network
    cv::Mat sample;
    if (image.channels() == 3 && num_channels_ == 1)
        cv::cvtColor (image, sample, cv::COLOR_BGR2GRAY);
    else if (image.channels() == 4 && num_channels_ == 1)
        cv::cvtColor (image, sample, cv::COLOR_BGRA2GRAY);
    else if (image.channels() == 4 && num_channels_ == 3)
        cv::cvtColor (image, sample, cv::COLOR_BGRA2BGR);
    else if (image.channels() == 1 && num_channels_ == 3)
        cv::cvtColor (image, sample, cv::COLOR_GRAY2BGR);
    else
        sample = image;

    // resize
    cv::Mat sample_resized;
    if (sample.size() != input_geometry_)
        cv::resize (sample, sample_resized, input_geometry_);
    else
        sample_resized = sample;

    // sample_float
    cv::Mat sample_float;
    if (num_channels_ == 3)
        sample_resized.convertTo (sample_float, CV_32FC3);
    else
        sample_resized.convertTo (sample_float, CV_32FC1);

    // sample_normalized
    cv::Mat sample_normalized;
    cv::subtract (sample_float, mean_, sample_normalized);

    // this operation will write the separate BGR planes directly to the input layer
    // of the network because it is wrapped by the cv::Mat objects in input_channels
    cv::split (sample_normalized, *input_channels);

    CHECK (reinterpret_cast<float*>(input_channels->at(0).data) ==
           net_->input_blobs()[0]->cpu_data()) <<
           "Input channels are not wrapping the input layer of the network";
}


int main ( int argc, char** argv )
{
    if ( argc != 6 )
    {
        std::cerr << "Usage: " << argv[0]
                  << " deploy.prototxt network.caffemodel"
                  << " mean.binaryproto labels.txt img.jpg" << std::endl;
        return 1;
    }

    // If commented, the log will be shown in the console.
    // ::google::InitGoogleLogging ( argv[0] );
    std::string model_file = argv[1];
    std::string trained_file = argv[2];
    std::string mean_file = argv[3];
    std::string label_file = argv[4];
    std::string file = argv[5];

    std::cout << "classifier:" << std::endl;
    Classifier classifier ( model_file, trained_file, mean_file, label_file );

    std::cout << "------ Prediction for " << file << " -------" << std::endl;
    cv::Mat image = cv::imread (file, -1);
    CHECK (!image.empty()) << "Unable to decode image " << file;
    int N = 5;
    std::vector<Prediction> predictions = classifier.classify (image, N);

    std::cout << predictions.size() << std::endl;
    for (size_t i = 0; i < predictions.size(); ++i)
    {
        Prediction p = predictions[i];
        std::cout << std::fixed << std::setprecision(4) << p.second << " - \""
                  << p.first
                  << "\""
                  << std::endl;
    } 
}
