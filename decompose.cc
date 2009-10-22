/* ausdm.cc                                                        -*- C++ -*-
   Jeremy Barnes, 6 August 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   AusDM entry.
*/

#include "data.h"

#include <fstream>
#include <iterator>
#include <iostream>

#include "arch/exception.h"
#include "utils/string_functions.h"
#include "utils/pair_utils.h"
#include "utils/vector_utils.h"
#include "utils/filter_streams.h"
#include "utils/configuration.h"
#include "arch/timers.h"
#include "utils/guard.h"
#include "arch/threads.h"

#include "boosting/worker_task.h"

#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/positional_options.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/progress.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/bind.hpp>

#include "boosting/perceptron_generator.h"
#include "algebra/matrix_ops.h"
#include "math/xdiv.h"


using namespace std;
using namespace ML;

typedef double LFloat;

struct Twoway_Layer : public Dense_Layer<LFloat> {
    Twoway_Layer(size_t inputs, size_t outputs,
                 Transfer_Function_Type transfer,
                 Thread_Context & context)
        : Dense_Layer<LFloat>(inputs, outputs, transfer, context)
    {
        ibias.resize(inputs);
    }

    distribution<LFloat> ibias;

    distribution<double> iapply(const distribution<double> & output) const
    {
        distribution<double> activation = weights * output;
        activation += ibias;
        transfer(&activation[0], &activation[0], inputs(), transfer_function);
        return activation;
    }

    distribution<float> iapply(const distribution<float> & output) const
    {
        distribution<float> activation = multiply_r<float>(weights, output);
        activation += ibias;
        transfer(&activation[0], &activation[0], inputs(), transfer_function);
        return activation;
    }

    distribution<double>
    itransfer(const distribution<double> & activation) const
    {
        int ni = inputs();
        if (activation.size() != ni)
            throw Exception("invalid sizes in itransfer");
        distribution<double> result(ni);
        transfer(&activation[0], &result[0], ni, transfer_function);
        return activation;
    }

    distribution<float>
    itransfer(const distribution<float> & activation) const
    {
        int ni = inputs();
        if (activation.size() != ni)
            throw Exception("invalid sizes in itransfer");
        distribution<float> result(ni);
        transfer(&activation[0], &result[0], ni, transfer_function);
        return activation;
    }

    distribution<double> iderivative(const distribution<double> & input) const
    {
        if (input.size() != this->inputs())
            throw Exception("iderivative(): wrong size");
        int ni = this->inputs();
        distribution<double> result(ni);
        derivative(&input[0], &result[0], ni, transfer_function);
        return result;
    }

    distribution<float> iderivative(const distribution<float> & input) const
    {
        if (input.size() != this->inputs())
            throw Exception("iderivative(): wrong size");
        int ni = this->inputs();
        distribution<float> result(ni);
        derivative(&input[0], &result[0], ni, transfer_function);
        return result;
    }
};

template<typename Float>
distribution<Float>
add_noise(const distribution<Float> & inputs,
          const distribution<Float> & cleared_values,
          Thread_Context & context,
          float prob_cleared)
{
    distribution<Float> result = inputs;

    for (unsigned i = 0;  i < inputs.size();  ++i) {
        if (context.random01() < prob_cleared)
            result[i] = cleared_values[i];
    }
    
    return result;
}

