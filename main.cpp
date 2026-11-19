#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <string>
#include <vector>
#include <list>
#include <cstdint>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
}

typedef HRESULT (WINAPI *PFN_DirectSoundCreate)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);

struct AudioChunk {
    WAVEHDR hdr;
    std::vector<uint8_t> data;
};

bool FileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void CleanFinishedAudio(HWAVEOUT hWaveOut, std::list<AudioChunk>& queue) {
    while (!queue.empty() && (queue.front().hdr.dwFlags & WHDR_DONE)) {
        waveOutUnprepareHeader(hWaveOut, &queue.front().hdr, sizeof(WAVEHDR));
        queue.pop_front();
    }
}

bool PlayVideoFile(HWND hwnd, int screenW, int screenH, const std::string& videoPath, bool& exitApp) {
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, videoPath.c_str(), nullptr, nullptr) < 0) {
        return false;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    int videoStream = -1;
    int audioStream = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = i;
        } else if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
            audioStream = i;
        }
    }

    if (videoStream < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Setup Video Decoder
    AVCodecParameters* vPar = fmtCtx->streams[videoStream]->codecpar;
    const AVCodec* vCodec = avcodec_find_decoder(vPar->codec_id);
    AVCodecContext* vCtx = avcodec_alloc_context3(vCodec);
    avcodec_parameters_to_context(vCtx, vPar);
    vCtx->thread_count = 4;
    if (avcodec_open2(vCtx, vCodec, nullptr) < 0) {
        avcodec_free_context(&vCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    double srcAspect = (double)vCtx->width / (double)vCtx->height;
    double dstAspect = (double)screenW / (double)screenH;
    int drawW = screenW, drawH = screenH, drawX = 0, drawY = 0;
    if (srcAspect > dstAspect) {
        drawW = screenW;
        drawH = (int)(screenW / srcAspect);
        drawY = (screenH - drawH) / 2;
    } else {
        drawH = screenH;
        drawW = (int)(screenH * srcAspect);
        drawX = (screenW - drawW) / 2;
    }
    drawW = (drawW + 3) & ~3;

    SwsContext* swsCtx = sws_getContext(
        vCtx->width, vCtx->height, vCtx->pix_fmt,
        drawW, drawH, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = drawW;
    bmi.bmiHeader.biHeight = -drawH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> rgbBuffer(drawW * drawH * 4);
    uint8_t* dstData[4] = { rgbBuffer.data(), nullptr, nullptr, nullptr };
    int dstLinesize[4] = { drawW * 4, 0, 0, 0 };

    // Setup Audio Decoder (DirectSound GlobalFocus 48kHz + WaveOut Fallback)
    AVCodecContext* aCtx = nullptr;
    SwrContext* swrCtx = nullptr;
    HMODULE hDsound = nullptr;
    LPDIRECTSOUND pDS = nullptr;
    LPDIRECTSOUNDBUFFER pDSBuf = nullptr;
    const DWORD DS_BUF_SIZE = 48000 * 4 * 2; // 2 detik ring buffer
    DWORD dsWriteOffset = 0;
    bool dsStarted = false;

    HWAVEOUT hWaveOut = nullptr;
    std::list<AudioChunk> audioQueue;

    if (audioStream >= 0) {
        AVCodecParameters* aPar = fmtCtx->streams[audioStream]->codecpar;
        const AVCodec* aCodec = avcodec_find_decoder(aPar->codec_id);
        if (aCodec) {
            aCtx = avcodec_alloc_context3(aCodec);
            avcodec_parameters_to_context(aCtx, aPar);
            if (avcodec_open2(aCtx, aCodec, nullptr) == 0) {
                AVChannelLayout inLayout = aCtx->ch_layout;
                if (inLayout.order == AV_CHANNEL_ORDER_UNSPEC) {
                    int ch = inLayout.nb_channels > 0 ? inLayout.nb_channels : 2;
                    av_channel_layout_default(&inLayout, ch);
                }

                AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(
                        &swrCtx,
                        &outLayout, AV_SAMPLE_FMT_S16, 48000,
                        &inLayout, aCtx->sample_fmt, aCtx->sample_rate,
                        0, nullptr) >= 0 && swr_init(swrCtx) >= 0) {

                    WAVEFORMATEX wfx = {};
                    wfx.wFormatTag = WAVE_FORMAT_PCM;
                    wfx.nChannels = 2;
                    wfx.nSamplesPerSec = 48000;
                    wfx.wBitsPerSample = 16;
                    wfx.nBlockAlign = 4;
                    wfx.nAvgBytesPerSec = 48000 * 4;

                    // Coba DirectSound dulu (paling stabil di GameHub/Wine)
                    hDsound = LoadLibraryA("dsound.dll");
                    if (hDsound) {
                        auto pDirectSoundCreate = (PFN_DirectSoundCreate)GetProcAddress(hDsound, "DirectSoundCreate");
                        if (pDirectSoundCreate && SUCCEEDED(pDirectSoundCreate(NULL, &pDS, NULL))) {
                            pDS->SetCooperativeLevel(hwnd, DSSCL_PRIORITY);

                            DSBUFFERDESC primDesc = {};
                            primDesc.dwSize = sizeof(DSBUFFERDESC);
                            primDesc.dwFlags = DSBCAPS_PRIMARYBUFFER;
                            LPDIRECTSOUNDBUFFER pPrimary = nullptr;
                            if (SUCCEEDED(pDS->CreateSoundBuffer(&primDesc, &pPrimary, NULL))) {
                                pPrimary->SetFormat(&wfx);
                                pPrimary->Release();
                            }

                            DSBUFFERDESC secDesc = {};
                            secDesc.dwSize = sizeof(DSBUFFERDESC);
                            secDesc.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
                            secDesc.dwBufferBytes = DS_BUF_SIZE;
                            secDesc.lpwfxFormat = &wfx;
                            if (FAILED(pDS->CreateSoundBuffer(&secDesc, &pDSBuf, NULL))) {
                                pDS->Release();
                                pDS = nullptr;
                            }
                        }
                    }

                    // Fallback ke WaveOut jika DirectSound tidak tersedia
                    if (!pDSBuf) {
                        waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
                    }
                }
            }
        }
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* vFrame = av_frame_alloc();
    AVFrame* aFrame = av_frame_alloc();

    bool firstFrame = true;
    double firstPts = 0.0;
    ULONGLONG startTick = 0;
    ULONGLONG openTick = GetTickCount64();

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                exitApp = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (exitApp) break;

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            exitApp = true;
            break;
        }
        if ((GetTickCount64() - openTick > 700) && (GetAsyncKeyState(VK_RIGHT) & 0x8000)) {
            av_packet_unref(pkt);
            break;
        }

        if (pkt->stream_index == videoStream) {
            if (avcodec_send_packet(vCtx, pkt) == 0) {
                while (avcodec_receive_frame(vCtx, vFrame) == 0) {
                    sws_scale(swsCtx, vFrame->data, vFrame->linesize, 0, vCtx->height, dstData, dstLinesize);

                    double pts = 0.0;
                    if (vFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        pts = vFrame->best_effort_timestamp * av_q2d(fmtCtx->streams[videoStream]->time_base);
                    }

                    if (firstFrame) {
                        firstPts = pts;
                        startTick = GetTickCount64();
                        firstFrame = false;
                    } else {
                        double targetElapsed = pts - firstPts;
                        double actualElapsed = (GetTickCount64() - startTick) / 1000.0;
                        double delay = targetElapsed - actualElapsed;
                        if (delay > 0.002 && delay < 1.0) {
                            Sleep((DWORD)(delay * 1000.0));
                        }
                    }

                    HDC hdc = GetDC(hwnd);
                    StretchDIBits(
                        hdc, drawX, drawY, drawW, drawH,
                        0, 0, drawW, drawH,
                        rgbBuffer.data(), &bmi, DIB_RGB_COLORS, SRCCOPY
                    );
                    ReleaseDC(hwnd, hdc);
                }
            }
        } else if (pkt->stream_index == audioStream && aCtx && swrCtx && (pDSBuf || hWaveOut)) {
            if (avcodec_send_packet(aCtx, pkt) == 0) {
                while (avcodec_receive_frame(aCtx, aFrame) == 0) {
                    int maxOutSamples = (int)av_rescale_rnd(
                        swr_get_delay(swrCtx, aCtx->sample_rate) + aFrame->nb_samples,
                        48000, aCtx->sample_rate, AV_ROUND_UP
                    );

                    std::vector<uint8_t> pcmBuf(maxOutSamples * 4);
                    uint8_t* outPtr = pcmBuf.data();
                    int converted = swr_convert(
                        swrCtx, &outPtr, maxOutSamples,
                        (const uint8_t**)aFrame->extended_data, aFrame->nb_samples
                    );

                    if (converted > 0) {
                        DWORD pcmBytes = (DWORD)(converted * 4);

                        if (pDSBuf) {
                            if (dsStarted) {
                                while (!exitApp) {
                                    DWORD playCursor = 0, writeCursor = 0;
                                    if (FAILED(pDSBuf->GetCurrentPosition(&playCursor, &writeCursor))) break;
                                    DWORD queued = (dsWriteOffset >= playCursor)
                                        ? (dsWriteOffset - playCursor)
                                        : (DS_BUF_SIZE - playCursor + dsWriteOffset);
                                    if (queued + pcmBytes < DS_BUF_SIZE - 4096) break;
                                    Sleep(5);
                                }
                            }

                            void *ptr1 = nullptr, *ptr2 = nullptr;
                            DWORD bytes1 = 0, bytes2 = 0;
                            if (SUCCEEDED(pDSBuf->Lock(dsWriteOffset, pcmBytes, &ptr1, &bytes1, &ptr2, &bytes2, 0))) {
                                std::memcpy(ptr1, pcmBuf.data(), bytes1);
                                if (ptr2 && bytes2 > 0) {
                                    std::memcpy(ptr2, pcmBuf.data() + bytes1, bytes2);
                                }
                                pDSBuf->Unlock(ptr1, bytes1, ptr2, bytes2);
                                dsWriteOffset = (dsWriteOffset + pcmBytes) % DS_BUF_SIZE;

                                if (!dsStarted && dsWriteOffset >= 48000) {
                                    pDSBuf->Play(0, 0, DSBPLAY_LOOPING);
                                    dsStarted = true;
                                }
                            }
                        } else if (hWaveOut) {
                            CleanFinishedAudio(hWaveOut, audioQueue);
                            while (audioQueue.size() > 60) {
                                Sleep(5);
                                CleanFinishedAudio(hWaveOut, audioQueue);
                            }

                            audioQueue.emplace_back();
                            AudioChunk& chunk = audioQueue.back();
                            chunk.data.assign(pcmBuf.begin(), pcmBuf.begin() + pcmBytes);
                            std::memset(&chunk.hdr, 0, sizeof(WAVEHDR));
                            chunk.hdr.lpData = (LPSTR)chunk.data.data();
                            chunk.hdr.dwBufferLength = pcmBytes;
                            waveOutPrepareHeader(hWaveOut, &chunk.hdr, sizeof(WAVEHDR));
                            waveOutWrite(hWaveOut, &chunk.hdr, sizeof(WAVEHDR));
                        }
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (pDSBuf) {
        pDSBuf->Stop();
        pDSBuf->Release();
    }
    if (pDS) pDS->Release();
    if (hDsound) FreeLibrary(hDsound);

    if (hWaveOut) {
        waveOutReset(hWaveOut);
        for (auto& chunk : audioQueue) {
            waveOutUnprepareHeader(hWaveOut, &chunk.hdr, sizeof(WAVEHDR));
        }
        audioQueue.clear();
        waveOutClose(hWaveOut);
    }
    if (swrCtx) swr_free(&swrCtx);
    if (aCtx) avcodec_free_context(&aCtx);
    sws_freeContext(swsCtx);
    av_frame_free(&vFrame);
    av_frame_free(&aFrame);
    av_packet_free(&pkt);
    avcodec_free_context(&vCtx);
    avformat_close_input(&fmtCtx);

    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    dir = dir.substr(0, dir.find_last_of("\\/"));

    std::vector<std::string> playlist;
    bool hasIntro = false;

    std::string introPath = dir + "\\intro.mp4";
    if (FileExists(introPath)) {
        playlist.push_back(introPath);
        hasIntro = true;
    }

    for (int i = 1; i <= 50; ++i) {
        std::string gpPath = dir + "\\gameplay" + std::to_string(i) + ".mp4";
        if (FileExists(gpPath)) {
            playlist.push_back(gpPath);
        }
    }

    if (playlist.empty()) {
        MessageBoxA(NULL, "File intro.mp4 atau gameplay1.mp4 tidak ditemukan di folder ini.", "Grand Theft Auto VI", MB_OK | MB_ICONERROR);
        return 0;
    }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "GTA6PlayerWindow";
    RegisterClassA(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExA(
        0, "GTA6PlayerWindow", "Grand Theft Auto VI",
        WS_POPUP | WS_VISIBLE, 0, 0, screenW, screenH,
        NULL, NULL, hInstance, NULL
    );
    ShowCursor(FALSE);

    size_t idx = 0;
    bool exitApp = false;
    while (!exitApp) {
        if (!PlayVideoFile(hwnd, screenW, screenH, playlist[idx], exitApp)) {
            break;
        }
        idx++;
        if (idx >= playlist.size()) {
            idx = (hasIntro && playlist.size() > 1) ? 1 : 0;
        }
    }

    ShowCursor(TRUE);
    DestroyWindow(hwnd);
    return 0;
}
