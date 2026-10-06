#ifndef CONVAI_SCENE_AUTO_TAGGER_CORE_H
#define CONVAI_SCENE_AUTO_TAGGER_CORE_H

#include <stdint.h>

#if defined(_WIN32)
#  define CONVAI_SAT_CALL __cdecl
#  if defined(CONVAI_SAT_CORE_BUILD)
#    define CONVAI_SAT_API __declspec(dllexport)
#  elif defined(CONVAI_SAT_DYNAMIC_LOAD)
#    define CONVAI_SAT_API
#  else
#    define CONVAI_SAT_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define CONVAI_SAT_CALL
#  define CONVAI_SAT_API __attribute__((visibility("default")))
#else
#  define CONVAI_SAT_CALL
#  define CONVAI_SAT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CONVAI_SAT_ABI_VERSION_4 4u
#define CONVAI_SAT_ERROR_MESSAGE_CAPACITY 256u
#define CONVAI_SAT_EVIDENCE_DESCRIPTOR_SIDE 32u
#define CONVAI_SAT_EVIDENCE_DESCRIPTOR_PIXELS 1024u
#define CONVAI_SAT_EVIDENCE_PAYLOAD_CAPACITY 5128u
#define CONVAI_SAT_MAX_IMAGE_DIMENSION 8192u
#define CONVAI_SAT_MAX_IMAGE_PIXELS 16777216u
#define CONVAI_SAT_MAX_COMPONENT_PROXY_POINTS 8u
#define CONVAI_SAT_MAX_VIEW_CANDIDATES 4u
#define CONVAI_SAT_MAX_COMPONENT_PROXIES 4096u
#define CONVAI_SAT_VISION_MAX_IMAGE_BYTES (10u * 1024u * 1024u)
#define CONVAI_SAT_VISION_MAX_CELLS_PER_SHEET 64u

typedef int32_t convai_sat_status;
enum
{
    CONVAI_SAT_STATUS_OK = 0,
    CONVAI_SAT_STATUS_INVALID_ARGUMENT = 1,
    CONVAI_SAT_STATUS_UNSUPPORTED_ABI = 2,
    CONVAI_SAT_STATUS_OUT_OF_MEMORY = 3,
    CONVAI_SAT_STATUS_INTERNAL_ERROR = 4,
    /* 5 was BUFFER_TOO_SMALL; retired with the ABI v2 buffer negotiation. */
    CONVAI_SAT_STATUS_TRANSPORT_UNAVAILABLE = 6,
    CONVAI_SAT_STATUS_REQUEST_FAILED = 7
};

typedef uint32_t convai_sat_pixel_format;
enum
{
    CONVAI_SAT_PIXEL_FORMAT_BGRA8_UNORM = 1u,
    CONVAI_SAT_PIXEL_FORMAT_RGBA8_UNORM = 2u
};

typedef uint32_t convai_sat_view_plan_input_flags;
enum
{
    CONVAI_SAT_VIEW_PLAN_INPUT_NONE = 0u,
    CONVAI_SAT_VIEW_PLAN_HAS_PRESENTATION_HINT = 1u << 0,
    CONVAI_SAT_VIEW_PLAN_SINGLE_EXPLICIT_METALLIC_SURFACE = 1u << 1,
    /* The caller reflected a left-handed source frame across Y. Plan in that
     * source frame and reflect the ordered candidates back, preserving the
     * archived source-frame sign and fallback policy exactly. */
    CONVAI_SAT_VIEW_PLAN_PRESERVE_REFLECTED_Y_ORDER = 1u << 2
};

/* Caller-provided facts about the candidate produced by engine-side
 * discovery. These are inputs to the eligibility decision; the core never
 * derives them. */
