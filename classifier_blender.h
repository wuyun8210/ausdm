/* classifier_blender.h                                            -*- C++ -*-
   Jeremy Barnes, 1 November 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Dump the data off to a classifier.
*/

#ifndef __ausdm__classifier_blender_h__
#define __ausdm__classifier_blender_h__

#include "blender.h"
#include "jml/boosting/dense_features.h"
#include "jml/boosting/classifier.h"
#include "jml/boosting/probabilizer.h"


/*****************************************************************************/
/* CLASSIFIER_BLENDER                                                        */
/*****************************************************************************/

struct Classifier_Blender : public Blender {

    Classifier_Blender();

    virtual ~Classifier_Blender();

    virtual void configure(const ML::Configuration & config,
                           const std::string & name,
                           int random_seed,
                           Target target);
    
    virtual void init(const Data & training_data,
                      const ML::distribution<float> & example_weights);

    virtual float predict(const ML::distribution<float> & models) const;

    virtual std::string explain(const ML::distribution<float> & models) const;

    virtual distribution<float>
    get_features(const ML::distribution<float> & models) const;

    virtual distribution<float>
    get_features(const ML::distribution<float> & models,
                 const ML::distribution<float> & decomp,
                 const Target_Stats & stats) const;

    virtual boost::shared_ptr<ML::Dense_Feature_Space>
    feature_space() const;

    void generate_training_data(ML::Training_Data & cls_training_data,
                                const Data & train) const;
    
    distribution<float>
    predict_rmse_binary_features(const ML::Feature_Set & fset) const;

    std::string trainer_config_file;
    std::string trainer_name;
    
    int random_seed;

    boost::shared_ptr<ML::Dense_Feature_Space> fs;
    boost::shared_ptr<ML::Classifier_Impl>     classifier;

    std::vector<boost::shared_ptr<ML::Classifier_Impl> > classifiers;
    std::vector<ML::GLZ_Probabilizer> probabilizers;

    // For final GLM to get probabilities
    distribution<double> params;

    Target target;

    const Decomposition * decomposition;
    int nm;
    int nv;
    int ndecomposed;
    std::vector<int> recomposition_orders;
    std::vector<Model_Stats> model_stats;
    std::vector<std::string> model_names;

    bool use_decomposition_features;
    bool use_extra_features;
    bool use_recomp_features;
    bool use_regression;
    bool debug_predict;
    bool flatten_range;
};

#endif /* __ausdm__classifier_blender_h__ */


