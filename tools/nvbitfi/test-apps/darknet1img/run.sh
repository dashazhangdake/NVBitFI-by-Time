eval ${PRELOAD_FLAG} ${BIN_DIR}/darknet detector test ${DATASET_DIR}/kitti.data ${DATASET_DIR}/yolov3-kitti.cfg ${DATASET_DIR}/yolov3-kitti_last.weights ${DATASET_DIR}/street.png -ext_output -dont_show> stdout.txt 2> stderr.txt