typedef uint32_t convai_sat_significance_reason_flags;
enum
{
    CONVAI_SAT_SIGNIFICANCE_REASON_NONE = 0u,
    CONVAI_SAT_SIGNIFICANCE_REASON_OBJECT_SCALE = 1u << 0,
    CONVAI_SAT_SIGNIFICANCE_REASON_LARGE_GEOMETRY = 1u << 1,
    CONVAI_SAT_SIGNIFICANCE_REASON_VERY_SMALL = 1u << 2,
    CONVAI_SAT_SIGNIFICANCE_REASON_MULTI_PART = 1u << 3,
    CONVAI_SAT_SIGNIFICANCE_REASON_MATERIAL_DETAIL = 1u << 4,
    CONVAI_SAT_SIGNIFICANCE_REASON_IMPORTANT_HINT = 1u << 5,
    CONVAI_SAT_SIGNIFICANCE_REASON_STRUCTURAL_HINT = 1u << 6,
    CONVAI_SAT_SIGNIFICANCE_REASON_AUTHORED_LABEL = 1u << 7,
    CONVAI_SAT_SIGNIFICANCE_REASON_PLAIN_ENGINE_PRIMITIVE = 1u << 8,
    CONVAI_SAT_SIGNIFICANCE_REASON_AUTHORED_PANEL = 1u << 9,
    CONVAI_SAT_SIGNIFICANCE_REASON_LOW_GEOMETRY_AUTHORED_SURFACE = 1u << 10,
    CONVAI_SAT_SIGNIFICANCE_REASON_LOW_GEOMETRY = 1u << 11
};

typedef uint32_t convai_sat_view_plan_flags;
enum
{
    CONVAI_SAT_VIEW_PLAN_NONE = 0u,
    CONVAI_SAT_VIEW_PLAN_PLANAR = 1u << 0,
    CONVAI_SAT_VIEW_PLAN_OPPOSED_BROAD_FACES = 1u << 1
};

typedef uint32_t convai_sat_view_candidate_flags;
enum
{
    CONVAI_SAT_VIEW_CANDIDATE_NONE = 0u,
    CONVAI_SAT_VIEW_CANDIDATE_SHAPE_PROFILE = 1u << 0
};

typedef uint32_t convai_sat_capture_lighting_policy;
enum
{
    CONVAI_SAT_CAPTURE_LIGHTING_PRESERVE_MATERIAL_SPECULAR = 0u,
    CONVAI_SAT_CAPTURE_LIGHTING_SUPPRESS_DIRECT_SPECULAR = 1u,
    CONVAI_SAT_CAPTURE_LIGHTING_SUPPRESS_DIRECT_SPECULAR_WITH_METAL_RECOVERY = 2u
};

/* Request-scope facts that bypass the eligibility rejection gate. */
typedef uint32_t convai_sat_capture_facts_flags;
enum
{
    CONVAI_SAT_CAPTURE_FACTS_NONE = 0u,
    CONVAI_SAT_CAPTURE_FACTS_SINGLE_CANDIDATE_REQUEST = 1u << 0,
    CONVAI_SAT_CAPTURE_FACTS_REVIEW_BATCH_REQUEST = 1u << 1,
    CONVAI_SAT_CAPTURE_FACTS_FORCE_EXPLICIT_SCOPE = 1u << 2,
    CONVAI_SAT_CAPTURE_FACTS_OPPOSED_BROAD_FACE = 1u << 3,
    CONVAI_SAT_CAPTURE_FACTS_HAS_EXISTING_OBJECT = 1u << 4
};

typedef uint32_t convai_sat_eligibility_decision;
enum
{
    CONVAI_SAT_ELIGIBILITY_RETAIN = 0u,
    CONVAI_SAT_ELIGIBILITY_EXCLUDE = 1u
};

typedef uint32_t convai_sat_eligibility_reason;
enum
{
    CONVAI_SAT_ELIGIBILITY_REASON_NONE = 0u,
    CONVAI_SAT_ELIGIBILITY_REASON_OBJECT_NOT_CLEARLY_VISIBLE = 1u,
    CONVAI_SAT_ELIGIBILITY_REASON_NO_USEFUL_VISUAL_DETAIL = 2u
};

#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
#pragma pack(push, 8)
#endif

typedef struct convai_sat_error_v1
{
    uint32_t struct_size;
    int32_t code;
    char message[CONVAI_SAT_ERROR_MESSAGE_CAPACITY];
} convai_sat_error_v1;

/*
 * Read-only, caller-owned image memory. The library never retains a pointer.
 * geometry_mask is optional; a nonzero byte marks object geometry. When it is
 * absent, RGB distance from background_rgba is used to derive foreground.
 */
