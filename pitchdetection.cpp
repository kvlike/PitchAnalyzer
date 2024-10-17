#define SDL_MAIN_HANDLED
#include <iostream>
#include <aubio/aubio.h>
#include <portaudio.h>
#include <sndfile.h>
#include <samplerate.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <cmath>
#include <tinyfiledialogs.h>

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 1024
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define MAX_PITCH_HISTORY 100

float zoomFactor = 1.0f;
float verticalOffset = 0.0f;

bool useMicInput = true;
SNDFILE *audioFile = nullptr;
SF_INFO sfInfo;

std::vector<float> pitchHistory(MAX_PITCH_HISTORY, -1.0f);
int pitchIndex = 0;

fvec_t *input = nullptr;
aubio_pitch_t *pitch = nullptr;
fvec_t *output = nullptr;

PaStream *stream = nullptr;

static int paCallback(const void *inputBuffer, void *outputBuffer,
                      unsigned long framesPerBuffer,
                      const PaStreamCallbackTimeInfo *timeInfo,
                      PaStreamCallbackFlags statusFlags,
                      void *userData) {
    fvec_t *input = (fvec_t *)userData;
    const float *in = (const float *)inputBuffer;

    for (unsigned long i = 0; i < framesPerBuffer; i++) {
        input->data[i] = in[i];
    }
    return paContinue;
}

static int paFileCallback(const void *inputBuffer, void *outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo *timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData) {
    fvec_t *input = (fvec_t *)userData;
    float *out = (float *)outputBuffer;

    sf_count_t numRead = sf_read_float(audioFile, out, framesPerBuffer);
    if (numRead < framesPerBuffer) {
        sf_seek(audioFile, 0, SEEK_SET);
    }

    for (unsigned long i = 0; i < framesPerBuffer; i++) {
        input->data[i] = out[i];
    }
    return paContinue;
}

void handleSDLEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            exit(0);
        } else if (event.type == SDL_KEYDOWN) {
            switch (event.key.keysym.sym) {
                case SDLK_m:
                    if (useMicInput) {
                        char const *filterPatterns[1] = {"*.wav"};
                        char const *filePath = tinyfd_openFileDialog("Select Audio File", "", 1, filterPatterns, NULL, 0);
                        if (filePath) {
                            if (audioFile) {
                                sf_close(audioFile);
                            }
                            sfInfo.format = 0;
                            audioFile = sf_open(filePath, SFM_READ, &sfInfo);
                            if (!audioFile) {
                                std::cerr << "Failed to open audio file: " << sf_strerror(NULL) << std::endl;
                            } else {
                                if (sfInfo.samplerate != SAMPLE_RATE) {
                                    std::cerr << "Sample rate mismatch: " << sfInfo.samplerate << " != " << SAMPLE_RATE << std::endl;
                                    sf_close(audioFile);
                                    audioFile = nullptr;
                                    break;
                                }
                                useMicInput = false;
                                Pa_StopStream(stream);
                                Pa_CloseStream(stream);
                                PaError err = Pa_OpenDefaultStream(&stream, 0, 1, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER, paFileCallback, input);
                                if (err != paNoError) {
                                    std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
                                }
                                err = Pa_StartStream(stream);
                                if (err != paNoError) {
                                    std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
                                }
                            }
                        }
                    } else {
                        if (audioFile) {
                            sf_close(audioFile);
                            audioFile = nullptr;
                        }
                        useMicInput = true;
                        Pa_StopStream(stream);
                        Pa_CloseStream(stream);
                        PaError err = Pa_OpenDefaultStream(&stream, 1, 0, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER, paCallback, input);
                        if (err != paNoError) {
                            std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
                        }
                        err = Pa_StartStream(stream);
                        if (err != paNoError) {
                            std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
                        }
                    }
                    break;
                case SDLK_PLUS:
                case SDLK_EQUALS:
                    zoomFactor *= 1.1f;
                    break;
                case SDLK_MINUS:
                    zoomFactor /= 1.1f;
                    break;
                case SDLK_UP:
                    verticalOffset -= 10.0f;
                    break;
                case SDLK_DOWN:
                    verticalOffset += 10.0f;
                    break;
            }
        }
    }
}

