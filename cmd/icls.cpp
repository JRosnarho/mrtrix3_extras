/*
 * Copyright (c) 2008-2016 the MRtrix3 contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/
 *
 * MRtrix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * For more details, see www.mrtrix.org
 *
 */


#include "command.h"
#include "progressbar.h"
#include "image.h"
#include "algo/threaded_loop.h"
#include "math/constrained_least_squares.h"

using namespace MR;
using namespace App;

void usage ()
{
  AUTHOR = "J-Donald Tournier (jdtournier@gmail.com)";
  SYNOPSIS = "perform generic inequality-constrained least-squares on input images";

  DESCRIPTION
    + "perform generic inequality-constrained least-squares on input images"
    + "i.e. solve for   MX = Y\n\n     such that   CX >= 0";

  ARGUMENTS
    + Argument ("input", "the input images Y.").type_image_in ()
    + Argument ("problem", "the problem matrix M")
    + Argument ("output", "the output solution image X.").type_image_out ();

  OPTIONS

    + Option ("constraint", "specify the constraint matrix. By default, the algorithm will solve for a non-negative solution vector.")
    +   Argument ("matrix").type_file_in()

    + Option ("niter", "specify the maximum number of iterations to perform (default: 10 x num_parameters)")
    +   Argument ("num").type_integer (0)

    + Option ("tolerance", "specify the tolerance on the change in the solution, used to establishe convergence (default: 0.0)")
    +   Argument ("value").type_float (0.0)

    + Option ("solution_norm", "specify the regularisation to apply on the solution norm - useful for poorly condition problems (default: 0.0)")
    +   Argument ("value").type_float (0.0)

    + Option ("constraint_norm", "specify the regularisation to apply on the constraint vector norm - useful for poorly condition problems (default: 0.0)")
    +   Argument ("value").type_float (0.0)

    + Option ("prediction", "output predicted image")
    +   Argument ("image").type_image_out();
}




using value_type = float;
using compute_type = double;

class Processor {
  public:
    Processor (const Math::ICLS::Problem<compute_type>& problem, Image<value_type>& prediction) :
      solve (problem),
      x(problem.H.cols()),
      b(problem.H.rows()),
      prediction (prediction) { }

    void operator() (Image<value_type>& in, Image<value_type>& out)
    {
      for (auto l = Loop (3) (in); l; ++l)
        b[in.index(3)] = in.value();

      auto niter = solve (x, b);
      if (niter >= solve.problem().max_niter)
        INFO ("voxel at [ " + str(in.index(0)) + " " + str(in.index(1)) + " " + str(in.index(2)) + " ] failed to converge");

      for (auto l = Loop (3) (out); l; ++l)
        out.value() = x[out.index(3)];

      if (prediction.valid()) {
        assign_pos_of (in, 0, 3).to (prediction);
        b = solve.problem().H * x;
        for (auto l = Loop (3) (prediction); l; ++l)
          prediction.value() = b[prediction.index(3)];
      }
    }

    Math::ICLS::Solver<compute_type> solve;
    Eigen::VectorXd x, b;
    Image<value_type> prediction;
};





void run ()
{
  auto max_iterations      = get_option_value ("niter",           0  );
  auto tolerance           = get_option_value ("tolerance",       0.0);
  auto solution_norm_reg   = get_option_value ("solution_norm",   0.0);
  auto constraint_norm_reg = get_option_value ("constraint_norm", 0.0);

  auto problem_matrix    = load_matrix<compute_type> (argument[1]);
  decltype (problem_matrix) constraint_matrix;

  auto opt = get_options ("constraint");
  if (opt.size()) {
    constraint_matrix = load_matrix<compute_type> (opt[0][0]);
    if (problem_matrix.cols() != constraint_matrix.cols())
      throw Exception ("number of columns in problem matrix \"" + std::string (argument[1]) + "\" does not match number of columns in constraint matrix \"" + std::string(opt[0][0]) + "\"");
  }
  else
    constraint_matrix = decltype(constraint_matrix)::Identity (problem_matrix.cols(), problem_matrix.cols());

  Math::ICLS::Problem<compute_type> problem (problem_matrix, constraint_matrix, solution_norm_reg, constraint_norm_reg, max_iterations, tolerance);

  auto in = Image<value_type>::open (argument[0]);
  if (in.size(3) != ssize_t (problem.num_measurements()))
    throw Exception ("number of volumes in input image \"" + std::string (argument[0]) + "\" does not match number of columns in problem matrix \"" + std::string (argument[1]) + "\"");

  opt = get_options ("prediction");
  Image<value_type> prediction;
  if (opt.size()) {
    Header header = in;
    header.datatype() = DataType::Float32;
    prediction = Image<value_type>::create (opt[0][0], header);
  }

  Header header (in);
  header.size (3) = problem.num_parameters();
  header.datatype() = DataType::Float32;
  auto out = Image<value_type>::create (argument[2], header);

  ThreadedLoop ("performing constrained least-squares fit", in, 0, 3)
    .run (Processor (problem, prediction), in, out);
}

