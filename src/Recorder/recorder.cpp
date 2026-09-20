#include "recorder.hpp"
#include <sstream>
#include <CCGL.h>
#include <filesystem>
#include <fstream>
#include "../Logic/logic.hpp"
#include <cstdlib>

// completely stolen from mat, sorry dude

// changed very little!

std::string narrow(const wchar_t* str);
inline auto narrow(const std::wstring& str) { return narrow(str.c_str()); }
std::wstring widen(const char* str);
inline auto widen(const std::string& str) { return widen(str.c_str()); }

std::string narrow(const wchar_t* str) {
    int size = WideCharToMultiByte(CP_UTF8, 0, str, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) { /* fuck */ }
    auto buffer = new char[size];
    WideCharToMultiByte(CP_UTF8, 0, str, -1, buffer, size, nullptr, nullptr);
    std::string result(buffer, size_t(size) - 1);
    delete[] buffer;
    return result;
}

std::wstring widen(const char* str) {
    int size = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
    if (size <= 0) { /* fuck */ }
    auto buffer = new wchar_t[size];
    MultiByteToWideChar(CP_UTF8, 0, str, -1, buffer, size);
    std::wstring result(buffer, size_t(size) - 1);
    delete[] buffer;
    return result;
}

Recorder::Recorder() : m_width(1920), m_height(1080), m_fps(60) {}