void drawGradientBackground(SDL_Renderer *renderer) {
    for (int y = 0; y < WINDOW_HEIGHT; ++y) {
        float t = static_cast<float>(y) / WINDOW_HEIGHT;
        int r = static_cast<int>((1 - t) * 30 + t * 0);
        int g = static_cast<int>((1 - t) * 30 + t * 0);
        int b = static_cast<int>((1 - t) * 30 + t * 50);
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_RenderDrawLine(renderer, 0, y, WINDOW_WIDTH, y);
    }
}

void drawReferenceLines(SDL_Renderer *renderer, TTF_Font *font, const std::vector<std::pair<std::string, float>>& notes) {
    for (const auto& note : notes) {
        float logFreq = std::log10(note.second);
        float logMinFreq = std::log10(32.70);
        float logMaxFreq = std::log10(4186.01);
        int y = WINDOW_HEIGHT - static_cast<int>(((logFreq - logMinFreq) / (logMaxFreq - logMinFreq)) * WINDOW_HEIGHT * zoomFactor + verticalOffset);

        int octave = note.first.back() - '0';
        int colorIntensity = 255 - (octave * 30);
        SDL_SetRenderDrawColor(renderer, colorIntensity, colorIntensity, 255, 100);

        SDL_RenderDrawLine(renderer, 0, y, WINDOW_WIDTH, y);

        SDL_Color textColor = {255, 255, 255, 255};
        SDL_Surface* surface = TTF_RenderText_Blended(font, note.first.c_str(), textColor);
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_Rect dstrect = {0, y - 10, surface->w, surface->h};
        SDL_RenderCopy(renderer, texture, NULL, &dstrect);
        SDL_FreeSurface(surface);
        SDL_DestroyTexture(texture);
    }
}

void drawSmoothPitchLine(SDL_Renderer *renderer, const std::vector<float>& pitchHistory, int pitchIndex) {
    SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
    for (int i = 0; i < MAX_PITCH_HISTORY - 1; i++) {
        float pitch1 = pitchHistory[(pitchIndex + i) % MAX_PITCH_HISTORY];
        float pitch2 = pitchHistory[(pitchIndex + i + 1) % MAX_PITCH_HISTORY];

        if (pitch1 <= 0 || pitch2 <= 0) {
            continue;
        }

        int x1 = WINDOW_WIDTH - (MAX_PITCH_HISTORY - i) * (WINDOW_WIDTH / MAX_PITCH_HISTORY);
        int x2 = WINDOW_WIDTH - (MAX_PITCH_HISTORY - i - 1) * (WINDOW_WIDTH / MAX_PITCH_HISTORY);

        float logFreq1 = std::log10(pitch1);
        float logFreq2 = std::log10(pitch2);

        float logMinFreq = std::log10(32.70);
        float logMaxFreq = std::log10(4186.01);

        int y1 = WINDOW_HEIGHT - static_cast<int>(((logFreq1 - logMinFreq) / (logMaxFreq - logMinFreq)) * WINDOW_HEIGHT * zoomFactor + verticalOffset);
        int y2 = WINDOW_HEIGHT - static_cast<int>(((logFreq2 - logMinFreq) / (logMaxFreq - logMinFreq)) * WINDOW_HEIGHT * zoomFactor + verticalOffset);

        SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
    }
}

