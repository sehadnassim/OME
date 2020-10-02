//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Benjamin
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <fstream>
#include <mutex>

#define OV_LOG_DIR              "logs"
#define OV_LOG_DIR_SVC          "/var/log/ovenmediaengine"
#define OV_LOG_FILE             "ovenmediaengine.log"

//TODO(Getroot): This is temporary code for testing. This will change to more elegant code in the future.
#define OV_STAT1_LOG_FILE       "ovenmediaengine_webrtc_stat.log"
#define OV_STAT2_LOG_FILE       "hls_rtsp_session.log"
#define OV_STAT3_LOG_FILE       "hls_rtsp_reqeuest.log"
#define OV_STAT4_LOG_FILE       "hls_rtsp_viewers.log"

namespace ov
{
    class LogWrite
    {
    public:
        LogWrite(std::string log_file_name);
        virtual ~LogWrite() = default;
        void Write(const char* log);
        void SetLogPath(const char* log_path);

        static void Initialize(bool start_service);

    private:
        void Initialize();

        std::mutex _log_stream_mutex;
        std::ofstream _log_stream;
        int _last_day;
        std::string _log_path;
        std::string _log_file_name;
        std::string _log_file;

        static bool _start_service;
    };
}

