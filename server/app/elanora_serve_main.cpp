// elanora_serve -- hosts the phone collector and receives its recordings.
//
//   elanora_serve --port 8080 --web web --root data/datasets
//
// Binds 0.0.0.0 on purpose: the phone reaches this over the LAN, so binding
// localhost would serve only the machine that cannot do Bluetooth anyway.
//
// No TLS. Whether the phone needs HTTPS depends on whether Bluefy enforces
// secure context for Web Bluetooth, which the probe page answers in two
// minutes; adding OpenSSL on MSVC before knowing would be a large cost for a
// maybe.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "elanora/collector/session.hpp"
#include "elanora/server/round_store.hpp"
#include "elanora/server/schedule_json.hpp"
#include "httplib.h"

namespace {

std::string local_addresses(int port) {
    // Printed at startup so the operator does not have to go and run ipconfig
    // while wearing a headset.
    std::string out;
    char host[256]{};
    if (gethostname(host, sizeof(host) - 1) != 0) return out;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return out;
    for (addrinfo* p = res; p; p = p->ai_next) {
        char buf[64]{};
        auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) {
            out += "    http://" + std::string(buf) + ":" + std::to_string(port) + "\n";
        }
    }
    freeaddrinfo(res);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8080;
    std::string web = "web";
    std::string root = "data/datasets";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--web") == 0 && i + 1 < argc) web = argv[++i];
        else if (std::strcmp(argv[i], "--root") == 0 && i + 1 < argc) root = argv[++i];
    }

    httplib::Server srv;
    if (!srv.set_mount_point("/", web)) {
        std::fprintf(stderr, "cannot serve %s -- run from the repo root, or pass --web\n",
                     web.c_str());
        return 1;
    }

    // A round is ~400 KB of CSV across four streams. The default cap would
    // reject every upload with a 413 and no explanation.
    srv.set_payload_max_length(64ull * 1024 * 1024);

    // No caching. The phone must never run a stale copy of the collector after
    // a fix, and silently doing so would look like the fix failed.
    srv.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
    });

    srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });

    srv.Get("/schedule", [](const httplib::Request& req, httplib::Response& res) {
        using namespace elanora::collector;
        auto num = [&](const char* k, int dflt) {
            if (!req.has_param(k)) return dflt;
            try { return std::stoi(req.get_param_value(k)); }
            catch (const std::exception&) { return dflt; }
        };
        uint64_t seed = 84120ull;
        if (req.has_param("seed")) {
            try { seed = std::stoull(req.get_param_value("seed")); }
            catch (const std::exception&) { /* keep the default */ }
        }
        const int count = num("count", kProtocolFreqCount);
        const int jitter = num("jitter", kProtocolJitter);
        const int tone = num("tone", kProtocolTone);

        const auto freqs = geometric_set(kProtocolFreqLo, kProtocolFreqHi, count);
        const auto rounds = build_schedule(freqs, jitter, tone, seed);
        res.set_content(elanora::server::schedule_json(rounds), "application/json");
    });

    srv.Post("/round", [&root](const httplib::Request& req, httplib::Response& res) {
        elanora::server::RoundUpload r;
        std::string err;
        if (!elanora::server::parse_round(req.body, r, err) ||
            !elanora::server::store_round(root, r, err)) {
            // 400 with the reason, never a bare 500: the phone shows this text
            // to the operator, who is the only person who can act on it.
            res.status = 400;
            res.set_content(err, "text/plain");
            std::fprintf(stderr, "round rejected: %s\n", err.c_str());
            return;
        }
        std::printf("stored %s (%s)\n", r.trial_id.c_str(), r.condition.c_str());
        std::fflush(stdout);
        res.set_content("stored", "text/plain");
    });

    std::printf("elanora_serve  web=%s  root=%s\n", web.c_str(), root.c_str());
    std::printf("  open on the phone, in Bluefy:\n%s", local_addresses(port).c_str());
    std::fflush(stdout);

    if (!srv.listen("0.0.0.0", port)) {
        std::fprintf(stderr, "listen failed on port %d\n", port);
        return 1;
    }
    return 0;
}