typedef struct convai_sat_image_view_v1
{
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t row_stride_bytes;
    convai_sat_pixel_format pixel_format;
    const uint8_t* pixels;
    uint64_t pixel_bytes;
    const uint8_t* geometry_mask;
    uint32_t geometry_mask_row_stride_bytes;
    uint32_t background_epsilon;
    uint64_t geometry_mask_bytes;
    uint8_t background_rgba[4];
    uint32_t reserved[4];
} convai_sat_image_view_v1;

/*
 * Rendered-view evidence produced by select_view for every probe image. All
 * fields are core-written evidence; shape_profile_candidate and
 * front_preference are derived from the view plan inside select_view.
 * All booleans use uint32_t at the ABI boundary.
 */
typedef struct convai_sat_view_metrics_v1
{
    uint32_t struct_size;
    uint32_t valid;
    int32_t foreground_pixels;
    int32_t interior_pixels;
    float foreground_fraction;
    float border_touch_fraction;
    float color_entropy;
    float edge_detail;
    float silhouette_information;
    uint32_t shape_profile_candidate;
    int32_t central_foreground_pixels;
    float central_tone_entropy;
    float central_color_entropy;
    float central_edge_detail;
    float central_foreground_fill;
    float central_authored_content;
    float capture_quality;
    float front_preference;
    float selection_score;
    uint32_t reserved[8];
} convai_sat_view_metrics_v1;

/*
 * Opaque, fixed-capacity descriptor. Its payload is an implementation detail;
 * only evaluate_capture and compare_evidence may interpret it. Descriptors are
 * run-local and may only be compared when both were created by a core exposing
 * the same view_policy_id_utf8 value.
 */
typedef struct convai_sat_evidence_descriptor_v1
{
    uint32_t struct_size;
    uint32_t descriptor_version;
    uint32_t payload_size;
    uint32_t reserved;
    uint8_t payload[CONVAI_SAT_EVIDENCE_PAYLOAD_CAPACITY];
} convai_sat_evidence_descriptor_v1;

typedef struct convai_sat_vec3d
{
    double x;
    double y;
    double z;
} convai_sat_vec3d;

/* Actor-local proxy points, normally the eight transformed component corners. */
typedef struct convai_sat_component_proxy_v1
{
    uint32_t struct_size;
    uint32_t point_count;
    convai_sat_vec3d points[CONVAI_SAT_MAX_COMPONENT_PROXY_POINTS];
    uint32_t reserved[4];
} convai_sat_component_proxy_v1;

typedef struct convai_sat_view_plan_input_v1
{
    uint32_t struct_size;
    convai_sat_view_plan_input_flags flags;
    uint32_t component_count;
    uint32_t reserved0;
    const convai_sat_component_proxy_v1* components;
    /* Right-handed actor-local direction from the object toward the preferred
     * camera. +Z is up. Ignored unless HAS_PRESENTATION_HINT is set, but must
     * always be finite — zero-initialize when unused. */
    convai_sat_vec3d presentation_direction_actor_local;
    float presentation_preference;
    uint32_t reserved[5];
} convai_sat_view_plan_input_v1;

typedef struct convai_sat_view_candidate_v1
{
    /* Right-handed actor-local direction from the object toward the camera;
     * +Z is up. The caller chooses distance and performs world conversion. */
    convai_sat_vec3d direction_actor_local;
    float front_preference;
    convai_sat_view_candidate_flags flags;
    uint32_t reserved[4];
} convai_sat_view_candidate_v1;

typedef struct convai_sat_view_plan_v1
{
    uint32_t struct_size;
    uint32_t candidate_count;
    convai_sat_view_plan_flags flags;
    convai_sat_capture_lighting_policy lighting_policy;
    convai_sat_view_candidate_v1 candidates[CONVAI_SAT_MAX_VIEW_CANDIDATES];
    uint32_t reserved[8];
} convai_sat_view_plan_v1;

/*
 * Probe images submitted in view-plan candidate order: images[i] is the
 * engine's render of plan->candidates[i]. image_count must not exceed the
 * plan's candidate_count. A failed render keeps its slot so later candidates
 * stay aligned with their plan entries: submit it with pixels NULL and
 * pixel_bytes 0, and it is excluded from analysis and selection. At least one
 * probe must be present.
 */
