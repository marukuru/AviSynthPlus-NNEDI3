// SPDX-License-Identifier: GPL-2.0-or-later
#include <avisynth.h>
#include "../nnedi3/PortableKernels.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

const AVS_Linkage *AVS_linkage = nullptr;

static std::vector<int> planes(const VideoInfo &vi)
{
    if (!vi.IsPlanar()) return {0};
    if (vi.IsY()) return {PLANAR_Y};
    std::vector<int> result = vi.IsRGB() ? std::vector<int>{PLANAR_G, PLANAR_B, PLANAR_R}
                                       : std::vector<int>{PLANAR_Y, PLANAR_U, PLANAR_V};
    if (vi.IsYUVA() || vi.IsPlanarRGBA()) result.push_back(PLANAR_A);
    return result;
}

class Pattern : public GenericVideoFilter {
public:
    Pattern(PClip clip, int mode) : GenericVideoFilter(clip), mode_(mode) {}
    int __stdcall SetCacheHints(int hint, int) override { return hint == CACHE_GET_MTMODE ? MT_NICE_FILTER : 0; }
    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment *env) override
    {
        PVideoFrame frame = env->NewVideoFrame(vi);
        const int size = vi.ComponentSize();
        const uint32_t mask = vi.BitsPerComponent() == 32 ? 65535 : (1u << vi.BitsPerComponent()) - 1;
        for (int plane : planes(vi)) {
            for (int y = 0; y < frame->GetHeight(plane); ++y) {
                uint8_t *row = frame->GetWritePtr(plane) + y * frame->GetPitch(plane);
                for (int x = 0; x < frame->GetRowSize(plane) / size; ++x) {
                    uint32_t v = uint32_t(x + 17*y + 131*n + 197*plane) * 2654435761u;
                    v ^= v >> 13;
                    v = mode_ == 1 ? mask / 2 : mode_ == 2 ? ((x+y+n)&1 ? mask : 0) : v & mask;
                    if (size == 1) row[x] = static_cast<uint8_t>(v);
                    else if (size == 2) reinterpret_cast<uint16_t *>(row)[x] = static_cast<uint16_t>(v);
                    else reinterpret_cast<float *>(row)[x] = float(v) / 65535.0f -
                        (vi.IsYUV() && (plane == PLANAR_U || plane == PLANAR_V) ? 0.5f : 0.0f);
                }
            }
        }
        env->propSetInt(env->getFramePropsRW(frame), "NNEDI3TestFrame", n, 0);
        return frame;
    }
private:
    int mode_;
};

static AVSValue __cdecl makePattern(AVSValue args, void *, IScriptEnvironment *)
{
    return new Pattern(args[0].AsClip(), args[1].AsInt(0));
}

static PClip eval(IScriptEnvironment *env, const std::string &script)
{
    try { return env->Invoke("Eval", script.c_str()).AsClip(); }
    catch (...) { std::cerr << "Script: " << script << "\n"; throw; }
}

static std::string source(const std::string &format, int width = 68, int height = 66, int mode = 0, int length = 4)
{
    return "TestPattern(BlankClip(width=" + std::to_string(width) + ",height=" + std::to_string(height) +
        ",length=" + std::to_string(length) + ",pixel_type=\"" + format + "\")," + std::to_string(mode) + ")";
}

static void require(bool value, const std::string &message)
{
    if (!value) throw std::runtime_error(message);
}

static void compare(IScriptEnvironment *env, PClip a, PClip b, const std::string &label, bool bob = false)
{
    std::cout << "Checking " << label << std::endl;
    const VideoInfo &vi = a->GetVideoInfo();
    const VideoInfo &vb = b->GetVideoInfo();
    require(vi.width == vb.width && vi.height == vb.height && vi.pixel_type == vb.pixel_type &&
        vi.num_frames == vb.num_frames && vi.fps_numerator == vb.fps_numerator &&
        vi.fps_denominator == vb.fps_denominator, label + ": metadata differs");
    for (int n : {3, 0, 2, 1, 3, 0}) {
        auto fa = a->GetFrame(n, env), fb = b->GetFrame(n, env);
        for (int plane : planes(vi)) {
            for (int y = 0; y < fa->GetHeight(plane); ++y) {
                if (std::memcmp(fa->GetReadPtr(plane) + y*fa->GetPitch(plane),
                    fb->GetReadPtr(plane) + y*fb->GetPitch(plane), fa->GetRowSize(plane)))
                    throw std::runtime_error(label + ": frame " + std::to_string(n) +
                        " plane " + std::to_string(plane) + " row " + std::to_string(y) + " differs");
            }
        }
        int error = 0;
        const auto property = env->propGetInt(env->getFramePropsRO(fb), "NNEDI3TestFrame", 0, &error);
        require(!error && property == (bob ? n/2 : n), label + ": frame property lost");
    }
}

