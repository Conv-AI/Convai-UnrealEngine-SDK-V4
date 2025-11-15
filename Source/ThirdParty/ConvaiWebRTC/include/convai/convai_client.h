#ifndef CONVAI_CLIENT_H
#define CONVAI_CLIENT_H

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

    class CONVAI_CLIENT_API ConvaiClient
    {
    public:
        explicit ConvaiClient(const char *logFilePath = "", const bool &bCaptureFFILogs = false);
        ~ConvaiClient();

        bool Initialize(bool aec_enabled = true, bool vad_enabled = true);
        bool Connect(const char *url, const char *auth_value, const char *auth_header, const char *char_id, const char *connection_type, const char *llm_provider, const char *blendshape_provider);
        void Disconnect();
        bool IsConnected() const;
        bool StartAudioPublishing();
        bool StartVideoPublishing(uint32_t Width, uint32_t Height);
        bool SendTextMessage(const char *message);
        bool SendTriggerMessage(const char *trigger_name, const char *trigger_message);
        bool UpdateTemplateKeys(const char *template_keys_json);
        bool UpdateDynamicInfo(const char *context_text);
        void SendAudio(const int16_t *audio_data, size_t num_frames);
        void SendReferenceAudio(const int16_t *audio_data, size_t num_frames);
        void SendImage(uint32_t Width, uint32_t Height, uint8_t* data_ptr);

        // Callbacks
        void SetConvaiClientListner(IConvaiClientListner *Listner);

    private:
        std::shared_ptr<ConvaiClientImpl> impl_;
    };

    class CONVAI_CLIENT_API IConvaiClientListner
    {
    public:
        virtual ~IConvaiClientListner() = default;
        virtual void OnConnectedToServer() = 0;
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