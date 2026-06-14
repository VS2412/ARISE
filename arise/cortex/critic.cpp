#include "cortex/critic.hpp"

#include "cortex/sub_agent.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace arise {

namespace {

// Forbidden patterns. We do dumb lowercase substring matching plus a couple
// of regex-y manual scans so we don't have to pull in <regex> here. Each
// entry is a phrase that, once found in lowercased content, makes the
// content unconditionally unsafe. Keep entries short and specific so we
// don't reject benign mentions ("don't run rm -rf accidentally").
const std::vector<std::string>& kBuiltin() {
    static const std::vector<std::string> v = {
        "rm -rf /",
        "rm -rf ~",
        "rm -rf $home",
        "rm -rf .",
        "rm  -rf /",         // double space defence
        ":(){:|:&};:",       // classic fork bomb (de-spaced)
        "dd if=/dev/zero",
        "dd if=/dev/urandom of=/dev/",
        "mkfs.",
        "mkfs ",
        "chmod 777 /",
        "chmod -r 777",
        "chown -r root",
        "curl | sh",
        "curl|sh",
        "wget | sh",
        "wget|sh",
        "| sh",          // catches `curl http://… | sh` regardless of body
        "| bash",        // ditto for bash
        "| bash -",
        "| sudo sh",
        "sudo rm -rf",
        "/dev/sda",
        "shutdown -h now",
        "reboot now",
        "halt -p",
    };
    return v;
}

std::string lowerNoWs(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            // Keep one space so denylist tokens with spaces still match,
            // collapse multiples to avoid dodging via padding.
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else {
            out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

std::string trimToTail(const std::string& s, int max_chars) {
    if (max_chars <= 0 || int(s.size()) <= max_chars) return s;
    return std::string("[…truncated…]\n") + s.substr(s.size() - max_chars);
}

} // namespace

const std::vector<std::string>& Critic::builtinDenylist() { return kBuiltin(); }

std::vector<std::string>
Critic::matchDenylist(const std::string& content,
                      const std::vector<std::string>& extra_patterns) {
    std::vector<std::string> hits;
    if (content.empty()) return hits;

    auto haystack = lowerNoWs(content);
    auto check = [&](const std::vector<std::string>& patterns) {
        for (const auto& p : patterns) {
            if (p.empty()) continue;
            auto needle = lowerNoWs(p);
            if (haystack.find(needle) != std::string::npos) {
                hits.push_back(p);
            }
        }
    };
    check(kBuiltin());
    check(extra_patterns);

    // Dedupe, preserve order.
    std::vector<std::string> deduped;
    for (auto& h : hits) {
        if (std::find(deduped.begin(), deduped.end(), h) == deduped.end()) {
            deduped.push_back(h);
        }
    }
    return deduped;
}

Critic::Critic(Config cfg) : cfg_(std::move(cfg)) {}

Critic::Review Critic::reviewContent(const std::string& content,
                                     std::string_view kind) const {
    Review r;

    if (content.empty()) {
        r.verdict  = "empty content — auto-reject";
        r.approved = false;
        return r;
    }

    auto matches = matchDenylist(content, cfg_.additional_denylist);
    r.matches = matches;
    if (!matches.empty()) {
        r.approved = false;
        r.verdict  = "rejected: matched denylist pattern(s)";
        return r;
    }

    if (!cfg_.require_llm_for_approval) {
        r.approved = true;
        r.verdict  = "approved: no denylist match (llm consultation disabled)";
        return r;
    }

    // Need an LLM but none provided → safe default reject.
    if (!cfg_.llm) {
        r.approved = false;
        r.verdict  = "rejected: llm required but not configured";
        return r;
    }

    auto llm_review = askLlm_(content, kind);
    r.approved = llm_review.approved;
    r.from_llm = true;
    r.verdict  = llm_review.verdict;
    r.llm_raw  = llm_review.llm_raw;
    return r;
}

Critic::Review Critic::askLlm_(const std::string& content,
                               std::string_view kind) const {
    Review r;
    r.from_llm = true;
    if (!cfg_.llm) {
        r.approved = false;
        r.verdict  = "rejected: llm not configured";
        return r;
    }

    std::string preview = trimToTail(content, cfg_.llm_max_chars);
    std::string prompt =
        "You are ARISE's Critic. Decide whether to APPROVE or REJECT the "
        "following content for execution / persistence. Reply JSON only:\n"
        "{\"approved\": <true|false>, \"reason\": \"<one short phrase>\"}\n\n"
        "Reject if the content:\n"
        " - performs destructive filesystem operations on user data,\n"
        " - elevates privileges (sudo) without an obvious benign reason,\n"
        " - exfiltrates data to a remote host,\n"
        " - rewrites init / system services,\n"
        " - opens network sockets / listeners not justified by the task,\n"
        " - looks obfuscated or hides intent.\n"
        "Approve only if the content's purpose is clear and bounded.\n\n"
        "Kind: ";
    prompt.append(kind);
    prompt.append("\nContent:\n");
    prompt.append(preview);
    prompt.append("\nJSON:");

    auto run = cfg_.llm->run(prompt);
    r.llm_raw = run.raw;
    if (!run.ok) {
        r.approved = false;
        r.verdict  = "rejected: llm unreachable or unparseable (" + run.error + ")";
        return r;
    }
    auto blob = SubAgent::firstJsonObject(run.output);
    if (!blob) {
        r.approved = false;
        r.verdict  = "rejected: llm produced no JSON";
        return r;
    }
    try {
        auto j = json::parse(*blob);
        bool ok = j.value("approved", false);
        std::string reason = j.value("reason", std::string{});
        r.approved = ok;
        r.verdict  = (ok ? "approved by llm: " : "rejected by llm: ") + reason;
        return r;
    } catch (const std::exception& e) {
        r.approved = false;
        r.verdict  = std::string("rejected: llm parse error: ") + e.what();
        return r;
    }
}

} // namespace arise
