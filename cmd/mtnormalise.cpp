/*
 * Copyright (c) 2008-2018 the MRtrix3 contributors.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at http://mozilla.org/MPL/2.0/
 *
 * MRtrix3 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * For more details, see http://www.mrtrix.org/
 */


#include "command.h"
#include "image.h"
#include "algo/loop.h"
#include "transform.h"
#include "math/least_squares.h"
#include "algo/threaded_copy.h"
#include "adapter/replicate.h"

using namespace MR;
using namespace App;

#define DEFAULT_NORM_VALUE 0.28209479177
#define DEFAULT_MAIN_ITER_VALUE 15
#define DEFAULT_BALANCE_MAXITER_VALUE 7
#define DEFAULT_POLY_ORDER 3

const char* poly_order_choices[] = { "0", "1", "2", "3", nullptr };

void usage ()
{
  AUTHOR = "Thijs Dhollander (thijs.dhollander@gmail.com), Rami Tabbara (rami.tabbara@florey.edu.au) and David Raffelt (david.raffelt@florey.edu.au)";

  SYNOPSIS = "Multi-tissue informed log-domain intensity normalisation";

  REFERENCES
    + "Raffelt, D.; Dhollander, T.; Tournier, J.-D.; Tabbara, R.; Smith, R. E.; Pierre, E. & Connelly, A. " // Internal
    "Bias Field Correction and Intensity Normalisation for Quantitative Analysis of Apparent Fibre Density. "
    "In Proc. ISMRM, 2017, 26, 3541";

  DESCRIPTION
   + "This command inputs any number of tissue components (e.g. from multi-tissue CSD) "
     "and outputs corresponding normalised tissue components. Intensity normalisation is "
     "performed in the log-domain, and can smoothly vary spatially to accomodate the "
     "effects of (residual) intensity inhomogeneities."

   + "The -mask option is mandatory and is optimally provided with a brain mask "
     "(such as the one obtained from dwi2mask earlier in the processing pipeline). "
     "Outlier areas with exceptionally low or high combined tissue contributions are "
     "accounted for and reoptimised as the intensity inhomogeneity estimation becomes "
     "more accurate."

   + "Example usage: mtnormalise wmfod.mif wmfod_norm.mif gm.mif gm_norm.mif csf.mif csf_norm.mif -mask mask.mif.";


  ARGUMENTS
    + Argument ("input output", "list of all input and output tissue compartment files. See example usage in the description.").type_various().allow_multiple();

  OPTIONS
    + OptionGroup ("Options that affect the operation of the mtnormalise command")

    + Option ("mask", "the mask defines the data used to compute the intensity normalisation. This option is mandatory.").required ()
    + Argument ("image").type_image_in ()

    + Option ("order", "the maximum order of the polynomial basis used to fit the normalisation field in the log-domain. An order of 0 is equivalent to not allowing spatial variance of the intensity normalisation factor. (default: " + str(DEFAULT_POLY_ORDER) + ")")
    + Argument ("number").type_choice (poly_order_choices)

    + Option ("niter", "set the number of iterations. (default: " + str(DEFAULT_MAIN_ITER_VALUE) + ")")
    + Argument ("number").type_integer()

    + Option ("value", "specify the (positive) reference value to which the summed tissue compartments will be normalised. "
                       "(default: " + str(DEFAULT_NORM_VALUE, 6) + ", SH DC term for unit angular integral)")
    + Argument ("number").type_float (std::numeric_limits<default_type>::min())

    + Option ("balanced", "incorporate the per-tissue balancing factors into scaling of the output images "
                          "(NOTE: use of this option has critical consequences for AFD intensity normalisation; "
                          "should not be used unless these consequences are fully understood)")

    + OptionGroup ("Options for outputting data to verify successful operation of the mtnormalise command")

    + Option ("check_norm", "output the final estimated spatially varying intensity level that is used for normalisation.")
    + Argument ("image").type_image_out ()

    + Option ("check_mask", "output the final mask used to compute the normalisation. "
                            "This mask excludes regions identified as outliers by the optimisation process.")
    + Argument ("image").type_image_out ()

    + Option ("check_factors", "output the tissue balance factors computed during normalisation.")
    + Argument ("file").type_file_out ();

}


