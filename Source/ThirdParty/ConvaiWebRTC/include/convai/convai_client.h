#ifndef CONVAI_CLIENT_H
#define CONVAI_CLIENT_H

#include <cstdint>
#include <memory>

#ifdef _WIN32
#ifdef CONVAI_BUILD_SHARED
// Suppress C4251 warnings for private implementation members
#pragma warning(push)
#pragma warning(disable : 4251)

#ifdef CONVAI_CLIENT_EXPORTS
#define CONVAI_CLIENT_API __declspec(dllexport)
#else
#define CONVAI_CLIENT_API __declspec(dllimport)
#endif
#else
#define CONVAI_CLIENT_API
#endif
#else
// Linux/Unix platforms
#ifdef CONVAI_BUILD_SHARED
#ifdef CONVAI_CLIENT_EXPORTS
#define CONVAI_CLIENT_API __attribute__((visibility("default")))
#else
#define CONVAI_CLIENT_API
#endif
#else
#define CONVAI_CLIENT_API
#endif
#endif

namespace convai
{
    class ConvaiClientImpl;
    class IConvaiClientListner;

    enum class AECType
    {
        External,  
        Internal,    
        None        
    };

    struct CONVAI_CLIENT_API ConvaiInvocationMetadata
    {
        const char* source = nullptr;
        const char* client_version = nullptr;
        const char* extra_metadata = nullptr;
    };

    struct CONVAI_CLIENT_API ConvaiVadParams
    {
        float confidence = -1.0f;   // 0.0–1.0, server default 0.7
        float start_secs = -1.0f;   // seconds, server default 0.2
        float stop_secs  = -1.0f;   // seconds, server default 2.2
        float min_volume = -1.0f;   // 0.0–1.0, server default 0.6
    };

    struct CONVAI_CLIENT_API ConvaiConnectionConfig
    {
        const char* url = nullptr;
        const char* auth_value = nullptr;
        const char* auth_header = nullptr;
        const char* character_id = nullptr;
        const char* connection_type = nullptr;
        const char* llm_provider = nullptr;
        const char* blendshape_provider = nullptr;
        const char* end_user_id = nullptr;        
        const char* end_user_metadata = nullptr;
        const char* blendshape_format = nullptr;
        const char* emotion_provider = nullptr;
        int chunk_size = 10;
        int output_fps = 90;
        float frames_buffer_duration = 0.0f;
        const ConvaiInvocationMetadata* invocation_metadata = nullptr;
        // JSON string with shape: { "actions": [...], "objects": [...], "characters": [...], "current_attention_object": "..." }
        // When non-null and non-empty, sent as top-level "action_config" in the connect request.
        const char* action_config = nullptr;
        const ConvaiVadParams* vad_params = nullptr;
    };

    struct CONVAI_CLIENT_API ConvaiAECConfig
    {
        AECType aec_type = AECType::External;
        
        // Common settings
        bool aec_enabled = true;
        bool noise_suppression_enabled = true;
        bool gain_control_enabled = true;
        
        // External AEC specific settings
        bool vad_enabled = true;        // Voice Activity Detection (External only)
        int vad_mode = 3;               // 0-3, higher = more aggressive (External only)
        
        // Internal AEC specific settings
        bool high_pass_filter_enabled = true;  // High-pass filter (Internal only)
        
        // Audio settings
        int sample_rate = 48000;
    };

    // Per-client echo-cancellation counters. The host needs these to tell a
    // canceller that is aligned from one that is merely running: mic_chunks and
    // reference_chunks must track each other to within a fraction of a percent
    // over a window, or the two streams are on different clocks and the filter
    // cannot stay locked no matter how good it is.
    struct CONVAI_CLIENT_API ConvaiAECStats
    {
        uint64_t mic_chunks = 0;         // 10 ms chunks pushed through the canceller
        uint64_t reference_chunks = 0;   // 10 ms far-end chunks fed to the canceller
        uint64_t reference_calls = 0;    // SendReferenceAudio invocations
        uint64_t published_frames = 0;   // frames handed to the transport
        uint64_t dropped_frames = 0;     // frames the publish queue shed under backlog
        uint64_t queue_depth = 0;        // frames waiting to be published right now
        double last_rms_in = 0.0;        // mic RMS entering the canceller
        double last_rms_out = 0.0;       // mic RMS leaving it
    };

    // Get library version
    CONVAI_CLIENT_API const char* GetConvaiClientVersion();

    class CONVAI_CLIENT_API ConvaiClient
    {
    public:
        explicit ConvaiClient();
        ~ConvaiClient();

        bool Initialize(const ConvaiAECConfig& aec_config);
        bool Connect(const ConvaiConnectionConfig& config);
        void Disconnect();
        bool IsConnected() const;
        bool StartAudioPublishing();
        bool StartVideoPublishing(uint32_t Width, uint32_t Height);
        bool StopVideoPublishing();
        bool SendMessageWithLabel(const char *label, const char *type, const char *data_json);
        bool SendRawMessage(const char *json_str);
        void SendAudio(const int16_t *audio_data, size_t num_frames);
        void SendReferenceAudio(const int16_t *audio_data, size_t num_frames);
        void SendImage(uint32_t Width, uint32_t Height, uint8_t* data_ptr);
        AECType GetActiveAECType() const;

        // Delay hint from playout to capture, in milliseconds. The canceller
        // estimates this itself; the hint only shortens re-convergence. Ignored
        // by backends that own their own alignment (External). Returns false if
        // no canceller is active or the backend refused it.
        bool SetStreamDelay(int32_t delay_ms);

        // Snapshot of this client's AEC counters. False when no canceller is
        // active, in which case out is left untouched.
        bool GetAECStats(ConvaiAECStats& out) const;

        // Callbacks
        void SetConvaiClientListner(IConvaiClientListner *Listner);

    private:
        std::shared_ptr<ConvaiClientImpl> impl_;
    };

    class CONVAI_CLIENT_API IConvaiClientListner
    {
    public:
        virtual ~IConvaiClientListner() = default;
        virtual void OnConnectedToServer(const char* session_id, const char* char_session_id) = 0;
        virtual void OnDisconnectedFromServer() = 0;
        virtual void OnAttendeeConnected(const char *attendee_id) = 0;
        virtual void OnAttendeeDisconnected(const char *attendee_id) = 0;
        virtual void OnActiveSpeakerChanged(const char *Speaker) = 0;
        virtual void OnAudioData(const char *attendee_id, const int16_t *audio_data, size_t num_frames,
                                 uint32_t sample_rate, uint32_t bits_per_sample, uint32_t num_channels) = 0;
        virtual void OnDataPacketReceived(const char *JsonData, const char *attendee_id) = 0;
        virtual void OnLog(const char *log_message) = 0;
    };
} // namespace convai

#ifdef _WIN32
#ifdef CONVAI_BUILD_SHARED
#pragma warning(pop)
#endif
#endif

#endif // CONVAI_CLIENT_H