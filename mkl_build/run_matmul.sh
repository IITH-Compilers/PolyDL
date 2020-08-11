# Run the following command- sh run_matmul.sh 10 20 30 1

set -x
OUT=matmul_perf.csv
#rm ${OUT}
# BENCHDNN=/nfs_home/stavarag/work/polyscientist/mkl_build/mkl-dnn/build/tests/benchdnn/benchdnn
BENCHDNN=/homes/gaganiith/work/temp/polyscientist_mkldnn_matmul/mkl_build/mkl-dnn/build/tests/benchdnn/benchdnn

#Default values.
m=10
n=20
k=30
config_num=1

#Taking values from terminal
m=$1
n=$2
k=$3
config_num=${4}

export KMP_AFFINITY=granularity=fine,compact,1,28
# export OMP_NUM_THREADS=$mb

config="m${m}n${n}k${k}"

#echo -n $config, >> ${OUT}
GFLOPS=`$BENCHDNN --matmul --mode=p --cfg=f32 $config"_nconfigg" | grep "_nconfigg" | cut -d"," -f10`
echo -n ${config_num},$GFLOPS >> ${OUT}
echo "" >> ${OUT}