using ValueType = float;
using ImageType = Image<ValueType>;
using MaskType = Image<bool>;

// Function to get the number of basis vectors based on the desired order
int GetBasisVecs(int order)
{
  int n_basis_vecs;
    switch (order) {
      case 0:
        n_basis_vecs = 1;
        break;
      case 1:
        n_basis_vecs = 4;
        break;
      case 2:
        n_basis_vecs = 10;
        break;
      default:
        n_basis_vecs = 20;
        break;
      }
  return n_basis_vecs;
};

// Un-templated PolyBasisFunction struct to get the user specified amount of basis functions
struct PolyBasisFunction { MEMALIGN (PolyBasisFunction)

  PolyBasisFunction(const int order) : n_basis_vecs (GetBasisVecs(order)) { };

  const int n_basis_vecs;

  FORCE_INLINE Eigen::MatrixXd operator () (const Eigen::Vector3& pos) {
    double x = pos[0];
    double y = pos[1];
    double z = pos[2];
    Eigen::MatrixXd basis(n_basis_vecs, 1);
    basis(0) = 1.0;
    if (n_basis_vecs < 4)
      return basis;

    basis(1) = x;
    basis(2) = y;
    basis(3) = z;
    if (n_basis_vecs < 10)
      return basis;

    basis(4) = x * x;
    basis(5) = y * y;
    basis(6) = z * z;
    basis(7) = x * y;
    basis(8) = x * z;
    basis(9) = y * z;
    if (n_basis_vecs < 20)
      return basis;

    basis(10) = x * x * x;
    basis(11) = y * y * y;
    basis(12) = z * z * z;
    basis(13) = x * x * y;
    basis(14) = x * x * z;
    basis(15) = y * y * x;
    basis(16) = y * y * z;
    basis(17) = z * z * x;
    basis(18) = z * z * y;
    basis(19) = x * y * z;
  return basis;
  }
};

struct mask_refiner {
  FORCE_INLINE void operator() (Image<float>& summed, Image<bool>& initial_mask, Image<bool>& refined) const
  {
    refined.value() = ( std::isfinite(float(summed.value())) && summed.value() > 0.f && initial_mask.value() );
  }
};

// Struct calculating the exponentials of the Norm Field Image
struct NormFieldIm {
    void operator () (Image<float> norm_field_image, const Image<float> norm_field_log) {
         norm_field_image.value() = std::exp (norm_field_log.value());
   }
};

// Struct calculating the initial summed_log values
struct SummedLog {
  SummedLog (const size_t n_tissue_types, Eigen::VectorXd balance_factors) : n_tissue_types (n_tissue_types), balance_factors (balance_factors) { }

  template <class ImageType>
  void operator () (ImageType& summed_log, ImageType& combined_tissue, ImageType& norm_field_image) {
      for (size_t j = 0; j < n_tissue_types; ++j) {
        combined_tissue.index(3) = j;
        summed_log.value() += balance_factors(j) * combined_tissue.value() / norm_field_image.value();
      }
  summed_log.value() = std::log(summed_log.value());
  }

  const size_t n_tissue_types;
  Eigen::VectorXd balance_factors;
};

// Templated struct calculating the norm_field_log values
struct NormFieldLog {

   NormFieldLog (Eigen::MatrixXd norm_field_weights, Transform transform, struct PolyBasisFunction basis_function) : norm_field_weights (norm_field_weights), transform (transform), basis_function (basis_function){ }

   template <class ImageType>
   void operator () (ImageType& norm_field_log) {
       Eigen::Vector3 vox (norm_field_log.index(0), norm_field_log.index(1), norm_field_log.index(2));
       Eigen::Vector3 pos = transform.voxel2scanner * vox;
       norm_field_log.value() = basis_function (pos).col(0).dot (norm_field_weights.col(0));
   }

