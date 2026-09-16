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
#include <random>
#include <string>

#include "elanora/collector/session.hpp"
#include "elanora/server/round_store.hpp"
#include "elanora/server/schedule_json.hpp"
#include "httplib.h"

namespace {

// A write token, because the intended deployment is a public tunnel.
//
// Reaching the phone requires HTTPS (Bluefy refuses plain HTTP outright), and
// the cheapest trusted certificate is a tunnel with a public hostname. The URL
// is unguessable, but "unguessable" is not "authenticated": anyone who learned
// it could POST fabricated trials into the dataset. For a project whose entire
// design rests on being able to trust the data, an injected round is a worse
// outcome than a lost one.
//
// Reads stay open. Serving the page and the schedule to a stranger costs
// nothing; writing to the dataset is what needs proving.
std::string random_token() {
    static const char* kHex = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i) out += kHex[rd() % 16];
    return out;
}

bool authorized(const httplib::Request& req, const std::string& token) {
    if (token.empty()) return true;   // explicitly disabled with --no-token
    // Header first; the query parameter exists because a page loaded from a
    // scanned QR carries the token in its URL and nothing else.
    if (req.get_header_value("X-Elanora-Token") == token) return true;
    if (req.has_param("k") && req.get_param_value("k") == token) return true;
    return false;
}

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
    std::string token;
    bool no_token = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--web") == 0 && i + 1 < argc) web = argv[++i];
        else if (std::strcmp(argv[i], "--root") == 0 && i + 1 < argc) root = argv[++i];
        else if (std::strcmp(argv[i], "--token") == 0 && i + 1 < argc) token = argv[++i];
        else if (std::strcmp(argv[i], "--no-token") == 0) no_token = true;
    }
    if (!no_token && token.empty()) token = random_token();

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

    // Every request, with the peer address. Without this a phone that cannot
    // reach the machine at all and a phone that reaches it and then fails in
    // the page look identical from the operator's side -- which is exactly the
    // ambiguity that makes "it doesn't work" impossible to act on.
    srv.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        std::printf("  %s  %s %s -> %d\n", req.remote_addr.c_str(),
                    req.method.c_str(), req.path.c_str(), res.status);
        std::fflush(stdout);
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
        // Coverage across sessions rather than session length: bank b of n
        // takes every nth frequency, so each session stays near 18 rounds
        // while n sessions together cover the full set.
        const int banks = num("banks", 1);
        const int bank = num("bank", 0);

        const auto all = geometric_set(kProtocolFreqLo, kProtocolFreqHi, count);
        const auto freqs = frequency_bank(all, bank, banks);
        const auto rounds = build_schedule(freqs, jitter, tone, seed);
        res.set_content(elanora::server::schedule_json(rounds), "application/json");
    });

    // Read-only, so no token: it exposes what is already being served, and an
    // operator watching rounds land should not have to paste a secret to do it.
    srv.Get("/status", [&root](const httplib::Request&, httplib::Response& res) {
        res.set_content(elanora::server::dataset_status_json(root), "application/json");
    });

    srv.Post("/round", [&root, &token](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, token)) {
            res.status = 401;
            res.set_content("bad or missing write token", "text/plain");
            return;
        }
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

    srv.Post("/survey", [&root, &token](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, token)) {
            res.status = 401;
            res.set_content("bad or missing write token", "text/plain");
            return;
        }
        std::string err;
        if (!elanora::server::store_survey(root, req.body, err)) {
            res.status = 400;
            res.set_content(err, "text/plain");
            return;
        }
        res.set_content("stored", "text/plain");
    });

    srv.Post("/session", [&root, &token](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, token)) {
            res.status = 401;
            res.set_content("bad or missing write token", "text/plain");
            return;
        }
        std::string err;
        if (!elanora::server::store_session(root, req.body, err)) {
            res.status = 400;
            res.set_content(err, "text/plain");
            return;
        }
        res.set_content("stored", "text/plain");
    });

    srv.Post("/subject", [&root, &token](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, token)) {
            res.status = 401;
            res.set_content("bad or missing write token", "text/plain");
            return;
        }
        std::string err;
        if (!elanora::server::store_subject(root, req.body, err)) {
            res.status = 400;
            res.set_content(err, "text/plain");
            return;
        }
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
