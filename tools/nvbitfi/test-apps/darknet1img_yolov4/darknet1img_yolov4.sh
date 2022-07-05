#!/bin/bash

FIDATA=$PWD/../test-apps-dependencies/darknet_data
./darknet detector test $FIDATA/coco.data $FIDATA/yolov4.cfg $FIDATA/yolov4.weights $FIDATA/street.png -ext_output -dont_show