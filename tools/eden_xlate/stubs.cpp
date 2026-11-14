// sr-recomp, translator study: the emulator logging, assert handlers, settings paths and time zones PokeDinner's shader
// recompiler links against, reduced to what it needs offline (eden-xlate, tools/eden_xlate/main.cpp).
#include <cstdio>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"

namespace Common::Log {
void FmtLogMessageImpl(Class, Level level, const char* filename, unsigned int line, const char*, fmt::string_view format,
                       const fmt::format_args& args) {
    static const bool quiet = std::getenv("XLATE_LOG") == nullptr;
    if (quiet) return;
    std::fprintf(stderr, "[%d] %s:%u %s\n", (int)level, filename, line, fmt::vformat(format, args).c_str());
}
} // namespace Common::Log

// paths and time zones the settings' defaults name; the translator reads none of them
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/time_zone.h"
namespace Common::FS {
const std::filesystem::path& GetEdenPath(EdenPath) { static const std::filesystem::path p{"."}; return p; }
std::string PathToUTF8String(const std::filesystem::path& path) { return path.string(); }
} // namespace Common::FS
namespace Common::TimeZone {
const std::array<const char*, 46>& GetTimeZoneStrings() { static const std::array<const char*, 46> z{}; return z; }
std::string FindSystemTimeZone() { return "UTC"; }
} // namespace Common::TimeZone

void AssertFailSoftImpl() {}
[[noreturn]] void AssertFatalImpl() { throw std::runtime_error("assert failed in the translator"); }