int main(int argc, char *argv[]) {
    PaError err;

    input = new_fvec(FRAMES_PER_BUFFER);
    pitch = new_aubio_pitch("default", FRAMES_PER_BUFFER, FRAMES_PER_BUFFER / 4, SAMPLE_RATE);
    aubio_pitch_set_tolerance(pitch, 0.8);
    output = new_fvec(1);

    err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }

    err = Pa_OpenDefaultStream(&stream, 1, 0, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER, paCallback, input);
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    if (TTF_Init() == -1) {
        std::cerr << "SDL_ttf could not initialize! TTF_Error: " << TTF_GetError() << std::endl;
        return 1;
    }

    TTF_Font *font = TTF_OpenFont("arial.ttf", 12);
    if (font == nullptr) {
        std::cerr << "Failed to load font! TTF_Error: " << TTF_GetError() << std::endl;
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Pitch Visualizer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::cerr << "Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr) {
        std::cerr << "Renderer could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    const std::vector<std::pair<std::string, float>> notes = {
        {"C1", 32.70}, {"C#1", 34.65}, {"D1", 36.71}, {"D#1", 38.89}, {"E1", 41.20}, {"F1", 43.65}, {"F#1", 46.25}, {"G1", 49.00}, {"G#1", 51.91}, {"A1", 55.00}, {"A#1", 58.27}, {"B1", 61.74},
        {"C2", 65.41}, {"C#2", 69.30}, {"D2", 73.42}, {"D#2", 77.78}, {"E2", 82.41}, {"F2", 87.31}, {"F#2", 92.50}, {"G2", 98.00}, {"G#2", 103.83}, {"A2", 110.00}, {"A#2", 116.54}, {"B2", 123.47},
        {"C3", 130.81}, {"C#3", 138.59}, {"D3", 146.83}, {"D#3", 155.56}, {"E3", 164.81}, {"F3", 174.61}, {"F#3", 185.00}, {"G3", 196.00}, {"G#3", 207.65}, {"A3", 220.00}, {"A#3", 233.08}, {"B3", 246.94},
        {"C4", 261.63}, {"C#4", 277.18}, {"D4", 293.66}, {"D#4", 311.13}, {"E4", 329.63}, {"F4", 349.23}, {"F#4", 369.99}, {"G4", 392.00}, {"G#4", 415.30}, {"A4", 440.00}, {"A#4", 466.16}, {"B4", 493.88},
        {"C5", 523.25}, {"C#5", 554.37}, {"D5", 587.33}, {"D#5", 622.25}, {"E5", 659.25}, {"F5", 698.46}, {"F#5", 739.99}, {"G5", 783.99}, {"G#5", 830.61}, {"A5", 880.00}, {"A#5", 932.33}, {"B5", 987.77},
        {"C6", 1046.50}, {"C#6", 1108.73}, {"D6", 1174.66}, {"D#6", 1244.51}, {"E6", 1318.51}, {"F6", 1396.91}, {"F#6", 1479.98}, {"G6", 1567.98}, {"G#6", 1661.22}, {"A6", 1760.00}, {"A#6", 1864.66}, {"B6", 1975.53},
        {"C7", 2093.00}, {"C#7", 2217.46}, {"D7", 2349.32}, {"D#7", 2489.02}, {"E7", 2637.02}, {"F7", 2793.83}, {"F#7", 2959.96}, {"G7", 3135.96}, {"G#7", 3322.44}, {"A7", 3520.00}, {"A#7", 3729.31}, {"B7", 3951.07},
        {"C8", 4186.01}
    };

    bool quit = false;
    SDL_Event e;
    while (!quit) {
        handleSDLEvents();

        if (useMicInput) {
            aubio_pitch_do(pitch, input, output);
        } else {
            aubio_pitch_do(pitch, input, output);
        }
        float detected_pitch = output->data[0];

        if (detected_pitch > 0) {
            pitchHistory[pitchIndex] = detected_pitch;
        } else {
            pitchHistory[pitchIndex] = -1.0f;
        }
        pitchIndex = (pitchIndex + 1) % MAX_PITCH_HISTORY;

        drawGradientBackground(renderer);
        drawReferenceLines(renderer, font, notes);
        drawSmoothPitchLine(renderer, pitchHistory, pitchIndex);
        SDL_RenderPresent(renderer);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    del_aubio_pitch(pitch);
    del_fvec(input);
    del_fvec(output);

    err = Pa_StopStream(stream);
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }

    err = Pa_CloseStream(stream);
    if (err != paNoError) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }

    Pa_Terminate();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    TTF_CloseFont(font);
    TTF_Quit();

    SDL_Quit();

    return 0;
}