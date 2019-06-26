#include "command.h"
#include "progressbar.h"

#include <fstream>

using namespace MR;
using namespace App;

void usage()
{
  AUTHOR = "";
  
  COPYRIGHT = "";

  DESCRIPTION
    + "The purpose of this command is to automate the calibration of the signal acquisition for the problem"
      "matrix of the icls.cpp file";

   /* Further Description:
      EDIT 26/06/2019 12:02: 
      The program will take any input image containing a T1, PD and T2 weighted scan of the brain at process 
      the image using icls.cpp with an initial problem matrix that was created by using reacurring values 
      seen through observational tests.
      After having acquired the first output the program should choose the best 100 voxels of each fraction
      image, store those values, and  find those voxals in the input images for each weighting and fraction 
      type.
      We will then average out the values of the 100 results for each type and weighting and replace the 
      initial values of the problem matrix with these new values. Afterwards we will use the icls.cpp command
      again, and repeat this process until the problem matrix converges onto a certain value.
    */
  ARGUMENTS
    + Argument("input","the input image.").type_image_in()
    + Argument("output","the output image.").type_image_out();

   // I don't have any options at the moment, will revisit this once the bulk of the problem has been 
   // done.
}

void run()
{
  // First we need to predefine the H matrix so that we can use the icls command for the first output.
  // Furthermor we need to save it as a text file
  float H[3][3] = {{650,350,900},{1000,1400,900},{350,1500,250}};
  std::ofstream fout("H.txt");
  fout << H;

  // We then need to run our first iteration of the icls command

}
