#!/bin/bash

# extract_H weights signals H
# output.mif input.mif NewH.txt
function extract_H {
  H0=($(mrconvert "$1" -coord 3 0 -quiet - | mrthreshold - -top 100 - | mrstats -mask - "$2" -output mean))
  H1=($(mrconvert "$1" -coord 3 1 -quiet - | mrthreshold - -top 100 - | mrstats -mask - "$2" -output mean))
  H2=($(mrconvert "$1" -coord 3 2 -quiet - | mrthreshold - -top 100 - | mrstats -mask - "$2" -output mean))

  cat > "$3" <<EOD
${H0[0]} ${H1[0]} ${H2[0]}
${H0[1]} ${H1[1]} ${H2[1]}
${H0[2]} ${H1[2]} ${H2[2]}
EOD
}

# output.mif mask.mif
function get_mask {
  mrmath "$1" -axis 3 sum - | mrthreshold - - | maskfilter - median -extent 9 - | maskfilter - erode -npass 9 "$2"
}


# normalise_and_rescale weights output.mif mask.mif rescaled_output.mif
function normalise_and_rescale {
  mrconvert "$1" __tmp-[].mif -force
  #scale=($(~/mrtrix3_mine/bin/mtnormalise __tmp{,_norm}-0.mif __tmp{,_norm}-1.mif __tmp{,_norm}-2.mif -mask "$2" -info -force 2>&1 | grep "Balance factors" | tail -n 1 | awk '{ print $6, $7, $8 }'))
  #scale=($(~/mrtrix3_extras/bin/mtnormalise __tmp{,_norm}-0.mif __tmp{,_norm}-1.mif __tmp{,_norm}-2.mif -mask "$2" -info -force 2>&1 | grep "Balance factors" | tail -n 1 | awk '{ print $6, $7, $8 }'))
  scale=($(~/mrtrix3_extras/bin/mtnormalise __tmp{,_norm}-0.mif __tmp{,_norm}-1.mif __tmp{,_norm}-2.mif -mask "$2" -info -balanced  -force 2>&1 | grep "Balance factors" | tail -n 1 | awk '{ print $6, $7, $8 }'))
  #scale=($(~/mrtrix3/bin/mtnormalise __tmp{,_norm}-0.mif __tmp{,_norm}-1.mif __tmp{,_norm}-2.mif -mask "$2" -info -force 2>&1 | grep "Balance factors" | tail -n 1 | awk '{ print $6, $7, $8 }'))
  echo ${scale[@]} 
  for n in {0..2}; do mrcalc __tmp_norm-$n.mif ${scale[$n]} -mult __tmp_norm_rescaled-$n.mif -force; done
  mrcat __tmp_norm_rescaled-*.mif -axis 3 "$3"
  rm -f __tmp*
}

# comparing H.txt and NewH.txt
# H.txt NewH.txt
function H_comparison {

# Read H.txt values
ind=0;
while read line;
    do declare -a HARR0;
       for words in $line;
       do HARR0[$ind]=$words;
       (( ind++ ));
       done;
done < "$1";

# Read NewH.txt values
ind=0;
while read line;
    do declare -a HARR1;
       for words in $line;
       do HARR1[$ind]=$words;
       (( ind++ ));
       done;
done < "$2";

# Find the difference bewteen both
declare -a DIFF; test=0;

for i in {0..8};
  do DIFF[$i]=$(bc  <<< "scale=2;${HARR0[$i]}-${HARR1[$i]}");
  DIFF[$i]=$(bc <<< "scale=2;sqrt( ${DIFF[$i]}*${DIFF[$i]} )");
  test=$(bc <<< "scale=2;$test+${DIFF[$i]}");
done

# Unset all the used arrays
local test=$(bc <<< "scale=2;($test/9)");
echo "$test"
unset HARR0 HARR1 DIFF;
}

# Compare the values of both rescaled outputs to verify the values are the same
# input1 input2
function compare_outputs {
  mrcalc "$1" "$2" -subtract - | mrstats -
}

# automatically compute volume fractions of a set image
# input.mif output.mif
function get_fraction {
  cat > H.txt <<EOD
650 350 900
1000 1400 900
350 1500 250
EOD

 ~/mrtrix3_extras/bin/icls "$1" H.txt "$2" -quiet;
 extract_H "$2" "$1" H.txt
 get_mask "$2" mask.mif
 normalise_and_rescale "$2" mask.mif output_rescaled.mif

# for loop variant without a tolerance condition
#for i in {0..10};
#do ~/mrtrix3_extras/bin/icls "$1" H.txt "$2" -quiet;
#   extract_H "$2" "$1" H.txt
#   if [ "$i" != "10" ];
#    then rm "$2";
#   fi;
#done
#get_mask "$2" mask.mif
#normalise_and_rescale "$2" mask.mif output_rescaled.mif
#rm mask.mif

# loop with a tolerance condition
#tol=$(bc <<< "scale=4;0.001");
#test=$(bc <<< "scale=4;$tol+1");

#while [ "$(bc <<< "$tol < $test")" == "1"  ];
#do ~/mrtrix3_extras/bin/icls "$1" H.txt "$2" -quiet;
   #extract_H "$2" "$1" NewH.txt
   #test=$(H_comparison H.txt NewH.txt)
   #if [ 1 -eq "$(echo "$tol < $test" |bc)" ];
    #then cp NewH.txt H.txt;
    #rm "$2" NewH.txt;
   #fi;
 #done
#cp NewH.txt H.txt;
#rm NewH.txt;
#get_mask "$2" mask.mif
#normalise_and_rescale "$2" mask.mif output_rescaled.mif
#rm mask.mif
}