// SIMD reductions and reciprocal/exp approximations need not be bit exact to C.
// With the prescreener disabled every missing pixel exercises the predictor.
static void compareNumerical(IScriptEnvironment *env, PClip a, PClip b, const std::string &label, bool prescreen = false)
{
    const auto &vi = a->GetVideoInfo();
    const int size = vi.ComponentSize();
    const double full = size == 4 ? 1.0 : (1u << vi.BitsPerComponent()) - 1;
    double maxError = 0, squared = 0;
    uint64_t count = 0, outliers = 0;
    for (int n : {0, 1, 2}) {
        auto fa = a->GetFrame(n, env), fb = b->GetFrame(n, env);
        for (int plane : planes(vi)) for (int y = 0; y < fa->GetHeight(plane); ++y) {
            const auto *ra = fa->GetReadPtr(plane) + y*fa->GetPitch(plane);
            const auto *rb = fb->GetReadPtr(plane) + y*fb->GetPitch(plane);
            for (int x = 0; x < fa->GetRowSize(plane)/size; ++x) {
                const double va = size == 4 ? reinterpret_cast<const float *>(ra)[x] :
                    size == 2 ? reinterpret_cast<const uint16_t *>(ra)[x] : ra[x];
                const double vb = size == 4 ? reinterpret_cast<const float *>(rb)[x] :
                    size == 2 ? reinterpret_cast<const uint16_t *>(rb)[x] : rb[x];
                require(std::isfinite(va) && std::isfinite(vb), label + ": non-finite pixel");
                const double error = std::abs(va-vb)/full;
                maxError = std::max(maxError, error);
                squared += error*error;
                outliers += error > 0.01;
                ++count;
            }
        }
    }
    const double rms = std::sqrt(squared/count);
    const double lsb = size == 4 ? 0 : 1.0/full;
    // Prescreener decisions very near zero can flip across reciprocal/FMA
    // implementations. Limit both their frequency and total output error.
    if (prescreen ? (rms > 0.003 + 0.5*lsb || double(outliers)/count > 0.001) :
        (maxError > 2*lsb + 0.001 || rms > 0.5*lsb + 0.0002))
        throw std::runtime_error(label + ": max normalized error " + std::to_string(maxError) +
            ", RMS " + std::to_string(rms));
}

static void checkKeptRows(IScriptEnvironment *env, const std::string &format, int field, bool dh)
{
    auto input = eval(env, source(format, 68, 64));
    env->SetVar("test_input", input);
    auto output = eval(env, "nnedi3(test_input,field=" + std::to_string(field) +
        ",dh=" + (dh ? "true" : "false") + ",nns=0,nsize=0,threads=3)");
    const auto &vi = output->GetVideoInfo();
    require(vi.height == (dh ? 128 : 64) && vi.num_frames == (field > 1 ? 8 : 4), "field/dh metadata");
    require(!vi.IsFieldBased(), "output must be frame based");
    for (int n = 0; n < 4; ++n) {
        auto src = input->GetFrame(field > 1 ? n/2 : n, env), dst = output->GetFrame(n, env);
        int f = field > 1 ? ((n&1) ? (field == 3 ? 0 : 1) : (field == 3 ? 1 : 0)) : field;
        for (int plane : planes(vi)) {
            for (int y = 1-f; y < dst->GetHeight(plane); y += 2) {
                int sy = dh ? y/2 : y;
                require(std::memcmp(src->GetReadPtr(plane) + sy*src->GetPitch(plane),
                    dst->GetReadPtr(plane) + y*dst->GetPitch(plane), dst->GetRowSize(plane)) == 0,
                    format + ": retained field changed");
            }
        }
    }
}