int main(int argc, char ** argv)
{
    // Filename to dump decomposition to
    string output_file;

    // Configuration file to use
    string config_file = "config.txt";

    // Name of decomposer in config file
    string decomposer_name;

    // Extra configuration options
    vector<string> extra_config_options;

    // What type of target do we predict?
    //string target_type;
    {
        using namespace boost::program_options;

        options_description config_options("Configuration");

        config_options.add_options()
            ("config-file,c", value<string>(&config_file),
             "configuration file to read configuration options from")
            ("decomposer-name,n", value<string>(&decomposer_name),
             "name of decomposer in configuration file")
            ("extra-config-option", value<vector<string> >(&extra_config_options),
             "extra configuration option=value (can go directly on command line)");

        options_description control_options("Control Options");

        control_options.add_options()
            //("target-type,t", value<string>(&target_type),
            // "select target type: auc or rmse")
            ("output-file,o",
             value<string>(&output_file),
             "dump output file to the given filename");

        positional_options_description p;
        p.add("extra-config-option", -1);

        options_description all_opt;
        all_opt
            .add(config_options)
            .add(control_options);

        all_opt.add_options()
            ("help,h", "print this message");
        
        variables_map vm;
        store(command_line_parser(argc, argv)
              .options(all_opt)
              .positional(p)
              .run(),
              vm);
        notify(vm);

        if (vm.count("help")) {
            cout << all_opt << endl;
            return 1;
        }
    }

#if 0
    Target target;
    if (target_type == "auc") target = AUC;
    else if (target_type == "rmse") target = RMSE;
    else throw Exception("target type " + target_type + " not known");

    if (decomposer_name == "")
        decomposer_name = target_type;
#endif

    // Load up configuration
    Configuration config;
    if (config_file != "") config.load(config_file);

    // Allow configuration to be overridden on the command line
    config.parse_command_line(extra_config_options);

    // Load up the data
    Timer timer;

    cerr << "loading data...";

    // The decomposition is entirely unsupervised, and so doesn't use the
    // label at all; thus we can put the training and validation sets together

    // Data[s/m/l][auc/rmse]
    Data data[3][2];

    const char * const size_names[3]   = { "S", "M", "L" };
    const char * const target_names[2] = { "RMSE", "AUC" };
    const char * const set_names[2]    = { "Train", "Score" };

    set<string> model_names;

    // (Small only for the moment until we get the hang of it)

    for (unsigned i = 0;  i < 1;  ++i) {
        for (unsigned j = 0;  j < 2;  ++j) {
            for (unsigned k = 0;  k < 1;  ++k) {
                string filename = format("download/%s_%s_%s.csv",
                                         size_names[i],
                                         target_names[j],
                                         set_names[k]);
                Data & this_data = data[i][j];

                this_data.load(filename, (Target)j, (k == 0) /* clear first */);
                
                model_names.insert(this_data.model_names.begin(),
                                   this_data.model_names.end());
            }
        }
    }

    cerr << "done" << endl;

    cerr << model_names.size() << " total models" << endl;

    // Now, we look at all of the column names to get an idea of the input
    // dimensions.  


    // Denoising auto encoder
    // We train a stack of layers, one at a time

    const Data & to_train
        = data[0][0];
    
    int nx = to_train.targets.size();
    int nm = to_train.models.size();
    int nh = nm / 2;

    Thread_Context thread_context;
    
    Twoway_Layer layer(nm, nh, TF_TANH, thread_context);

    float prob_cleared = 0.25;

    // Float type to use for calculations
    typedef double CFloat;

    distribution<CFloat> cleared_values(nm);


    double learning_rate = 1e-5;


    for (unsigned iter = 0;  iter < 10;  ++iter) {
        cerr << "iter " << iter << " training on " << nx << " examples"
             << endl;
        Timer timer;

        boost::progress_display progress(nx, cerr);

        int ni JML_UNUSED = layer.inputs();
        int no JML_UNUSED= layer.outputs();

        double total_mse = 0.0;
        
        for (unsigned x = 0;  x < nx;  ++x, ++progress) {
            // Present this input
            distribution<CFloat> model_input(nm);
            for (unsigned m = 0;  m < nm;  ++m)
                model_input[m] = 0.8 * to_train.models[m][x];
            
            // Add noise
            distribution<CFloat> noisy_input
                = add_noise(model_input, cleared_values, thread_context,
                            prob_cleared);

            distribution<CFloat> hidden_act
                = layer.activation(noisy_input);
            
            // Apply the layer
            distribution<CFloat> hidden_rep
                = layer.apply(noisy_input);
            
            // Reconstruct the input
            distribution<CFloat> denoised_input
                = layer.iapply(hidden_rep);
            
            // Error signal
            distribution<CFloat> diff
                = model_input - denoised_input;
            
            // Overall error
            double error = pow(diff.two_norm(), 2);
            
            total_mse += error;
        
            // Now we solve for the gradient direction for the two biases as
            // well as for the weights matrix
            //
            // If f() is the activation function for the forward direction and
            // g() is the activation function for the reverse direction, we can
            // write
            //
            // h = f(Wi1 + b)
            //
            // where i1 is the (noisy) inputs, h is the hidden unit outputs, W
            // is the weight matrix and b is the forward bias vector.  Going
            // back again, we then take
            // 
            // i2 = g(W*h + c) = g(W*f(Wi + b) + c)
            //
            // where i2 is the denoised approximation of the true input weights
            // (i) and W* is W transposed.
            //
            // Using the MSE, we get
            //
            // e = sqr(||i2 - i||) = sum(sqr(i2 - i))
            //
            // where e is the MSE.
            //
            // Differentiating with respect to i2, we get
            //
            // de/di2 = 2(i2 - i)
            //
            // Finally, we want to know the gradient direction for each of the
            // parameters W, b and c.  Taking c first, we get
            //
            // de/dc = de/di2 di2/dc
            //       = 2 (i2 - i) g'(i2)
            //
            // As for b, we get
            //
            // de/db = de/di2 di2/db
            //       = 2 (i2 - i) g'(i2) W* f'(Wi + b)
            //
            // And for W:
            //
            // de/dW = de/di2 di2/dW
            //       = 2 (i2 - i) g'(i2) [ h + W* f'(Wi + b) i ]
            //
            // Since we want to minimise the reconstruction error, we use the
            // negative of the gradient.

            // NOTE: here, the activation function for the input and the output
            // are the same.
        
            boost::multi_array<LFloat, 2> & W
                = layer.weights;
            distribution<LFloat> & b = layer.bias;
            distribution<LFloat> & c JML_UNUSED = layer.ibias;

            distribution<CFloat> c_updates
                = -2 * diff * layer.iderivative(denoised_input);

#if 0
            cerr << "c_updates = " << c_updates << endl;

            distribution<CFloat> c_updates_numeric(ni);

            // Calculate numerically the c updates
            for (unsigned i = 0;  i < ni;  ++i) {
                // Reconstruct the input
                distribution<CFloat> denoised_activation2
                    = W * hidden_rep + c;
                
                float epsilon = 1e-4;

                denoised_activation2[i] += epsilon;

                distribution<CFloat> denoised_input2 = denoised_activation2;
                layer.transform(denoised_input2);
            
                // Error signal
                distribution<CFloat> diff2
                    = model_input - denoised_input2;
            
                // Overall error
                double error2 = pow(diff2.two_norm(), 2);

                double delta = error2 - error;

                c_updates_numeric[i] = delta / epsilon;
            }

            cerr << "c_updates_numeric = " << c_updates_numeric
                 << endl;
#endif

            distribution<CFloat> hidden_activation
                = multiply_r<CFloat>(noisy_input, W) + b.cast<CFloat>();

            distribution<CFloat> hidden_deriv
                = layer.derivative(hidden_activation);

            distribution<CFloat> b_updates
                = multiply_r<CFloat>(c_updates, W) * hidden_deriv;

#if 0
            cerr << "b_updates = " << c_updates << endl;

            distribution<CFloat> b_updates_numeric(no);

            // Calculate numerically the c updates
            for (unsigned i = 0;  i < no;  ++i) {

                double epsilon = 1e-5;

                // Apply the layer
                distribution<CFloat> hidden_act2
                    = layer.activation(noisy_input);

                hidden_act2[i] += epsilon;
                
                distribution<CFloat> hidden_rep2
                    = layer.transfer(hidden_act2);
                
                //cerr << "hidden_rep = " << hidden_rep << endl;
                //cerr << "hidden_rep2 = " << hidden_rep2 << endl;

                distribution<CFloat> denoised_input2
                    = layer.iapply(hidden_rep2);
            
                //cerr << "denoised_input = " << denoised_input << endl;
                //cerr << "denoised_input2 = " << denoised_input2 << endl;
                    

                // Error signal
                distribution<CFloat> diff2
                    = model_input - denoised_input2;
            
                //cerr << "diff = " << diff << endl;
                //cerr << "diff2 = " << diff2 << endl;

                // Overall error
                double error2 = pow(diff2.two_norm(), 2);

                double delta = error2 - error;

                b_updates_numeric[i] = xdiv(delta, epsilon);

                cerr << "error = " << error << " error2 = " << error2
                     << " delta = " << delta
                     << " diff " << b_updates[i]
                     << " diff2 " << b_updates_numeric[i] << endl;

            }

            cerr << "b_updates_numeric = " << b_updates_numeric
                 << endl;
#endif


            boost::multi_array<double, 2> W_updates(boost::extents[ni][no]);
            boost::multi_array<double, 2> W_factors(boost::extents[ni][no]);

            distribution<double> factor_totals(no);
            distribution<double> W_factorsi(no);

            for (unsigned i = 0;  i < ni;  ++i)
                for (unsigned j = 0;  j < no;  ++j)
                    factor_totals[j] += c_updates[i] * W[i][j];

            for (unsigned i = 0;  i < ni;  ++i) {
                for (unsigned j = 0;  j < no;  ++j)
                    W_factorsi[j] = hidden_deriv[j] * model_input[i];

                for (unsigned j = 0;  j < no;  ++j)
                    W_updates[i][j]
                        = c_updates[i] * hidden_rep[j]
                        + factor_totals[j] * W_factorsi[j];
            }
            

#if 1  // test numerically
            for (unsigned i = 0;  i < ni;  ++i) {

                for (unsigned j = 0;  j < no;  ++j) {
                    double epsilon = 1e-6;

                    double old_w = W[i][j];
                    W[i][j] += epsilon;

                    cerr << "oldw " << old_w << " neww "
                         << W[i][j] << endl;

                    // Apply the layer
                    distribution<CFloat> hidden_act2
                        = layer.activation(noisy_input);

                    //cerr << "noisy_input = " << noisy_input << endl;
                    //cerr << "hidden_act = " << hidden_act << endl;
                    //cerr << "hidden_act2 = " << hidden_act2 << endl;

                    distribution<CFloat> hidden_rep2
                        = layer.transfer(hidden_act2);
                    
                    //cerr << "hidden_rep = " << hidden_rep << endl;
                    //cerr << "hidden_rep2 = " << hidden_rep2 << endl;
                    
                    distribution<CFloat> denoised_input2
                        = layer.iapply(hidden_rep2);
                    
                    //cerr << "denoised_input = " << denoised_input << endl;
                    //cerr << "denoised_input2 = " << denoised_input2 << endl;

                     // Error signal
                    distribution<CFloat> diff2
                        = model_input - denoised_input2;
                    
                    //cerr << "diff = " << diff << endl;
                    //cerr << "diff2 = " << diff2 << endl;
                    
                    // Overall error
                    double error2 = pow(diff2.two_norm(), 2);
                    
                    double delta = error2 - error;

                    double deriv2 = xdiv(delta, epsilon);

                    cerr << "error = " << error << " error2 = " << error2
                         << " delta = " << delta
                         << " deriv " << W_updates[i][j]
                         << " deriv2 " << deriv2 << endl;

#if 0
                    // look at dh/dwij
                    distribution<double> dh_dwij2
                        = (hidden_rep2 - hidden_rep) / epsilon;

                    cerr << "dh_dwij = " << dh_dwij << endl;
                    cerr << "dh_dwij2 = " << dh_dwij2 << endl;
#endif

                   W[i][j] = old_w;
                    
                }
            }
#endif  // if one/zero
            //    = c_updates * (hidden_rep + noisy_input * W * hidden_deriv);
        
            c -= learning_rate * c_updates;
            b -= learning_rate * b_updates;

            for (unsigned i = 0;  i < ni;  ++i)
                for (unsigned j = 0;  j < no;  ++j)
                    W[i][j] -= learning_rate * W_updates[i][j];
            //W -= learning_rate * W_updates;
        }

        cerr << "rmse of iteration: " << sqrt(total_mse / nx)
             << endl;
        cerr << timer.elapsed() << endl;
    }

    cerr << timer.elapsed() << endl;
}