   Eigen::MatrixXd norm_field_weights;
   Transform transform;
   struct PolyBasisFunction basis_function;
};

// Function to define the output values at the beginning of the run () function
ImageType DefineOutput(vector<std::string> output_filenames, vector<Header> output_headers) {
  ImageType output_image;
  for (size_t j = 0; j < output_filenames.size(); ++j) {
     output_image = ImageType::scratch (output_headers[j], output_filenames[j]);
  }
return output_image;
};

/*
// Function to perform outlier rejection
int OutlierRejection(float outlier_range, MaskType& mask, MaskType& initial_mask, vector<float> summed_log_values, ImageType summed_log, ImageType norm_field_image, ImageType combined_tissue, Eigen::VectorXd balance_factors, int num_voxels){

    ThreadedLoop (summed_log).run (
        [&balance_factors] (ImageType& sum, ImageType& comb, ImageType& field) {
          ValueType s = 0.0;
          for (int j = 0; j < comb.size(3); ++j) {
            comb.index(3) = j;
            s += balance_factors[j] * comb.value() / field.value();
          }
          sum.value() = std::log (s);
        }, summed_log, combined_tissue, norm_field_image);


    threaded_copy (initial_mask, mask);

//    vector<float> summed_log_values;
    summed_log_values.reserve (num_voxels);
    for (auto i = Loop (0, 3) (mask, summed_log); i; ++i) {
      if (mask.value())
        summed_log_values.push_back (summed_log.value());
    }

    num_voxels = summed_log_values.size();

    const auto lower_quartile_it = summed_log_values.begin() + std::round ((float)num_voxels * 0.25f);
    std::nth_element (summed_log_values.begin(), lower_quartile_it, summed_log_values.end());
    const float lower_quartile = *lower_quartile_it;
    const auto upper_quartile_it = summed_log_values.begin() + std::round ((float)num_voxels * 0.75f);
    std::nth_element (lower_quartile_it, upper_quartile_it, summed_log_values.end());
    const float upper_quartile = *upper_quartile_it;
    const float lower_outlier_threshold = lower_quartile - outlier_range * (upper_quartile - lower_quartile);
    const float upper_outlier_threshold = upper_quartile + outlier_range * (upper_quartile - lower_quartile);


    for (auto i = Loop (0, 3) (mask, summed_log); i; ++i) {
      if (mask.value()) {
        if (summed_log.value() < lower_outlier_threshold || summed_log.value() > upper_outlier_threshold) {
          mask.value() = 0;
          num_voxels--;
        }
      }
    }

return num_voxels;
};
*/

/*
// Mask Modification ThreadedLoop
struct MaskModification {
   MaskModification (size_t& num_voxels, float lower_outlier_threshold, float upper_outlier_threshold) : num_voxels (num_voxels), lower_outlier_threshold (lower_outlier_threshold), upper_outlier_threshold (upper_outlier_threshold) { } 
    void operator () (MaskType& mask, ImageType summed_log) {
       if (mask.value()) {
         if (summed_log.value() < lower_outlier_threshold || summed_log.value() > upper_outlier_threshold) {
           mask.value() = 0;
           num_voxels--;
         }
       }
   }
 size_t& num_voxels;
 float lower_outlier_threshold;
 float upper_outlier_threshold;
};
*/

// Function to compute the Choleski decompostion based on
// an N by n Eigen matrix and an N by 1 Eigen vector
Eigen::VectorXd Choleski(Eigen::MatrixXd X, Eigen::VectorXd y) {
      Eigen::MatrixXd M (X.cols(), X.cols());
      Eigen::VectorXd alpha (X.cols());
      M = X.transpose()*X;
      alpha = X.transpose()*y;
      Eigen::VectorXd res = M.llt().solve (alpha);
      return res;
};


