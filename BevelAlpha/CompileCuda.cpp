#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: CompileCuda <nvrtc DLL> <kernel source> <output header>\n";
        return 1;
    }
    const auto library = std::filesystem::absolute(argv[1]);
    SetDllDirectoryW(library.parent_path().c_str()); // Also find NVRTC's builtins.
    const HMODULE nvrtc = LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!nvrtc) {
        std::cerr << "Could not load NVRTC, Windows error " << GetLastError() << '\n';
        return 1;
    }
    using Program = void*;
    const auto create = reinterpret_cast<int (*)(Program*, const char*, const char*, int,
        const char* const*, const char* const*)>(GetProcAddress(nvrtc, "nvrtcCreateProgram"));
    const auto compile = reinterpret_cast<int (*)(Program, int, const char* const*)>(
        GetProcAddress(nvrtc, "nvrtcCompileProgram"));
    using Size = int (*)(Program, std::size_t*);
    using Read = int (*)(Program, char*);
    const auto logSize = reinterpret_cast<Size>(GetProcAddress(nvrtc, "nvrtcGetProgramLogSize"));
    const auto getLog = reinterpret_cast<Read>(GetProcAddress(nvrtc, "nvrtcGetProgramLog"));
    const auto ptxSize = reinterpret_cast<Size>(GetProcAddress(nvrtc, "nvrtcGetPTXSize"));
    const auto getPtx = reinterpret_cast<Read>(GetProcAddress(nvrtc, "nvrtcGetPTX"));
    const auto destroy = reinterpret_cast<int (*)(Program*)>(GetProcAddress(nvrtc, "nvrtcDestroyProgram"));
    if (!create || !compile || !logSize || !getLog || !ptxSize || !getPtx || !destroy) return 1;

    std::ifstream source(argv[2], std::ios::binary);
    if (!source) return 1;
    const std::string code{std::istreambuf_iterator<char>(source), {}};
    const std::string include = "--include-path=" +
        std::filesystem::absolute(argv[2]).parent_path().string();
    const char* options[]{"--gpu-architecture=compute_75", "--std=c++14", "--fmad=false",
                          include.c_str()};
    Program program = nullptr;
    if (create(&program, code.c_str(), "BevelCuda.cu", 0, nullptr, nullptr)) return 1;
    const int result = compile(program, 4, options);
    std::size_t size = 0;
    if (!logSize(program, &size) && size > 1) {
        std::vector<char> log(size);
        if (!getLog(program, log.data())) std::cerr << log.data();
    }
    if (result || ptxSize(program, &size) || size == 0) {
        destroy(&program);
        return 1;
    }
    std::vector<char> ptx(size);
    const int readResult = getPtx(program, ptx.data());
    destroy(&program);
    if (readResult) return 1;
    std::ofstream output(argv[3], std::ios::binary);
    // Keep individual source literals below the MSVC string literal size limit.
    output << "static constexpr char bevelCudaPtx[] =\n";
    for (std::size_t offset = 0; offset < size - 1; offset += 10000) {
        const std::size_t count = std::min<std::size_t>(10000, size - 1 - offset);
        output << "R\"BevelPTX(";
        output.write(ptx.data() + offset, count);
        output << ")BevelPTX\"\n";
    }
    output << ";\n";
    return output ? 0 : 1;
}
