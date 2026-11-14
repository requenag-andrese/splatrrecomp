// Translator study. Build: clang++ -std=c++20 -O2 -w -static -I<workspace>/vendor/pokedinner/src lower_test.cpp -o lower_test.exe
// Feeds every program of shader-programs.bin (graphics: 0x80-byte header, then 16-byte instructions; compute: instructions)
// to PokeDinner's Ampere -> Maxwell lowering (header only) and counts what it lowers, and why the rest fails.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "shader_recompiler/frontend/ampere/program_capture.h"

using namespace Shader::Ampere;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "lower_test <shader-programs.bin> [failed-dir]\n"); return 2; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fseek(f, 0, SEEK_END); long size = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> b(size);
    if (std::fread(b.data(), 1, size, f) != (size_t)size) return 2;
    std::fclose(f);
    if (std::memcmp(b.data(), "SRRCSPRG", 8)) { std::fprintf(stderr, "not SRRCSPRG\n"); return 2; }
    const size_t head = 12 + 32 + 0x880;
    size_t o = 16;
    std::map<int, int> ok, all;
    std::map<std::string, int> why;
    std::map<std::string, std::string> example;
    double secs = 0;
    size_t lowered_insts = 0, native_insts = 0;
    while (b.size() - o >= head) {
        u32 stage, len, c1;
        std::memcpy(&stage, &b[o], 4), std::memcpy(&len, &b[o + 4], 4), std::memcpy(&c1, &b[o + 8], 4);
        const unsigned char* code = &b[o + head];
        size_t skip = stage == 5 ? 0 : 0x80;
        std::vector<Instruction> native;
        ProgramCaptureState cap;
        bool bad_target = false;
        for (size_t at = skip, i = 0; at + 16 <= len && i < ProgramCaptureState::MaximumInstructions; at += 16, i++) {
            Instruction in;
            std::memcpy(in.words.data(), code + at, 16);
            native.push_back(in);
            auto r = cap.Observe(in, (u32)i);
            if (r == ProgramCaptureState::Result::InvalidTarget) { bad_target = true; break; }
            if (r == ProgramCaptureState::Result::Complete) break;
        }
        all[stage]++;
        char hash[17];
        for (int i = 0; i < 8; i++) std::snprintf(hash + 2 * i, 3, "%02x", b[o + 12 + i]);
        if (bad_target) {
            why["capture: branch outside the program"]++;
        } else {
            auto t0 = std::chrono::steady_clock::now();
            LoweredProgram l = LowerToMaxwell(native, "");
            secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (l) {
                ok[stage]++, lowered_insts += l.instructions.size(), native_insts += native.size();
            } else {
                size_t at = l.error_offset / 16;
                unsigned op = at < native.size() ? native[at].words[0] & 0xfff : 0;
                char key[160];
                std::snprintf(key, sizeof key, "%.*s [op %03x]", (int)l.error.size(), l.error.data(), op);
                if (!why[key]++) example[key] = hash;
            }
        }
        o += head + len + c1;
    }
    const char* names[6] = {"vertex", "tess-c", "tess-e", "geometry", "fragment", "compute"};
    int tot = 0, tok = 0;
    for (auto& [s, n] : all) {
        std::printf("%-8s %6d of %6d lowered\n", s < 6 ? names[s] : "?", ok[s], n);
        tot += n, tok += ok[s];
    }
    std::printf("total    %6d of %6d lowered (%.1f%%), %.2f s lowering, %.2fx instructions after lowering\n", tok, tot,
                100.0 * tok / tot, secs, native_insts ? (double)lowered_insts / native_insts : 0);
    std::vector<std::pair<int, std::string>> v;
    for (auto& [k, n] : why) v.push_back({n, k});
    std::sort(v.rbegin(), v.rend());
    for (auto& [n, k] : v) std::printf("  %6d  %s  (e.g. %s)\n", n, k.c_str(), example[k].c_str());
}
