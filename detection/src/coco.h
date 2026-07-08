/* coco.h — the 80 COCO class names (RTMDet-tiny stock output) and the mapping to
 * the AIRPOC contract classes. The stock COCO model detects people and vehicles;
 * "drone" has no COCO class and arrives with the trained model. coco_to_airpoc()
 * returns "human"/"vehicle" for the classes we surface, or NULL to drop the rest. */
#ifndef DET_COCO_H
#define DET_COCO_H

#define COCO_NUM_CLASSES 80

static const char *const COCO_CLASSES[COCO_NUM_CLASSES] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light",
    "fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow",
    "elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
    "skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket","bottle",
    "wine glass","cup","fork","knife","spoon","bowl","banana","apple","sandwich","orange",
    "broccoli","carrot","hot dog","pizza","donut","cake","chair","couch","potted plant","bed",
    "dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone","microwave","oven",
    "toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"
};

/* person -> human; ground vehicles -> vehicle; everything else -> dropped. */
static inline const char *coco_to_airpoc(int c)
{
    if (c == 0) return "human";                       /* person */
    if (c == 2 || c == 5 || c == 7) return "vehicle"; /* car, bus, truck */
    return 0;
    /* Deliberately NOT bicycle/motorcycle/train: our "vehicle" target is cars,
     * trucks, and (via the trained model) military ground vehicles — a parked
     * bicycle is not a target. This mapping is a stock-model placeholder; the
     * trained model has native classes and this file goes away. */
}

#endif /* DET_COCO_H */