void Recorder::start(const std::string& path) {
     /*AllocConsole();
     static std::ofstream conout("CONOUT$", std::ios::out);
     std::cout.rdbuf(conout.rdbuf());*/
    auto& logic = Logic::get();
    m_recording = true;
    m_frame_has_data = false;
    try {
        m_current_frame.resize(m_width * m_height * 3, 0);
    } catch (const std::exception& e) {
        // At 8K this is a ~99.5MB request; in a 32-bit process with a fragmented
        // heap that can fail even when total free memory looks sufficient.
        // Previously this threw uncaught here and crashed the whole game -- now
        // it fails as a clean, visible error instead.
        logic.error = "Couldn't allocate the " + std::to_string(m_width) + "x" + std::to_string(m_height) +
            " frame buffer (" + e.what() + "). Try freeing memory (fewer overlays/mods loaded, a fresh "
            "game launch) or confirm GD.exe is Large-Address-Aware.";
        m_recording = false;
        return;
    }
    m_finished_level = false;
    m_last_frame_t = m_extra_t = 0;
    m_after_end_extra_time = 0.f;
    m_renderer.m_width = m_width;
    m_renderer.m_height = m_height;
    if (!m_renderer.begin(m_current_frame.data())) {
        m_recording = false;
        return;
    }
    auto gm = gd::GameManager::sharedState();
    auto play_layer = gm->getPlayLayer();
    auto song_file = play_layer->m_level->getAudioFileName();
    auto fade_in = true;
    auto fade_out = true;
    auto bg_volume = gm->m_fBackgroundMusicVolume;
    auto sfx_volume = gm->m_fEffectsVolume;
    if (play_layer->m_level->songID == 0)
        song_file = CCFileUtils::sharedFileUtils()->fullPathForFilename(song_file.c_str(), false);
    auto is_testmode = play_layer->m_isTestMode;
    auto song_offset = m_song_start_offset;

    if (!std::filesystem::exists("ffmpeg\\ffmpeg.exe")) {
        logic.error = "FFmpeg not found! Download FFmpeg though the Echo installer!";
        return;
    }


    std::thread([&, path, song_file, fade_in, fade_out, bg_volume, sfx_volume, is_testmode, song_offset]() {
        std::stringstream stream;
        stream << '"' << "ffmpeg\\ffmpeg.exe" << '"' << " -y -f rawvideo -pix_fmt rgb24 -s " << m_width << "x" << m_height << " -r " << m_fps
            << " -i - ";
        if (!m_codec.empty())
            stream << "-c:v " << m_codec << " ";
        stream << "-b:v " << m_bitrate << "M ";
        if (!m_extra_args.empty())
            stream << m_extra_args << " ";
        else
            stream << "-pix_fmt yuv420p ";
        auto playlayer = gd::GameManager::sharedState()->getPlayLayer();
        int end_frame = playlayer->timeForXPos(playlayer->m_endPortal->getPositionX()) * logic.fps;
        if (!m_vf_args.empty()) {
            if (color_fix) {
                stream << "-cq 0 -vf colorspace=all=bt709:iall=bt470bg:fast=1," << m_vf_args << ",\"vflip\"" << " -an \"" << path << "\" ";
            }
            else stream << "-cq 0 -vf " << m_vf_args << ",\"vflip\"" << " -an \"" << path << "\" ";
        }
        else {
            if (color_fix) {
                stream << "-cq 0 -vf colorspace=all=bt709:iall=bt470bg:fast=1,\"vflip\"" << " -an \"" << path << "\" ";
            }
            else stream << "-cq 0 -vf \"vflip\"" << " -an \"" << path << "\" ";
        }
        //m_vf_args = "";

        auto process = subprocess::Popen(stream.str());
        while (m_recording || m_frame_has_data) {
            m_lock.lock();
            if (m_frame_has_data) {
                const auto& frame = m_current_frame; // copy it
                m_frame_has_data = false;
                m_lock.unlock();
                process.m_stdin.write(frame.data(), frame.size());
            }
            else m_lock.unlock();
        }

        if (process.close()) {
            logic.error = "Error creating video file!";
            return;
        }
        if (!m_include_audio || !std::filesystem::exists(song_file)) return;
        wchar_t buffer[MAX_PATH];
        if (!GetTempFileNameW(widen(std::filesystem::temp_directory_path().string()).c_str(), L"", 0, buffer)) {
            logic.error = "Could not retrieve temp file!";
            return;
        }
        auto temp_path = narrow(buffer) + "." + std::filesystem::path(path).filename().string();
        std::filesystem::rename(buffer, temp_path);
        auto total_time = m_last_frame_t - 1.f / m_fps; // 1 frame too short?
        {
            std::stringstream stream;
            stream << '"' << "ffmpeg\\ffmpeg.exe" << '"' << " -y -ss " << song_offset << " -i \"" << song_file
                << "\" -i \"" << path << "\" -t " << total_time << " -c:v " << ((v_fade_in_time > 0 || v_fade_out_time > 0 || a_fade_in_time > 0 || a_fade_out_time > 0) ? "libx264 -preset fast -crf 10" : "copy") << " ";

            // Video fade-in and fade-out
            if (fade_in && v_fade_in_time > 0)
                stream << "-vf \"fade=in:st=0:d=" << v_fade_in_time << ",";
            if (fade_out && v_fade_out_time > 0)
                stream << "fade=out:st=" << total_time - v_fade_out_time << ":d=" << v_fade_out_time << "\" ";

            if (!m_extra_audio_args.empty())
                stream << m_extra_audio_args << " ";

            stream << "-af \"volume=1";

            // Audio fade-in
            if (!is_testmode && fade_in && a_fade_in_time > 0)
                stream << ",afade=t=in:st=0:d=" << a_fade_in_time;

            // Audio fade-out
            if (fade_out && a_fade_out_time > 0 && m_finished_level)
                stream << ",afade=t=out:st=" << total_time - a_fade_out_time << ":d=" << a_fade_out_time;
            else if (fade_out && a_fade_out_time > 0 && !m_finished_level)
                stream << ",afade=t=out:st=" << total_time - a_fade_out_time << ":d=" << a_fade_out_time;

            stream << "\" ";

            stream << "\"" << temp_path << "\" ";

            auto process = subprocess::Popen(stream.str());
            if (process.close()) {
                logic.error = "Error adding music!";
                return;
            }
        }

        std::filesystem::remove(widen(path));
        std::filesystem::rename(temp_path, widen(path));
        }).detach();
}

void Recorder::stop() {
    m_renderer.end();
    m_recording = false;
}

bool MyRenderTexture::begin(void* initial_data) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &m_old_fbo);

    m_texture = new CCTexture2D;
    {
        if (m_width <= 0) m_width = 1;
        if (m_height <= 0) m_height = 1;

        // Reuses Recorder::m_current_frame's buffer (passed in as initial_data)
        // instead of a second malloc of the same width*height*3 size. At 8K that
        // second allocation was an extra ~99.5MB needed at the exact moment of
        // session start, on top of m_current_frame's own identical-size buffer --
        // removing it halves peak memory pressure right when it mattered most.
        m_texture->initWithData(initial_data, kCCTexture2DPixelFormat_RGB888, m_width, m_height, CCSize(static_cast<float>(m_width), static_cast<float>(m_height)));
    }

    glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &m_old_rbo);

    glGenFramebuffersEXT(1, &m_fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_fbo);

    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, m_texture->getName(), 0);

    // Nothing previously checked whether the FBO actually came out usable. At 8K,
    // if the driver can't back an attachment this large, this silently records
    // into a broken FBO -- glReadPixels then returns nothing but the zero-fill
    // every frame, i.e. exactly "black video, working audio, no crash, no error".
    auto status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    bool fbo_ok = status == GL_FRAMEBUFFER_COMPLETE_EXT;
    if (!fbo_ok) {
        Logic::get().error = "Recording FBO incomplete at " + std::to_string(m_width) + "x" +
            std::to_string(m_height) + " (GL status " + std::to_string(status) +
            ") -- this resolution likely exceeds what this GPU/driver can back as a render target.";
    }

    m_texture->setAliasTexParameters();

    m_texture->autorelease();

    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, m_old_rbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_old_fbo);

    return fbo_ok;
}