typedef struct convai_sat_select_view_input_v4
{
    uint32_t struct_size;
    uint32_t image_count;
    const convai_sat_view_plan_v1* plan;
    const convai_sat_image_view_v1* images;
    uint32_t reserved[4];
} convai_sat_select_view_input_v4;

/*
 * metrics[i] is the evidence for images[i]; entries beyond metric_count are
 * zeroed. Selection semantics are archived policy: strict score comparison,
 * exact ties retain input order.
 */
typedef struct convai_sat_select_view_result_v4
{
    uint32_t struct_size;
    int32_t selected_index;
    float selection_margin;
    float selected_score;
    uint32_t metric_count;
    convai_sat_view_metrics_v1 metrics[CONVAI_SAT_MAX_VIEW_CANDIDATES];
    uint32_t reserved[4];
} convai_sat_select_view_result_v4;

/*
 * Final-capture eligibility facts. probe_metrics normally points at the
 * metrics array a select_view call produced for this candidate (all probes,
 * not only the winner); it may be empty, and entries with a zero struct_size
 * (select_view's unused tail, absent probes) are skipped rather than
 * rejected. significance facts come from engine-side discovery and are never
 * derived by the core.
 */
typedef struct convai_sat_evaluate_capture_input_v4
{
    uint32_t struct_size;
    convai_sat_capture_facts_flags flags;
    float significance_score;
    uint32_t primitive_count;
    convai_sat_significance_reason_flags significance_reason_flags;
    uint32_t probe_metric_count;
    const convai_sat_view_metrics_v1* probe_metrics;
    convai_sat_image_view_v1 final_image;
    uint32_t reserved[4];
} convai_sat_evaluate_capture_input_v4;

/*
 * The function initializes decision to RETAIN before validating inputs, so
 * malformed or unavailable evidence fails closed rather than excluding an
 * object. A non-NONE reason is produced only with EXCLUDE. The evidence
 * descriptor is built for every valid final image — including early-retained
 * washed-out captures — and carries content exactly when the image yields
 * informative evidence; descriptor_valid mirrors informative_evidence, and
 * the descriptor is zeroed when uninformative.
 */
typedef struct convai_sat_evaluate_capture_result_v4
{
    uint32_t struct_size;
    convai_sat_eligibility_decision decision;
    convai_sat_eligibility_reason reason;
    uint32_t readable_centered_coverage;
    uint32_t informative_evidence;
    uint32_t meaningful_mid_scale_structure;
    uint32_t descriptor_valid;
    uint32_t reserved0;
    convai_sat_evidence_descriptor_v1 descriptor;
    uint32_t reserved[4];
} convai_sat_evaluate_capture_result_v4;

typedef convai_sat_status (CONVAI_SAT_CALL *convai_sat_build_view_plan_fn)(
    const convai_sat_view_plan_input_v1* input,
    convai_sat_view_plan_v1* out_plan,
    convai_sat_error_v1* out_error);

typedef convai_sat_status (CONVAI_SAT_CALL *convai_sat_select_view_fn)(
    const convai_sat_select_view_input_v4* input,
    convai_sat_select_view_result_v4* out_result,
    convai_sat_error_v1* out_error);

typedef convai_sat_status (CONVAI_SAT_CALL *convai_sat_evaluate_capture_fn)(
    const convai_sat_evaluate_capture_input_v4* input,
    convai_sat_evaluate_capture_result_v4* out_result,
    convai_sat_error_v1* out_error);

typedef convai_sat_status (CONVAI_SAT_CALL *convai_sat_compare_evidence_fn)(
    const convai_sat_evidence_descriptor_v1* left,
    const convai_sat_evidence_descriptor_v1* right,
    uint32_t* out_equivalent,
    convai_sat_error_v1* out_error);

