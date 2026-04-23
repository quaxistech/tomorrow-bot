#include "runtime_manifest.hpp"

#ifndef TB_GIT_SHA
#define TB_GIT_SHA "unknown"
#endif
#ifndef TB_VERSION
#define TB_VERSION "0.0.0"
#endif
#ifndef TB_BUILD_TYPE
#define TB_BUILD_TYPE "unknown"
#endif

namespace tb::app {

RuntimeManifest RuntimeManifest::build(const std::string& config_hash,
                                        const std::string& exchange_endpoint,
                                        int64_t now_ns)
{
    RuntimeManifest m;
    m.git_sha = TB_GIT_SHA;
    m.version = TB_VERSION;
    m.build_type = TB_BUILD_TYPE;
    m.config_hash = config_hash;
    m.exchange_endpoint = exchange_endpoint;
    m.session_start_ns = now_ns;
    return m;
}

} // namespace tb::app
