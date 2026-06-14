#include "cortex/privacy_filter.hpp"

#include <algorithm>
#include <cctype>

namespace arise {

const char* PrivacyFilter::reasonToString(Reason r) {
    switch (r) {
        case Reason::CreditCard:        return "credit_card";
        case Reason::ApiKey:             return "api_key";
        case Reason::BearerToken:        return "bearer_token";
        case Reason::SshPath:            return "ssh_path";
        case Reason::PasswordContext:    return "password_context";
        case Reason::AwsAccessKey:       return "aws_access_key";
        case Reason::PrivateKeyHeader:   return "private_key_header";
    }
    return "unknown";
}

namespace {

bool isWordChar(char c) {
    auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '-';
}

bool isAlnum(char c) {
    return std::isalnum(static_cast<unsigned char>(c));
}

bool isDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c));
}

bool startsWith(std::string_view s, std::size_t i, std::string_view needle) {
    if (i + needle.size() > s.size()) return false;
    return s.compare(i, needle.size(), needle) == 0;
}

bool startsWithCaseInsensitive(std::string_view s, std::size_t i,
                               std::string_view needle) {
    if (i + needle.size() > s.size()) return false;
    for (std::size_t k = 0; k < needle.size(); ++k) {
        char a = char(std::tolower(static_cast<unsigned char>(s[i+k])));
        char b = char(std::tolower(static_cast<unsigned char>(needle[k])));
        if (a != b) return false;
    }
    return true;
}

// Walks `s` from `start` consuming characters that match `pred`. Returns
// the count consumed.
template <class Pred>
std::size_t spanWhile(std::string_view s, std::size_t start, Pred pred) {
    std::size_t i = start;
    while (i < s.size() && pred(s[i])) ++i;
    return i - start;
}

void mergeOverlapping(std::vector<PrivacyFilter::Hit>& v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.start < b.start; });
    std::vector<PrivacyFilter::Hit> out;
    out.push_back(v.front());
    for (std::size_t i = 1; i < v.size(); ++i) {
        auto& last = out.back();
        const auto& cur  = v[i];
        if (cur.start <= last.start + last.length) {
            auto end = std::max(last.start + last.length, cur.start + cur.length);
            last.length = end - last.start;
        } else {
            out.push_back(cur);
        }
    }
    v = std::move(out);
}

std::string buildRedacted(std::string_view text,
                          const std::vector<PrivacyFilter::Hit>& hits,
                          const std::string& replacement) {
    std::string out;
    out.reserve(text.size());
    std::size_t cursor = 0;
    for (const auto& h : hits) {
        if (h.start > cursor) out.append(text.substr(cursor, h.start - cursor));
        out.append(replacement);
        cursor = h.start + h.length;
    }
    if (cursor < text.size()) out.append(text.substr(cursor));
    return out;
}

} // namespace

bool PrivacyFilter::luhnCheck(std::string_view digits) {
    if (digits.empty()) return false;
    int sum = 0;
    bool double_it = false;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (!isDigit(*it)) return false;
        int d = *it - '0';
        if (double_it) { d *= 2; if (d > 9) d -= 9; }
        sum += d;
        double_it = !double_it;
    }
    return sum % 10 == 0;
}

std::string PrivacyFilter::preview(std::string_view text,
                                   std::size_t start, std::size_t len,
                                   std::size_t cap) {
    if (start >= text.size()) return {};
    auto take = std::min(len, text.size() - start);
    if (cap > 0 && take > cap) take = cap;
    std::string out(text.substr(start, take));
    if (len > take) out += "…";
    return out;
}

PrivacyFilter::PrivacyFilter(Config cfg) : cfg_(std::move(cfg)) {}

PrivacyFilter::Result PrivacyFilter::scan(std::string_view text) const {
    Result r;
    if (text.empty()) {
        r.passed = true;
        return r;
    }
    // Order matters only for redaction overlap merging.
    scanPrivateKeys_  (text, r);
    scanCreditCards_  (text, r);
    scanAwsKeys_      (text, r);
    scanApiKeys_      (text, r);
    scanBearerTokens_ (text, r);
    scanSshPaths_     (text, r);
    scanPasswordCtx_  (text, r);

    mergeOverlapping(r.hits);
    r.redacted_text = buildRedacted(text, r.hits, cfg_.replacement);
    r.passed = r.hits.empty();
    return r;
}