/*
 * Vision family. Unlike the deterministic functions above, these perform
 * network I/O: they post to the Convai vision endpoint through the
 * convai_http_helper runtime library, which is loaded dynamically from the
 * core's own directory at first use. When the helper is absent the vision
 * functions fail with CONVAI_SAT_STATUS_TRANSPORT_UNAVAILABLE and everything
 * else in this library keeps working (see docs/adr/0001 and 0003).
 *
 * Calls are blocking and single-shot: one HTTP attempt per call, no retries.
 * Callers own threading, retry policy, backoff, and rate limiting.
 *
 * The endpoint base URL resolves in priority order:
 *   1. a -ConvaiProdURL=<base> flag on the process command line;
 *   2. a "base_url" key in the request's parameter list;
 *   3. the built-in production endpoint.
 */

typedef uint32_t convai_sat_vision_task;
enum
{
    /* Numbered contact sheet; one returned object per occupied cell. */
    CONVAI_SAT_VISION_TASK_OBJECT_TAGGING = 0u,
    /* One image of one complete assembly; one returned object whose
     * description covers the whole assembly. */
    CONVAI_SAT_VISION_TASK_ASSEMBLY_DESCRIPTION = 1u,
    /* One image of a visible environment; one returned object whose
     * description covers the scene as a whole. */
    CONVAI_SAT_VISION_TASK_SCENE_CONTEXT_DESCRIPTION = 2u
};

/*
 * Transport and server parameters as an extensible key/value list. Reserved
 * keys consumed by the core: "api_key" becomes the CONVAI-API-KEY header,
 * "auth_token" becomes the API-AUTH-TOKEN header, "base_url" overrides the
 * endpoint base; their values must be non-empty. The keys "prompt" and
 * "image" are rejected — those request parts are owned by the core. Every
 * other key (for example "character_id", "model", "session_id") is forwarded
 * verbatim as a multipart form field, so a new server-side parameter is a new
 * key, not an ABI change.
 */
typedef struct convai_sat_kv_v4
{
    const char* key_utf8;
    const char* value_utf8;
} convai_sat_kv_v4;

/* One occupied contact-sheet cell, in visual numbering order (index is the
 * 1-based caller order). All strings are UTF-8; optional fields may be NULL. */
typedef struct convai_sat_vision_cell_v3
{
    uint32_t struct_size;
    uint32_t context_only;
    const char* echo_id_utf8;
    const char* previous_proposal_name_utf8;
    const char* previous_proposal_description_utf8;
    const char* refinement_note_utf8;
    uint32_t reserved[4];
} convai_sat_vision_cell_v3;

typedef struct convai_sat_vision_image_v3
{
    uint32_t struct_size;
    uint32_t cell_count;
    /* Unique basename within the request; results map back by it. */
    const char* file_name_utf8;
    const uint8_t* png_bytes;
    uint64_t png_size;
    const convai_sat_vision_cell_v3* cells;
    uint32_t reserved[4];
} convai_sat_vision_image_v3;

/*
 * Vision Autotag: the core's protected prompt template and structured
 * response parsing. cells describe the contact-sheet contents; the response
 * maps back by image file name and cell echo id. The current protocol
 * generation addresses exactly one contact sheet, so image_count must be 1;
 * the array shape is kept so a multi-sheet protocol is a protocol-id bump,
 * not an ABI break.
 */
typedef struct convai_sat_vision_autotag_request_v4
{
    uint32_t struct_size;
    convai_sat_vision_task task;
    const char* scene_description_utf8;
    const char* description_focus_utf8;
    uint32_t image_count;
    int32_t timeout_seconds; /* <= 0 selects the built-in default */
    const convai_sat_vision_image_v3* images;
    uint32_t param_count;
    uint32_t reserved0;
    const convai_sat_kv_v4* params;
    uint32_t reserved[4];
} convai_sat_vision_autotag_request_v4;

/*
 * Vision Request: a caller-supplied prompt posted to the same endpoint. The
 * protected template is never applied and the response body is returned raw
 * rather than parsed. images are optional (image_count may be zero) and the
 * count is uncapped here — the server owns the per-request limit; cells are
 * ignored. Requires a "character_id" param and either "api_key" or
 * "auth_token".
 */
typedef struct convai_sat_vision_request_v4
{
    uint32_t struct_size;
    uint32_t image_count;
    const char* prompt_utf8;
    int32_t timeout_seconds; /* <= 0 selects the built-in default */
    uint32_t reserved0;
    const convai_sat_vision_image_v3* images;
    uint32_t param_count;
    uint32_t reserved1;
    const convai_sat_kv_v4* params;
    uint32_t reserved[4];
} convai_sat_vision_request_v4;

