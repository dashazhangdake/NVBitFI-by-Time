eval ${PRELOAD_FLAG} ${BIN_DIR}/darknet detector test ${DATASET_DIR}/coco.data ${DATASET_DIR}/yolov3-tiny.cfg ${DATASET_DIR}/yolov3-tiny.weights ${DATASET_DIR}/street.png -ext_output -dont_show> stdout.txt 2> stderr.txt