void MyRenderTexture::capture(std::mutex& lock, std::vector<u8>& data, volatile bool& lul) {
    glViewport(0, 0, m_width, m_height);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &m_old_fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_fbo);

    auto director = CCDirector::sharedDirector();
    auto scene = director->getRunningScene();
    scene->visit();

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    lock.lock();
    lul = true;
    glReadPixels(0, 0, m_width, m_height, GL_RGB, GL_UNSIGNED_BYTE, data.data());
    // Report a glReadPixels failure once instead of silently shipping data's
    // untouched zero-fill (a black frame) for the entire render with no signal
    // that anything went wrong.
    static bool reported_read_error = false;
    if (!reported_read_error) {
        auto err = glGetError();
        if (err != GL_NO_ERROR) {
            Logic::get().error = "glReadPixels failed at " + std::to_string(m_width) + "x" +
                std::to_string(m_height) + " (GL error " + std::to_string(err) + ") -- frames will be black.";
            reported_read_error = true;
        }
    }
    lock.unlock();

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_old_fbo);
    director->setViewport();
}

void MyRenderTexture::end() {
    m_texture->release();
}

void Recorder::capture_frame() {
    while (m_frame_has_data) {}
    m_renderer.capture(m_lock, m_current_frame, m_frame_has_data);
}

void Recorder::handle_recording(gd::PlayLayer* play_layer, float dt) {
    double difference = play_layer->m_endPortal->getPositionX() - play_layer->m_pPlayer1->getPositionX();
    double frame_time = (60.f * Logic::get().player_speed * Logic::get().player_acceleration) * (1.f / Logic::get().fps);

    double abs_value_difference = difference > 0 ? difference : 0.f - difference;
    Logic::get().completed_level = abs_value_difference <= 349.f || play_layer->m_hasCompletedLevel;

    // timeForXPos2() isn't available on the current gd.h build -- always take
    // the ssb_fix=false fallback (play_layer->m_time) that already existed
    // here, instead of the scroll-speed-corrected value. Loses the "Scroll
    // Speed Bugfix" precision on levels with speed portals; doesn't affect
    // whether recording/capture runs at all.
    Logic::get().tfx2_calculated = play_layer->m_time;
    
    if (!Logic::get().completed_level || m_after_end_extra_time < m_after_end_duration + 3.5) {

        if (Logic::get().completed_level) {
            m_after_end_extra_time += dt;
            m_finished_level = true;
        }
        auto frame_dt = 1. / static_cast<double>(m_fps);

        if (Logic::get().completed_level || play_layer->m_isDead) {
            Logic::get().tfx2_calculated += dt;
        }

        auto time = (ssb_fix ? Logic::get().tfx2_calculated : play_layer->m_time) + m_extra_t - m_last_frame_t;

        if (time >= frame_dt) {
            // Explicit per-frame audio resync also depended on timeForXPos2();
            // removed for the same reason. Background music now plays at FMOD's
            // own pace instead of being manually re-locked to frame time on
            // every capture -- audio was already reported working before this
            // change, this doesn't touch that path, just stops re-syncing it.
            m_extra_t = time - frame_dt;
            m_last_frame_t = ssb_fix ? Logic::get().tfx2_calculated : play_layer->m_time;
            capture_frame();
        }
    }
    else {
        stop();
    }
}

void Recorder::update_song_offset(gd::PlayLayer* play_layer) {
    // timeForXPos2() unavailable on the current gd.h -- m_time is the closest
    // existing equivalent (see handle_recording() for the same substitution).
    m_song_start_offset = play_layer->m_pLevelSettings->m_songStartOffset + play_layer->m_time;
}