/* Opaque; pointers handed out by vision_result_view stay valid until the
 * result is freed with vision_result_free. Both allocation and free live
 * inside this library. */
typedef struct convai_sat_vision_result convai_sat_vision_result;

typedef struct convai_sat_vision_object_view_v3
{
    uint32_t struct_size;
    int32_t cell_index; /* 1-based within its image; 0 when absent */
    float confidence;
    uint32_t reserved0;
    const char* echo_id_utf8;
    const char* name_utf8;
    const char* description_utf8;
    uint32_t reserved[4];
} convai_sat_vision_object_view_v3;

typedef struct convai_sat_vision_result_image_v4
{
    uint32_t struct_size;
    uint32_t object_count;
    /* Matches the request image's file_name_utf8. */
    const char* file_name_utf8;
    const convai_sat_vision_object_view_v3* objects;
    uint32_t reserved[4];
} convai_sat_vision_result_image_v4;

/*
 * Complete read-out of one vision result. All pointers reference memory owned
 * by the result handle. images is non-empty only for parsed Vision Autotag
 * responses; raw_body_utf8 always carries the complete response body.
 * server_message_utf8 is a short server-provided detail for non-2xx
 * responses; NULL when none.
 */
typedef struct convai_sat_vision_result_view_v4
{
    uint32_t struct_size;
    int32_t http_status;
    uint32_t image_count;
    uint32_t reserved0;
    const char* server_message_utf8;
    const char* raw_body_utf8;
    uint64_t raw_body_size;
    const convai_sat_vision_result_image_v4* images;
    uint32_t reserved[4];
} convai_sat_vision_result_view_v4;

/*
 * Both request functions return OK whenever an HTTP response was received,
 * including 4xx/5xx; the caller reads the status code from the result view
 * (429 means rate limited and retryable). They fail with
 * TRANSPORT_UNAVAILABLE when the helper library is absent and REQUEST_FAILED
 * when no response was received or a 2xx autotag body did not parse.
 */
typedef convai_sat_status (CONVAI_SAT_CALL *convai_sat_vision_autotag_fn)(
    const convai_sat_vision_autotag_request_v4* request,
    convai_sat_vision_result** out_result,
    convai_sat_error_v1* out_error);

typedef convai_sat_status (CONVAI_SAT_CALL *convai_sat_vision_request_fn)(
    const convai_sat_vision_request_v4* request,
    convai_sat_vision_result** out_result,
    convai_sat_error_v1* out_error);

typedef convai_sat_status (CONVAI_SAT_CALL *convai_sat_vision_result_view_fn)(
    const convai_sat_vision_result* result,
    convai_sat_vision_result_view_v4* out_view,
    convai_sat_error_v1* out_error);

typedef void (CONVAI_SAT_CALL *convai_sat_vision_result_free_fn)(
    convai_sat_vision_result* result);

typedef struct convai_sat_api_v4
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t reserved[2];
    const char* core_version_utf8;
    /* Opaque policy identity covering view planning, selection, eligibility,
     * and evidence descriptors as one deterministic-policy generation. */
    const char* view_policy_id_utf8;
    /* Opaque protocol identity covering the embedded prompt templates and
     * request encoding; changes whenever either changes. */
    const char* vision_protocol_id_utf8;
    convai_sat_build_view_plan_fn build_view_plan;
    convai_sat_select_view_fn select_view;
    convai_sat_evaluate_capture_fn evaluate_capture;
    convai_sat_compare_evidence_fn compare_evidence;
    convai_sat_vision_autotag_fn vision_autotag;
    convai_sat_vision_request_fn vision_request;
    convai_sat_vision_result_view_fn vision_result_view;
    convai_sat_vision_result_free_fn vision_result_free;
} convai_sat_api_v4;

#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
#pragma pack(pop)
#endif

/* The library's only exported symbol. Returns NULL for an unsupported ABI. */
CONVAI_SAT_API const void* CONVAI_SAT_CALL
convai_sat_get_api(uint32_t requested_abi_version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CONVAI_SCENE_AUTO_TAGGER_CORE_H */