void run ()
{
  if (argument.size() % 2)
    throw Exception ("The number of arguments must be even, provided as pairs of each input and its corresponding output file.");

  const int order = get_option_value<int> ("order", DEFAULT_POLY_ORDER);
  PolyBasisFunction basis_function (order);


  vector<Adapter::Replicate<ImageType>> input_images; // TODO: why Adapter::Replicate ?
  vector<Header> output_headers;
  vector<std::string> output_filenames;
  ImageType output_image;

  ProgressBar input_progress ("loading input images", 3*argument.size()/2);

  // Open input images and prepare output image headers
  for (size_t i = 0; i < argument.size(); i += 2) {
    input_progress++;

    auto image = ImageType::open (argument[i]);

    if (image.ndim () > 4)
      throw Exception ("Input image \"" + image.name() + "\" contains more than 4 dimensions.");

    // Elevate image dimensions to ensure it is 4-dimensional
    // e.g. x,y,z -> x,y,z,1
    // This ensures consistency across multiple tissue input images
    Header h_image4d (image);
    h_image4d.ndim() = 4; // TODO: generate output images directly, not just headers

    input_images.emplace_back (image, h_image4d);

    if (i > 0)
      check_dimensions (input_images[0], input_images[i / 2], 0, 3);

    if (Path::exists (argument[i + 1]) && !App::overwrite_files)
      throw Exception ("Output file \"" + argument[i] + "\" already exists. (use -force option to force overwrite)");

    output_headers.push_back (std::move (h_image4d));
    output_filenames.push_back (argument[i + 1]);
  }

  // Preparing default settings to the output images
  output_image = DefineOutput(output_filenames, output_headers);

  // Setting the n_tissue_types
  const size_t n_tissue_types = input_images.size();

  // Load the mask and refine the initial mask to exclude non-positive summed tissue components
  Header header_3D (input_images[0]);
  header_3D.ndim() = 3;
  header_3D.datatype() = DataType::Float32;
  auto opt = get_options ("mask");

  auto orig_mask = MaskType::open (opt[0][0]);
  Header mask_header (orig_mask);
  mask_header.ndim() = 3;
  mask_header.datatype() = DataType::Bit;
  Stride::set (mask_header, header_3D);

  auto initial_mask = MaskType::scratch (mask_header, "Initial processing mask");
  auto mask = MaskType::scratch (mask_header, "Processing mask");
  auto prev_mask = MaskType::scratch (mask_header, "Previous processing mask");

  {
    auto summed = ImageType::scratch (header_3D, "Summed tissue volumes");
    for (size_t j = 0; j < input_images.size(); ++j) {
      input_progress++;
      struct ValueAccumulator {
        FORCE_INLINE void operator () (decltype(summed)& sum, decltype(input_images[0])& in) const { sum.value() += in.value(); }
      };
      ThreadedLoop (summed, 0, 3).run (ValueAccumulator(), summed, input_images[j]);
    }
    ThreadedLoop (summed, 0, 3).run (mask_refiner(), summed, orig_mask, initial_mask);
  }

  threaded_copy (initial_mask, mask);


  // Load input images into single 4d-image and zero-clamp combined-tissue image
  Header h_combined_tissue (input_images[0]);
  h_combined_tissue.ndim () = 4;
  h_combined_tissue.size (3) = n_tissue_types;
  auto combined_tissue = ImageType::scratch (h_combined_tissue, "Tissue components");

  for (size_t i = 0; i < n_tissue_types; ++i) {
    input_progress++;

    combined_tissue.index (3) = i;

    ThreadedLoop (combined_tissue, 0, 3).run (
        [](decltype(combined_tissue)& comb, decltype(input_images[0]) in) { comb.value() = std::max<float>(in.value (), 0.f); },
        combined_tissue, input_images[i]);
  }

  size_t num_voxels = 0;
  ThreadedLoop (mask, 0, 3).run ([&num_voxels](decltype(mask) mask) { if (mask.value()) ++num_voxels; }, mask);

  if (!num_voxels)
    throw Exception ("Mask contains no valid voxels.");


  const float reference_value = get_option_value ("reference", DEFAULT_NORM_VALUE);
  const float log_ref_value = std::log (reference_value);
  const size_t max_iter = get_option_value ("niter", DEFAULT_MAIN_ITER_VALUE);
  const size_t max_balance_iter = DEFAULT_BALANCE_MAXITER_VALUE;

  // Initialise normalisation fields in both image and log domain
  Eigen::MatrixXd norm_field_weights;

  auto norm_field_image = ImageType::scratch (header_3D, "Normalisation field (intensity)");
  auto norm_field_log = ImageType::scratch (header_3D, "Normalisation field (log-domain)");

  ThreadedLoop (norm_field_image).run (
      [](decltype(norm_field_image)& in) { in.value() = 1.0; },
      norm_field_image);

  Eigen::VectorXd balance_factors (Eigen::VectorXd::Ones (n_tissue_types));

  size_t iter = 1;

  // Store lambda-function for performing outlier-rejection.
  // We perform a coarse outlier-rejection initially as well as
  // a finer outlier-rejection within each iteration of the
  // tissue (re)balancing loop
  auto outlier_rejection = [&](float outlier_range) {  // TODO: why is this a lambda? Move to dedicated function
  auto summed_log = ImageType::scratch (header_3D, "Log of summed tissue volumes");
  //ThreadedLoop (summed_log, 0, 3).run (SummedLog(n_tissue_types, balance_factors), summed_log, combined_tissue, norm_field_image);

    ThreadedLoop (summed_log).run (
        [&balance_factors] (ImageType& sum, ImageType& comb, ImageType& field) {
          ValueType s = 0.0;
          for (int j = 0; j < comb.size(3); ++j) {
            comb.index(3) = j;
            s += balance_factors[j] * comb.value() / field.value();
          }
          sum.value() = std::log (s);
        }, summed_log, combined_tissue, norm_field_image);


    threaded_copy (initial_mask, mask);

    vector<float> summed_log_values;
    summed_log_values.reserve (num_voxels);
    for (auto i = Loop (0, 3) (mask, summed_log); i; ++i) {
      if (mask.value())
        summed_log_values.push_back (summed_log.value());
    }

    num_voxels = summed_log_values.size();

    const auto lower_quartile_it = summed_log_values.begin() + std::round ((float)num_voxels * 0.25f);
    std::nth_element (summed_log_values.begin(), lower_quartile_it, summed_log_values.end());
    const float lower_quartile = *lower_quartile_it;
    const auto upper_quartile_it = summed_log_values.begin() + std::round ((float)num_voxels * 0.75f);
    std::nth_element (lower_quartile_it, upper_quartile_it, summed_log_values.end());
    const float upper_quartile = *upper_quartile_it;
    const float lower_outlier_threshold = lower_quartile - outlier_range * (upper_quartile - lower_quartile);
    const float upper_outlier_threshold = upper_quartile + outlier_range * (upper_quartile - lower_quartile);


    for (auto i = Loop (0, 3) (mask, summed_log); i; ++i) {
      if (mask.value()) {
        if (summed_log.value() < lower_outlier_threshold || summed_log.value() > upper_outlier_threshold) {
          mask.value() = 0;
          num_voxels--;
        }
      }
    }


// ThreadedLoop(mask).run(MaskModification(num_voxels, lower_outlier_threshold, upper_outlier_threshold),mask, summed_log);

/*
    ThreadedLoop (mask).run (
        [&](MaskType& mask, ImageType& sum) {
        if (mask.value())
          if (sum.value() < lower_outlier_threshold || sum.value() > upper_outlier_threshold) {
            mask.value() = false;
            --num_voxels; // TODO: can't modify this variable in a multi-treaded context. Use a functor
          }
        }, mask, summed_log);
*/
  };


  input_progress.done ();
  ProgressBar progress ("performing log-domain intensity normalisation", max_iter);

/*
  // Pre-writing the summed_log variable and the summed_log_values vector
  vector<float> summed_log_values;
  auto summed_log = ImageType::scratch (header_3D, "Log of summed tissue volumes");
*/

  // Perform an initial outlier rejection prior to the first iteration
  outlier_rejection (3.f);
  // auto vox_count = OutlierRejection(3.f, mask, initial_mask, summed_log_values, summed_log, norm_field_image, combined_tissue, balance_factors, num_voxels);

  threaded_copy (mask, prev_mask);

  while (iter <= max_iter) {

    INFO ("Iteration: " + str(iter));

    // Iteratively compute tissue balance factors with outlier rejection
    size_t balance_iter = 1;
    bool balance_converged = false;

    while (!balance_converged && balance_iter <= max_balance_iter) {

      DEBUG ("Balance and outlier rejection iteration " + str(balance_iter) + " starts.");

      if (n_tissue_types > 1) {

        // Solve for tissue balance factors
        Eigen::MatrixXd X (num_voxels, n_tissue_types);
        Eigen::VectorXd y (Eigen::VectorXd::Ones (num_voxels));
        uint32_t index = 0;

        for (auto i = Loop (0, 3) (mask, combined_tissue, norm_field_image); i; ++i) {
          if (mask.value()) {
            for (size_t j = 0; j < n_tissue_types; ++j) {
              combined_tissue.index (3) = j;
              X (index, j) = combined_tissue.value() / norm_field_image.value();
            }
            ++index;
          }
        }

//      balance_factors = X.colPivHouseholderQr().solve(y);

/*
      // THEORY OF GETTING NEEDED MATRICES FOR CHOLESKY //
       Eigen::MatrixXd M (n_tissue_types, n_tissue_types);
       Eigen::VectorXd alpha (n_tissue_types);
       M = X.transpose()*X;
       alpha = X.transpose()*y;
       balance_factors = M.llt().solve (alpha);
//       balance_factors = M.colPivHouseholderQr().solve(alpha);
*/
       balance_factors = Choleski(X, y);

        // Ensure our balance factors satisfy the condition that sum(log(balance_factors)) = 0
        double log_sum = 0.0;
        for (size_t j = 0; j < n_tissue_types; ++j) {
          if (balance_factors(j) <= 0.0)
            throw Exception ("Non-positive tissue balance factor was computed."
                             " Tissue index: " + str(j+1) + " Balance factor: " + str(balance_factors(j)) +
                             " Needs to be strictly positive!");
          log_sum += std::log (balance_factors(j));
        }
        balance_factors /= std::exp (log_sum / n_tissue_types);
      }

      INFO ("Balance factors (" + str(balance_iter) + "): " + str(balance_factors.transpose()));

      // Perform outlier rejection on log-domain of summed images
      outlier_rejection(1.5f);
      // auto new_vox_count = OutlierRejection(1.5f, mask, initial_mask, summed_log_values, summed_log, norm_field_image, combined_tissue, balance_factors, num_voxels);

      // Check for convergence
      balance_converged = true;

      for (auto i = Loop (0, 3) (mask, prev_mask); i; ++i) {
        if (mask.value() != prev_mask.value()) {
          balance_converged = false;
          break;
        }
      }
/*
      if (new_vox_count != vox_count){
      balance_converged = false;
      vox_count = new_vox_count;
      }
*/
      threaded_copy (mask, prev_mask);

      balance_iter++;
    }


    // Solve for normalisation field weights in the log domain
    Transform transform (mask);
    Eigen::MatrixXd norm_field_basis (num_voxels, basis_function.n_basis_vecs);
    Eigen::VectorXd y (num_voxels);
    uint32_t index = 0;
    for (auto i = Loop (0, 3) (mask, combined_tissue); i; ++i) {
      if (mask.value()) {
        Eigen::Vector3 vox (mask.index(0), mask.index(1), mask.index(2));
        Eigen::Vector3 pos = transform.voxel2scanner * vox;
        norm_field_basis.row (index) = basis_function (pos).col(0);

        double sum = 0.0;
        for (size_t j = 0; j < n_tissue_types; ++j) {
          combined_tissue.index(3) = j;
          sum += balance_factors(j) * combined_tissue.value() ;
        }
        y (index++) = std::log(sum) - log_ref_value;
      }
    }

//    norm_field_weights = norm_field_basis.colPivHouseholderQr().solve(y);

/*
      // THEORY OF GETTING NEEDED MATRICES FOR CHOLESKY //
       Eigen::MatrixXd m (basis_function.n_basis_vecs, basis_function.n_basis_vecs);
       Eigen::VectorXd Alpha (basis_function.n_basis_vecs);
       m = norm_field_basis.transpose()*norm_field_basis;
       Alpha = norm_field_basis.transpose()*y;
       norm_field_weights = m.llt().solve (Alpha);
//      norm_field_weights = m.colPivHouseholderQr().solve(Alpha);
*/
    norm_field_weights = Choleski(norm_field_basis, y);

    // Generate normalisation field in the log domain
    ThreadedLoop (norm_field_log, 0, 3).run (NormFieldLog(norm_field_weights, transform, basis_function), norm_field_log);

    // Generate normalisation field in the image domain
    ThreadedLoop (norm_field_image, 0, 3).run (NormFieldIm(),norm_field_image, norm_field_log);

    progress++;
    iter++;
  }

  progress.done();

  ProgressBar output_progress("writing output images", output_filenames.size());

  opt = get_options ("check_norm");
  if (opt.size()) {
    auto norm_field_output = ImageType::create (opt[0][0], header_3D);
    threaded_copy (norm_field_image, norm_field_output);
  }

  opt = get_options ("check_mask");
  if (opt.size()) {
    auto mask_output = ImageType::create (opt[0][0], mask);
    threaded_copy (mask, mask_output);
  }

  opt = get_options ("check_factors");
  if (opt.size()) {
    File::OFStream factors_output (opt[0][0]);
    factors_output << balance_factors;
  }

  // Compute log-norm scale parameter (geometric mean of normalisation field in outlier-free mask).
  double lognorm_scale (0.0);
  if (num_voxels) {
  struct LogNormScale {
    LogNormScale (double& lognorm_scale, uint32_t num_voxels) : lognorm_scale (lognorm_scale), num_voxels (num_voxels) { }
    FORCE_INLINE void operator () (decltype(mask) mask_in, decltype(norm_field_log) norm_field_lg) { if (mask_in.value ()){ lognorm_scale += norm_field_lg.value (); } lognorm_scale = std::exp(lognorm_scale / (double)num_voxels); }

    double& lognorm_scale;
    uint32_t num_voxels;
  };
  ThreadedLoop (mask, 0, 3).run (LogNormScale(lognorm_scale, num_voxels), mask, norm_field_log);
  }

  const bool output_balanced = get_options("balanced").size();

  for (size_t j = 0; j < output_filenames.size(); ++j) {
    output_progress++;

    float balance_multiplier = 1.0f;
    output_headers[j].keyval()["lognorm_scale"] = str(lognorm_scale);
    if (output_balanced) {
      balance_multiplier = balance_factors[j];
      output_headers[j].keyval()["lognorm_balance"] = str(balance_multiplier);
    }
    output_image = ImageType::create (output_filenames[j], output_headers[j]);
    const size_t n_vols = input_images[j].size(3);
    const Eigen::VectorXf zero_vec = Eigen::VectorXf::Zero (n_vols);

  struct ReadInOutput {
     ReadInOutput (Eigen::VectorXf zero_vec, float balance_multiplier) : zero_vec (zero_vec), balance_multiplier (balance_multiplier) { }
     FORCE_INLINE void operator () (decltype(output_image)& out_im, decltype(input_images[0]) in_im, decltype(norm_field_image) norm_field_im) { in_im.index(3) = 0; if (in_im.value() < 0.f) { out_im.row(3) = zero_vec; } else { out_im.row(3) = Eigen::VectorXf{in_im.row(3)} * balance_multiplier / norm_field_im.value(); } }
     Eigen::VectorXf zero_vec;
     float balance_multiplier;
  };
  ThreadedLoop (output_image, 0, 3).run (ReadInOutput(zero_vec, balance_multiplier), output_image, input_images[j], norm_field_image);
  }
}