// ─── credit cards (Luhn) ───────────────────────────────────────────────────

void PrivacyFilter::scanCreditCards_(std::string_view text, Result& r) const {
    std::size_t i = 0;
    while (i < text.size()) {
        if (!isDigit(text[i])) { ++i; continue; }
        // Span digits + allow internal spaces / hyphens.
        std::size_t start = i;
        std::string digits_only;
        std::size_t j = i;
        while (j < text.size()) {
            char c = text[j];
            if (isDigit(c))                    { digits_only.push_back(c); ++j; }
            else if ((c == ' ' || c == '-') && !digits_only.empty()
                     && j + 1 < text.size()
                     && isDigit(text[j+1]))    { ++j; }
            else                                break;
        }
        if (int(digits_only.size()) >= cfg_.min_card_len
            && int(digits_only.size()) <= cfg_.max_card_len
            && luhnCheck(digits_only)) {
            // Don't flag if surrounded by other word characters that look
            // like part of an unrelated id / hash.
            bool boundary_left  = (start == 0)  || !isAlnum(text[start - 1]);
            bool boundary_right = (j == text.size()) || !isAlnum(text[j]);
            if (boundary_left && boundary_right) {
                Hit h;
                h.reason  = Reason::CreditCard;
                h.start   = start;
                h.length  = j - start;
                h.preview = preview(text, h.start, h.length);
                r.hits.push_back(h);
            }
        }
        i = j == i ? i + 1 : j;
    }
}

// ─── API keys ──────────────────────────────────────────────────────────────

void PrivacyFilter::scanApiKeys_(std::string_view text, Result& r) const {
    static const char* kPrefixes[] = {
        "sk-", "sk_live_", "sk_test_",
        "pk_live_", "pk_test_",
        "ghp_", "gho_", "ghu_", "ghs_", "ghr_",
        "xoxb-", "xoxp-", "xoxa-",
        "AIza",          // Google API key prefix
        "ya29.",         // Google OAuth bearer (start)
    };
    for (const char* prefix : kPrefixes) {
        std::string_view p(prefix);
        std::size_t i = 0;
        while ((i = text.find(p, i)) != std::string_view::npos) {
            if (i > 0 && isWordChar(text[i - 1])) { ++i; continue; }
            // Consume the body of the key.
            std::size_t end = i + p.size();
            std::size_t body = spanWhile(text, end, [](char c) {
                return isWordChar(c) || c == '.' || c == '/';
            });
            std::size_t total_len = (end + body) - i;
            int after_prefix = int(body);
            if (after_prefix >= cfg_.min_token_len) {
                Hit h;
                h.reason  = Reason::ApiKey;
                h.start   = i;
                h.length  = total_len;
                h.preview = preview(text, h.start, h.length);
                r.hits.push_back(h);
            }
            i = end + body;
            if (i == 0) ++i;       // safety
        }
    }
}

// ─── Bearer tokens ─────────────────────────────────────────────────────────

void PrivacyFilter::scanBearerTokens_(std::string_view text, Result& r) const {
    std::size_t i = 0;
    while (i < text.size()) {
        if (!startsWithCaseInsensitive(text, i, "Bearer")) { ++i; continue; }
        // Word boundary on the left.
        if (i > 0 && isWordChar(text[i - 1])) { ++i; continue; }
        std::size_t after = i + 6;
        if (after >= text.size() || (text[after] != ' ' && text[after] != '\t')) {
            ++i; continue;
        }
        // Skip whitespace.
        while (after < text.size()
               && (text[after] == ' ' || text[after] == '\t')) ++after;
        std::size_t body = spanWhile(text, after, [](char c) {
            return isWordChar(c) || c == '.';
        });
        if (int(body) >= cfg_.min_token_len) {
            Hit h;
            h.reason  = Reason::BearerToken;
            h.start   = i;
            h.length  = (after - i) + body;
            h.preview = preview(text, h.start, h.length);
            r.hits.push_back(h);
            i = after + body;
        } else {
            ++i;
        }
    }
}

// ─── SSH paths ─────────────────────────────────────────────────────────────

