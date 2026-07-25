#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>
#if 0
/// @brief  鎺ㄧ悊缁撴灉绫诲瀷鏋氫妇
enum class ResultType {
    UNKNOWN,     ///< 鏈煡缁撴灉绫诲瀷
    DETECTION,   ///< 妫€娴嬬粨鏋滅被鍨?   
    CLASSIFICATION,  ///< 鍒嗙被缁撴灉绫诲瀷
    SEGMENTATION,  ///< 鍒嗗壊缁撴灉绫诲瀷
    POSE,          ///< 鍏抽敭鐐规娴嬬粨鏋滅被鍨?
    OCR,           ///< 鏂囨湰璇嗗埆缁撴灉绫诲瀷
    TRACK,         ///< 璺熻釜缁撴灉绫诲瀷
    EMBEDDING,     ///< 宓屽叆缁撴灉绫诲瀷
    LLM            ///< 澶ц瑷€妯″瀷缁撴灉绫诲瀷
};

/// @brief 妫€娴嬫缁撴瀯浣?
struct DetectionBox {
    int class_id;   ///< 妫€娴嬫鐨勭被鍒獻D
    float score;   ///< 妫€娴嬫鐨勭疆淇″害

    float x1;       ///< 妫€娴嬫宸︿笂瑙抶鍧愭爣
    float y1;       ///< 妫€娴嬫宸︿笂瑙抷鍧愭爣
    float x2;       ///< 妫€娴嬫鍙充笅瑙抶鍧愭爣
    float y2;       ///< 妫€娴嬫鍙充笅瑙抷鍧愭爣
};

/// @brief 妫€娴嬬粨鏋滅粨鏋勪綋
struct DetectionResult {
    std::vector<DetectionBox> boxes; ///< 妫€娴嬫鍒楄〃
};

/// @brief 鍒嗙被缁撴灉缁撴瀯浣?
struct ClassificationResult {
    int class_id;   ///< 鍒嗙被缁撴灉鐨勭被鍒獻D
    float score;   ///< 鍒嗙被缁撴灉鐨勭疆淇″害
};

/// @brief 鍒嗗壊鎺╃爜缁撴瀯浣?
struct SegmentationMask {
    int width;   ///< 鍒嗗壊鎺╃爜鐨勫搴?
    int height;   ///< 鍒嗗壊鎺╃爜鐨勯珮搴?

    std::vector<uint8_t> mask; ///< 鍒嗗壊鎺╃爜鏁版嵁
};

/// @brief 鍒嗗壊缁撴灉缁撴瀯浣?
struct SegmentationResult {
    std::vector<SegmentationMask> masks; ///< 鍒嗗壊鎺╃爜鍒楄〃
};


/// @brief 鍏抽敭鐐圭粨鏋勪綋
struct KeyPoint {
    float x;       ///< 鍏抽敭鐐箈鍧愭爣
    float y;       ///< 鍏抽敭鐐箉鍧愭爣
    float score;    ///< 鍏抽敭鐐圭殑缃俊搴?
};

/// @brief 濮挎€佹娴嬬粨鏋滅粨鏋勪綋
struct PoseResult {
    std::vector<KeyPoint> keypoints; ///< 鍏抽敭鐐瑰垪琛?
};
#endif
///////////////////////////////////////////////////////////////////
struct Rectangle {
    float x;       ///< 鐭╁舰宸︿笂瑙抶鍧愭爣
    float y;       ///< 鐭╁舰宸︿笂瑙抷鍧愭爣
    float width;   ///< 鐭╁舰鐨勫搴?
    float height;  ///< 鐭╁舰鐨勯珮搴?
};

/// @brief 鍏抽敭鐐圭粨鏋勪綋
struct KeyPoint {
    float x;       ///< 鍏抽敭鐐箈鍧愭爣
    float y;       ///< 鍏抽敭鐐箉鍧愭爣
    float score;    ///< 鍏抽敭鐐圭殑缃俊搴?
};

/// @brief 瀵硅薄鍏冩暟鎹粨鏋勪綋锛堢涓€闃舵锛氱洰鏍囨娴嬶級
struct ObjectMeta {
    int id{-1}; ///< 瀵硅薄ID
    int class_id{-1}; ///< 瀵硅薄鐨勭被鍒獻D
    float score{0.0f}; ///< 瀵硅薄鐨勭疆淇″害
    Rectangle rect; ///< 瀵硅薄鐨勭煩褰㈠尯鍩?
};

/// @brief 濮挎€佺粨鏋滅粨鏋勪綋
struct PoseMeta {    
    std::vector<KeyPoint> keypoints; ///< 鍏抽敭鐐瑰悕绉板垪琛?
};

/// @brief 鍒嗗壊鎺╃爜鍏冩暟鎹粨鏋勪綋
struct MaskMeta {
    int width;   ///< 鍒嗗壊鎺╃爜鐨勫搴?
    int height;   ///< 鍒嗗壊鎺╃爜鐨勯珮搴?

    std::vector<uint8_t> mask; ///< 鍒嗗壊鎺╃爜鏁版嵁
};

/// @brief 鍒嗙被鍏冩暟鎹粨鏋勪綋锛堢浜岄樁娈碉細鍒嗙被锛?
struct ClassificationMeta {
    int class_id;   ///< 鍒嗙被缁撴灉鐨勭被鍒獻D
    float score;   ///< 鍒嗙被缁撴灉鐨勭疆淇″害
};

/// @brief 瀵硅薄缁撴灉缁撴瀯浣擄紙绗簩闃舵锛氬垎绫伙級
struct ObjectResult {
    ObjectMeta object;  ///< 瀵硅薄鍏冩暟鎹?
    std::optional<PoseMeta> pose; ///< 濮挎€佺粨鏋?
    std::optional<MaskMeta> mask;   ///< 鍒嗗壊鎺╃爜缁撴灉
    std::optional<ClassificationMeta> cls; ///< 鍒嗙被缁撴灉
};

/// @brief 甯х粨鏋滅粨鏋勪綋锛堟眹鎬伙級
struct FrameResult {
    uint64_t frame_id; ///< 甯D
    uint64_t pts; ///< 鏃堕棿鎴?
    std::vector<ObjectResult> objects;  ///< 瀵硅薄缁撴灉鍒楄〃
};

/// @brief 妫€娴嬬粨鏋滅粨鏋勪綋
struct DetectionBox {
    int class_id; ///< 妫€娴嬫鐨勭被鍒獻D
    float confidence; ///< 妫€娴嬫鐨勭疆淇″害
    float x1; ///< 妫€娴嬫宸︿笂瑙抶鍧愭爣
    float y1; ///< 妫€娴嬫宸︿笂瑙抷鍧愭爣
    float x2; ///< 妫€娴嬫鍙充笅瑙抶鍧愭爣
    float y2; ///< 妫€娴嬫鍙充笅瑙抷鍧愭爣
};