static void expectError(IScriptEnvironment *env, const std::string &args)
{
    try {
        const auto script = source("Y8") + ".nnedi3(" + args + ")";
        env->Invoke("Eval", script.c_str());
    }
    catch (const AvisynthError &) { return; }
    throw std::runtime_error("accepted invalid arguments: " + args);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    IScriptEnvironment2 *env = CreateScriptEnvironment2(8);
    if (!env) return 2;
    env->ClearAutoloadDirs(); // Exercise only this plugin and builtins.
    AVS_linkage = env->GetAVSLinkage();
    int result = 0;
    try {
        const std::string load = "LoadPlugin(\"" + std::string(argv[1]) + "\")";
        env->Invoke("Eval", load.c_str());
        env->AddFunction("TestPattern", "c[mode]i", makePattern, nullptr);
        std::vector<int> modes{1};
#ifdef NNEDI3_X86_ASM
        for (int opt = 2; opt <= 8; ++opt) {
            if (resolveOptimization(opt, env->GetCPUFlags(), true) == opt) modes.push_back(opt);
            else expectError(env, "opt=" + std::to_string(opt));
        }
#else
        for (int opt = 2; opt <= 8; ++opt) expectError(env, "opt=" + std::to_string(opt));
#endif
        if (argc > 2 && std::string(argv[2]) == "--benchmark") {
            for (const std::string format : {"Y8", "Y16", "Y32"}) for (int opt : modes) {
                auto clip = eval(env, source(format, 640, 360, 0, 16) +
                    ".nnedi3(field=1,nns=1,nsize=6,opt=" + std::to_string(opt) + ",threads=1)");
                clip->GetFrame(0, env);
                auto start = std::chrono::steady_clock::now();
                uint64_t checksum = 0;
                for (int n = 1; n < 13; ++n) {
                    auto frame = clip->GetFrame(n, env);
                    checksum += frame->GetReadPtr()[frame->GetPitch()];
                }
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                std::cout << format << " opt=" << opt << " (" << optimizationName(opt) << "): " <<
                    12/seconds << " fps (checksum " << checksum << ")" << std::endl;
            }
            for (int threads : {1, 2, 4, 8}) {
                auto clip = eval(env, source("Y8", 640, 360, 0, 16) +
                    ".nnedi3(field=1,nns=1,nsize=6,threads=" + std::to_string(threads) + ")");
                clip->GetFrame(0, env); // Warm weights, scratch and workers.
                auto start = std::chrono::steady_clock::now();
                uint64_t checksum = 0;
                for (int n = 1; n < 13; ++n) {
                    auto frame = clip->GetFrame(n, env);
                    checksum += frame->GetReadPtr()[frame->GetPitch()];
                }
                double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                std::cout << threads << " threads: " << 12/seconds << " fps (checksum " << checksum << ")\n";
            }
        } else {
            int cases = 0;
            for (const std::string format : {"Y8", "Y10", "Y12", "Y14", "Y16", "Y32", "YV12", "YV16", "YV24",
                    "YV411", "YUV420P10", "YUV422P16", "YUV444PS", "YUVA420P8", "YUVA444P16", "YUVA444PS",
                    "RGBP", "RGBP16", "RGBPS", "RGBAP", "RGBAP16", "RGBAPS", "RGB24", "RGB32", "RGB48", "RGB64", "YUY2"}) {
                for (int pscrn : {0, 1, 2, 3, 4}) {
                    const auto base = source(format) + ".nnedi3(field=1,nns=0,nsize=0,pscrn=" + std::to_string(pscrn);
                    compare(env, eval(env, base + ",threads=1)"), eval(env, base + ",threads=3)"), format + "/pscrn=" + std::to_string(pscrn));
                    ++cases;
                }
            }
            for (const std::string format : {"Y8", "Y16", "Y32", "YUV420P10", "RGBAP16", "RGB32", "RGB48", "RGB64"}) {
                for (int fapprox : {0, 1, 2, 3, 4, 8, 15}) {
                    auto base = source(format) + ".nnedi3(field=3,nns=0,nsize=1,qual=2,etype=1,pscrn=1,fapprox=" + std::to_string(fapprox);
                    compare(env, eval(env, base + ",threads=1)"), eval(env, base + ",threads=4).Prefetch(3)"), format + "/Prefetch/fapprox=" + std::to_string(fapprox), true);
                    ++cases;
                }
                if (format == "RGB32" || format == "RGB48" || format == "RGB64") continue;
                for (int field : {0, 1, 2, 3}) checkKeptRows(env, format, field, false);
                for (int field : {0, 1}) checkKeptRows(env, format, field, true);
            }
            for (int nsize = 0; nsize < 7; ++nsize) {
                auto base = source("Y8", 36, 34) + ".nnedi3(field=0,nns=" + std::to_string(nsize%5) + ",nsize=" + std::to_string(nsize);
                compare(env, eval(env, base + ",threads=1)"), eval(env, base + ",threads=128)"), "network dimensions");
                ++cases;
            }
            for (const std::string format : {"Y8", "YUV420P10", "RGBAP16", "RGB24", "YUY2"}) {
                auto base = source(format, 36, 32) + ".nnedi3_rpow2(rfactor=2,nns=0,nsize=0";
                compare(env, eval(env, base + ",threads=1)"), eval(env, base + ",threads=3).Prefetch(2)"), format + "/rpow2");
                compare(env, eval(env, base + ",threads=1,cshift=\"Spline36Resize\",fwidth=80,fheight=70)"),
                    eval(env, base + ",threads=2,cshift=\"Spline36Resize\",fwidth=80,fheight=70)"), format + "/rpow2 resize");
                cases += 2;
            }
            for (int mode : {1, 2}) {
                auto base = source("Y16", 68, 66, mode) + ".nnedi3(field=1,nns=0,nsize=0";
                compare(env, eval(env, base + ",threads=1)"), eval(env, base + ",threads=0,prefetch=2,logicalCores=false)"), "flat/extreme");
                ++cases;
            }
            for (const std::string format : {"Y8", "Y16", "Y32", "YV12", "YUVA420P16", "YUY2"}) {
                for (int width : {4, 16, 36}) {
                    auto base = source(format, width, 4) + ".nnedi3(field=1,nns=0,nsize=0";
                    compare(env, eval(env, base + ",threads=1)"), eval(env, base + ",threads=128)"), format + "/small");
                    ++cases;
                }
            }
            for (int field : {0, 1}) {
                auto base = source("Y8", 1, 1) + ".nnedi3(dh=true,field=" + std::to_string(field) + ",nns=0,nsize=0";
                compare(env, eval(env, base + ",threads=1)"), eval(env, base + ",threads=0)"), "one pixel");
                ++cases;
            }
            for (int range = 0; range <= 4; ++range) {
                auto base = source("YUV444P16") + ".nnedi3(field=0,nns=0,nsize=0,range=" + std::to_string(range);
                compare(env, eval(env, base + ",threads=1)"), eval(env, base + ",threads=3)"), "range");
                ++cases;
            }
            for (int opt : modes) {
                for (const std::string format : {"Y8", "Y10", "Y12", "Y14", "Y16", "Y32"}) {
                    // All network window shapes and float/integer/exp alternatives.
                    for (int nsize = 0; nsize < 7; ++nsize) for (int fapprox : {0, 3, 4, 8, 15}) {
                        auto base = source(format, 36, 34) + ".nnedi3(field=1,nns=0,pscrn=0,qual=2,nsize=" +
                            std::to_string(nsize) + ",fapprox=" + std::to_string(fapprox);
                        compareNumerical(env, eval(env, base + ",opt=1)"),
                            eval(env, base + ",opt=" + std::to_string(opt) + ",threads=2)"),
                            format + "/numerical/opt=" + std::to_string(opt) + "/nsize=" +
                            std::to_string(nsize) + "/fapprox=" + std::to_string(fapprox));
                        ++cases;
                    }
                    for (int pscrn : {0, 1, 2, 3, 4}) for (int fapprox : {0, 3, 15}) {
                        auto base = source(format) + ".nnedi3(field=1,nns=0,nsize=1,opt=" + std::to_string(opt) +
                            ",pscrn=" + std::to_string(pscrn) + ",fapprox=" + std::to_string(fapprox);
                        if (pscrn > 0 && opt > 1) {
                            auto reference = source(format) + ".nnedi3(field=1,nns=0,nsize=1,opt=1,pscrn=" +
                                std::to_string(pscrn) + ",fapprox=" + std::to_string(fapprox) + ")";
                            compareNumerical(env, eval(env, reference), eval(env, base + ")"),
                                format + "/prescreener/opt=" + std::to_string(opt) + "/pscrn=" +
                                std::to_string(pscrn) + "/fapprox=" + std::to_string(fapprox), true);
                            ++cases;
                        }
                        compare(env, eval(env, base + ",threads=1)"), eval(env, base + ",threads=3).Prefetch(2)"),
                            format + "/opt=" + std::to_string(opt) + "/pscrn=" + std::to_string(pscrn) +
                            "/fapprox=" + std::to_string(fapprox));
                        ++cases;
                    }
                }
            }
            for (const auto args : {"threads=-1", "threads=129", "opt=9", "pscrn=5", "nns=5", "nsize=7",
                    "qual=0", "field=4", "field=2,dh=true", "fapprox=16", "range=5", "prefetch=-2147483648"})
                expectError(env, args);
            std::cout << "Passed " << cases << " output comparisons, retained-field/property checks and argument validation.\n";
        }
    } catch (const IScriptEnvironment::NotFound &) {
        std::cerr << "Function not found\n"; result = 1;
    } catch (const AvisynthError &e) {
        std::cerr << "AviSynth: " << e.msg << '\n'; result = 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n'; result = 1;
    }
    env->DeleteScriptEnvironment();
    return result;
}