void PrivacyFilter::scanSshPaths_(std::string_view text, Result& r) const {
    static constexpr std::string_view kNeedle = "/.ssh/";
    std::size_t i = 0;
    while ((i = text.find(kNeedle, i)) != std::string_view::npos) {
        std::size_t start = i;
        // Walk back to the start of this path token.
        while (start > 0 && (isWordChar(text[start - 1]) || text[start - 1] == '/'
                             || text[start - 1] == '~')) --start;
        std::size_t end = i + kNeedle.size();
        end += spanWhile(text, end, [](char c) {
            return isWordChar(c) || c == '.' || c == '/';
        });
        Hit h;
        h.reason  = Reason::SshPath;
        h.start   = start;
        h.length  = end - start;
        h.preview = preview(text, h.start, h.length);
        r.hits.push_back(h);
        i = end;
    }
}

// ─── password=foo ──────────────────────────────────────────────────────────

void PrivacyFilter::scanPasswordCtx_(std::string_view text, Result& r) const {
    static const char* kKeys[] = {
        "password", "passwd", "passphrase", "secret_key", "secret-key",
        "client_secret",
    };
    auto lower_at = [&](std::size_t i) {
        return char(std::tolower(static_cast<unsigned char>(text[i])));
    };
    for (const char* key : kKeys) {
        std::string_view kv(key);
        std::size_t i = 0;
        while (i + kv.size() < text.size()) {
            // case-insensitive prefix match
            bool match = true;
            for (std::size_t k = 0; k < kv.size(); ++k) {
                if (lower_at(i + k) != kv[k]) { match = false; break; }
            }
            if (!match) { ++i; continue; }
            // word boundary on the left
            if (i > 0 && isWordChar(text[i - 1])) { ++i; continue; }
            // sep ∈ {=, :, ' '} possibly with surrounding whitespace
            std::size_t j = i + kv.size();
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
            if (j >= text.size() || (text[j] != '=' && text[j] != ':')) { ++i; continue; }
            ++j;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t' ||
                                        text[j] == '"' || text[j] == '\'')) ++j;
            std::size_t body = spanWhile(text, j, [](char c) {
                return c != '\n' && c != '\r' && c != '"' && c != '\'';
            });
            if (body >= 4) {
                Hit h;
                h.reason  = Reason::PasswordContext;
                h.start   = i;
                h.length  = (j - i) + body;
                h.preview = preview(text, h.start, h.length);
                r.hits.push_back(h);
                i = j + body;
            } else {
                ++i;
            }
        }
    }
}

// ─── AWS access keys ───────────────────────────────────────────────────────

void PrivacyFilter::scanAwsKeys_(std::string_view text, Result& r) const {
    // AKIA[0-9A-Z]{16}
    std::size_t i = 0;
    while (i + 20 <= text.size()) {
        if (!startsWith(text, i, "AKIA")) { ++i; continue; }
        if (i > 0 && isWordChar(text[i - 1])) { ++i; continue; }
        bool ok = true;
        for (std::size_t k = 4; k < 20; ++k) {
            char c = text[i + k];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
                ok = false; break;
            }
        }
        if (!ok) { ++i; continue; }
        if (i + 20 < text.size() && isWordChar(text[i + 20])) { ++i; continue; }
        Hit h;
        h.reason  = Reason::AwsAccessKey;
        h.start   = i;
        h.length  = 20;
        h.preview = preview(text, h.start, h.length);
        r.hits.push_back(h);
        i += 20;
    }
}

// ─── PEM private key headers ───────────────────────────────────────────────

void PrivacyFilter::scanPrivateKeys_(std::string_view text, Result& r) const {
    static constexpr std::string_view kBegin = "-----BEGIN";
    static constexpr std::string_view kEnd   = "-----END";
    std::size_t i = 0;
    while ((i = text.find(kBegin, i)) != std::string_view::npos) {
        // Look ahead for "PRIVATE KEY-----"
        auto rest = text.substr(i, std::min<std::size_t>(80, text.size() - i));
        if (rest.find("PRIVATE KEY") == std::string_view::npos) { ++i; continue; }
        std::size_t end = text.find(kEnd, i);
        if (end == std::string_view::npos) end = text.size();
        else {
            auto end_close = text.find("-----", end + kEnd.size());
            end = (end_close == std::string_view::npos) ? text.size()
                                                         : end_close + 5;
        }
        Hit h;
        h.reason  = Reason::PrivateKeyHeader;
        h.start   = i;
        h.length  = end - i;
        h.preview = preview(text, h.start, h.length);
        r.hits.push_back(h);
        i = end;
    }
}

} // namespace arise
